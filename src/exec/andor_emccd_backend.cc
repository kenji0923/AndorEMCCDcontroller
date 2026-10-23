/**
 * @file andor_emccd_backend.cc
 * @brief Camera backend service: owns the iXon and serves it over ZeroMQ.
 *
 * Usage:
 *   andor_emccd_backend [--config backend.json] [--sim]
 *                       [--control EP]... [--data EP]...
 */

#include <atomic>
#include <csignal>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

#include <argparse/argparse.hpp>
#include <spdlog/spdlog.h>
#include <zmq.hpp>

#include <AndorEMCCDcontroller/AcquisitionEngine.h>
#include <AndorEMCCDcontroller/Camera.h>
#include <AndorEMCCDcontroller/ControlServer.h>
#include <AndorEMCCDcontroller/Protocol.h>

namespace aec = AndorEMCCDcontroller;

namespace {

std::atomic<bool> g_stop{false};
std::atomic<bool> g_abort_warmup{false};

// First signal: stop (with warm-up); second signal: skip the warm-up.
void OnSignal(int) {
  if (g_stop) g_abort_warmup = true;
  g_stop = true;
}

/** Backend configuration file (all keys optional). */
struct BackendConfig {
  std::string camera = "andor";  // "andor" or "sim"
  std::string andor_config_dir = "/usr/local/etc/andor";
  std::string ipc_dir = aec::kDefaultIpcDir;
  int control_port = aec::kDefaultControlPort;
  int data_port = aec::kDefaultDataPort;
  std::string bind_address = "*";
  std::vector<std::string> control_endpoints;  // Overrides the defaults.
  std::vector<std::string> data_endpoints;
  aec::EngineConfig engine;
  aec::Json initial_settings = aec::Json::object();
  double control_lease_s = 10.0;  // Control is released after this silence.
};

bool LoadConfig(const std::string& path, BackendConfig* c, std::string* error) {
  std::ifstream in(path);
  if (!in) {
    *error = "cannot open " + path;
    return false;
  }
  try {
    const aec::Json j = aec::Json::parse(in, nullptr, true, true);
    c->camera = j.value("camera", c->camera);
    c->andor_config_dir = j.value("andor_config_dir", c->andor_config_dir);
    c->ipc_dir = j.value("ipc_dir", c->ipc_dir);
    c->control_port = j.value("control_port", c->control_port);
    c->data_port = j.value("data_port", c->data_port);
    c->bind_address = j.value("bind_address", c->bind_address);
    c->control_endpoints =
        j.value("control_endpoints", c->control_endpoints);
    c->data_endpoints = j.value("data_endpoints", c->data_endpoints);
    c->engine.history_frames =
        j.value("history_frames", c->engine.history_frames);
    c->engine.history_bytes =
        static_cast<std::size_t>(j.value("history_megabytes", 2048.0) * 1e6);
    c->engine.status_interval_s =
        j.value("status_interval_s", c->engine.status_interval_s);
    c->engine.publish_hwm = j.value("publish_hwm", c->engine.publish_hwm);
    c->control_lease_s = j.value("control_lease_s", c->control_lease_s);
    c->engine.settings_path = j.value("settings_file", c->engine.settings_path);
    c->engine.warmup_on_exit =
        j.value("warmup_on_exit", c->engine.warmup_on_exit);
    c->engine.warmup_target_c =
        j.value("warmup_target_c", c->engine.warmup_target_c);
    c->engine.warmup_timeout_s =
        j.value("warmup_timeout_s", c->engine.warmup_timeout_s);
    c->initial_settings = j.value("initial_settings", aec::Json::object());
  } catch (const std::exception& e) {
    *error = path + ": " + e.what();
    return false;
  }
  return true;
}

/** IPC endpoints are only added when the socket directory is usable. */
bool IpcDirUsable(const std::string& dir) {
  std::error_code ec;
  if (!std::filesystem::is_directory(dir, ec)) {
    std::filesystem::create_directories(dir, ec);
    if (ec) return false;
  }
  const auto probe = std::filesystem::path(dir) / ".probe";
  std::ofstream out(probe);
  if (!out) return false;
  out.close();
  std::filesystem::remove(probe, ec);
  return true;
}

}  // namespace

int main(int argc, char** argv) {
  argparse::ArgumentParser program("andor_emccd_backend", AEC_VERSION);
  program.add_description(
      "Andor iXon EMCCD backend service (control: ROUTER, data: PUB).");
  program.add_argument("--config").help("JSON configuration file");
  program.add_argument("--sim").help("use the simulated camera").flag();
  program.add_argument("--control")
      .help("control endpoint (repeatable), e.g. tcp://*:47100")
      .append();
  program.add_argument("--data")
      .help("data endpoint (repeatable), e.g. tcp://*:47101")
      .append();
  program.add_argument("--ipc-dir").help("directory for IPC sockets");
  program.add_argument("--settings-file").help("persist settings here");
  program.add_argument("--no-warmup")
      .help("do not warm the sensor before shutdown")
      .flag();
  program.add_argument("--log-level").default_value(std::string("info"));

  try {
    program.parse_args(argc, argv);
  } catch (const std::exception& e) {
    std::cerr << e.what() << "\n" << program;
    return 2;
  }
  spdlog::set_level(
      spdlog::level::from_str(program.get<std::string>("--log-level")));
  // journald adds its own timestamps.
  spdlog::set_pattern(std::getenv("INVOCATION_ID") ? "[%l] %v"
                                                   : "[%H:%M:%S.%e] [%l] %v");

  BackendConfig config;
  std::string error;
  if (auto path = program.present("--config")) {
    if (!LoadConfig(*path, &config, &error)) {
      spdlog::critical("{}", error);
      return 1;
    }
  }
  if (program.get<bool>("--sim")) config.camera = "sim";
  if (auto dir = program.present("--ipc-dir")) config.ipc_dir = *dir;
  if (auto path = program.present("--settings-file")) {
    config.engine.settings_path = *path;
  }
  if (program.get<bool>("--no-warmup")) config.engine.warmup_on_exit = false;
  const auto control_args = program.get<std::vector<std::string>>("--control");
  const auto data_args = program.get<std::vector<std::string>>("--data");
  if (!control_args.empty()) config.control_endpoints = control_args;
  if (!data_args.empty()) config.data_endpoints = data_args;

  if (config.control_endpoints.empty() || config.data_endpoints.empty()) {
    std::vector<std::string> control{
        aec::TcpEndpoint(config.bind_address, config.control_port)};
    std::vector<std::string> data{
        aec::TcpEndpoint(config.bind_address, config.data_port)};
    if (IpcDirUsable(config.ipc_dir)) {
      control.push_back(aec::IpcEndpoint(config.ipc_dir, aec::kControlIpcName));
      data.push_back(aec::IpcEndpoint(config.ipc_dir, aec::kDataIpcName));
    } else {
      spdlog::warn("IPC directory {} not writable; TCP only", config.ipc_dir);
    }
    if (config.control_endpoints.empty()) config.control_endpoints = control;
    if (config.data_endpoints.empty()) config.data_endpoints = data;
  }
  config.engine.data_endpoints = config.data_endpoints;
  config.engine.abort_warmup = &g_abort_warmup;

  // Settings: defaults < config file < persisted settings file.
  std::vector<std::string> errors;
  if (!aec::MergeSettings(config.initial_settings,
                          &config.engine.initial_settings, &errors)) {
    for (const auto& e : errors) spdlog::error("initial_settings: {}", e);
    return 1;
  }
  if (!config.engine.settings_path.empty() &&
      !aec::LoadSettingsFile(config.engine.settings_path,
                             &config.engine.initial_settings, &error)) {
    spdlog::warn("ignoring persisted settings: {}", error);
  }

  std::unique_ptr<aec::Camera> camera;
  const bool simulated = config.camera == "sim";
  if (simulated) {
    camera = aec::MakeSimulatedCamera();
    spdlog::info("using the simulated camera");
  } else {
    camera = aec::MakeAndorCamera(config.andor_config_dir);
    if (!camera) {
      spdlog::critical("built without the Andor SDK; use --sim");
      return 1;
    }
  }

  std::signal(SIGINT, OnSignal);
  std::signal(SIGTERM, OnSignal);

  zmq::context_t context(2);
  aec::AcquisitionEngine engine(std::move(camera), &context, config.engine);
  aec::ControlServer server(&engine, &context, config.control_endpoints,
                            simulated);
  server.SetControlLease(config.control_lease_s);
  // The server binds the inproc event socket the engine connects to.
  if (!server.Bind(&error)) {
    spdlog::critical("{}", error);
    return 1;
  }
  if (!engine.BindSockets(&error)) {
    spdlog::critical("{}", error);
    return 1;
  }
  // Without a camera the service keeps answering (state "disconnected" with
  // the SDK error) and retries, e.g. until the kernel driver is loaded.
  constexpr double kRetryInterval = 30.0;
  double next_retry = 0.0;
  double next_publish = 0.0;
  auto try_start = [&] {
    if (!engine.Running() && aec::UnixTimeNow() >= next_publish) {
      next_publish = aec::UnixTimeNow() + config.engine.status_interval_s;
      engine.PublishWhileStopped();
    }
    if (engine.Running() || aec::UnixTimeNow() < next_retry) return;
    next_retry = aec::UnixTimeNow() + kRetryInterval;
    std::string start_error;
    if (engine.Start(&start_error)) {
      spdlog::info("backend ready");
    } else {
      spdlog::error("camera start failed (retry in {:.0f} s): {}",
                    kRetryInterval, start_error);
    }
  };
  try {
    server.Serve(g_stop, try_start);
  } catch (const std::exception& e) {
    spdlog::critical("server: {}", e.what());
  }
  spdlog::info("shutting down (signal again to skip the sensor warm-up)");
  engine.Stop();
  return 0;
}
