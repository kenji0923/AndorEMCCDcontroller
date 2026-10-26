/**
 * @file Camera.h
 * @brief Abstract camera driven by the acquisition engine.
 *
 * Implementations are not thread-safe: the engine calls every method from
 * its own thread, except Interrupt() which may be called from any thread.
 */

#ifndef AndorEMCCDcontroller__Camera_h
#define AndorEMCCDcontroller__Camera_h

#include <limits>
#include <memory>
#include <string>
#include <vector>

#include <AndorEMCCDcontroller/CameraSettings.h>
#include <AndorEMCCDcontroller/Protocol.h>

namespace AndorEMCCDcontroller {

/**
 * @brief Temperature reading with the SDK cooling status.
 *
 * temperature_c is the CCD sensor. It does not show the heat sink: when the
 * heat sink overheats, its thermal switch cuts the TE cooler and the head
 * beeps (iXon3 hardware guide 1.3.7) while the sensor still reads normal
 * until it warms up. The other fields are what the SDK offers besides it;
 * NaN or -1 when the camera does not report them.
 */
struct TemperatureReading {
  double temperature_c = 0.0;
  TemperatureStatus status = TemperatureStatus::kUnknown;
  bool cooler_on = false;
  std::string error;  ///< SDK error when status is kUnknown.
  /// "AmbientTemp" of GetTemperatureStatus: the head's cooler controller.
  double ambient_temperature_c = std::numeric_limits<double>::quiet_NaN();
  /// "CoolerVolts" of GetTemperatureStatus. On the DU897 it behaves like
  /// the TE cooler supply (about 7.3 V while cooling, a little lower under
  /// more load), not like a percentage of drive.
  double cooler_voltage_v = std::numeric_limits<double>::quiet_NaN();
  int tec_overheat = -1;  ///< GetTECStatus: -1 unknown, 0 normal, 1 tripped.
};

/** Geometry of the frames produced by the applied settings. */
struct FrameGeometry {
  int width = 0;
  int height = 0;
  int hbin = 1;
  int vbin = 1;
  int x0 = 1;
  int y0 = 1;
  PixelType pixel_type = PixelType::kUint16;
};

/**
 * @brief Image area actually read out for @p s on a sensor of the given size.
 *
 * Clamps the sub image to the sensor and rounds it down to whole bins.
 */
FrameGeometry ComputeGeometry(const CameraSettings& s, int sensor_width,
                              int sensor_height);

class Camera {
 public:
  virtual ~Camera() = default;

  /** Initializes the hardware. Returns false and sets @p error on failure. */
  virtual bool Open(std::string* error) = 0;

  /** Shuts the hardware down (the caller handles warming up). */
  virtual void Close() = 0;

  /** Static information; refreshed after ApplyAcquisitionSettings(). */
  virtual CameraInfo Info() const = 0;

  /**
   * @brief Applies everything except the cooler settings. Camera must be idle.
   *
   * @return Human readable problems; empty on full success. Settings that
   *         failed may be corrected in @p s to what the camera accepted.
   */
  virtual std::vector<std::string> ApplyAcquisitionSettings(
      CameraSettings* s) = 0;

  /** Applies cooler, target temperature and fan settings. */
  virtual std::vector<std::string> ApplyThermalSettings(
      const CameraSettings& s) = 0;

  /** Geometry of the frames for the last applied settings. */
  virtual FrameGeometry Geometry() const = 0;

  virtual bool StartAcquisition(std::string* error) = 0;
  virtual void AbortAcquisition() = 0;

  /** True while the camera is still running the current acquisition. */
  virtual bool IsAcquiring() = 0;

  /**
   * @brief Blocks until new data is available, Interrupt() is called or the
   *        timeout passes. Returns true when data may be available.
   */
  virtual bool WaitForData(int timeout_ms) = 0;

  /** Wakes up a WaitForData() call from another thread. */
  virtual void Interrupt() = 0;

  /**
   * @brief Reads every frame acquired since the previous call.
   *
   * Only the geometry, pixel type, camera time and payload of the returned
   * frames are filled; the engine fills the remaining header fields.
   */
  virtual bool RetrieveNewFrames(std::vector<Frame>* frames,
                                 std::string* error) = 0;

  virtual bool SendSoftwareTrigger(std::string* error) = 0;

  virtual TemperatureReading ReadTemperature() = 0;

  /** Timings for the applied settings (valid after Apply...). */
  virtual Timings GetTimings() = 0;
  // Read-only SDK calibration; unsupported values remain absent/null.
  virtual Json GetCalibration(double wavelength_nm) {
    return Json{{"wavelength_nm", wavelength_nm}, {"source", "unavailable"}};
  }
};

/** Simulated iXon3 897 for development without the PCI card. */
std::unique_ptr<Camera> MakeSimulatedCamera();

/**
 * @brief Real camera through the Andor SDK2 (nullptr if built without it).
 *
 * @param config_dir Directory with the Andor configuration files
 *        (usually /usr/local/etc/andor), passed to Initialize().
 */
std::unique_ptr<Camera> MakeAndorCamera(const std::string& config_dir);

}  // namespace AndorEMCCDcontroller

#endif
