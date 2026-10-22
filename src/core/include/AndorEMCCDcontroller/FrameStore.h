/**
 * @file FrameStore.h
 * @brief Declaration of AndorEMCCDcontroller::FrameStore.
 */

#ifndef AndorEMCCDcontroller__FrameStore_h
#define AndorEMCCDcontroller__FrameStore_h

#include <cstddef>
#include <cstdint>
#include <deque>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include <AndorEMCCDcontroller/Protocol.h>

namespace AndorEMCCDcontroller {

/** Bookkeeping of one acquisition run. */
struct RunInfo {
  std::uint64_t run_id = 0;
  std::string mode;
  std::int64_t frames = 0;    ///< Frames read out so far.
  std::int64_t expected = 0;  ///< 0: unbounded (continuous).
  bool complete = false;
  bool aborted = false;
  std::uint64_t first_seq = 0;
  double start_time = 0.0;
};

void to_json(Json& j, const RunInfo& r);

/**
 * @brief Thread-safe history of the most recent frames.
 *
 * The engine thread appends; the control server reads concurrently. Frames
 * are evicted oldest first when either the frame or the byte limit is hit.
 */
class FrameStore {
 public:
  FrameStore(std::size_t max_frames, std::size_t max_bytes);

  void BeginRun(const RunInfo& run);
  /** @p frame.header.seq must be the next global sequence number. */
  void Append(Frame frame);
  void EndRun(std::uint64_t run_id, bool aborted);

  std::optional<RunInfo> Run(std::uint64_t run_id) const;
  std::optional<RunInfo> LatestRun() const;
  std::vector<RunInfo> Runs() const;

  /**
   * @brief Copies (shares) frames [start, start + count) of a run.
   *
   * count < 0 means "all frames acquired so far". Fails if any requested
   * frame was already evicted or does not exist yet.
   */
  bool Get(std::uint64_t run_id, std::int64_t start, std::int64_t count,
           std::vector<Frame>* out, std::string* error) const;

  std::optional<Frame> Latest() const;

  std::size_t Size() const;

  // Stream sessions start at the next acquired frame; idle sessions expire
  // after 60 seconds. ReadStream advances this client's send cursor only.
  // Until the next request acknowledges batch_id, the reply is retained for
  // retry, up to 16 MiB (or one larger frame) per session, max 256 sessions.
  void OpenStream(const std::string& client);
  void CloseStream(const std::string& client);
  Json ReadStream(const std::string& client, std::size_t count,
                  std::vector<Frame>* out, std::uint64_t ack = 0);

 private:
  struct Stream {
    std::uint64_t next_seq = 0;
    std::uint64_t dropped = 0;
    std::uint64_t first_seq = 0;
    std::uint64_t last_seq = 0;
    double first_timestamp = 0;
    double last_timestamp = 0;
    double last_seen = 0;
    std::uint64_t batch_id = 0;
    Json pending_reply;
    std::vector<Frame> pending_frames;
  };
  std::map<std::string, Stream> streams_;
  std::uint64_t next_seq_ = 0;
  void ExpireStreamsLocked();

  void EvictLocked();

  mutable std::mutex mutex_;
  std::size_t max_frames_;
  std::size_t max_bytes_;
  std::size_t bytes_ = 0;
  std::deque<Frame> frames_;
  std::map<std::uint64_t, RunInfo> runs_;
};

}  // namespace AndorEMCCDcontroller

#endif
