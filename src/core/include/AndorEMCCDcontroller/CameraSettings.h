/**
 * @file CameraSettings.h
 * @brief Camera settings, static camera information and live status.
 *
 * All three types round-trip through JSON with the same key names that the
 * control protocol uses, so clients can read and write them as plain
 * dictionaries.
 */

#ifndef AndorEMCCDcontroller__CameraSettings_h
#define AndorEMCCDcontroller__CameraSettings_h

#include <cstdint>
#include <string>
#include <vector>

#include <AndorEMCCDcontroller/Protocol.h>

namespace AndorEMCCDcontroller {

/** Values equal the SDK SetAcquisitionMode() codes. */
enum class AcquisitionMode {
  kSingle = 1,      ///< One frame per start.
  kAccumulate = 2,  ///< Sum of number_accumulations frames per start.
  kKinetic = 3,     ///< Series of number_kinetics frames.
  kContinuous = 5,  ///< Run till abort (live view).
};

/** Values equal the SDK SetTriggerMode() codes. */
enum class TriggerMode {
  kInternal = 0,
  kExternal = 1,          ///< Each trigger edge starts an exposure.
  kExternalStart = 6,     ///< One edge starts an internally timed series.
  kExternalExposure = 7,  ///< Exposure lasts while the trigger is high.
  kSoftware = 10,         ///< "software_trigger" command starts exposure.
};

/** Values equal the SDK SetFanMode() codes. */
enum class FanMode { kFull = 0, kLow = 1, kOff = 2 };

/** Values equal the SDK SetShutter() mode codes. */
enum class ShutterMode { kAuto = 0, kOpen = 1, kClosed = 2 };

enum class TemperatureStatus {
  kOff,
  kNotReached,
  kNotStabilized,
  kStabilized,
  kDrift,
  kUnknown,
};

enum class CameraState { kIdle, kAcquiring, kError, kDisconnected };

std::string ToString(AcquisitionMode v);
std::string ToString(TriggerMode v);
std::string ToString(FanMode v);
std::string ToString(ShutterMode v);
std::string ToString(TemperatureStatus v);
std::string ToString(CameraState v);

/** All names accepted for each enum, in display order. */
const std::vector<std::string>& AcquisitionModeNames();
const std::vector<std::string>& TriggerModeNames();
const std::vector<std::string>& FanModeNames();
const std::vector<std::string>& ShutterModeNames();

/**
 * @brief User-settable camera parameters.
 *
 * Image coordinates are 1-based and inclusive (SDK SetImage convention).
 * hend/vend = 0 means "to the sensor edge".
 */
struct CameraSettings {
  // Acquisition
  AcquisitionMode acquisition_mode = AcquisitionMode::kContinuous;
  TriggerMode trigger_mode = TriggerMode::kInternal;
  bool fast_external_trigger = false;
  bool trigger_invert = false;  ///< false: rising edge, true: falling edge.
  double exposure_time_s = 0.01;
  int number_accumulations = 1;
  double accumulation_cycle_time_s = 0.0;
  int number_kinetics = 1;
  double kinetic_cycle_time_s = 0.0;  ///< 0: as fast as possible.
  bool frame_transfer = true;

  // Readout
  int output_amplifier = 0;  ///< 0: EMCCD, 1: conventional.
  int hs_speed_index = 0;
  int vs_speed_index = -1;  ///< -1: fastest recommended.
  int vs_amplitude = 0;
  int preamp_gain_index = 0;
  bool baseline_clamp = true;
  int count_convert_mode = 0;  ///< 0: counts, 1: electrons, 2: photons.
  double count_convert_wavelength_nm = 780.0;

  // EM gain
  /// false: the register runs at the mode's minimum (DAC 0 = off, x1 in the
  /// linear/real modes); em_gain is kept as the set point for switching on.
  bool em_gain_enabled = false;
  int em_gain = 0;
  int em_gain_mode = 0;  ///< SetEMGainMode: 0 DAC 255, 1 DAC 4095, 2 linear, 3 real.
  bool em_advanced = false;

  // Image area
  int hbin = 1;
  int vbin = 1;
  int hstart = 1;
  int hend = 0;
  int vstart = 1;
  int vend = 0;
  bool isolated_crop = false;  ///< SetIsolatedCropMode, uses crop_* below.
  int crop_width = 0;
  int crop_height = 0;

  // Thermal
  bool cooler_on = false;
  int target_temperature_c = -70;
  FanMode fan_mode = FanMode::kFull;
  bool cooler_persist_on_shutdown = false;

  // External shutter TTL
  ShutterMode shutter_mode = ShutterMode::kOpen;
  bool shutter_ttl_high_open = true;
  int shutter_opening_time_ms = 0;
  int shutter_closing_time_ms = 0;

  bool metadata = true;
};

void to_json(Json& j, const CameraSettings& s);

/**
 * @brief Overwrite fields of @p s with the keys present in @p patch.
 *
 * Unknown keys and values of the wrong type or out of the generic range are
 * reported in @p errors and leave the field untouched.
 *
 * @return true if every key was accepted.
 */
bool MergeSettings(const Json& patch, CameraSettings* s,
                   std::vector<std::string>* errors);

/** Settings keys that only affect the cooler and can change at any time. */
bool IsThermalKey(const std::string& key);

/** One output amplifier with its horizontal shift speeds. */
struct AmplifierInfo {
  std::string name;
  std::vector<double> hs_speeds_mhz;
};

/** Static information about the connected camera. */
struct CameraInfo {
  bool simulated = false;
  std::string model;
  std::string head_model;
  std::string serial;
  std::string controller_card;
  std::string sdk_version;
  std::string driver_version;
  int sensor_width = 0;
  int sensor_height = 0;
  double pixel_size_um_x = 0.0;
  double pixel_size_um_y = 0.0;
  int bit_depth = 16;
  std::vector<AmplifierInfo> amplifiers;
  std::vector<double> vs_speeds_us;
  int recommended_vs_speed_index = 0;
  std::vector<std::string> vs_amplitudes;
  std::vector<double> preamp_gains;
  int em_gain_min = 0;  ///< For the current EM gain mode.
  int em_gain_max = 0;
  int temperature_min_c = -100;
  int temperature_max_c = 20;
  int max_hbin = 1;
  int max_vbin = 1;
  std::vector<std::string> trigger_modes;      ///< Available on this camera.
  std::vector<std::string> acquisition_modes;
  bool has_isolated_crop = false;
  bool has_count_convert = false;
  bool has_metadata = false;
  bool has_frame_transfer = false;
};

void to_json(Json& j, const CameraInfo& info);

/** Acquisition timings reported by the camera for the applied settings. */
struct Timings {
  double exposure_s = 0.0;
  double accumulate_s = 0.0;
  double kinetic_s = 0.0;
  double readout_s = 0.0;
};

void to_json(Json& j, const Timings& t);

/** Which client holds the right to change settings ("control"). */
struct ControlInfo {
  std::string holder_id;  ///< Random id of the holding client; "" if free.
  std::string holder;     ///< Display name "name@ip".
  double since = 0.0;     ///< Unix time of the grant.
};

void to_json(Json& j, const ControlInfo& c);

/** Live status published periodically. */
struct CameraStatus {
  CameraState state = CameraState::kDisconnected;
  double temperature_c = 0.0;
  TemperatureStatus temperature_status = TemperatureStatus::kUnknown;
  bool cooler_on = false;
  std::uint64_t run_id = 0;
  std::string run_mode;
  std::int64_t frames_in_run = 0;  ///< Frames read out in the current run.
  std::int64_t frames_expected = 0;  ///< 0: unbounded.
  bool run_complete = true;
  std::uint64_t total_frames = 0;
  double fps = 0.0;
  int em_gain = 0;  ///< EM gain in effect; 0: off or conventional amplifier.
  Timings timings;
  std::string last_error;
  double timestamp = 0.0;
  ControlInfo control;
};

void to_json(Json& j, const CameraStatus& s);

/** EM gain in effect for @p s; 0 when off or on the conventional amplifier. */
inline int EffectiveEmGain(const CameraSettings& s) {
  return s.output_amplifier == 0 && s.em_gain_enabled ? s.em_gain : 0;
}

}  // namespace AndorEMCCDcontroller

#endif
