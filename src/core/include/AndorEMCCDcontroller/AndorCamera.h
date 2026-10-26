/**
 * @file AndorCamera.h
 * @brief Declaration of AndorEMCCDcontroller::AndorCamera.
 */

#ifndef AndorEMCCDcontroller__AndorCamera_h
#define AndorEMCCDcontroller__AndorCamera_h

#include <atomic>
#include <chrono>
#include <limits>
#include <string>
#include <vector>

#include <AndorEMCCDcontroller/Camera.h>

namespace AndorEMCCDcontroller {

/**
 * @brief iXon camera through the Andor SDK2 (libandor.so).
 *
 * Requires the andordrvlx kernel module for PCI systems.
 */
class AndorCamera : public Camera {
 public:
  explicit AndorCamera(std::string config_dir);
  ~AndorCamera() override;

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
  Json GetCalibration(double wavelength_nm) override;
  Json GetQeTable(double wavelength_nm) override;

 private:
  void QueryInfo();
  void ProbeThermalReadouts();
  void ReadThermalExtras(TemperatureReading* r);
  Frame EmptyFrame() const;
  bool RetrieveAccumulated(std::vector<Frame>* frames, std::string* error);

  std::string config_dir_;
  bool open_ = false;
  CameraInfo info_;
  CameraSettings settings_;
  Json sensitivity_ = nullptr;
  unsigned int sensitivity_status_ = 0;
  FrameGeometry geometry_;
  Timings timings_;
  std::atomic<bool> acquiring_{false};
  bool cooler_on_ = false;
  long next_image_ = 1;  ///< Next 1-based SDK image index to read.
  bool accumulate_read_ = false;  ///< Summed frame of this run delivered.
  // Capability bits from GetCapabilities().
  unsigned int set_functions_ = 0;
  unsigned int features_ = 0;
  unsigned int trigger_caps_ = 0;
  bool internal_shutter_ = false;
  TemperatureReading last_temperature_;
  // Last values of the readouts polled every few seconds.
  std::chrono::steady_clock::time_point next_extras_{};
  double ambient_c_ = std::numeric_limits<double>::quiet_NaN();
  double cooler_voltage_v_ = std::numeric_limits<double>::quiet_NaN();
  int tec_overheat_ = -1;
};

}  // namespace AndorEMCCDcontroller

#endif
