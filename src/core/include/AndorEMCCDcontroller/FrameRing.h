#ifndef ANDOREMCCDCONTROLLER_FRAME_RING_H_
#define ANDOREMCCDCONTROLLER_FRAME_RING_H_

#include <AndorEMCCDcontroller/Protocol.h>

#include <cstddef>
#include <cstdint>
#include <deque>
#include <limits>
#include <mutex>

namespace AndorEMCCDcontroller {

// Retains received images for display even after the processing queue is read.
class FrameRing {
 public:
  explicit FrameRing(std::size_t capacity);
  void Append(const Frame& frame);
  void Clear();
  // 1 disables averaging. Otherwise count must be strictly below capacity.
  // Uses available images during warm-up; never mixes runs or geometries.
  Frame Average(std::size_t count, std::size_t* used = nullptr,
                std::uint64_t through_seq =
                    std::numeric_limits<std::uint64_t>::max()) const;

 private:
  const std::size_t capacity_;
  mutable std::mutex mutex_;
  std::deque<Frame> frames_;
};

}  // namespace AndorEMCCDcontroller
#endif  // ANDOREMCCDCONTROLLER_FRAME_RING_H_
