/**
 * @file AcquisitionEngine.cc
 * @brief Implementation of AndorEMCCDcontroller::AcquisitionEngine.
 */

#include <AndorEMCCDcontroller/AcquisitionEngine.h>

#include <algorithm>
#include <chrono>
#include <fstream>
#include <stdexcept>

#include <spdlog/spdlog.h>

#include <AndorEMCCDcontroller/Telemetry.h>

namespace AndorEMCCDcontroller {

namespace {

constexpr int kWaitTimeoutMs = 50;
constexpr int kIdleTimeoutMs = 50;

double SteadyNow() {
  using namespace std::chrono;
  return duration<double>(steady_clock::now().time_since_epoch()).count();
}

std::string Join(const std::vector<std::string>& items) {
  std::string out;
  for (const auto& item : items) out += (out.empty() ? "" : "; ") + item;
  return out;
}

std::int64_t ExpectedFrames(const CameraSettings& s) {
  switch (s.acquisition_mode) {
    case AcquisitionMode::kSingle:
    case AcquisitionMode::kAccumulate:
      return 1;
    case AcquisitionMode::kKinetic:
      return s.number_kinetics;
    case AcquisitionMode::kContinuous:
      return 0;
  }
  return 0;
}

}  // namespace

bool LoadSettingsFile(const std::string& path, CameraSettings* s,
                      std::string* error) {
  std::ifstream in(path);
  if (!in) return true;
  try {
    Json j = Json::parse(in);
    std::vector<std::string> errors;
    if (!MergeSettings(j, s, &errors)) {
      *error = path + ": " + Join(errors);
      return false;
    }
  } catch (const std::exception& e) {
    *error = path + ": " + e.what();
    return false;
  }
  return true;
}

AcquisitionEngine::AcquisitionEngine(std::unique_ptr<Camera> camera,
                                     zmq::context_t* context,
                                     EngineConfig config)
    : camera_(std::move(camera)),
      context_(context),
      config_(std::move(config)),
      store_(config_.history_frames, config_.history_bytes),
      settings_(config_.initial_settings),
      thermal_watch_(config_.cooling_lost_margin_c) {}

AcquisitionEngine::~AcquisitionEngine() { Stop(); }

bool AcquisitionEngine::BindSockets(std::string* error) {
  pub_ = std::make_unique<zmq::socket_t>(*context_, zmq::socket_type::pub);
  // Each frame is three parts; the HWM counts parts.
  pub_->set(zmq::sockopt::sndhwm, config_.publish_hwm * 3);
  pub_->set(zmq::sockopt::linger, 0);
  for (const auto& endpoint : config_.data_endpoints) {
    try {
      pub_->bind(endpoint);
      spdlog::info("data channel listening on {}", endpoint);
    } catch (const zmq::error_t& e) {
      *error = "bind " + endpoint + ": " + e.what();
      return false;
    }
  }
  events_ = std::make_unique<zmq::socket_t>(*context_, zmq::socket_type::push);
  events_->set(zmq::sockopt::sndhwm, 1);
  events_->set(zmq::sockopt::linger, 0);
  events_->connect(kEngineEventsEndpoint);
  return true;
}

bool AcquisitionEngine::Start(std::string* error) {
  if (Running()) return true;
  if (!camera_->Open(error)) {
    {
      std::lock_guard<std::mutex> lock(status_mutex_);
      status_.state = CameraState::kDisconnected;
      status_.last_error = *error;
      status_.timestamp = UnixTimeNow();
    }
    PublishStatus();
    return false;
  }
  std::vector<std::string> warnings =
      camera_->ApplyAcquisitionSettings(&settings_);
  {
    std::lock_guard<std::mutex> lock(status_mutex_);
    status_.em_gain = EffectiveEmGain(settings_);
  }
  // Never start an uncontrolled warm-up at boot: if the sensor is being
  // cooled (e.g. cooler mode "maintain" from a previous session), keep it.
  const TemperatureReading now = camera_->ReadTemperature();
  if (now.cooler_on && !settings_.cooler_on) {
    warnings.push_back("cooler was running; kept on (set cooler_on=false to "
                       "switch it off)");
    settings_.cooler_on = true;
  }
  const auto thermal = camera_->ApplyThermalSettings(settings_);
  warnings.insert(warnings.end(), thermal.begin(), thermal.end());
  if (!warnings.empty()) spdlog::warn("initial settings: {}", Join(warnings));
  acquisition_dirty_ = false;
  info_ = camera_->Info();
  spdlog::info("camera: {} serial {} ({}x{})", info_.model, info_.serial,
               info_.sensor_width, info_.sensor_height);
  if (!info_.thermal_probe.empty()) {
    spdlog::info("thermal readouts: {}", info_.thermal_probe);
  }
  {
    std::lock_guard<std::mutex> lock(status_mutex_);
    status_.state = CameraState::kIdle;
    status_.last_error = Join(warnings);
  }
  stop_ = false;
  thread_ = std::thread(&AcquisitionEngine::ThreadMain, this);
  return true;
}

void AcquisitionEngine::Stop() {
  if (!thread_.joinable()) return;
  stop_ = true;
  camera_->Interrupt();
  task_cv_.notify_all();
  thread_.join();
}

Json AcquisitionEngine::Call(std::function<Json()> fn) {
  if (!Running()) {
    throw std::runtime_error("camera not available: " + Status().last_error);
  }
  auto task = std::make_unique<Task>();
  task->fn = std::move(fn);
  std::future<Json> result = task->result.get_future();
  {
    std::lock_guard<std::mutex> lock(task_mutex_);
    tasks_.push_back(std::move(task));
  }
  task_cv_.notify_all();
  camera_->Interrupt();
  if (result.wait_for(std::chrono::duration<double>(config_.call_timeout_s)) !=
      std::future_status::ready) {
    // The task stays queued; its result is discarded when it finally runs.
    throw std::runtime_error("camera thread did not respond");
  }
  return result.get();
}

Json AcquisitionEngine::GetInfo() {
  return Call([this] { return Json(info_); });
}

Json AcquisitionEngine::GetSettings() {
  return Call([this] { return Json(settings_); });
}

Json AcquisitionEngine::GetCalibration(double wavelength_nm) {
  if (!(wavelength_nm >= 100.0 && wavelength_nm <= 2000.0))
    throw std::runtime_error("wavelength_nm must be 100..2000");
  return Call([this, wavelength_nm] {
    return camera_->GetCalibration(wavelength_nm);
  });
}

Json AcquisitionEngine::SetSettings(const Json& patch) {
  return Call([this, patch] { return DoSetSettings(patch); });
}

Json AcquisitionEngine::StartRun(const Json& args) {
  return Call([this, args] {
    std::vector<std::string> warnings;
    if (args.contains("settings")) {
      if (acquiring_) DoAbort(true);
      Json r = DoSetSettings(args.at("settings"));
      for (const auto& w : r["warnings"]) warnings.push_back(w);
    }
    if (acquiring_) DoAbort(true);
    Json r = DoStartRun();
    for (const auto& w : r["warnings"]) warnings.push_back(w);
    r["warnings"] = warnings;
    return r;
  });
}

Json AcquisitionEngine::Arm(const Json& args) {
  return Call([this, args] {
    if (acquiring_) DoAbort(true);
    Json patch = args.value("settings", Json::object());
    const int n = args.value("n_frames", settings_.number_kinetics);
    if (n < 1) throw std::runtime_error("n_frames must be >= 1");
    patch["acquisition_mode"] = "kinetic";
    patch["number_kinetics"] = n;
    Json set = DoSetSettings(patch);
    Json r = DoStartRun();
    for (const auto& w : r["warnings"]) set["warnings"].push_back(w);
    r["warnings"] = set["warnings"];
    return r;
  });
}

Json AcquisitionEngine::StopRun() {
  return Call([this] {
    const bool was = acquiring_;
    if (acquiring_) DoAbort(true);
    return Json{{"stopped", was}, {"run_id", run_id_}};
  });
}

Json AcquisitionEngine::SoftwareTrigger() {
  return Call([this] {
    std::string error;
    if (!camera_->SendSoftwareTrigger(&error)) {
      throw std::runtime_error(error);
    }
    return Json{{"run_id", run_id_}};
  });
}

void AcquisitionEngine::SetControl(const ControlInfo& control) {
  {
    std::lock_guard<std::mutex> lock(status_mutex_);
    status_.control = control;
  }
  publish_now_ = true;
  task_cv_.notify_all();
}

void AcquisitionEngine::PublishWhileStopped() {
  if (Running()) return;
  {
    std::lock_guard<std::mutex> lock(status_mutex_);
    status_.timestamp = UnixTimeNow();
  }
  PublishStatus();
}

CameraStatus AcquisitionEngine::Status() const {
  std::lock_guard<std::mutex> lock(status_mutex_);
  return status_;
}

Json AcquisitionEngine::DoSetSettings(const Json& patch) {
  CameraSettings candidate = settings_;
  std::vector<std::string> errors;
  if (!MergeSettings(patch, &candidate, &errors)) {
    throw std::runtime_error(Join(errors));
  }
  bool acquisition_changed = false;
  bool thermal_changed = false;
  for (auto it = patch.begin(); it != patch.end(); ++it) {
    (IsThermalKey(it.key()) ? thermal_changed : acquisition_changed) = true;
  }

  std::vector<std::string> warnings;
  bool restart = false;
  if (acquiring_ && acquisition_changed) {
    if (ExpectedFrames(settings_) != 0) {
      throw std::runtime_error(
          "run " + std::to_string(run_id_) +
          " is in progress; call stop before changing acquisition settings");
    }
    DoAbort(true);
    restart = true;
  }
  if (patch.contains("em_gain") && !candidate.em_gain_enabled &&
      candidate.output_amplifier == 0) {
    warnings.push_back("em_gain is stored but EM gain is off "
                       "(set em_gain_enabled=true to use it)");
  }
  if (acquisition_changed) {
    settings_ = candidate;
    acquisition_dirty_ = true;
    ApplyAcquisitionIfDirty(&warnings);
  }
  if (thermal_changed) {
    settings_.cooler_on = candidate.cooler_on;
    settings_.target_temperature_c = candidate.target_temperature_c;
    settings_.fan_mode = candidate.fan_mode;
    settings_.cooler_persist_on_shutdown = candidate.cooler_persist_on_shutdown;
    if (acquiring_) {
      thermal_pending_ = true;  // Applied when the run ends.
      warnings.push_back("cooler settings will be applied after this run");
    } else {
      const auto w = camera_->ApplyThermalSettings(settings_);
      warnings.insert(warnings.end(), w.begin(), w.end());
    }
  }
  SaveSettings();
  info_ = camera_->Info();

  Json result{{"settings", settings_},
              {"warnings", warnings},
              {"timings", camera_->GetTimings()}};
  if (restart) {
    Json r = DoStartRun();
    result["run_id"] = r["run_id"];
  }
  {
    std::lock_guard<std::mutex> lock(status_mutex_);
    status_.timings = camera_->GetTimings();
  }
  return result;
}

void AcquisitionEngine::ApplyAcquisitionIfDirty(
    std::vector<std::string>* warnings) {
  if (!acquisition_dirty_) return;
  const auto w = camera_->ApplyAcquisitionSettings(&settings_);
  warnings->insert(warnings->end(), w.begin(), w.end());
  acquisition_dirty_ = false;
  {
    std::lock_guard<std::mutex> lock(status_mutex_);
    status_.em_gain = EffectiveEmGain(settings_);
  }
  if (!w.empty()) spdlog::warn("apply settings: {}", Join(w));
}

Json AcquisitionEngine::DoStartRun() {
  std::vector<std::string> warnings;
  ApplyAcquisitionIfDirty(&warnings);
  std::string error;
  if (!camera_->StartAcquisition(&error)) {
    {
      std::lock_guard<std::mutex> lock(status_mutex_);
      status_.last_error = error;
    }
    throw std::runtime_error(error);
  }
  acquiring_ = true;
  ++run_id_;
  run_frames_ = 0;
  RunInfo run;
  run.run_id = run_id_;
  run.mode = ToString(settings_.acquisition_mode);
  run.expected = ExpectedFrames(settings_);
  run.first_seq = next_seq_;
  run.start_time = UnixTimeNow();
  store_.BeginRun(run);
  fps_window_start_ = SteadyNow();
  fps_window_frames_ = 0;
  fps_ = 0.0;
  spdlog::info("run {} started ({}, trigger {}, {} frame(s))", run_id_,
               run.mode, ToString(settings_.trigger_mode), run.expected);
  {
    std::lock_guard<std::mutex> lock(status_mutex_);
    status_.state = CameraState::kAcquiring;
    status_.run_id = run_id_;
    status_.run_mode = run.mode;
    status_.frames_in_run = 0;
    status_.frames_expected = run.expected;
    status_.run_complete = false;
    status_.timings = camera_->GetTimings();
  }
  PublishStatus();
  NotifyEvent();
  return Json{{"run_id", run_id_},
              {"expected", run.expected},
              {"timings", camera_->GetTimings()},
              {"warnings", warnings}};
}

void AcquisitionEngine::DoAbort(bool aborted) {
  camera_->AbortAcquisition();
  DrainFrames();
  FinishRun(aborted);
}

void AcquisitionEngine::FinishRun(bool aborted) {
  if (!acquiring_) return;
  acquiring_ = false;
  store_.EndRun(run_id_, aborted);
  spdlog::info("run {} {} with {} frame(s)", run_id_,
               aborted ? "aborted" : "complete", run_frames_);
  if (thermal_pending_) {
    thermal_pending_ = false;
    const auto w = camera_->ApplyThermalSettings(settings_);
    if (!w.empty()) spdlog::warn("cooler settings: {}", Join(w));
  }
  {
    std::lock_guard<std::mutex> lock(status_mutex_);
    status_.state = CameraState::kIdle;
    status_.run_complete = true;
    status_.fps = 0.0;
  }
  PublishStatus();
  NotifyEvent();
}

void AcquisitionEngine::ThreadMain() {
  double next_status = 0.0;
  while (!stop_) {
    try {
      LoopOnce(&next_status);
    } catch (const std::exception& e) {
      // Keep the thread (and the camera) alive: report and carry on.
      spdlog::error("engine: {}", e.what());
      {
        std::lock_guard<std::mutex> lock(status_mutex_);
        status_.last_error = std::string("engine: ") + e.what();
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
  }
  try {
    if (acquiring_) DoAbort(true);
    RunPendingTasks();
    WarmUp();
  } catch (const std::exception& e) {
    spdlog::error("shutdown: {}", e.what());
  }
  camera_->Close();
  {
    std::lock_guard<std::mutex> lock(status_mutex_);
    status_.state = CameraState::kDisconnected;
  }
  PublishStatus();
}

void AcquisitionEngine::LoopOnce(double* next_status_ptr) {
  double& next_status = *next_status_ptr;
  RunPendingTasks();
  if (acquiring_) {
    camera_->WaitForData(kWaitTimeoutMs);
    DrainFrames();
    if (!camera_->IsAcquiring()) {
      DrainFrames();
      FinishRun(false);
    }
  } else {
    std::unique_lock<std::mutex> lock(task_mutex_);
    task_cv_.wait_for(lock, std::chrono::milliseconds(kIdleTimeoutMs),
                      [this] {
                        return stop_ || !tasks_.empty() || publish_now_;
                      });
  }
  const double now = SteadyNow();
  if (now >= next_status || publish_now_.exchange(false)) {
    next_status = now + config_.status_interval_s;
    PollStatus();
    PublishStatus();
  }
}

void AcquisitionEngine::RunPendingTasks() {
  for (;;) {
    std::unique_ptr<Task> task;
    {
      std::lock_guard<std::mutex> lock(task_mutex_);
      if (tasks_.empty()) return;
      task = std::move(tasks_.front());
      tasks_.pop_front();
    }
    try {
      task->result.set_value(task->fn());
    } catch (...) {
      task->result.set_exception(std::current_exception());
    }
  }
}

void AcquisitionEngine::DrainFrames() {
  std::vector<Frame> frames;
  std::string error;
  if (!camera_->RetrieveNewFrames(&frames, &error)) {
    spdlog::error("{}", error);
  } else if (!error.empty()) {
    spdlog::warn("{}", error);
  }
  if (!error.empty()) {
    std::lock_guard<std::mutex> lock(status_mutex_);
    status_.last_error = error;
  }
  if (frames.empty()) return;

  const double now = UnixTimeNow();
  const Timings timings = camera_->GetTimings();
  for (auto& frame : frames) {
    FrameHeader& h = frame.header;
    h.run_id = run_id_;
    h.index = run_frames_++;
    h.seq = next_seq_++;
    h.exposure_s = timings.exposure_s;
    h.em_gain = EffectiveEmGain(settings_);
    h.timestamp = now;
    h.temperature_c = temperature_.temperature_c;
    h.camera_settings = settings_;
    PublishFrame(frame);
    store_.Append(std::move(frame));
  }

  fps_window_frames_ += static_cast<std::int64_t>(frames.size());
  const double t = SteadyNow();
  if (t - fps_window_start_ >= 1.0) {
    fps_ = fps_window_frames_ / (t - fps_window_start_);
    fps_window_start_ = t;
    fps_window_frames_ = 0;
  }
  {
    std::lock_guard<std::mutex> lock(status_mutex_);
    status_.frames_in_run = run_frames_;
    status_.total_frames = next_seq_;
    status_.fps = fps_;
  }
  NotifyEvent();
}

void AcquisitionEngine::PollStatus() {
  temperature_ = camera_->ReadTemperature();
  const int target = std::clamp(settings_.target_temperature_c,
                                info_.temperature_min_c,
                                info_.temperature_max_c);
  if (thermal_watch_.Update(temperature_, target)) {
    switch (thermal_watch_.Level()) {
      case ThermalAlarm::kAlarm:
        spdlog::error("thermal alarm: {}", thermal_watch_.Reason());
        break;
      case ThermalAlarm::kWarning:
        spdlog::warn("thermal warning: {}", thermal_watch_.Reason());
        break;
      case ThermalAlarm::kOk:
        spdlog::info("thermal state normal again ({:.1f} C)",
                     temperature_.temperature_c);
        break;
    }
  }
  std::lock_guard<std::mutex> lock(status_mutex_);
  status_.temperature_c = temperature_.temperature_c;
  status_.temperature_status = temperature_.status;
  status_.cooler_on = temperature_.cooler_on;
  status_.target_temperature_c = target;
  status_.fan_mode = settings_.fan_mode;
  status_.ambient_temperature_c = temperature_.ambient_temperature_c;
  status_.cooler_voltage_v = temperature_.cooler_voltage_v;
  status_.tec_overheat = temperature_.tec_overheat;
  status_.thermal_alarm = thermal_watch_.Level();
  status_.thermal_alarm_reason = thermal_watch_.Reason();
  status_.thermal_alarm_count = thermal_watch_.AlarmCount();
  status_.timestamp = UnixTimeNow();
}

void AcquisitionEngine::PublishStatus() {
  const CameraStatus status = Status();
  if (config_.telemetry) config_.telemetry->Offer(status);
  if (!pub_) return;
  const std::string body = Json(status).dump();
  pub_->send(zmq::buffer(std::string_view(kTopicStatus)),
             zmq::send_flags::sndmore | zmq::send_flags::dontwait);
  pub_->send(zmq::buffer(body), zmq::send_flags::dontwait);
}

void AcquisitionEngine::PublishFrame(const Frame& frame) {
  const std::string header = Json(frame.header).dump();
  pub_->send(zmq::buffer(std::string_view(kTopicFrame)),
             zmq::send_flags::sndmore | zmq::send_flags::dontwait);
  pub_->send(zmq::buffer(header),
             zmq::send_flags::sndmore | zmq::send_flags::dontwait);
  // Zero copy: the message keeps the payload alive until ZeroMQ sent it.
  auto* hold = new std::shared_ptr<const std::vector<std::uint8_t>>(frame.data);
  zmq::message_t payload(
      const_cast<std::uint8_t*>(frame.data->data()), frame.data->size(),
      [](void*, void* hint) {
        delete static_cast<std::shared_ptr<const std::vector<std::uint8_t>>*>(
            hint);
      },
      hold);
  pub_->send(payload, zmq::send_flags::dontwait);
}

void AcquisitionEngine::NotifyEvent() {
  if (!events_) return;
  events_->send(zmq::buffer(std::string_view("e")),
                zmq::send_flags::dontwait);
}

void AcquisitionEngine::SaveSettings() const {
  if (config_.settings_path.empty()) return;
  const std::string tmp = config_.settings_path + ".tmp";
  {
    std::ofstream out(tmp);
    if (!out) {
      spdlog::warn("cannot write {}", tmp);
      return;
    }
    out << Json(settings_).dump(2) << "\n";
  }
  std::rename(tmp.c_str(), config_.settings_path.c_str());
}

void AcquisitionEngine::WarmUp() {
  temperature_ = camera_->ReadTemperature();
  auto warm = [this] {
    // An unreadable temperature is never taken as "warm".
    return temperature_.status != TemperatureStatus::kUnknown &&
           temperature_.temperature_c >= config_.warmup_target_c;
  };
  auto aborted = [this] {
    return config_.abort_warmup && config_.abort_warmup->load();
  };
  if (!config_.warmup_on_exit || settings_.cooler_persist_on_shutdown ||
      warm()) {
    return;
  }
  spdlog::info("warming sensor from {:.1f} C to {:.1f} C before shutdown",
               temperature_.temperature_c, config_.warmup_target_c);
  CameraSettings off = settings_;
  off.cooler_on = false;
  camera_->ApplyThermalSettings(off);
  const double deadline = SteadyNow() + config_.warmup_timeout_s;
  while (SteadyNow() < deadline) {
    PollStatus();
    PublishStatus();
    if (warm()) return;
    if (aborted()) {
      spdlog::warn("warm-up skipped at {:.1f} C", temperature_.temperature_c);
      return;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
  }
  spdlog::warn("warm-up timed out at {:.1f} C", temperature_.temperature_c);
}

}  // namespace AndorEMCCDcontroller
