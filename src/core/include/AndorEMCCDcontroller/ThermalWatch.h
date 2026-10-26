/**
 * @file ThermalWatch.h
 * @brief Declaration of AndorEMCCDcontroller::ThermalWatch.
 */

#ifndef AndorEMCCDcontroller__ThermalWatch_h
#define AndorEMCCDcontroller__ThermalWatch_h

#include <cstdint>
#include <string>

#include <AndorEMCCDcontroller/Camera.h>
#include <AndorEMCCDcontroller/CameraSettings.h>

namespace AndorEMCCDcontroller {

/**
 * @brief Judges the cooling health from successive temperature readings.
 *
 * The iXon3 heat sink has a thermal switch that cuts the TE cooler and
 * sounds the buzzer when it overheats (hardware guide 1.3.7), but the SDK
 * cannot read it for this camera. The sensor then warms away from the set
 * point, which is what this watch detects:
 *  - kAlarm: with the cooler on, the sensor is more than the margin above
 *    the set point after having reached it, or the camera reports a TEC
 *    over-temperature trip;
 *  - kWarning: the SDK reports a drift from the set point, or the sensor
 *    cannot be read.
 */
class ThermalWatch {
 public:
  explicit ThermalWatch(double cooling_lost_margin_c = 5.0);

  /** Evaluates one reading; returns true when the kind of problem changed. */
  bool Update(const TemperatureReading& r, int target_c);

  ThermalAlarm Level() const { return level_; }
  /** Human readable explanation with the current values; "" when ok. */
  const std::string& Reason() const { return reason_; }
  /** Number of times the alarm level was entered. */
  std::uint64_t AlarmCount() const { return alarm_count_; }

 private:
  double margin_c_;
  bool reached_ = false;  ///< Set point reached since cooler/target changed.
  int target_c_ = 0;
  ThermalAlarm level_ = ThermalAlarm::kOk;
  std::string kind_;  ///< Short key of the current problem, "" when ok.
  std::string reason_;
  std::uint64_t alarm_count_ = 0;
};

}  // namespace AndorEMCCDcontroller

#endif
