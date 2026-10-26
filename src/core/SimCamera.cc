/**
 * @file SimCamera.cc
 * @brief Simulated iXon3 DU-897 used without the PCI card.
 *
 * Reproduces the readout timing model, the trigger modes (external triggers
 * arrive at AEC_SIM_TRIGGER_HZ, default 10 Hz), the TE cooler and produces
 * images with a bias, read noise and two Gaussian spots.
 */

#include <AndorEMCCDcontroller/SimCamera.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <random>

namespace AndorEMCCDcontroller {

namespace {

using Clock = std::chrono::steady_clock;

constexpr int kSensorWidth = 512;
constexpr int kSensorHeight = 512;
constexpr double kAmbientC = 20.0;
constexpr double kHeadIdleC = 22.0;      ///< Head with the cooler off.
constexpr double kHeadTrippedC = 55.0;   ///< Heat sink hot enough to trip.
constexpr double kTripDurationS = 60.0;  ///< AEC_SIM_TRIP_AT_S trips this long.
constexpr double kStableAfterS = 10.0;   ///< Within 2 C this long: stabilized.
constexpr std::size_t kNoiseTableSize = 1u << 20;

double Seconds(Clock::duration d) {
  return std::chrono::duration<double>(d).count();
}

}  // namespace

struct SimCamera::State {
  std::mutex mutex;
  std::condition_variable cv;
  bool interrupted = false;
};

SimCamera::SimCamera() : state_(std::make_unique<State>()) {
  info_.simulated = true;
  info_.model = "iXon3 897 (simulated)";
  info_.head_model = "DU897_BV";
  info_.serial = "SIM0001";
  info_.controller_card = "CCI-23 (simulated)";
  info_.sdk_version = "simulated";
  info_.driver_version = "simulated";
  info_.sensor_width = kSensorWidth;
  info_.sensor_height = kSensorHeight;
  info_.pixel_size_um_x = 16.0;
  info_.pixel_size_um_y = 16.0;
  info_.bit_depth = 14;
  info_.amplifiers = {{"Electron Multiplying", {10.0, 5.0, 3.0, 1.0}},
                      {"Conventional", {3.0, 1.0, 0.08}}};
  info_.vs_speeds_us = {0.3, 0.5, 0.9, 1.7, 3.3};
  info_.recommended_vs_speed_index = 2;
  info_.vs_amplitudes = {"Normal", "+1", "+2", "+3", "+4"};
  info_.preamp_gains = {1.0, 2.4, 5.1};
  info_.em_gain_min = 0;
  info_.em_gain_max = 255;
  info_.temperature_min_c = -100;
  info_.temperature_max_c = 20;
  info_.max_hbin = kSensorWidth;
  info_.max_vbin = kSensorHeight;
  info_.trigger_modes = TriggerModeNames();
  info_.acquisition_modes = AcquisitionModeNames();
  info_.has_isolated_crop = true;
  info_.has_count_convert = true;
  info_.has_metadata = true;
  info_.has_frame_transfer = true;
  info_.has_ambient_temperature = true;
  info_.has_cooler_voltage = true;
  info_.thermal_probe = "simulated ambient temperature and cooler voltage";

  if (const char* hz = std::getenv("AEC_SIM_TRIGGER_HZ")) {
    trigger_hz_ = std::max(0.01, std::atof(hz));
  }
  if (const char* at = std::getenv("AEC_SIM_TRIP_AT_S")) {
    trip_at_s_ = std::max(0.0, std::atof(at));
  }

  std::mt19937 rng(12345);
  std::normal_distribution<float> normal(0.0f, 1.0f);
  noise_.resize(kNoiseTableSize);
  for (auto& v : noise_) v = normal(rng);

  temperature_c_ = kAmbientC;
  last_temperature_update_ = Clock::now();
  geometry_ = ComputeGeometry(settings_, kSensorWidth, kSensorHeight);
}

SimCamera::~SimCamera() = default;

bool SimCamera::Open(std::string* /*error*/) {
  opened_ = Clock::now();
  return true;
}

void SimCamera::Close() { acquiring_ = false; }

CameraInfo SimCamera::Info() const { return info_; }

std::vector<std::string> SimCamera::ApplyAcquisitionSettings(
    CameraSettings* s) {
  std::vector<std::string> problems;
  const int n_hs = static_cast<int>(
      info_.amplifiers.at(std::clamp(s->output_amplifier, 0, 1))
          .hs_speeds_mhz.size());
  if (s->hs_speed_index >= n_hs) {
    problems.push_back("hs_speed_index: only " + std::to_string(n_hs) +
                       " speeds for this amplifier");
    s->hs_speed_index = 0;
  }
  if (s->vs_speed_index >= static_cast<int>(info_.vs_speeds_us.size())) {
    problems.push_back("vs_speed_index: out of range");
    s->vs_speed_index = -1;
  }
  if (s->preamp_gain_index >= static_cast<int>(info_.preamp_gains.size())) {
    problems.push_back("preamp_gain_index: out of range");
    s->preamp_gain_index = 0;
  }
  if (s->vs_amplitude >= static_cast<int>(info_.vs_amplitudes.size())) {
    problems.push_back("vs_amplitude: out of range");
    s->vs_amplitude = 0;
  }
  info_.em_gain_max = s->em_gain_mode == 1 ? 4095
                      : s->em_gain_mode >= 2 ? (s->em_advanced ? 1000 : 300)
                                             : 255;
  info_.em_gain_min = s->em_gain_mode >= 2 ? 1 : 0;
  if (s->em_gain > info_.em_gain_max || s->em_gain < info_.em_gain_min) {
    problems.push_back("em_gain: clamped to [" +
                       std::to_string(info_.em_gain_min) + ", " +
                       std::to_string(info_.em_gain_max) + "]");
    s->em_gain = std::clamp(s->em_gain, info_.em_gain_min, info_.em_gain_max);
  }
  // Same restriction as the SDK: software triggers need run till abort.
  if (s->trigger_mode == TriggerMode::kSoftware &&
      s->acquisition_mode != AcquisitionMode::kContinuous) {
    problems.push_back(
        "trigger_mode software works only in continuous (run till abort) "
        "mode; send software_trigger during a continuous run");
  }
  if (s->count_convert_mode != 0 && !s->baseline_clamp) {
    problems.push_back("count_convert_mode: requires baseline_clamp");
    s->count_convert_mode = 0;
  }
  const bool external =
      s->trigger_mode == TriggerMode::kExternal ||
      s->trigger_mode == TriggerMode::kExternalExposure;
  if (s->acquisition_mode == AcquisitionMode::kAccumulate && external &&
      s->trigger_mode == TriggerMode::kExternalExposure) {
    problems.push_back("trigger_mode: external_exposure not available in "
                       "accumulate mode");
    s->trigger_mode = TriggerMode::kExternal;
  }
  if (s->acquisition_mode == AcquisitionMode::kKinetic &&
      s->number_accumulations > 1) {
    problems.push_back(
        "number_accumulations: ignored in kinetic mode (use "
        "acquisition_mode=accumulate)");
  }
  settings_ = *s;
  geometry_ = ComputeGeometry(settings_, kSensorWidth, kSensorHeight);
  ComputeTimings();
  return problems;
}

std::vector<std::string> SimCamera::ApplyThermalSettings(
    const CameraSettings& s) {
  UpdateTemperature();
  cooler_on_ = s.cooler_on;
  target_c_ = std::clamp(s.target_temperature_c, info_.temperature_min_c,
                         info_.temperature_max_c);
  stable_since_valid_ = false;
  if (target_c_ != s.target_temperature_c) {
    return {"target_temperature_c: clamped to " + std::to_string(target_c_)};
  }
  return {};
}

FrameGeometry SimCamera::Geometry() const { return geometry_; }

void SimCamera::ComputeTimings() {
  const auto& s = settings_;
  const auto& amp = info_.amplifiers.at(std::clamp(s.output_amplifier, 0, 1));
  const double hs_hz =
      amp.hs_speeds_mhz.at(std::min<std::size_t>(s.hs_speed_index,
                                                 amp.hs_speeds_mhz.size() - 1)) *
      1e6;
  const int vs_index = s.vs_speed_index < 0 ? info_.recommended_vs_speed_index
                                            : s.vs_speed_index;
  const double vs_s = info_.vs_speeds_us.at(vs_index) * 1e-6;
  const int rows = s.isolated_crop ? geometry_.height * geometry_.vbin
                                   : kSensorHeight;
  // Shift rows, then digitize the binned pixels plus a fixed overhead.
  const double shift = kSensorHeight * vs_s;
  const double readout = rows * vs_s +
                         static_cast<double>(geometry_.width) *
                             geometry_.height / hs_hz +
                         geometry_.height * 2e-6;
  const bool ft = s.frame_transfer && info_.has_frame_transfer;
  double cycle = ft ? std::max(s.exposure_time_s + shift, readout)
                    : s.exposure_time_s + readout;
  timings_.exposure_s = s.exposure_time_s;
  timings_.readout_s = readout;
  timings_.accumulate_s = std::max(cycle, s.accumulation_cycle_time_s);
  double frame_period =
      s.acquisition_mode == AcquisitionMode::kAccumulate
          ? timings_.accumulate_s * std::max(1, s.number_accumulations)
          : cycle;
  if (s.acquisition_mode == AcquisitionMode::kKinetic ||
      s.acquisition_mode == AcquisitionMode::kContinuous) {
    frame_period = std::max(frame_period, s.kinetic_cycle_time_s);
  }
  timings_.kinetic_s = frame_period;
}

bool SimCamera::StartAcquisition(std::string* /*error*/) {
  std::lock_guard<std::mutex> lock(state_->mutex);
  acquiring_ = true;
  start_ = Clock::now();
  frames_generated_ = 0;
  pending_software_triggers_ = 0;
  switch (settings_.acquisition_mode) {
    case AcquisitionMode::kSingle:
    case AcquisitionMode::kAccumulate:
      frames_target_ = 1;
      break;
    case AcquisitionMode::kKinetic:
      frames_target_ = settings_.number_kinetics;
      break;
    case AcquisitionMode::kContinuous:
      frames_target_ = -1;
      break;
  }
  ScheduleFirstFrame();
  return true;
}

void SimCamera::ScheduleFirstFrame() {
  const double trigger_period = 1.0 / trigger_hz_;
  double first = timings_.kinetic_s;
  switch (settings_.trigger_mode) {
    case TriggerMode::kInternal:
      first = settings_.exposure_time_s + timings_.readout_s;
      break;
    case TriggerMode::kExternal:
    case TriggerMode::kExternalStart:
      first = trigger_period + settings_.exposure_time_s + timings_.readout_s;
      break;
    case TriggerMode::kExternalExposure:
      first = trigger_period + 0.5 * trigger_period + timings_.readout_s;
      break;
    case TriggerMode::kSoftware:
      first = -1.0;
      break;
  }
  next_frame_at_ = first;
}

double SimCamera::FramePeriod() const {
  switch (settings_.trigger_mode) {
    case TriggerMode::kExternal:
    case TriggerMode::kExternalExposure:
      return std::max(1.0 / trigger_hz_, timings_.readout_s);
    default:
      return timings_.kinetic_s;
  }
}

void SimCamera::AbortAcquisition() {
  std::lock_guard<std::mutex> lock(state_->mutex);
  acquiring_ = false;
}

bool SimCamera::IsAcquiring() { return acquiring_; }

bool SimCamera::WaitForData(int timeout_ms) {
  std::unique_lock<std::mutex> lock(state_->mutex);
  auto deadline = Clock::now() + std::chrono::milliseconds(timeout_ms);
  if (acquiring_ && next_frame_at_ >= 0.0) {
    const auto due = start_ + std::chrono::duration_cast<Clock::duration>(
                                  std::chrono::duration<double>(next_frame_at_));
    deadline = std::min(deadline, due);
  }
  state_->cv.wait_until(lock, deadline,
                        [this] { return state_->interrupted; });
  state_->interrupted = false;
  return acquiring_;
}

void SimCamera::Interrupt() {
  std::lock_guard<std::mutex> lock(state_->mutex);
  state_->interrupted = true;
  state_->cv.notify_all();
}

bool SimCamera::RetrieveNewFrames(std::vector<Frame>* frames,
                                  std::string* /*error*/) {
  std::lock_guard<std::mutex> lock(state_->mutex);
  if (!acquiring_) return true;
  const double now = Seconds(Clock::now() - start_);
  constexpr int kMaxPerCall = 256;
  int produced = 0;
  while (produced < kMaxPerCall &&
         (frames_target_ < 0 || frames_generated_ < frames_target_)) {
    if (settings_.trigger_mode == TriggerMode::kSoftware) {
      if (pending_software_triggers_ == 0) break;
      --pending_software_triggers_;
      frames->push_back(MakeFrame(now));
    } else {
      if (next_frame_at_ < 0.0 || next_frame_at_ > now) break;
      frames->push_back(MakeFrame(next_frame_at_));
      next_frame_at_ += FramePeriod();
    }
    ++frames_generated_;
    ++produced;
  }
  if (frames_target_ >= 0 && frames_generated_ >= frames_target_) {
    acquiring_ = false;
  }
  return true;
}

Frame SimCamera::MakeFrame(double t) {
  const FrameGeometry& g = geometry_;
  const std::size_t n = static_cast<std::size_t>(g.width) * g.height;
  auto data = std::make_shared<std::vector<std::uint8_t>>(
      n * PixelTypeSize(g.pixel_type));

  const auto& s = settings_;
  const double accum = s.acquisition_mode == AcquisitionMode::kAccumulate
                           ? std::max(1, s.number_accumulations)
                           : 1;
  const double gain =
      (1.0 + EffectiveEmGain(s) * 0.2) /
      info_.preamp_gains.at(std::min<std::size_t>(s.preamp_gain_index, 2));
  const double bin = static_cast<double>(g.hbin) * g.vbin;
  const double exposure =
      s.trigger_mode == TriggerMode::kExternalExposure ? 0.5 / trigger_hz_
                                                       : s.exposure_time_s;
  const double bias = 100.0 * accum;
  const double read_noise = 4.0 * std::sqrt(accum) *
                            (1.0 + EffectiveEmGain(s) / 100.0);
  // Spot A: fixed, intensity fluctuates by 5 %. Spot B: circles the centre.
  const double fluct = 1.0 + 0.05 * noise_[(frame_counter_ * 7919) %
                                           kNoiseTableSize];
  const double amp_a = 3000.0 * exposure / 0.01 * gain * bin * accum * fluct;
  const double amp_b = 0.5 * amp_a;
  const double ax = 0.5 * kSensorWidth;
  const double ay = 0.5 * kSensorHeight;
  const double bx = ax + 120.0 * std::cos(0.5 * t);
  const double by = ay + 120.0 * std::sin(0.5 * t);
  const double sa2 = 2.0 * 25.0 * 25.0;
  const double sb2 = 2.0 * 10.0 * 10.0;
  const std::size_t noise_offset =
      (frame_counter_ * 104729u) % kNoiseTableSize;
  ++frame_counter_;

  for (int row = 0; row < g.height; ++row) {
    const double y = g.y0 - 1 + (row + 0.5) * g.vbin;
    const double dya = (y - ay) * (y - ay);
    const double dyb = (y - by) * (y - by);
    for (int col = 0; col < g.width; ++col) {
      const double x = g.x0 - 1 + (col + 0.5) * g.hbin;
      double v = bias;
      const double ea = (dya + (x - ax) * (x - ax)) / sa2;
      if (ea < 20.0) v += amp_a * std::exp(-ea);
      const double eb = (dyb + (x - bx) * (x - bx)) / sb2;
      if (eb < 20.0) v += amp_b * std::exp(-eb);
      const std::size_t i = static_cast<std::size_t>(row) * g.width + col;
      v += read_noise * noise_[(noise_offset + i) & (kNoiseTableSize - 1)];
      if (g.pixel_type == PixelType::kUint16) {
        const auto p = static_cast<std::uint16_t>(std::clamp(v, 0.0, 65535.0));
        std::memcpy(data->data() + 2 * i, &p, 2);
      } else {
        const auto p = static_cast<std::int32_t>(std::clamp(v, -2e9, 2e9));
        std::memcpy(data->data() + 4 * i, &p, 4);
      }
    }
  }

  Frame frame;
  frame.header.width = g.width;
  frame.header.height = g.height;
  frame.header.pixel_type = g.pixel_type;
  frame.header.hbin = g.hbin;
  frame.header.vbin = g.vbin;
  frame.header.x0 = g.x0;
  frame.header.y0 = g.y0;
  frame.header.camera_time_s = s.metadata ? t : -1.0;
  frame.data = std::move(data);
  return frame;
}

bool SimCamera::SendSoftwareTrigger(std::string* error) {
  std::lock_guard<std::mutex> lock(state_->mutex);
  if (settings_.trigger_mode != TriggerMode::kSoftware || !acquiring_ ||
      settings_.acquisition_mode != AcquisitionMode::kContinuous) {
    *error = "software trigger requires trigger_mode=software and a running "
             "continuous acquisition";
    return false;
  }
  ++pending_software_triggers_;
  state_->interrupted = true;
  state_->cv.notify_all();
  return true;
}

double SimCamera::CoolerVoltage() const {
  if (!cooler_on_ || tripped_) return 0.0;
  // Deeper set points need more drive; the controller saturates at 7.5 V.
  return std::clamp(1.5 + 0.06 * (kAmbientC - target_c_), 0.0, 7.5);
}

void SimCamera::UpdateTemperature() {
  const auto now = Clock::now();
  if (trip_at_s_ >= 0.0) {
    const double since_trip = Seconds(now - opened_) - trip_at_s_;
    tripped_ = since_trip >= 0.0 && since_trip < kTripDurationS;
  }
  const double dt = Seconds(now - last_temperature_update_);
  last_temperature_update_ = now;
  const double goal = cooler_on_ && !tripped_ ? target_c_ : kAmbientC;
  const double tau = 15.0;
  // Cooling power is finite: at most 2 K/s.
  double step = (goal - temperature_c_) * (1.0 - std::exp(-dt / tau));
  step = std::clamp(step, -2.0 * dt, 2.0 * dt);
  temperature_c_ += step;
  // The TE cooler heats the head in proportion to its drive.
  const double head_goal =
      tripped_ ? kHeadTrippedC : kHeadIdleC + 1.2 * CoolerVoltage();
  head_c_ += (head_goal - head_c_) * (1.0 - std::exp(-dt / tau));
}

TemperatureReading SimCamera::ReadTemperature() {
  UpdateTemperature();
  TemperatureReading r;
  r.temperature_c = std::round(temperature_c_ * 100.0) / 100.0;
  r.cooler_on = cooler_on_;
  r.ambient_temperature_c = std::round(head_c_ * 100.0) / 100.0;
  r.cooler_voltage_v = CoolerVoltage();
  if (!cooler_on_) {
    r.status = TemperatureStatus::kOff;
    return r;
  }
  const double error = std::abs(temperature_c_ - target_c_);
  const auto now = Clock::now();
  if (error > 2.0) {
    // Leaving the set point after stabilizing is a drift, as in the SDK.
    r.status = stable_since_valid_ &&
                       Seconds(now - stable_since_) > kStableAfterS
                   ? TemperatureStatus::kDrift
                   : TemperatureStatus::kNotReached;
    if (r.status != TemperatureStatus::kDrift) stable_since_valid_ = false;
  } else {
    if (!stable_since_valid_) {
      stable_since_valid_ = true;
      stable_since_ = now;
    }
    r.status = Seconds(now - stable_since_) > kStableAfterS
                   ? TemperatureStatus::kStabilized
                   : TemperatureStatus::kNotStabilized;
  }
  return r;
}

Timings SimCamera::GetTimings() { return timings_; }

std::unique_ptr<Camera> MakeSimulatedCamera() {
  return std::make_unique<SimCamera>();
}

}  // namespace AndorEMCCDcontroller
