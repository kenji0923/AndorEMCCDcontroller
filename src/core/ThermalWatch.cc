/**
 * @file ThermalWatch.cc
 * @brief Implementation of AndorEMCCDcontroller::ThermalWatch.
 */

#include <AndorEMCCDcontroller/ThermalWatch.h>

#include <cstdio>

namespace AndorEMCCDcontroller {

namespace {

// The SDK calls the temperature stabilized within 3 C of the set point
// (manual, "Temperature control"); overshooting below it counts as reached.
constexpr double kReachedBandC = 3.0;

}  // namespace

ThermalWatch::ThermalWatch(double cooling_lost_margin_c)
    : margin_c_(cooling_lost_margin_c) {}

bool ThermalWatch::Update(const TemperatureReading& r, int target_c) {
  if (!r.cooler_on || target_c != target_c_) reached_ = false;
  target_c_ = target_c;
  const bool valid = r.status != TemperatureStatus::kUnknown;
  if (r.cooler_on && valid && r.temperature_c <= target_c + kReachedBandC) {
    reached_ = true;
  }

  ThermalAlarm level = ThermalAlarm::kOk;
  std::string kind;
  std::string reason;
  if (r.tec_overheat == 1) {
    level = ThermalAlarm::kAlarm;
    kind = "tec_overheat";
    reason = "the camera reports a TEC over-temperature trip";
  } else if (r.cooler_on && valid && reached_ &&
             r.temperature_c > target_c + margin_c_) {
    char text[256];
    std::snprintf(text, sizeof(text),
                  "cooling lost: sensor %.1f C is %.1f C above the set point "
                  "%d C; the heat-sink thermal switch may have cut the TE "
                  "cooler (check the cooling water and the fan)",
                  r.temperature_c, r.temperature_c - target_c, target_c);
    level = ThermalAlarm::kAlarm;
    kind = "cooling_lost";
    reason = text;
  } else if (!valid) {
    level = ThermalAlarm::kWarning;
    kind = "unreadable";
    reason = "sensor temperature unreadable (" + r.error + ")";
  } else if (r.cooler_on && r.status == TemperatureStatus::kDrift) {
    level = ThermalAlarm::kWarning;
    kind = "drift";
    reason = "sensor temperature drifted from the set point";
  }

  if (level == ThermalAlarm::kAlarm && level_ != ThermalAlarm::kAlarm) {
    ++alarm_count_;
  }
  const bool changed = level != level_ || kind != kind_;
  level_ = level;
  kind_ = kind;
  reason_ = reason;
  return changed;
}

}  // namespace AndorEMCCDcontroller
