#ifndef ANDOREMCCDCONTROLLER_STREAM_RECORDER_H_
#define ANDOREMCCDCONTROLLER_STREAM_RECORDER_H_

#include <AndorEMCCDcontroller/Protocol.h>

#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <thread>

namespace AndorEMCCDcontroller {
struct StreamRecordingStatus {
  bool active = false;
  std::uint64_t saved = 0;
  std::uint64_t rate_skipped = 0;
  std::uint64_t queue_dropped = 0;
  std::uint64_t failed = 0;
  std::size_t queued = 0;
  std::string error;
};

// Writes raw frames to one uncompressed, multi-page BigTIFF on a worker.
// Start/Stop belong to one owner thread; Submit and Status are thread-safe.
class StreamRecorder {
 public:
  ~StreamRecorder();
  bool Start(const std::string& path, double max_fps, const Json& metadata,
             std::size_t queue_frames, std::size_t queue_bytes,
             std::string* error);
  void Submit(const Frame& frame);
  // Drain accepted frames, flush, close, and join the writer.
  void Stop();
  StreamRecordingStatus Status() const;

 private:
  struct Pending {
    Frame frame;
    Json accounting;
  };
  void WriteLoop(void* file);
  mutable std::mutex mutex_;
  std::condition_variable ready_;
  std::thread writer_;
  std::deque<Pending> queue_;
  StreamRecordingStatus status_;
  bool stopping_ = false;
  double max_fps_ = 0;
  bool have_last_ = false;
  double last_timestamp_ = 0;
  std::uint64_t last_run_ = 0;
  std::size_t max_frames_ = 0;
  std::size_t max_bytes_ = 0;
  std::size_t bytes_ = 0;
  Json metadata_;
  int tiff_orientation_ = 1;
  Json pending_loss_;
};
}  // namespace AndorEMCCDcontroller
#endif  // ANDOREMCCDCONTROLLER_STREAM_RECORDER_H_
