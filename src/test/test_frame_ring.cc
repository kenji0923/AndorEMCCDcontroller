#include <AndorEMCCDcontroller/FrameRing.h>
#include <gtest/gtest.h>

#include <cstring>
#include <stdexcept>

namespace aec = AndorEMCCDcontroller;
namespace {
template <typename T>
aec::Frame MakeFrame(std::uint64_t seq, T value, aec::PixelType type) {
  aec::Frame frame;
  frame.header.run_id = 1;
  frame.header.seq = seq;
  frame.header.width = 1;
  frame.header.height = 1;
  frame.header.pixel_type = type;
  auto data = std::make_shared<std::vector<std::uint8_t>>(sizeof(T));
  std::memcpy(data->data(), &value, sizeof(T));
  frame.data = data;
  return frame;
}

double Value(const aec::Frame& frame) {
  if (frame.header.pixel_type == aec::PixelType::kFloat32) {
    float value;
    std::memcpy(&value, frame.data->data(), sizeof(value));
    return value;
  }
  std::uint16_t value;
  std::memcpy(&value, frame.data->data(), sizeof(value));
  return value;
}
}  // namespace

TEST(FrameRing, LatestWindowWarmupAndOverwrite) {
  aec::FrameRing ring(4);
  std::size_t used = 99;
  EXPECT_FALSE(ring.Average(3, &used).data);
  EXPECT_EQ(used, 0u);
  auto first = MakeFrame<std::uint16_t>(0, 10, aec::PixelType::kUint16);
  ring.Append(first);
  EXPECT_EQ(Value(ring.Average(3, &used)), 10);
  EXPECT_EQ(used, 1u);
  ring.Append(MakeFrame<std::uint16_t>(1, 11, aec::PixelType::kUint16));
  auto average = ring.Average(3, &used);
  EXPECT_EQ(Value(average), 10.5);
  EXPECT_EQ(used, 2u);
  EXPECT_EQ(Value(first), 10);  // Raw image is unchanged.
  EXPECT_EQ(average.header.pixel_type, aec::PixelType::kFloat32);
  for (int i = 2; i < 6; ++i)
    ring.Append(MakeFrame<std::uint16_t>(i, static_cast<std::uint16_t>(10 + i),
                                         aec::PixelType::kUint16));
  EXPECT_EQ(Value(ring.Average(3, &used)), 14);
  EXPECT_EQ(used, 3u);
  EXPECT_EQ(Value(ring.Average(2)), 14.5);  // Setting changes immediately.
  EXPECT_EQ(Value(ring.Average(1)), 15);
  EXPECT_EQ(Value(ring.Average(3, &used, 3)), 12.5);
  EXPECT_EQ(used, 2u);  // Only retained frames through the processed image.
  EXPECT_FALSE(ring.Average(2, &used, 1).data);
  ring.Clear();
  EXPECT_FALSE(ring.Average(1).data);
}

TEST(FrameRing, BoundsRunAndGeometryChanges) {
  EXPECT_THROW(aec::FrameRing(0), std::invalid_argument);
  aec::FrameRing single(1);
  EXPECT_NO_THROW(single.Average(1));
  EXPECT_THROW(single.Average(2), std::invalid_argument);
  aec::FrameRing ring(4);
  EXPECT_THROW(ring.Average(0), std::invalid_argument);
  EXPECT_THROW(ring.Average(4), std::invalid_argument);
  ring.Append(MakeFrame<std::uint16_t>(0, 10, aec::PixelType::kUint16));
  auto next = MakeFrame<std::uint16_t>(1, 20, aec::PixelType::kUint16);
  next.header.run_id = 2;
  ring.Append(next);
  std::size_t used;
  EXPECT_EQ(Value(ring.Average(3, &used)), 20);
  EXPECT_EQ(used, 1u);
  next.header.x0 = 2;
  next.header.seq = 2;
  ring.Append(next);
  ring.Average(3, &used);
  EXPECT_EQ(used, 1u);
}

TEST(FrameRing, SignedAndFloatPixels) {
  aec::FrameRing ring(4);
  ring.Append(MakeFrame<std::int32_t>(0, -2000000000, aec::PixelType::kInt32));
  ring.Append(MakeFrame<std::int32_t>(1, 2000000001, aec::PixelType::kInt32));
  EXPECT_EQ(Value(ring.Average(2)), 0.5);
  ring.Append(MakeFrame<float>(2, 1.25f, aec::PixelType::kFloat32));
  ring.Append(MakeFrame<float>(3, 2.5f, aec::PixelType::kFloat32));
  EXPECT_EQ(Value(ring.Average(3)), 1.875);
}
