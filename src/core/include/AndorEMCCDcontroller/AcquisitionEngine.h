/**
 * @file AcquisitionEngine.h
 * @brief Declaration of AndorEMCCDcontroller::AcquisitionEngine.
 */

#ifndef AndorEMCCDcontroller__AcquisitionEngine_h
#define AndorEMCCDcontroller__AcquisitionEngine_h

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <zmq.hpp>

#include <AndorEMCCDcontroller/Camera.h>
#include <AndorEMCCDcontroller/CameraSettings.h>
#include <AndorEMCCDcontroller/FrameStore.h>

namespace AndorEMCCDcontroller {

inline constexpr const char* kEngineEventsEndpoint = "inproc://engine-events";

struct EngineConfig {
  std::vector<std::string> data_endpoints;
  std::size_t history_frames = 2000;
  std::size_t history_bytes = std::size_t{2} << 30;
  double status_interval_s = 0.5;
  int publish_hwm = 16;            ///< Frames queued per slow subscriber.
  std::string settings_path;       ///< Persisted settings; "" disables.
  CameraSettings initial_settings;
  bool warmup_on_exit = true;      ///< Warm the sensor before ShutDown().
  double warmup_target_c = -20.0;
  double warmup_timeout_s = 900.0;
  /** Set (e.g. by a second SIGINT) to skip the remaining warm-up. */
  const std::atomic<bool>* abort_warmup = nullptr;
  /** A request that the engine does not finish in time fails. */
  double call_timeout_s = 60.0;
};

/**
 * @brief Owns the camera and its thread; publishes frames and status.
 *
 * Every SDK call happens on the engine thread. Public methods are
 * thread-safe: they post a task to the engine thread and wait for it.
 * Errors are reported by throwing std::runtime_error.
 */
class AcquisitionEngine {
 public:
  AcquisitionEngine(std::unique_ptr<Camera> camera, zmq::context_t* context,
                    EngineConfig config);
  ~AcquisitionEngine();

  AcquisitionEngine(const AcquisitionEngine&) = delete;
  AcquisitionEngine& operator=(const AcquisitionEngine&) = delete;

  /** Binds the data (PUB) socket; call once, after the server's Bind(). */
  bool BindSockets(std::string* error);

  /**
   * @brief Opens the camera, applies the initial settings, starts the thread.
   *
   * May be retried after a failure (e.g. the driver was not loaded yet).
   */
  bool Start(std::string* error);

  bool Running() const { return thread_.joinable(); }

  /** Aborts, optionally warms the sensor up, closes the camera. */
  void Stop();

  Json GetInfo();
  Json GetSettings();
  /** Merges @p patch; restarts a running continuous acquisition. */
  Json SetSettings(const Json& patch);
  /** Starts a run with the current settings (optional "settings" patch). */
  Json StartRun(const Json& args);
  /** Kinetic series of "n_frames" frames for labscript shots. */
  Json Arm(const Json& args);
  Json StopRun();
  Json SoftwareTrigger();

  CameraStatus Status() const;

  /** Updates the control holder shown in the status; publishes soon. */
  void SetControl(const ControlInfo& control);

  /**
   * @brief Publishes the status while the engine thread is not running.
   *
   * Call from the thread that called Start() (it then owns the PUB socket),
   * so clients see "disconnected" and the SDK error.
   */
  void PublishWhileStopped();
  FrameStore& Store() { return store_; }
  const FrameStore& Store() const { return store_; }

 private:
  struct Task {
    std::function<Json()> fn;
    std::promise<Json> result;
  };

  Json Call(std::function<Json()> fn);
  void ThreadMain();
  void LoopOnce(double* next_status);
  void RunPendingTasks();
  void DrainFrames();
  void FinishRun(bool aborted);
  void PollStatus();
  void PublishStatus();
  void PublishFrame(const Frame& frame);
  void NotifyEvent();

  Json DoSetSettings(const Json& patch);
  Json DoStartRun();
  void DoAbort(bool aborted);
  void ApplyAcquisitionIfDirty(std::vector<std::string>* warnings);
  void SaveSettings() const;
  void WarmUp();

  std::unique_ptr<Camera> camera_;
  zmq::context_t* context_;
  EngineConfig config_;
  FrameStore store_;

  std::thread thread_;
  std::atomic<bool> stop_{false};
  std::atomic<bool> publish_now_{false};
  std::mutex task_mutex_;
  std::condition_variable task_cv_;
  std::deque<std::unique_ptr<Task>> tasks_;

  // Engine-thread state.
  std::unique_ptr<zmq::socket_t> pub_;
  std::unique_ptr<zmq::socket_t> events_;
  CameraSettings settings_;
  CameraInfo info_;
  bool acquisition_dirty_ = true;
  bool thermal_pending_ = false;
  bool acquiring_ = false;
  std::uint64_t run_id_ = 0;
  std::int64_t run_frames_ = 0;
  std::uint64_t next_seq_ = 0;
  TemperatureReading temperature_;

  mutable std::mutex status_mutex_;
  CameraStatus status_;

  // Frame-rate estimate.
  double fps_window_start_ = 0.0;
  std::int64_t fps_window_frames_ = 0;
  double fps_ = 0.0;
};

/** Loads a settings JSON file into @p s (missing file is not an error). */
bool LoadSettingsFile(const std::string& path, CameraSettings* s,
                      std::string* error);

}  // namespace AndorEMCCDcontroller

#endif
