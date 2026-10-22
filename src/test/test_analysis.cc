/**
 * @file test_analysis.cc
 * @brief ROI statistics, expressions and colormaps.
 */

#include <cmath>
#include <cstring>

#include <gtest/gtest.h>

#include <AndorEMCCDcontroller/Analysis.h>
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
