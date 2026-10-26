/**
 * @file CameraSettings.cc
 * @brief Implementation of AndorEMCCDcontroller::CameraSettings.
 */

#include <AndorEMCCDcontroller/CameraSettings.h>

#include <algorithm>
#include <cmath>
#include <functional>
#include <map>
#include <utility>

namespace AndorEMCCDcontroller {

namespace {

template <typename E>
struct EnumTable {
  std::vector<std::pair<E, std::string>> entries;

  std::string Name(E v) const {
    for (const auto& [e, n] : entries) {
      if (e == v) return n;
    }
    return "unknown";
  }

  bool Parse(const std::string& name, E* v) const {
    for (const auto& [e, n] : entries) {
      if (n == name) {
        *v = e;
        return true;
      }
    }
    return false;
  }

  std::vector<std::string> Names() const {
    std::vector<std::string> names;
    for (const auto& entry : entries) names.push_back(entry.second);
    return names;
  }
};

const EnumTable<AcquisitionMode>& AcquisitionModes() {
  static const EnumTable<AcquisitionMode> table{
      {{AcquisitionMode::kSingle, "single"},
       {AcquisitionMode::kAccumulate, "accumulate"},
       {AcquisitionMode::kKinetic, "kinetic"},
       {AcquisitionMode::kContinuous, "continuous"}}};
  return table;
}

const EnumTable<TriggerMode>& TriggerModes() {
  static const EnumTable<TriggerMode> table{
      {{TriggerMode::kInternal, "internal"},
       {TriggerMode::kExternal, "external"},
       {TriggerMode::kExternalStart, "external_start"},
       {TriggerMode::kExternalExposure, "external_exposure"},
       {TriggerMode::kSoftware, "software"}}};
  return table;
}

const EnumTable<FanMode>& FanModes() {
  static const EnumTable<FanMode> table{{{FanMode::kFull, "full"},
                                         {FanMode::kLow, "low"},
                                         {FanMode::kOff, "off"}}};
  return table;
}

const EnumTable<ShutterMode>& ShutterModes() {
  static const EnumTable<ShutterMode> table{{{ShutterMode::kAuto, "auto"},
                                             {ShutterMode::kOpen, "open"},
                                             {ShutterMode::kClosed, "closed"}}};
  return table;
}

/** Reads one JSON value into a settings field; returns an error or "". */
using FieldSetter =
    std::function<std::string(const Json& value, CameraSettings* s)>;

template <typename T>
FieldSetter Number(T CameraSettings::*field, double lo, double hi) {
  return [field, lo, hi](const Json& v, CameraSettings* s) -> std::string {
    if (!v.is_number()) return "expected a number";
    const double d = v.get<double>();
    if (d < lo || d > hi) {
      return "out of range [" + std::to_string(lo) + ", " +
             std::to_string(hi) + "]";
    }
    if constexpr (std::is_integral_v<T>) {
      if (!v.is_number_integer() && d != static_cast<double>(
                                            static_cast<long long>(d))) {
        return "expected an integer";
      }
      s->*field = static_cast<T>(static_cast<long long>(d));
    } else {
      s->*field = static_cast<T>(d);
    }
    return "";
  };
}

FieldSetter Bool(bool CameraSettings::*field) {
  return [field](const Json& v, CameraSettings* s) -> std::string {
    if (v.is_boolean()) {
      s->*field = v.get<bool>();
    } else if (v.is_number_integer()) {
      s->*field = v.get<int>() != 0;
    } else {
      return "expected a boolean";
    }
    return "";
  };
}

template <typename E>
FieldSetter Enum(E CameraSettings::*field, const EnumTable<E>& table) {
  return [field, &table](const Json& v, CameraSettings* s) -> std::string {
    if (v.is_string()) {
      if (table.Parse(v.get<std::string>(), &(s->*field))) return "";
    } else if (v.is_number_integer()) {
      const E e = static_cast<E>(v.get<int>());
      if (table.Name(e) != "unknown") {
        s->*field = e;
        return "";
      }
    }
    std::string allowed;
    for (const auto& n : table.Names()) allowed += (allowed.empty() ? "" : ", ") + n;
    return "expected one of: " + allowed;
  };
}

const std::map<std::string, FieldSetter>& Setters() {
  using S = CameraSettings;
  static const std::map<std::string, FieldSetter> setters{
      {"acquisition_mode", Enum(&S::acquisition_mode, AcquisitionModes())},
      {"trigger_mode", Enum(&S::trigger_mode, TriggerModes())},
      {"fast_external_trigger", Bool(&S::fast_external_trigger)},
      {"trigger_invert", Bool(&S::trigger_invert)},
      {"exposure_time_s", Number(&S::exposure_time_s, 0.0, 3600.0)},
      {"number_accumulations", Number(&S::number_accumulations, 1, 1e6)},
      {"accumulation_cycle_time_s",
       Number(&S::accumulation_cycle_time_s, 0.0, 3600.0)},
      {"number_kinetics", Number(&S::number_kinetics, 1, 1e8)},
      {"kinetic_cycle_time_s", Number(&S::kinetic_cycle_time_s, 0.0, 3600.0)},
      {"frame_transfer", Bool(&S::frame_transfer)},
      {"output_amplifier", Number(&S::output_amplifier, 0, 1)},
      {"hs_speed_index", Number(&S::hs_speed_index, 0, 64)},
      {"vs_speed_index", Number(&S::vs_speed_index, -1, 64)},
      {"vs_amplitude", Number(&S::vs_amplitude, 0, 16)},
      {"preamp_gain_index", Number(&S::preamp_gain_index, 0, 16)},
      {"baseline_clamp", Bool(&S::baseline_clamp)},
      {"count_convert_mode", Number(&S::count_convert_mode, 0, 2)},
      {"count_convert_wavelength_nm",
       Number(&S::count_convert_wavelength_nm, 100.0, 2000.0)},
      {"em_gain_enabled", Bool(&S::em_gain_enabled)},
      {"em_gain", Number(&S::em_gain, 0, 4095)},
      {"em_gain_mode", Number(&S::em_gain_mode, 0, 3)},
      {"em_advanced", Bool(&S::em_advanced)},
      {"hbin", Number(&S::hbin, 1, 4096)},
      {"vbin", Number(&S::vbin, 1, 4096)},
      {"hstart", Number(&S::hstart, 1, 65535)},
      {"hend", Number(&S::hend, 0, 65535)},
      {"vstart", Number(&S::vstart, 1, 65535)},
      {"vend", Number(&S::vend, 0, 65535)},
      {"isolated_crop", Bool(&S::isolated_crop)},
      {"crop_width", Number(&S::crop_width, 0, 65535)},
      {"crop_height", Number(&S::crop_height, 0, 65535)},
      {"cooler_on", Bool(&S::cooler_on)},
      {"target_temperature_c", Number(&S::target_temperature_c, -150, 50)},
      {"fan_mode", Enum(&S::fan_mode, FanModes())},
      {"cooler_persist_on_shutdown", Bool(&S::cooler_persist_on_shutdown)},
      {"shutter_mode", Enum(&S::shutter_mode, ShutterModes())},
      {"shutter_ttl_high_open", Bool(&S::shutter_ttl_high_open)},
      {"shutter_opening_time_ms", Number(&S::shutter_opening_time_ms, 0, 10000)},
      {"shutter_closing_time_ms", Number(&S::shutter_closing_time_ms, 0, 10000)},
      {"metadata", Bool(&S::metadata)},
  };
  return setters;
}

}  // namespace

std::string ToString(AcquisitionMode v) { return AcquisitionModes().Name(v); }
std::string ToString(TriggerMode v) { return TriggerModes().Name(v); }
std::string ToString(FanMode v) { return FanModes().Name(v); }
std::string ToString(ShutterMode v) { return ShutterModes().Name(v); }

std::string ToString(TemperatureStatus v) {
  switch (v) {
    case TemperatureStatus::kOff:
      return "off";
    case TemperatureStatus::kNotReached:
      return "not_reached";
    case TemperatureStatus::kNotStabilized:
      return "not_stabilized";
    case TemperatureStatus::kStabilized:
      return "stabilized";
    case TemperatureStatus::kDrift:
      return "drift";
    case TemperatureStatus::kUnknown:
      return "unknown";
  }
  return "unknown";
}

std::string ToString(CameraState v) {
  switch (v) {
    case CameraState::kIdle:
      return "idle";
    case CameraState::kAcquiring:
      return "acquiring";
    case CameraState::kError:
      return "error";
    case CameraState::kDisconnected:
      return "disconnected";
  }
  return "unknown";
}

std::string ToString(ThermalAlarm v) {
  switch (v) {
    case ThermalAlarm::kOk:
      return "ok";
    case ThermalAlarm::kWarning:
      return "warning";
    case ThermalAlarm::kAlarm:
      return "alarm";
  }
  return "unknown";
}

const std::vector<std::string>& AcquisitionModeNames() {
  static const auto names = AcquisitionModes().Names();
  return names;
}
const std::vector<std::string>& TriggerModeNames() {
  static const auto names = TriggerModes().Names();
  return names;
}
const std::vector<std::string>& FanModeNames() {
  static const auto names = FanModes().Names();
  return names;
}
const std::vector<std::string>& ShutterModeNames() {
  static const auto names = ShutterModes().Names();
  return names;
}

void to_json(Json& j, const CameraSettings& s) {
  j = Json{
      {"acquisition_mode", ToString(s.acquisition_mode)},
      {"trigger_mode", ToString(s.trigger_mode)},
      {"fast_external_trigger", s.fast_external_trigger},
      {"trigger_invert", s.trigger_invert},
      {"exposure_time_s", s.exposure_time_s},
      {"number_accumulations", s.number_accumulations},
      {"accumulation_cycle_time_s", s.accumulation_cycle_time_s},
      {"number_kinetics", s.number_kinetics},
      {"kinetic_cycle_time_s", s.kinetic_cycle_time_s},
      {"frame_transfer", s.frame_transfer},
      {"output_amplifier", s.output_amplifier},
      {"hs_speed_index", s.hs_speed_index},
      {"vs_speed_index", s.vs_speed_index},
      {"vs_amplitude", s.vs_amplitude},
      {"preamp_gain_index", s.preamp_gain_index},
      {"baseline_clamp", s.baseline_clamp},
      {"count_convert_mode", s.count_convert_mode},
      {"count_convert_wavelength_nm", s.count_convert_wavelength_nm},
      {"em_gain_enabled", s.em_gain_enabled},
      {"em_gain", s.em_gain},
      {"em_gain_mode", s.em_gain_mode},
      {"em_advanced", s.em_advanced},
      {"hbin", s.hbin},
      {"vbin", s.vbin},
      {"hstart", s.hstart},
      {"hend", s.hend},
      {"vstart", s.vstart},
      {"vend", s.vend},
      {"isolated_crop", s.isolated_crop},
      {"crop_width", s.crop_width},
      {"crop_height", s.crop_height},
      {"cooler_on", s.cooler_on},
      {"target_temperature_c", s.target_temperature_c},
      {"fan_mode", ToString(s.fan_mode)},
      {"cooler_persist_on_shutdown", s.cooler_persist_on_shutdown},
      {"shutter_mode", ToString(s.shutter_mode)},
      {"shutter_ttl_high_open", s.shutter_ttl_high_open},
      {"shutter_opening_time_ms", s.shutter_opening_time_ms},
      {"shutter_closing_time_ms", s.shutter_closing_time_ms},
      {"metadata", s.metadata},
  };
}

bool MergeSettings(const Json& patch, CameraSettings* s,
                   std::vector<std::string>* errors) {
  if (!patch.is_object()) {
    errors->push_back("settings must be a JSON object");
    return false;
  }
  bool ok = true;
  const auto& setters = Setters();
  for (auto it = patch.begin(); it != patch.end(); ++it) {
    const auto setter = setters.find(it.key());
    if (setter == setters.end()) {
      errors->push_back(it.key() + ": unknown setting");
      ok = false;
      continue;
    }
    const std::string error = setter->second(it.value(), s);
    if (!error.empty()) {
      errors->push_back(it.key() + ": " + error);
      ok = false;
    }
  }
  return ok;
}

bool IsThermalKey(const std::string& key) {
  return key == "cooler_on" || key == "target_temperature_c" ||
         key == "fan_mode" || key == "cooler_persist_on_shutdown";
}

void to_json(Json& j, const CameraInfo& info) {
  Json amps = Json::array();
  for (const auto& a : info.amplifiers) {
    amps.push_back({{"name", a.name}, {"hs_speeds_mhz", a.hs_speeds_mhz}});
  }
  j = Json{{"simulated", info.simulated},
           {"model", info.model},
           {"head_model", info.head_model},
           {"serial", info.serial},
           {"controller_card", info.controller_card},
           {"sdk_version", info.sdk_version},
           {"driver_version", info.driver_version},
           {"sensor_width", info.sensor_width},
           {"sensor_height", info.sensor_height},
           {"pixel_size_um", {info.pixel_size_um_x, info.pixel_size_um_y}},
           {"bit_depth", info.bit_depth},
           {"amplifiers", amps},
           {"vs_speeds_us", info.vs_speeds_us},
           {"recommended_vs_speed_index", info.recommended_vs_speed_index},
           {"vs_amplitudes", info.vs_amplitudes},
           {"preamp_gains", info.preamp_gains},
           {"em_gain_range", {info.em_gain_min, info.em_gain_max}},
           {"temperature_range_c",
            {info.temperature_min_c, info.temperature_max_c}},
           {"max_binning", {info.max_hbin, info.max_vbin}},
           {"trigger_modes", info.trigger_modes},
           {"acquisition_modes", info.acquisition_modes},
           {"fan_modes", FanModeNames()},
           {"shutter_modes", ShutterModeNames()},
           {"has_isolated_crop", info.has_isolated_crop},
           {"has_count_convert", info.has_count_convert},
           {"has_metadata", info.has_metadata},
           {"has_frame_transfer", info.has_frame_transfer},
           {"has_ambient_temperature", info.has_ambient_temperature},
           {"has_cooler_voltage", info.has_cooler_voltage},
           {"has_tec_status", info.has_tec_status},
           {"thermal_probe", info.thermal_probe}};
}

void to_json(Json& j, const Timings& t) {
  j = Json{{"exposure_s", t.exposure_s},
           {"accumulate_s", t.accumulate_s},
           {"kinetic_s", t.kinetic_s},
           {"readout_s", t.readout_s}};
}

void to_json(Json& j, const ControlInfo& c) {
  if (c.holder_id.empty()) {
    j = nullptr;
    return;
  }
  j = Json{{"holder_id", c.holder_id}, {"holder", c.holder}, {"since", c.since}};
}

void to_json(Json& j, const CameraStatus& s) {
  // Readouts the camera does not provide are null.
  const auto or_null = [](double v) {
    return std::isnan(v) ? Json() : Json(v);
  };
  j = Json{{"state", ToString(s.state)},
           {"temperature_c", s.temperature_c},
           {"temperature_status", ToString(s.temperature_status)},
           {"cooler_on", s.cooler_on},
           {"target_temperature_c", s.target_temperature_c},
           {"fan_mode", ToString(s.fan_mode)},
           {"ambient_temperature_c", or_null(s.ambient_temperature_c)},
           {"cooler_voltage_v", or_null(s.cooler_voltage_v)},
           {"tec_overheat",
            s.tec_overheat < 0 ? Json() : Json(s.tec_overheat != 0)},
           {"thermal_alarm", ToString(s.thermal_alarm)},
           {"thermal_alarm_reason", s.thermal_alarm_reason},
           {"thermal_alarm_count", s.thermal_alarm_count},
           {"run_id", s.run_id},
           {"run_mode", s.run_mode},
           {"frames_in_run", s.frames_in_run},
           {"frames_expected", s.frames_expected},
           {"run_complete", s.run_complete},
           {"total_frames", s.total_frames},
           {"fps", s.fps},
           {"em_gain", s.em_gain},
           {"timings", s.timings},
           {"last_error", s.last_error},
           {"timestamp", s.timestamp},
           {"control", s.control}};
}

}  // namespace AndorEMCCDcontroller
