/**
 * @file test_analysis.cc
 * @brief ROI statistics, expressions and colormaps.
 */

#include <cmath>
#include <cstring>
#include <limits>

#include <gtest/gtest.h>

#include <AndorEMCCDcontroller/Analysis.h>
#include <AndorEMCCDcontroller/DisplayConversion.h>
#include <AndorEMCCDcontroller/Colormap.h>
#include <AndorEMCCDcontroller/Expression.h>

namespace aec = AndorEMCCDcontroller;

namespace {

/** 4 x 3 uint16 frame with value = 10 * y + x. */
aec::Frame MakeFrame() {
  aec::Frame f;
  f.header.width = 4;
  f.header.height = 3;
  auto data = std::make_shared<std::vector<std::uint8_t>>(4 * 3 * 2);
  for (int y = 0; y < 3; ++y) {
    for (int x = 0; x < 4; ++x) {
      const std::uint16_t v = static_cast<std::uint16_t>(10 * y + x);
      std::memcpy(data->data() + 2 * (y * 4 + x), &v, 2);
    }
  }
  f.data = data;
  return f;
}

}  // namespace

TEST(Analysis, RoiStatistics) {
  const aec::Frame f = MakeFrame();
  aec::Roi roi;
  roi.x = 1;
  roi.y = 1;
  roi.width = 2;
  roi.height = 2;  // values 11 12 21 22
  const aec::RoiStats s = aec::ComputeRoiStats(f.header, f.data->data(), roi);
  EXPECT_EQ(s.count, 4);
  EXPECT_DOUBLE_EQ(s.sum, 66.0);
  EXPECT_DOUBLE_EQ(s.mean, 16.5);
  EXPECT_DOUBLE_EQ(s.min, 11.0);
  EXPECT_DOUBLE_EQ(s.max, 22.0);
  EXPECT_NEAR(s.std, std::sqrt((5.5 * 5.5 + 4.5 * 4.5) * 2 / 4.0), 1e-12);
}

TEST(Analysis, RoiIsClippedToFrame) {
  const aec::Frame f = MakeFrame();
  aec::Roi roi;
  roi.x = 3;
  roi.y = 2;
  roi.width = 10;
  roi.height = 10;
  const aec::RoiStats s = aec::ComputeRoiStats(f.header, f.data->data(), roi);
  EXPECT_EQ(s.count, 1);
  EXPECT_DOUBLE_EQ(s.sum, 23.0);

  roi.x = 100;
  const aec::RoiStats empty =
      aec::ComputeRoiStats(f.header, f.data->data(), roi);
  EXPECT_EQ(empty.count, 0);
  EXPECT_TRUE(std::isnan(empty.mean));
}

TEST(Analysis, PercentilesAndMapping) {
  const aec::Frame f = MakeFrame();
  double lo = 0;
  double hi = 0;
  aec::FrameMinMax(f.header, f.data->data(), &lo, &hi);
  EXPECT_EQ(lo, 0.0);
  EXPECT_EQ(hi, 23.0);
  aec::FramePercentiles(f.header, f.data->data(), 0.0, 100.0, &lo, &hi);
  EXPECT_EQ(lo, 0.0);
  EXPECT_EQ(hi, 23.0);
  std::vector<std::uint8_t> idx(12);
  aec::MapToIndices(f.header, f.data->data(), 0.0, 23.0, false, idx.data());
  EXPECT_EQ(idx[0], 0);
  EXPECT_EQ(idx[11], 255);
}

TEST(Analysis, ThresholdDisplayIncludesBoundaryAndPreservesRawPixels) {
  const aec::Frame frame = MakeFrame();
  const auto original = *frame.data;
  const auto before = aec::ComputeRoiStats(
      frame.header, frame.data->data(), aec::Roi{"all", 0, 0, 4, 3});
  std::vector<std::uint8_t> indices(12);
  aec::MapThresholdToIndices(frame.header, frame.data->data(), 11.0,
                            indices.data());
  EXPECT_EQ(indices, (std::vector<std::uint8_t>{0, 0, 0, 0, 0, 255, 255, 255,
                                               255, 255, 255, 255}));
  EXPECT_EQ(*frame.data, original);
  const auto after = aec::ComputeRoiStats(
      frame.header, frame.data->data(), aec::Roi{"all", 0, 0, 4, 3});
  EXPECT_DOUBLE_EQ(after.sum, before.sum);
  EXPECT_DOUBLE_EQ(after.mean, before.mean);
}

TEST(Analysis, ThresholdDisplaySupportsSignedAndFloatingPixels) {
  aec::FrameHeader header;
  header.width = 4;
  header.height = 1;
  std::uint8_t indices[4];
  header.pixel_type = aec::PixelType::kInt32;
  const std::int32_t signed_pixels[] = {-10, -1, 0, 100000};
  aec::MapThresholdToIndices(header,
      reinterpret_cast<const std::uint8_t*>(signed_pixels), -1.0, indices);
  EXPECT_EQ((std::vector<std::uint8_t>(indices, indices + 4)),
            (std::vector<std::uint8_t>{0, 255, 255, 255}));
  header.pixel_type = aec::PixelType::kFloat32;
  const float float_pixels[] = {1.25f, 1.5f, 1.75f,
                                std::numeric_limits<float>::quiet_NaN()};
  aec::MapThresholdToIndices(header,
      reinterpret_cast<const std::uint8_t*>(float_pixels), 1.5, indices);
  EXPECT_EQ((std::vector<std::uint8_t>(indices, indices + 4)),
            (std::vector<std::uint8_t>{0, 255, 255, 0}));
}

TEST(Expression, EvaluatesInOrderWithPreviousResults) {
  aec::ExpressionSet set;
  std::vector<std::string> errors;
  ASSERT_TRUE(set.Compile({"sig_sum", "bg_mean", "sig_count"},
                          {{"signal", "sig_sum - bg_mean * sig_count"},
                           {"ratio", "signal / sig_sum"},
                           {"root", "sqrt(abs(signal))"}},
                          &errors));
  const auto v = set.Evaluate({100.0, 2.0, 10.0});
  ASSERT_EQ(v.size(), 3u);
  EXPECT_DOUBLE_EQ(v[0], 80.0);
  EXPECT_DOUBLE_EQ(v[1], 0.8);
  EXPECT_DOUBLE_EQ(v[2], std::sqrt(80.0));
}

TEST(Expression, ReportsErrors) {
  aec::ExpressionSet set;
  std::vector<std::string> errors;
  EXPECT_FALSE(set.Compile({"a"}, {{"ok", "a + 1"}, {"bad", "a + "},
                                   {"sin", "a"}, {"unknown", "b * 2"}},
                           &errors));
  EXPECT_EQ(errors[0], "");
  EXPECT_NE(errors[1], "");
  EXPECT_NE(errors[2], "");  // Reserved name.
  EXPECT_NE(errors[3], "");  // Unknown variable.
  const auto v = set.Evaluate({1.0});
  EXPECT_DOUBLE_EQ(v[0], 2.0);
  EXPECT_TRUE(std::isnan(v[1]));
}

TEST(Analysis, Identifiers) {
  EXPECT_TRUE(aec::IsValidIdentifier("roi1"));
  EXPECT_TRUE(aec::IsValidIdentifier("Atoms_2"));
  EXPECT_FALSE(aec::IsValidIdentifier("1roi"));
  EXPECT_FALSE(aec::IsValidIdentifier("a-b"));
  EXPECT_FALSE(aec::IsValidIdentifier("pi"));
}

TEST(Colormap, KnownMaps) {
  const auto& names = aec::ColormapNames();
  EXPECT_GE(names.size(), 8u);
  const auto gray = aec::GetColormap("gray");
  EXPECT_EQ(gray[0], 0x000000u);
  EXPECT_EQ(gray[255], 0xffffffu);
  const auto viridis = aec::GetColormap("viridis");
  EXPECT_EQ(viridis[0], 0x440154u);  // matplotlib viridis start color.
  EXPECT_EQ(aec::GetColormap("viridis", true)[255], 0x440154u);
}

TEST(Analysis, BackgroundMeanAndFloatDisplayDoNotModifyRaw) {
  const aec::Frame raw = MakeFrame();
  aec::Roi bg;
  bg.name = "bg";
  bg.width = 2;
  bg.height = 1;
  bg.background = true;
  const auto metadata = aec::MeasureBackground(raw, {bg});
  ASSERT_TRUE(metadata.at("valid").get<bool>());
  EXPECT_DOUBLE_EQ(metadata.at("mean").get<double>(), .5);
  EXPECT_DOUBLE_EQ(metadata.at("sum").get<double>(), 1);
  EXPECT_EQ(metadata.at("count"), 2);
  const auto display = aec::SubtractBackground(raw, .5);
  EXPECT_EQ(raw.header.pixel_type, aec::PixelType::kUint16);
  EXPECT_EQ(display.header.pixel_type, aec::PixelType::kFloat32);
  EXPECT_NE(raw.data, display.data);
  EXPECT_DOUBLE_EQ(aec::PixelValue(raw.header, raw.data->data(), 0, 0), 0);
  EXPECT_DOUBLE_EQ(aec::PixelValue(display.header, display.data->data(), 0, 0),
                   -.5);
  EXPECT_DOUBLE_EQ(aec::PixelValue(display.header, display.data->data(), 1, 0),
                   .5);
  aec::Json saved = bg;
  EXPECT_TRUE(saved.get<aec::Roi>().background);
  bg.enabled = false;
  EXPECT_TRUE(aec::MeasureBackground(raw, {bg}).empty());
  bg.enabled = true;
  bg.x = 100;
  EXPECT_FALSE(aec::MeasureBackground(raw, {bg})["valid"].get<bool>());
}


TEST(DisplayConversion, FractionalNegativeValuesAndRawPreserved) {
  const auto raw = MakeFrame();
  aec::Json config{{"units", "photons"}, {"source", "manual"},
                   {"sensitivity_e_per_adu", 2.0}, {"em_gain", 10.0},
                   {"qe", 0.8}, {"baseline_adu", 1.0}};
  auto conversion = aec::ResolveDisplayConversion(config, {}, {});
  ASSERT_TRUE(conversion["valid"].get<bool>());
  auto converted = aec::ConvertDisplayFrame(raw, conversion);
  EXPECT_EQ(converted.header.pixel_type, aec::PixelType::kFloat32);
  EXPECT_DOUBLE_EQ(aec::PixelValue(converted.header, converted.data->data(), 0, 0), -.25);
  EXPECT_DOUBLE_EQ(aec::PixelValue(converted.header, converted.data->data(), 2, 0), .25);
  EXPECT_DOUBLE_EQ(aec::PixelValue(raw.header, raw.data->data(), 2, 0), 2);
  config["background_subtract_enabled"] = true;
  conversion = aec::ResolveDisplayConversion(config, {}, {}, {{"valid", true}, {"mean", .5}});
  converted = aec::ConvertDisplayFrame(raw, conversion);
  EXPECT_DOUBLE_EQ(aec::PixelValue(converted.header, converted.data->data(), 2, 0), .375);
}

TEST(DisplayConversion, RejectsMissingCalibrationAndDoubleConversion) {
  aec::Json config{{"units", "photons"}, {"source", "camera"}, {"wavelength_nm", 780.0}};
  EXPECT_FALSE(aec::ResolveDisplayConversion(config, {}, {})["valid"].get<bool>());
  config["source"] = "manual";
  config["sensitivity_e_per_adu"] = 2.0;
  config["em_gain"] = 10.0;
  config["qe"] = .8;
  config["baseline_adu"] = 100.0;
  EXPECT_FALSE(aec::ResolveDisplayConversion(config, {}, {{"count_convert_mode", 2}})["valid"].get<bool>());
  for (const auto& entry : std::vector<std::pair<std::string, double>>{
           {"em_gain", 0}, {"sensitivity_e_per_adu", -1}, {"qe", 1.1}}) {
    auto invalid = config;
    invalid[entry.first] = entry.second;
    EXPECT_FALSE(aec::ResolveDisplayConversion(invalid, {}, {})["valid"].get<bool>());
  }
  config["background_subtract_enabled"] = true;
  EXPECT_FALSE(aec::ResolveDisplayConversion(config, {}, {})["valid"].get<bool>());
}

TEST(DisplayConversion, RejectsCalibrationFromDifferentReadout) {
  aec::Json settings{{"output_amplifier", 0}, {"hs_speed_index", 0},
                     {"preamp_gain_index", 1}, {"em_gain_mode", 3},
                     {"em_gain_enabled", true}, {"em_gain", 20}, {"baseline_clamp", true}};
  aec::Json calibration{{"settings", settings}, {"sensitivity_e_per_adu", 2},
                        {"em_gain", 20}, {"qe", .8}, {"baseline_adu", 100},
                        {"wavelength_nm", 780}};
  const aec::Json config{{"units", "photons"}, {"source", "camera"}, {"wavelength_nm", 780}};
  EXPECT_TRUE(aec::ResolveDisplayConversion(config, calibration, settings)["valid"].get<bool>());
  settings["preamp_gain_index"] = 2;
  EXPECT_FALSE(aec::ResolveDisplayConversion(config, calibration, settings)["valid"].get<bool>());
}
