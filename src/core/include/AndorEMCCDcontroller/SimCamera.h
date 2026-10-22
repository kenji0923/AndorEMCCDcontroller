/**
 * @file SimCamera.h
 * @brief Declaration of AndorEMCCDcontroller::SimCamera.
 */

#ifndef AndorEMCCDcontroller__SimCamera_h
#define AndorEMCCDcontroller__SimCamera_h

#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include <AndorEMCCDcontroller/Camera.h>

namespace AndorEMCCDcontroller {

/**
 * @brief Software model of an iXon3 DU-897 (512 x 512, 16 um, EMCCD).
 */
class SimCamera : public Camera {
 public:
  SimCamera();
  ~SimCamera() override;

  bool Open(std::string* error) override;
  void Close() override;
  CameraInfo Info() const override;
  std::vector<std::string> ApplyAcquisitionSettings(
      CameraSettings* s) override;
  std::vector<std::string> ApplyThermalSettings(
      const CameraSettings& s) override;
  FrameGeometry Geometry() const override;
  bool StartAcquisition(std::string* error) override;
  void AbortAcquisition() override;
  bool IsAcquiring() override;
  bool WaitForData(int timeout_ms) override;
  void Interrupt() override;
  bool RetrieveNewFrames(std::vector<Frame>* frames,
                         std::string* error) override;
  bool SendSoftwareTrigger(std::string* error) override;
  TemperatureReading ReadTemperature() override;
  Timings GetTimings() override;

 private:
  using Clock = std::chrono::steady_clock;
  struct State;

  void ComputeTimings();
  void ScheduleFirstFrame();
  double FramePeriod() const;
  Frame MakeFrame(double t);
  void UpdateTemperature();

  std::unique_ptr<State> state_;
  CameraInfo info_;
  CameraSettings settings_;
  FrameGeometry geometry_;
  Timings timings_;
  std::vector<float> noise_;
  double trigger_hz_ = 10.0;

  std::atomic<bool> acquiring_{false};
  Clock::time_point start_;
  double next_frame_at_ = 0.0;  ///< Seconds from start_; <0: on trigger.
  std::int64_t frames_generated_ = 0;
  std::int64_t frames_target_ = 0;  ///< <0: unbounded.
  int pending_software_triggers_ = 0;
  std::uint64_t frame_counter_ = 0;

  double temperature_c_ = 20.0;
  bool cooler_on_ = false;
  int target_c_ = -70;
  Clock::time_point last_temperature_update_;
  Clock::time_point stable_since_;
  bool stable_since_valid_ = false;
};

}  // namespace AndorEMCCDcontroller

#endif
