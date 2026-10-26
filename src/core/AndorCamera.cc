/**
 * @file AndorCamera.cc
 * @brief Andor SDK2 implementation of AndorEMCCDcontroller::Camera.
 *
 * Written against atmcdLXd.h of SDK 2.104 and the SDK manual for the
 * iXon3 DU-897 with the CCI-23 PCI controller. The SDK keeps global state,
 * so only one instance may exist per process.
 */

#include <AndorEMCCDcontroller/AndorCamera.h>

#include <algorithm>
#include <cstring>
#include <ctime>

#include <atmcdLXd.h>

namespace AndorEMCCDcontroller {

namespace {

std::string ErrorName(unsigned int code) {
  switch (code) {
    case DRV_SUCCESS: return "DRV_SUCCESS";
    case DRV_VXDNOTINSTALLED: return "DRV_VXDNOTINSTALLED (driver not loaded)";
    case DRV_ERROR_SCAN: return "DRV_ERROR_SCAN";
    case DRV_ERROR_CHECK_SUM: return "DRV_ERROR_CHECK_SUM";
    case DRV_ERROR_FILELOAD: return "DRV_ERROR_FILELOAD";
    case DRV_ERROR_ACK: return "DRV_ERROR_ACK";
    case DRV_ERROR_PAGELOCK: return "DRV_ERROR_PAGELOCK";
    case DRV_ERROR_PAGEUNLOCK: return "DRV_ERROR_PAGEUNLOCK";
    case DRV_ACQUISITION_ERRORS: return "DRV_ACQUISITION_ERRORS";
    case DRV_KINETIC_TIME_NOT_MET: return "DRV_KINETIC_TIME_NOT_MET";
    case DRV_ACCUM_TIME_NOT_MET: return "DRV_ACCUM_TIME_NOT_MET";
    case DRV_NO_NEW_DATA: return "DRV_NO_NEW_DATA";
    case DRV_SPOOLERROR: return "DRV_SPOOLERROR";
    case DRV_TEMP_OFF: return "DRV_TEMP_OFF";
    case DRV_TEMP_NOT_STABILIZED: return "DRV_TEMP_NOT_STABILIZED";
    case DRV_TEMP_STABILIZED: return "DRV_TEMP_STABILIZED";
    case DRV_TEMP_NOT_REACHED: return "DRV_TEMP_NOT_REACHED";
    case DRV_TEMP_OUT_RANGE: return "DRV_TEMP_OUT_RANGE";
    case DRV_TEMP_NOT_SUPPORTED: return "DRV_TEMP_NOT_SUPPORTED";
    case DRV_TEMP_DRIFT: return "DRV_TEMP_DRIFT";
    case DRV_GENERAL_ERRORS: return "DRV_GENERAL_ERRORS";
    case DRV_INVALID_AUX: return "DRV_INVALID_AUX";
    case DRV_COF_NOTLOADED: return "DRV_COF_NOTLOADED";
    case DRV_FPGAPROG: return "DRV_FPGAPROG";
    case DRV_FLEXERROR: return "DRV_FLEXERROR";
    case DRV_GPIBERROR: return "DRV_GPIBERROR";
    case DRV_DATATYPE: return "DRV_DATATYPE";
    case DRV_DRIVER_ERRORS: return "DRV_DRIVER_ERRORS";
    case DRV_P1INVALID: return "DRV_P1INVALID";
    case DRV_P2INVALID: return "DRV_P2INVALID";
    case DRV_P3INVALID: return "DRV_P3INVALID";
    case DRV_P4INVALID: return "DRV_P4INVALID";
    case DRV_INIERROR: return "DRV_INIERROR";
    case DRV_COFERROR: return "DRV_COFERROR";
    case DRV_ACQUIRING: return "DRV_ACQUIRING";
    case DRV_IDLE: return "DRV_IDLE";
    case DRV_TEMPCYCLE: return "DRV_TEMPCYCLE";
    case DRV_NOT_INITIALIZED: return "DRV_NOT_INITIALIZED";
    case DRV_P5INVALID: return "DRV_P5INVALID";
    case DRV_P6INVALID: return "DRV_P6INVALID";
    case DRV_INVALID_MODE: return "DRV_INVALID_MODE";
    case DRV_INVALID_FILTER: return "DRV_INVALID_FILTER";
    case DRV_I2CERRORS: return "DRV_I2CERRORS";
    case DRV_I2CDEVNOTFOUND: return "DRV_I2CDEVNOTFOUND";
    case DRV_I2CTIMEOUT: return "DRV_I2CTIMEOUT";
    case DRV_P7INVALID: return "DRV_P7INVALID";
    case DRV_USBERROR: return "DRV_USBERROR";
    case DRV_IOCERROR: return "DRV_IOCERROR";
    case DRV_VRMVERSIONERROR: return "DRV_VRMVERSIONERROR";
    case DRV_GATESTEPERROR: return "DRV_GATESTEPERROR";
    case DRV_USB_INTERRUPT_ENDPOINT_ERROR:
      return "DRV_USB_INTERRUPT_ENDPOINT_ERROR";
    case DRV_RANDOM_TRACK_ERROR: return "DRV_RANDOM_TRACK_ERROR";
    case DRV_INVALID_TRIGGER_MODE: return "DRV_INVALID_TRIGGER_MODE";
    case DRV_LOAD_FIRMWARE_ERROR: return "DRV_LOAD_FIRMWARE_ERROR";
    case DRV_DIVIDE_BY_ZERO_ERROR: return "DRV_DIVIDE_BY_ZERO_ERROR";
    case DRV_INVALID_RINGEXPOSURES: return "DRV_INVALID_RINGEXPOSURES";
    case DRV_BINNING_ERROR: return "DRV_BINNING_ERROR";
    case DRV_INVALID_AMPLIFIER: return "DRV_INVALID_AMPLIFIER";
    case DRV_INVALID_COUNTCONVERT_MODE: return "DRV_INVALID_COUNTCONVERT_MODE";
    case DRV_ERROR_NOCAMERA: return "DRV_ERROR_NOCAMERA";
    case DRV_NOT_SUPPORTED: return "DRV_NOT_SUPPORTED";
    case DRV_NOT_AVAILABLE: return "DRV_NOT_AVAILABLE";
    case DRV_ERROR_MAP: return "DRV_ERROR_MAP";
    case DRV_ERROR_UNMAP: return "DRV_ERROR_UNMAP";
    case DRV_ERROR_MDL: return "DRV_ERROR_MDL";
    case DRV_ERROR_UNMDL: return "DRV_ERROR_UNMDL";
    case DRV_ERROR_BUFFSIZE: return "DRV_ERROR_BUFFSIZE";
    case DRV_ERROR_NOHANDLE: return "DRV_ERROR_NOHANDLE";
    default: return "error " + std::to_string(code);
  }
}

/** Collects "<what>: <SDK error>" for every failing call. */
class Checker {
 public:
  explicit Checker(std::vector<std::string>* problems) : problems_(problems) {}

  bool operator()(unsigned int code, const char* what) {
    if (code == DRV_SUCCESS) return true;
    problems_->push_back(std::string(what) + ": " + ErrorName(code));
    return false;
  }

 private:
  std::vector<std::string>* problems_;
};

TemperatureStatus ToTemperatureStatus(unsigned int code) {
  switch (code) {
    case DRV_TEMP_OFF: return TemperatureStatus::kOff;
    case DRV_TEMP_NOT_REACHED: return TemperatureStatus::kNotReached;
    case DRV_TEMP_NOT_STABILIZED: return TemperatureStatus::kNotStabilized;
    case DRV_TEMP_STABILIZED: return TemperatureStatus::kStabilized;
    case DRV_TEMP_DRIFT: return TemperatureStatus::kDrift;
    default: return TemperatureStatus::kUnknown;
  }
}

}  // namespace

AndorCamera::AndorCamera(std::string config_dir)
    : config_dir_(std::move(config_dir)) {}

AndorCamera::~AndorCamera() { Close(); }

bool AndorCamera::Open(std::string* error) {
  at_32 n_cameras = 0;
  GetAvailableCameras(&n_cameras);
  std::vector<char> dir(config_dir_.begin(), config_dir_.end());
  dir.push_back('\0');
  const unsigned int rc = Initialize(dir.data());
  if (rc != DRV_SUCCESS) {
    *error = "Initialize(" + config_dir_ + ") failed: " + ErrorName(rc) +
             " (cameras detected: " + std::to_string(n_cameras) + ")";
    return false;
  }
  open_ = true;
  QueryInfo();
  return true;
}

void AndorCamera::Close() {
  if (!open_) return;
  AbortAcquisition();
  ShutDown();
  open_ = false;
}

void AndorCamera::QueryInfo() {
  CameraInfo& info = info_;
  info.simulated = false;

  char text[256] = {0};
  if (GetHeadModel(text) == DRV_SUCCESS) info.head_model = text;
  info.model = "iXon " + info.head_model;
  int serial = 0;
  if (GetCameraSerialNumber(&serial) == DRV_SUCCESS) {
    info.serial = std::to_string(serial);
  }
  std::memset(text, 0, sizeof(text));
  if (GetControllerCardModel(text) == DRV_SUCCESS) info.controller_card = text;
  std::memset(text, 0, sizeof(text));
  if (GetVersionInfo(AT_SDKVersion, text, sizeof(text)) == DRV_SUCCESS) {
    info.sdk_version = text;
  }
  std::memset(text, 0, sizeof(text));
  if (GetVersionInfo(AT_DeviceDriverVersion, text, sizeof(text)) ==
      DRV_SUCCESS) {
    info.driver_version = text;
  }

  GetDetector(&info.sensor_width, &info.sensor_height);
  float px = 0.0f;
  float py = 0.0f;
  if (GetPixelSize(&px, &py) == DRV_SUCCESS) {
    info.pixel_size_um_x = px;
    info.pixel_size_um_y = py;
  }
  int depth = 16;
  if (GetBitDepth(0, &depth) == DRV_SUCCESS) info.bit_depth = depth;

  int n_amp = 0;
  GetNumberAmp(&n_amp);
  info.amplifiers.clear();
  for (int amp = 0; amp < n_amp; ++amp) {
    AmplifierInfo a;
    char name[64] = {0};
    a.name = GetAmpDesc(amp, name, sizeof(name)) == DRV_SUCCESS
                 ? std::string(name)
                 : (amp == 0 ? "Electron Multiplying" : "Conventional");
    int n_hs = 0;
    GetNumberHSSpeeds(0, amp, &n_hs);
    for (int i = 0; i < n_hs; ++i) {
      float mhz = 0.0f;
      GetHSSpeed(0, amp, i, &mhz);
      a.hs_speeds_mhz.push_back(mhz);
    }
    info.amplifiers.push_back(a);
  }

  int n_vs = 0;
  GetNumberVSSpeeds(&n_vs);
  info.vs_speeds_us.clear();
  for (int i = 0; i < n_vs; ++i) {
    float us = 0.0f;
    GetVSSpeed(i, &us);
    info.vs_speeds_us.push_back(us);
  }
  float fastest_us = 0.0f;
  int fastest_index = 0;
  if (GetFastestRecommendedVSSpeed(&fastest_index, &fastest_us) ==
      DRV_SUCCESS) {
    info.recommended_vs_speed_index = fastest_index;
  }

  int n_vsa = 0;
  info.vs_amplitudes.clear();
  if (GetNumberVSAmplitudes(&n_vsa) == DRV_SUCCESS) {
    for (int i = 0; i < n_vsa; ++i) {
      char s[64] = {0};
      info.vs_amplitudes.push_back(
          GetVSAmplitudeString(i, s) == DRV_SUCCESS ? std::string(s)
                                                    : std::to_string(i));
    }
  }

  int n_pa = 0;
  GetNumberPreAmpGains(&n_pa);
  info.preamp_gains.clear();
  for (int i = 0; i < n_pa; ++i) {
    float g = 0.0f;
    GetPreAmpGain(i, &g);
    info.preamp_gains.push_back(g);
  }

  GetEMGainRange(&info.em_gain_min, &info.em_gain_max);
  GetTemperatureRange(&info.temperature_min_c, &info.temperature_max_c);
  GetMaximumBinning(4, 0, &info.max_hbin);
  GetMaximumBinning(4, 1, &info.max_vbin);

  AndorCapabilities caps;
  std::memset(&caps, 0, sizeof(caps));
  caps.ulSize = sizeof(caps);
  if (GetCapabilities(&caps) == DRV_SUCCESS) {
    set_functions_ = caps.ulSetFunctions;
    features_ = caps.ulFeatures;
    trigger_caps_ = caps.ulTriggerModes;
    info.has_frame_transfer = (caps.ulAcqModes & AC_ACQMODE_FRAMETRANSFER) != 0;
    info.has_count_convert = (caps.ulFeatures & AC_FEATURES_COUNTCONVERT) != 0;
    info.has_metadata = (caps.ulFeatures & AC_FEATURES_METADATA) != 0;
    info.has_isolated_crop =
        (caps.ulSetFunctions & AC_SETFUNCTION_CROPMODE) != 0;
  }
  int internal_shutter = 0;
  internal_shutter_ = IsInternalMechanicalShutter(&internal_shutter) ==
                          DRV_SUCCESS &&
                      internal_shutter != 0;

  // IsTriggerModeAvailable() depends on the current settings, so the static
  // list comes from the capability bits (software trigger = "continuous").
  info.trigger_modes.clear();
  const std::pair<TriggerMode, unsigned int> triggers[] = {
      {TriggerMode::kInternal, AC_TRIGGERMODE_INTERNAL},
      {TriggerMode::kExternal, AC_TRIGGERMODE_EXTERNAL},
      {TriggerMode::kExternalStart, AC_TRIGGERMODE_EXTERNALSTART},
      {TriggerMode::kExternalExposure, AC_TRIGGERMODE_EXTERNALEXPOSURE},
      {TriggerMode::kSoftware, AC_TRIGGERMODE_CONTINUOUS}};
  for (const auto& [mode, bit] : triggers) {
    if (trigger_caps_ & bit) info.trigger_modes.push_back(ToString(mode));
  }
  if (info.trigger_modes.empty()) info.trigger_modes = TriggerModeNames();
  info.acquisition_modes = AcquisitionModeNames();
}

CameraInfo AndorCamera::Info() const { return info_; }

std::vector<std::string> AndorCamera::ApplyAcquisitionSettings(
    CameraSettings* s) {
  std::vector<std::string> problems;
  Checker ok(&problems);
  CameraInfo& info = info_;

  // Combinations the SDK accepts but that cannot work (manual "Software
  // Trigger", "External Exposure"): fix them up and say so.
  if (s->trigger_mode == TriggerMode::kSoftware &&
      s->acquisition_mode != AcquisitionMode::kContinuous) {
    problems.push_back(
        "trigger_mode software works only in continuous (run till abort) "
        "mode; send software_trigger during a continuous run");
  }
  if (s->trigger_mode == TriggerMode::kExternalExposure && s->frame_transfer &&
      !(features_ & AC_FEATURES_FTEXTERNALEXPOSURE)) {
    problems.push_back(
        "frame_transfer: switched off, not available with external_exposure");
    s->frame_transfer = false;
  }
  if (s->trigger_mode == TriggerMode::kExternalExposure &&
      s->acquisition_mode == AcquisitionMode::kKinetic &&
      !(features_ & AC_FEATURES_KINETICEXTERNALEXPOSURE)) {
    problems.push_back(
        "external_exposure in a kinetic series is not supported by this camera");
  }
  if (s->trigger_invert && !(trigger_caps_ & AC_TRIGGERMODE_INVERTED)) {
    problems.push_back("trigger_invert: not supported, using the rising edge");
    s->trigger_invert = false;
  }

  ok(SetReadMode(4), "SetReadMode(image)");
  ok(SetAcquisitionMode(static_cast<int>(s->acquisition_mode)),
     "SetAcquisitionMode");
  // Frame transfer is not meaningful for single scans; the SDK ignores it.
  if (info.has_frame_transfer) {
    ok(SetFrameTransferMode(s->frame_transfer ? 1 : 0), "SetFrameTransferMode");
  }

  const int n_amp = static_cast<int>(info.amplifiers.size());
  if (s->output_amplifier >= n_amp) {
    problems.push_back("output_amplifier: camera has " +
                       std::to_string(n_amp) + " amplifier(s)");
    s->output_amplifier = 0;
  }
  ok(SetOutputAmplifier(s->output_amplifier), "SetOutputAmplifier");
  const int n_hs = n_amp > 0 ? static_cast<int>(
                                   info.amplifiers[s->output_amplifier]
                                       .hs_speeds_mhz.size())
                             : 0;
  if (s->hs_speed_index >= n_hs) {
    problems.push_back("hs_speed_index: out of range for amplifier");
    s->hs_speed_index = 0;
  }
  ok(SetHSSpeed(s->output_amplifier, s->hs_speed_index), "SetHSSpeed");

  int pa_available = 0;
  IsPreAmpGainAvailable(0, s->output_amplifier, s->hs_speed_index,
                        s->preamp_gain_index, &pa_available);
  if (!pa_available) {
    problems.push_back("preamp_gain_index: not available at this HS speed");
    s->preamp_gain_index = 0;
  }
  ok(SetPreAmpGain(s->preamp_gain_index), "SetPreAmpGain");

  const int vs = s->vs_speed_index < 0 ? info.recommended_vs_speed_index
                                       : s->vs_speed_index;
  ok(SetVSSpeed(vs), "SetVSSpeed");
  if (!info.vs_amplitudes.empty()) {
    ok(SetVSAmplitude(s->vs_amplitude), "SetVSAmplitude");
  }
  ok(SetBaselineClamp(s->baseline_clamp ? 1 : 0), "SetBaselineClamp");

  if (s->output_amplifier == 0) {
    ok(SetEMGainMode(s->em_gain_mode), "SetEMGainMode");
    if (set_functions_ & AC_SETFUNCTION_EMADVANCED) {
      ok(SetEMAdvanced(s->em_advanced ? 1 : 0), "SetEMAdvanced");
    } else if (s->em_advanced) {
      problems.push_back("em_advanced: not available on this camera");
      s->em_advanced = false;
    }
    GetEMGainRange(&info.em_gain_min, &info.em_gain_max);
    const int gain = std::clamp(s->em_gain, info.em_gain_min, info.em_gain_max);
    if (gain != s->em_gain) {
      problems.push_back("em_gain: clamped to " + std::to_string(gain));
      s->em_gain = gain;
    }
    ok(SetEMCCDGain(s->em_gain_enabled ? s->em_gain : info.em_gain_min),
       "SetEMCCDGain");
  }

  geometry_ = ComputeGeometry(*s, info.sensor_width, info.sensor_height);
  const FrameGeometry& g = geometry_;
  if (info.has_isolated_crop || s->isolated_crop) {
    ok(SetIsolatedCropMode(s->isolated_crop ? 1 : 0, g.height * g.vbin,
                           g.width * g.hbin, g.vbin, g.hbin),
       "SetIsolatedCropMode");
  }
  ok(SetImage(g.hbin, g.vbin, g.x0, g.x0 + g.width * g.hbin - 1, g.y0,
              g.y0 + g.height * g.vbin - 1),
     "SetImage");

  ok(SetTriggerMode(static_cast<int>(s->trigger_mode)), "SetTriggerMode");
  const bool external = s->trigger_mode == TriggerMode::kExternal ||
                        s->trigger_mode == TriggerMode::kExternalStart ||
                        s->trigger_mode == TriggerMode::kExternalExposure;
  if (external) {
    ok(SetFastExtTrigger(s->fast_external_trigger ? 1 : 0),
       "SetFastExtTrigger");
    const unsigned int rc = SetTriggerInvert(s->trigger_invert ? 1 : 0);
    if (rc != DRV_SUCCESS && s->trigger_invert) {
      ok(rc, "SetTriggerInvert");
    }
  }

  ok(SetExposureTime(static_cast<float>(s->exposure_time_s)),
     "SetExposureTime");
  switch (s->acquisition_mode) {
    case AcquisitionMode::kAccumulate:
      ok(SetNumberAccumulations(s->number_accumulations),
         "SetNumberAccumulations");
      ok(SetAccumulationCycleTime(
             static_cast<float>(s->accumulation_cycle_time_s)),
         "SetAccumulationCycleTime");
      break;
    case AcquisitionMode::kKinetic:
      // iXon cannot combine kinetic series and accumulation (release notes).
      if (s->number_accumulations > 1) {
        problems.push_back(
            "number_accumulations: ignored in kinetic mode (use "
            "acquisition_mode=accumulate)");
      }
      ok(SetNumberAccumulations(1), "SetNumberAccumulations");
      ok(SetNumberKinetics(s->number_kinetics), "SetNumberKinetics");
      ok(SetKineticCycleTime(static_cast<float>(s->kinetic_cycle_time_s)),
         "SetKineticCycleTime");
      break;
    case AcquisitionMode::kContinuous:
      ok(SetKineticCycleTime(static_cast<float>(s->kinetic_cycle_time_s)),
         "SetKineticCycleTime");
      break;
    case AcquisitionMode::kSingle:
      break;
  }

  // Cameras with SHUTTEREX must use SetShutterEx (manual SetShutter note 2);
  // the internal shutter, if fitted, follows the same mode.
  const int ttl = s->shutter_ttl_high_open ? 1 : 0;
  const int mode = static_cast<int>(s->shutter_mode);
  if (features_ & AC_FEATURES_SHUTTEREX) {
    ok(SetShutterEx(ttl, internal_shutter_ ? mode : 1,
                    s->shutter_closing_time_ms, s->shutter_opening_time_ms,
                    mode),
       "SetShutterEx");
  } else if (features_ & AC_FEATURES_SHUTTER) {
    ok(SetShutter(ttl, mode, s->shutter_closing_time_ms,
                  s->shutter_opening_time_ms),
       "SetShutter");
  }

  if (info.has_count_convert) {
    if (s->count_convert_mode != 0 &&
        IsCountConvertModeAvailable(s->count_convert_mode) != DRV_SUCCESS) {
      problems.push_back(
          "count_convert_mode: not available with these settings (needs "
          "baseline clamp and a supported amplifier/readout)");
      s->count_convert_mode = 0;
    }
    if (s->count_convert_mode != 0) {
      ok(SetCountConvertWavelength(
             static_cast<float>(s->count_convert_wavelength_nm)),
         "SetCountConvertWavelength");
    }
    if (!ok(SetCountConvertMode(s->count_convert_mode),
            "SetCountConvertMode")) {
      s->count_convert_mode = 0;
    }
  } else if (s->count_convert_mode != 0) {
    problems.push_back("count_convert_mode: not supported by this camera");
    s->count_convert_mode = 0;
  }

  if (info.has_metadata) {
    ok(SetMetaData(s->metadata ? 1 : 0), "SetMetaData");
  }
  geometry_ = ComputeGeometry(*s, info.sensor_width, info.sensor_height);
  settings_ = *s;
  // GetSensitivity may reject calls during acquisition; cache it while idle.
  float sensitivity = 0.0f;
  sensitivity_status_ = GetSensitivity(0, s->hs_speed_index,
      s->output_amplifier, s->preamp_gain_index, &sensitivity);
  sensitivity_ = sensitivity_status_ == DRV_SUCCESS && sensitivity > 0
                     ? Json(sensitivity) : Json(nullptr);

  const unsigned int trigger_rc =
      IsTriggerModeAvailable(static_cast<int>(s->trigger_mode));
  if (trigger_rc != DRV_SUCCESS) {
    problems.push_back("trigger_mode " + ToString(s->trigger_mode) +
                       " is not available with these settings (" +
                       ErrorName(trigger_rc) + ")");
  }

  float exposure = 0.0f;
  float accumulate = 0.0f;
  float kinetic = 0.0f;
  if (GetAcquisitionTimings(&exposure, &accumulate, &kinetic) == DRV_SUCCESS) {
    timings_.exposure_s = exposure;
    timings_.accumulate_s = accumulate;
    timings_.kinetic_s = kinetic;
  }
  float readout = 0.0f;
  if (GetReadOutTime(&readout) == DRV_SUCCESS) timings_.readout_s = readout;
  return problems;
}

std::vector<std::string> AndorCamera::ApplyThermalSettings(
    const CameraSettings& s) {
  std::vector<std::string> problems;
  Checker ok(&problems);
  const int target = std::clamp(s.target_temperature_c, info_.temperature_min_c,
                                info_.temperature_max_c);
  if (target != s.target_temperature_c) {
    problems.push_back("target_temperature_c: clamped to " +
                       std::to_string(target));
  }
  ok(SetTemperature(target), "SetTemperature");
  ok(SetFanMode(static_cast<int>(s.fan_mode)), "SetFanMode");
  ok(SetCoolerMode(s.cooler_persist_on_shutdown ? 1 : 0), "SetCoolerMode");
  if (s.cooler_on) {
    ok(CoolerON(), "CoolerON");
  } else {
    ok(CoolerOFF(), "CoolerOFF");
  }
  cooler_on_ = s.cooler_on;
  return problems;
}

FrameGeometry AndorCamera::Geometry() const { return geometry_; }

bool AndorCamera::StartAcquisition(std::string* error) {
  next_image_ = 1;
  accumulate_read_ = false;
  unsigned int rc = PrepareAcquisition();
  if (rc == DRV_SUCCESS) rc = ::StartAcquisition();
  if (rc != DRV_SUCCESS) {
    *error = "StartAcquisition: " + ErrorName(rc);
    return false;
  }
  acquiring_ = true;
  return true;
}

void AndorCamera::AbortAcquisition() {
  // Returns DRV_IDLE harmlessly when nothing runs; GetStatus may also report
  // DRV_KINETIC_TIME_NOT_MET etc. while running, so do not gate on it.
  ::AbortAcquisition();
  acquiring_ = false;
}

bool AndorCamera::IsAcquiring() {
  int status = DRV_IDLE;
  if (GetStatus(&status) != DRV_SUCCESS) return true;  // Retry next time.
  // Everything except idle/temperature cycle means the run is still going
  // (DRV_ACCUM_TIME_NOT_MET, DRV_KINETIC_TIME_NOT_MET, DRV_ACQ_BUFFER, ...).
  return status != DRV_IDLE && status != DRV_TEMPCYCLE;
}

bool AndorCamera::WaitForData(int timeout_ms) {
  return WaitForAcquisitionTimeOut(timeout_ms) == DRV_SUCCESS;
}

void AndorCamera::Interrupt() { CancelWait(); }

Frame AndorCamera::EmptyFrame() const {
  const FrameGeometry& g = geometry_;
  Frame frame;
  frame.header.width = g.width;
  frame.header.height = g.height;
  frame.header.pixel_type = g.pixel_type;
  frame.header.hbin = g.hbin;
  frame.header.vbin = g.vbin;
  frame.header.x0 = g.x0;
  frame.header.y0 = g.y0;
  frame.header.camera_time_s = -1.0;
  return frame;
}

bool AndorCamera::RetrieveAccumulated(std::vector<Frame>* frames,
                                      std::string* error) {
  // The circular buffer holds the individual scans; the sum is read with
  // GetAcquiredData once the acquisition has finished (manual
  // "Accumulate", GetNumberNewImages).
  if (accumulate_read_ || IsAcquiring()) return true;
  const FrameGeometry& g = geometry_;
  const std::size_t n_pixels = static_cast<std::size_t>(g.width) * g.height;
  auto data = std::make_shared<std::vector<std::uint8_t>>(n_pixels * 4);
  const unsigned int rc = GetAcquiredData(
      reinterpret_cast<at_32*>(data->data()), static_cast<at_u32>(n_pixels));
  accumulate_read_ = true;
  if (rc != DRV_SUCCESS) {
    *error = "GetAcquiredData: " + ErrorName(rc);
    return false;
  }
  Frame frame = EmptyFrame();
  frame.header.pixel_type = PixelType::kInt32;
  frame.data = std::move(data);
  frames->push_back(std::move(frame));
  return true;
}

bool AndorCamera::RetrieveNewFrames(std::vector<Frame>* frames,
                                    std::string* error) {
  if (settings_.acquisition_mode == AcquisitionMode::kAccumulate) {
    return RetrieveAccumulated(frames, error);
  }
  at_32 first = 0;
  at_32 last = 0;
  const unsigned int rc = GetNumberNewImages(&first, &last);
  if (rc == DRV_NO_NEW_DATA) return true;
  if (rc != DRV_SUCCESS) {
    *error = "GetNumberNewImages: " + ErrorName(rc);
    return false;
  }
  if (last < first) return true;
  if (first > next_image_) {
    *error = "circular buffer overrun: lost images " +
             std::to_string(next_image_) + ".." + std::to_string(first - 1);
  }
  const FrameGeometry& g = geometry_;
  const std::size_t n_pixels = static_cast<std::size_t>(g.width) * g.height;
  const std::size_t count = static_cast<std::size_t>(last - first + 1);
  at_32 valid_first = 0;
  at_32 valid_last = 0;
  std::vector<std::uint8_t> bulk(count * n_pixels *
                                 PixelTypeSize(g.pixel_type));
  unsigned int read_rc;
  if (g.pixel_type == PixelType::kUint16) {
    read_rc = GetImages16(first, last, reinterpret_cast<at_u16*>(bulk.data()),
                          static_cast<at_u32>(count * n_pixels), &valid_first,
                          &valid_last);
  } else {
    read_rc = GetImages(first, last, reinterpret_cast<at_32*>(bulk.data()),
                        static_cast<at_u32>(count * n_pixels), &valid_first,
                        &valid_last);
  }
  if (read_rc != DRV_SUCCESS) {
    *error = "GetImages: " + ErrorName(read_rc);
    return false;
  }
  if (valid_first != first || valid_last != last) {
    // Where a partial result is placed in the buffer is undocumented: read
    // the valid range again with an exactly sized request next time.
    *error = "GetImages returned " + std::to_string(valid_first) + ".." +
             std::to_string(valid_last) + " of " + std::to_string(first) +
             ".." + std::to_string(last) + "; retrying";
    return true;
  }

  const std::size_t frame_bytes = n_pixels * PixelTypeSize(g.pixel_type);
  for (at_32 image = valid_first; image <= valid_last; ++image) {
    const std::size_t offset =
        static_cast<std::size_t>(image - first) * frame_bytes;
    Frame frame = EmptyFrame();
    if (settings_.metadata && info_.has_metadata) {
      SYSTEMTIME start;
      float ms_from_start = 0.0f;
      // The index base of GetMetaDataInfo is undocumented; 0-based is tried
      // first (TODO verify on hardware), the GetImages index as fallback.
      if (GetMetaDataInfo(&start, &ms_from_start,
                          static_cast<unsigned int>(image - 1)) ==
              DRV_SUCCESS ||
          GetMetaDataInfo(&start, &ms_from_start,
                          static_cast<unsigned int>(image)) == DRV_SUCCESS) {
        frame.header.camera_time_s = ms_from_start * 1e-3;
      }
    }
    frame.data = std::make_shared<std::vector<std::uint8_t>>(
        bulk.begin() + static_cast<std::ptrdiff_t>(offset),
        bulk.begin() + static_cast<std::ptrdiff_t>(offset + frame_bytes));
    frames->push_back(std::move(frame));
  }
  next_image_ = valid_last + 1;
  return true;
}

bool AndorCamera::SendSoftwareTrigger(std::string* error) {
  const unsigned int rc = ::SendSoftwareTrigger();
  if (rc != DRV_SUCCESS) {
    *error = "SendSoftwareTrigger: " + ErrorName(rc);
    return false;
  }
  return true;
}

TemperatureReading AndorCamera::ReadTemperature() {
  // Some systems cannot read the sensor while acquiring.
  if (acquiring_ && !(features_ & AC_FEATURES_TEMPERATUREDURINGACQUISITION) &&
      IsAcquiring()) {
    return last_temperature_;
  }
  float t = 0.0f;
  const unsigned int rc = GetTemperatureF(&t);
  const TemperatureStatus status = ToTemperatureStatus(rc);
  if (status == TemperatureStatus::kUnknown) {
    // DRV_ACQUIRING / DRV_ERROR_ACK / ...: t is not valid, keep the last
    // reading but mark it unknown.
    TemperatureReading r = last_temperature_;
    r.status = TemperatureStatus::kUnknown;
    return r;
  }
  TemperatureReading r;
  r.temperature_c = t;
  r.status = status;
  int on = 0;
  r.cooler_on = IsCoolerOn(&on) == DRV_SUCCESS ? on != 0 : cooler_on_;
  last_temperature_ = r;
  return r;
}

Timings AndorCamera::GetTimings() { return timings_; }

std::unique_ptr<Camera> MakeAndorCamera(const std::string& config_dir) {
  return std::make_unique<AndorCamera>(config_dir);
}

Json AndorCamera::GetCalibration(double wavelength_nm) {
  Json result{{"source", "Andor SDK2"}, {"settings", settings_},
              {"wavelength_nm", wavelength_nm},
              {"sensitivity_e_per_adu", sensitivity_},
              {"sensitivity_status", sensitivity_status_},
              {"sensitivity_source", "GetSensitivity (AD channel 0)"},
              {"em_gain", nullptr}, {"qe", nullptr}, {"baseline_adu", nullptr}};
  std::string sensor = info_.head_model;
  float qe = 0.0f;
  const auto rc = GetQE(sensor.data(), static_cast<float>(wavelength_nm), 0, &qe);
  result["qe_status"] = rc;
  result["qe_source"] = "GetQE, sensor model, mode 0";
  if (rc == DRV_SUCCESS && qe > 0 && qe <= 100) result["qe"] = qe / 100.0;
  if (settings_.output_amplifier != 0) {
    result["em_gain"] = 1.0;
    result["em_gain_source"] = "conventional output, no EM multiplication";
  } else if (!settings_.em_gain_enabled) {
    result["em_gain"] = 1.0;
    result["em_gain_source"] = "nominal unity at minimum setting; not measured";
  } else if (settings_.em_gain_mode == 3) {
    result["em_gain"] = settings_.em_gain;
    result["em_gain_source"] = "RealGain set point; not independently measured";
  } else {
    result["em_gain_source"] = "physical gain unavailable in DAC/linear modes";
  }
  if (settings_.baseline_clamp) {
    result["baseline_adu"] = 100.0;
    result["baseline_source"] = "nominal iXon clamp offset; use ROI for measured bias";
  }
  return result;
}

}  // namespace AndorEMCCDcontroller
