/**
 * @file test_imageio.cc
 * @brief Image writers produce files that read back correctly.
 */

#include <cstring>
#include <filesystem>
#include <fstream>

#include <gtest/gtest.h>

#include <AndorEMCCDcontroller/ImageIO.h>

namespace aec = AndorEMCCDcontroller;
namespace fs = std::filesystem;

namespace {

aec::Frame MakeFrame(int w, int h, std::uint16_t offset) {
  aec::Frame f;
  f.header.width = w;
  f.header.height = h;
  f.header.exposure_s = 0.01;
  f.header.timestamp = 1.7e9;
  auto data = std::make_shared<std::vector<std::uint8_t>>(w * h * 2);
  for (int i = 0; i < w * h; ++i) {
    const std::uint16_t v = static_cast<std::uint16_t>(offset + i);
    std::memcpy(data->data() + 2 * i, &v, 2);
  }
  f.data = data;
  return f;
}

fs::path TempDir() {
  const fs::path dir = fs::temp_directory_path() / "aec_test_imageio";
  fs::create_directories(dir);
  return dir;
}

}  // namespace

TEST(ImageIO, FormatFromPath) {
  aec::ImageFormat f;
  ASSERT_TRUE(aec::FormatFromPath("a/b.TIFF", &f));
  EXPECT_EQ(f, aec::ImageFormat::kTiff);
  ASSERT_TRUE(aec::FormatFromPath("x.fits", &f));
  EXPECT_EQ(f, aec::ImageFormat::kFits);
  EXPECT_FALSE(aec::FormatFromPath("x.jpg", &f));
}

TEST(ImageIO, TiffMultiPageRoundTrip) {
  const std::vector<aec::Frame> frames{MakeFrame(16, 8, 100),
                                       MakeFrame(16, 8, 200)};
  const std::string path = (TempDir() / "stack.tif").string();
  std::string error;
  ASSERT_TRUE(aec::WriteTiff(path, frames, {{"note", "test"}}, &error))
      << error;
  std::vector<aec::Frame> back;
  ASSERT_TRUE(aec::ReadTiff(path, &back, &error)) << error;
  ASSERT_EQ(back.size(), 2u);
  EXPECT_EQ(back[1].header.width, 16);
  EXPECT_EQ(*back[0].data, *frames[0].data);
  EXPECT_EQ(*back[1].data, *frames[1].data);
  EXPECT_DOUBLE_EQ(back[0].header.exposure_s, 0.01);
}

TEST(ImageIO, PngFitsNpyAreWritten) {
  const std::vector<aec::Frame> frames{MakeFrame(10, 5, 0),
                                       MakeFrame(10, 5, 50)};
  const fs::path dir = TempDir();
  std::string error;
  ASSERT_TRUE(aec::WriteFrames((dir / "one.png").string(),
                               aec::ImageFormat::kPng16, {frames[0]}, {},
                               &error))
      << error;
  ASSERT_TRUE(aec::WriteFrames((dir / "many.png").string(),
                               aec::ImageFormat::kPng16, frames, {}, &error));
  EXPECT_TRUE(fs::exists(dir / "many_0001.png"));
  ASSERT_TRUE(aec::WriteFits((dir / "cube.fits").string(), frames,
                             {{"settings", {{"em_gain", 10}}}}, &error))
      << error;
  ASSERT_TRUE(aec::WriteNpy((dir / "stack.npy").string(), frames, &error));

  std::ifstream npy(dir / "stack.npy", std::ios::binary);
  char magic[6];
  npy.read(magic, 6);
  EXPECT_EQ(std::string(magic + 1, 5), "NUMPY");
  EXPECT_EQ(fs::file_size(dir / "stack.npy") % 64, (10u * 5u * 2u * 2u) % 64);

  std::ifstream fits(dir / "cube.fits", std::ios::binary);
  char card[30] = {0};
  fits.read(card, 29);
  EXPECT_EQ(std::string(card, 6), "SIMPLE");
  EXPECT_EQ(fs::file_size(dir / "cube.fits") % 2880, 0u);

  std::ifstream png(dir / "one.png", std::ios::binary);
  char sig[4] = {0};
  png.read(sig, 4);
  EXPECT_EQ(std::string(sig + 1, 3), "PNG");
}
