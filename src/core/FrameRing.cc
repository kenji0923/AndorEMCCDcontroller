#include <AndorEMCCDcontroller/FrameRing.h>

#include <algorithm>
#include <cstring>
#include <stdexcept>
#include <vector>

namespace AndorEMCCDcontroller {
namespace {
bool SameGeometry(const FrameHeader& a, const FrameHeader& b) {
  return a.run_id == b.run_id && a.width == b.width && a.height == b.height &&
         a.pixel_type == b.pixel_type && a.hbin == b.hbin && a.vbin == b.vbin &&
         a.x0 == b.x0 && a.y0 == b.y0;
}

template <typename T>
void Accumulate(const Frame& frame, std::vector<double>* sum) {
  for (std::size_t i = 0; i < sum->size(); ++i) {
    T value;
    std::memcpy(&value, frame.data->data() + i * sizeof(T), sizeof(T));
    (*sum)[i] += value;
  }
}
}  // namespace

FrameRing::FrameRing(std::size_t capacity) : capacity_(capacity) {
  if (!capacity) throw std::invalid_argument("ring capacity must be positive");
}

void FrameRing::Append(const Frame& frame) {
  if (!frame.data || frame.header.width <= 0 || frame.header.height <= 0 ||
      frame.data->size() != frame.header.PayloadBytes()) {
    throw std::invalid_argument("invalid frame payload");
  }
  std::lock_guard<std::mutex> lock(mutex_);
  if (!frames_.empty() && !SameGeometry(frames_.back().header, frame.header)) {
    frames_.clear();
  }
  frames_.push_back(frame);
  while (frames_.size() > capacity_) frames_.pop_front();
}

void FrameRing::Clear() {
  std::lock_guard<std::mutex> lock(mutex_);
  frames_.clear();
}

Frame FrameRing::Average(std::size_t count, std::size_t* used,
                         std::uint64_t through_seq) const {
  if (!count || count > std::max<std::size_t>(1, capacity_ - 1)) {
    throw std::invalid_argument("average count must be below ring capacity");
  }
  if (used) *used = 0;
  std::vector<Frame> frames;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto it = frames_.rbegin();
         it != frames_.rend() && frames.size() < count; ++it) {
      if (it->header.seq <= through_seq) frames.push_back(*it);
    }
  }
  if (frames.empty()) return {};
  if (used) *used = frames.size();
  Frame result = frames.front();
  if (frames.size() == 1) return result;
  const std::size_t pixels =
      static_cast<std::size_t>(result.header.width) * result.header.height;
  std::vector<double> sum(pixels, 0.0);
  for (const auto& frame : frames) {
    switch (frame.header.pixel_type) {
      case PixelType::kUint16:
        Accumulate<std::uint16_t>(frame, &sum);
        break;
      case PixelType::kInt32:
        Accumulate<std::int32_t>(frame, &sum);
        break;
      case PixelType::kFloat32:
        Accumulate<float>(frame, &sum);
        break;
    }
  }
  auto data =
      std::make_shared<std::vector<std::uint8_t>>(pixels * sizeof(float));
  for (std::size_t i = 0; i < pixels; ++i) {
    const float value = static_cast<float>(sum[i] / frames.size());
    std::memcpy(data->data() + i * sizeof(float), &value, sizeof(float));
  }
  result.header.pixel_type = PixelType::kFloat32;
  result.data = std::move(data);
  return result;
}
}  // namespace AndorEMCCDcontroller
