/**
 * @file test_settings.cc
 * @brief Settings JSON round trip and validation.
 */

#include <gtest/gtest.h>

#include <AndorEMCCDcontroller/CameraSettings.h>

namespace aec = AndorEMCCDcontroller;

TEST(Settings, RoundTripThroughJson) {
  aec::CameraSettings s;
  s.trigger_mode = aec::TriggerMode::kExternalExposure;
  s.exposure_time_s = 0.123;
  s.em_gain_enabled = true;
  s.em_gain = 200;
  s.fan_mode = aec::FanMode::kLow;
  const aec::Json j = s;
  EXPECT_EQ(j["trigger_mode"], "external_exposure");
  EXPECT_EQ(j["fan_mode"], "low");

  aec::CameraSettings t;
  std::vector<std::string> errors;
  ASSERT_TRUE(aec::MergeSettings(j, &t, &errors)) << errors.front();
  EXPECT_EQ(aec::Json(t), j);
}

TEST(Settings, RejectsUnknownKeysAndBadValues) {
  aec::CameraSettings s;
  std::vector<std::string> errors;
  const aec::Json patch = {{"bogus", 1},
                           {"exposure_time_s", -1.0},
                           {"trigger_mode", "sometimes"},
                           {"em_gain", 1.5},
                           {"hbin", 4}};
  EXPECT_FALSE(aec::MergeSettings(patch, &s, &errors));
  EXPECT_EQ(errors.size(), 4u);
  EXPECT_EQ(s.hbin, 4);                 // Valid keys are still applied.
  EXPECT_DOUBLE_EQ(s.exposure_time_s, 0.01);
}

TEST(Settings, AcceptsSdkCodesForEnums) {
  aec::CameraSettings s;
  std::vector<std::string> errors;
  ASSERT_TRUE(aec::MergeSettings({{"trigger_mode", 10},
                                  {"acquisition_mode", 3}},
                                 &s, &errors));
  EXPECT_EQ(s.trigger_mode, aec::TriggerMode::kSoftware);
  EXPECT_EQ(s.acquisition_mode, aec::AcquisitionMode::kKinetic);
}

TEST(Settings, ThermalKeys) {
  EXPECT_TRUE(aec::IsThermalKey("cooler_on"));
  EXPECT_TRUE(aec::IsThermalKey("target_temperature_c"));
  EXPECT_FALSE(aec::IsThermalKey("exposure_time_s"));
}

TEST(Protocol, FrameHeaderJson) {
  aec::FrameHeader h;
  h.width = 64;
  h.height = 32;
  h.pixel_type = aec::PixelType::kInt32;
  h.camera_time_s = -1.0;
  const aec::Json j = h;
  EXPECT_TRUE(j["camera_time_s"].is_null());
  const auto back = j.get<aec::FrameHeader>();
  EXPECT_EQ(back.PayloadBytes(), 64u * 32u * 4u);
  EXPECT_LT(back.camera_time_s, 0.0);
}
