#include <AndorEMCCDcontroller/ImageIO.h>
#include <AndorEMCCDcontroller/StreamRecorder.h>
#include <gtest/gtest.h>

#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <thread>

namespace aec = AndorEMCCDcontroller;
namespace fs = std::filesystem;
namespace {
fs::path Path(const char* name) {
  const auto dir = fs::temp_directory_path() / "aec_stream_recorder_test";
  fs::create_directories(dir);
  const auto path = dir / name;
  fs::remove(path);
  return path;
}

aec::Frame Frame(int index) {
  aec::Frame f;
  f.header.run_id = 1;
  f.header.seq = index;
  f.header.index = index;
  f.header.width = 2;
  f.header.height = 1;
  f.header.timestamp = 1000 + index * 0.05;
  f.header.exposure_s = 0.05;
  auto data = std::make_shared<std::vector<std::uint8_t>>(4);
  const std::uint16_t pixels[2] = {static_cast<std::uint16_t>(index), 65535};
  std::memcpy(data->data(), pixels, 4);
  f.data = data;
  return f;
}
}  // namespace

TEST(StreamRecorder, BigTiffRateLimitedRawRoundTrip) {
  const auto path = Path("rate.tif");
  aec::StreamRecorder recorder;
  std::string error;
  ASSERT_TRUE(
      recorder.Start(path.string(), 10, {{"test", true}}, 20, 1024, &error))
      << error;
  for (int i = 0; i < 10; ++i) {
    auto frame = Frame(i);
    if (i == 1)
      frame.delivery = {{"server_loss",
                         {{"count", 3},
                          {"first_seq", 100},
                          {"last_seq", 102},
                          {"first_timestamp", 900},
                          {"last_timestamp", 901}}}};
    recorder.Submit(frame);
  }
  recorder.Stop();
  const auto status = recorder.Status();
  EXPECT_EQ(status.saved, 5u);
  EXPECT_EQ(status.rate_skipped, 5u);
  EXPECT_EQ(status.queue_dropped, 0u);
  EXPECT_EQ(status.queued, 0u);
  EXPECT_TRUE(status.error.empty()) << status.error;
  std::vector<aec::Frame> back;
  ASSERT_TRUE(aec::ReadTiff(path.string(), &back, &error)) << error;
  ASSERT_EQ(back.size(), 5u);
  for (int i = 0; i < 5; ++i) {
    EXPECT_EQ(back[i].header.seq, static_cast<std::uint64_t>(i * 2));
    EXPECT_EQ(*back[i].data, *Frame(i * 2).data);
  }
  std::ifstream file(path, std::ios::binary);
  unsigned char header[4];
  file.read(reinterpret_cast<char*>(header), 4);
  EXPECT_EQ(header[2],
            header[0] == 'I' ? 43 : 0);  // BigTIFF, even below 4 GiB.
  EXPECT_EQ(header[3], header[0] == 'I' ? 0 : 43);
}

TEST(StreamRecorder, WritesBeforeStopAndDrains) {
  const auto path = Path("progressive.tif");
  aec::StreamRecorder recorder;
  std::string error;
  ASSERT_TRUE(recorder.Start(path.string(), 0, {}, 20, 1024, &error)) << error;
  recorder.Submit(Frame(0));
  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds(3);
  while (recorder.Status().saved < 1 &&
         std::chrono::steady_clock::now() < deadline)
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  ASSERT_EQ(recorder.Status().saved, 1u);
  EXPECT_TRUE(recorder.Status().active);
  std::vector<aec::Frame> back;
  ASSERT_TRUE(aec::ReadTiff(path.string(), &back, &error)) << error;
  ASSERT_EQ(back.size(), 1u);
  for (int i = 1; i < 10; ++i) recorder.Submit(Frame(i));
  recorder.Stop();
  EXPECT_EQ(recorder.Status().saved, 10u);
  EXPECT_EQ(recorder.Status().rate_skipped, 0u);
  back.clear();
  ASSERT_TRUE(aec::ReadTiff(path.string(), &back, &error));
  EXPECT_EQ(back.size(), 10u);
}

TEST(StreamRecorder, RejectsOverwriteAndInvalidConfiguration) {
  auto path = Path("existing.tif");
  {
    std::ofstream file(path);
    file << "keep me";
  }
  aec::StreamRecorder recorder;
  std::string error;
  EXPECT_FALSE(recorder.Start(path.string(), 10, {}, 20, 1024, &error));
  std::ifstream file(path);
  std::string contents;
  std::getline(file, contents);
  EXPECT_EQ(contents, "keep me");
  path = Path("invalid.tif");
  EXPECT_FALSE(recorder.Start(path.string(), -1, {}, 20, 1024, &error));
  EXPECT_FALSE(recorder.Start(path.string(),
                              std::numeric_limits<double>::infinity(), {}, 20,
                              1024, &error));
  EXPECT_FALSE(recorder.Start(path.string(), 1, {}, 0, 1024, &error));
  EXPECT_FALSE(fs::exists(path));
}

TEST(StreamRecorder, BoundedQueueAndWriteFailureAreVisible) {
  aec::StreamRecorder recorder;
  std::string error;
  ASSERT_TRUE(
      recorder.Start(Path("bounded.tif").string(), 0, {}, 2, 3, &error));
  recorder.Submit(Frame(0));  // Four-byte frame exceeds byte capacity.
  recorder.Stop();
  EXPECT_EQ(recorder.Status().queue_dropped, 1u);
  EXPECT_EQ(recorder.Status().saved, 0u);
  const auto path = Path("failure.tif");
  ASSERT_TRUE(recorder.Start(path.string(), 0, {}, 2, 1024, &error));
  auto invalid = Frame(0);
  invalid.data = nullptr;
  recorder.Submit(invalid);
  recorder.Stop();
  EXPECT_FALSE(recorder.Status().error.empty());
  EXPECT_EQ(recorder.Status().failed, 1u);
  EXPECT_EQ(recorder.Status().saved, 0u);
}

TEST(StreamRecorder, NewRunResetsSamplingAndShapesCanChange) {
  aec::StreamRecorder recorder;
  std::string error;
  auto path = Path("runs.tif");
  ASSERT_TRUE(recorder.Start(path.string(), 1, {}, 20, 1024, &error));
  recorder.Submit(Frame(0));
  auto f = Frame(1);
  recorder.Submit(f);  // Skipped within run 1.
  f.header.run_id = 2;
  f.header.width = 1;
  f.header.height = 2;
  recorder.Submit(f);
  recorder.Stop();
  EXPECT_EQ(recorder.Status().saved, 2u);
  EXPECT_EQ(recorder.Status().rate_skipped, 1u);
  std::vector<aec::Frame> back;
  ASSERT_TRUE(aec::ReadTiff(path.string(), &back, &error));
  ASSERT_EQ(back.size(), 2u);
  EXPECT_EQ(back[1].header.width, 1);
  EXPECT_EQ(back[1].header.height, 2);
}

TEST(StreamRecorder, SignedAndFloatRawPixelsArePreserved) {
  aec::StreamRecorder recorder;
  std::string error;
  const auto path = Path("types.tif");
  ASSERT_TRUE(recorder.Start(path.string(), 0, {}, 20, 1024, &error));
  auto signed_frame = Frame(0);
  signed_frame.header.pixel_type = aec::PixelType::kInt32;
  auto signed_data = std::make_shared<std::vector<std::uint8_t>>(8);
  const std::int32_t signed_values[2] = {-123, 2147483647};
  std::memcpy(signed_data->data(), signed_values, 8);
  signed_frame.data = signed_data;
  recorder.Submit(signed_frame);
  auto float_frame = Frame(1);
  float_frame.header.pixel_type = aec::PixelType::kFloat32;
  auto float_data = std::make_shared<std::vector<std::uint8_t>>(8);
  const float float_values[2] = {-1.25f, 123.5f};
  std::memcpy(float_data->data(), float_values, 8);
  float_frame.data = float_data;
  recorder.Submit(float_frame);
  recorder.Stop();
  std::vector<aec::Frame> back;
  ASSERT_TRUE(aec::ReadTiff(path.string(), &back, &error));
  ASSERT_EQ(back.size(), 2u);
  EXPECT_EQ(back[0].header.pixel_type, aec::PixelType::kInt32);
  EXPECT_EQ(back[1].header.pixel_type, aec::PixelType::kFloat32);
  EXPECT_EQ(*back[0].data, *signed_frame.data);
  EXPECT_EQ(*back[1].data, *float_frame.data);
}
