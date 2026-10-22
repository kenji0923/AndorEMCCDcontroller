/**
 * @file FrameStore.cc
 * @brief Implementation of AndorEMCCDcontroller::FrameStore.
 */

#include <AndorEMCCDcontroller/FrameStore.h>

#include <algorithm>
#include <chrono>
#include <stdexcept>

namespace AndorEMCCDcontroller {

namespace {
constexpr std::size_t kMaxRunsKept = 256;

double SteadyNow() {
  return std::chrono::duration<double>(
      std::chrono::steady_clock::now().time_since_epoch()).count();
}
}  // namespace

void to_json(Json& j, const RunInfo& r) {
  j = Json{{"run_id", r.run_id},     {"mode", r.mode},
           {"frames", r.frames},     {"expected", r.expected},
           {"complete", r.complete}, {"aborted", r.aborted},
           {"first_seq", r.first_seq}, {"start_time", r.start_time}};
}

FrameStore::FrameStore(std::size_t max_frames, std::size_t max_bytes)
    : max_frames_(std::max<std::size_t>(1, max_frames)),
      max_bytes_(max_bytes) {}

void FrameStore::BeginRun(const RunInfo& run) {
  std::lock_guard<std::mutex> lock(mutex_);
  runs_[run.run_id] = run;
  while (runs_.size() > kMaxRunsKept) runs_.erase(runs_.begin());
}

void FrameStore::Append(Frame frame) {
  std::lock_guard<std::mutex> lock(mutex_);
  auto run = runs_.find(frame.header.run_id);
  if (run != runs_.end()) run->second.frames = frame.header.index + 1;
  next_seq_ = frame.header.seq + 1;
  bytes_ += frame.data ? frame.data->size() : 0;
  frames_.push_back(std::move(frame));
  EvictLocked();
}

void FrameStore::EndRun(std::uint64_t run_id, bool aborted) {
  std::lock_guard<std::mutex> lock(mutex_);
  auto run = runs_.find(run_id);
  if (run == runs_.end()) return;
  run->second.complete = true;
  run->second.aborted = aborted;
}

void FrameStore::EvictLocked() {
  while (!frames_.empty() &&
         (frames_.size() > max_frames_ || (max_bytes_ > 0 && bytes_ > max_bytes_ &&
                                           frames_.size() > 1))) {
    const auto& h = frames_.front().header;
    ExpireStreamsLocked();
    for (auto& entry : streams_) {
      auto& s = entry.second;
      if (h.seq < s.next_seq) continue;
      if (s.dropped == 0) {
        s.first_seq = h.seq;
        s.first_timestamp = h.timestamp;
      }
      ++s.dropped;
      s.last_seq = h.seq;
      s.last_timestamp = h.timestamp;
      s.next_seq = h.seq + 1;
    }
    bytes_ -= frames_.front().data ? frames_.front().data->size() : 0;
    frames_.pop_front();
  }
}

void FrameStore::ExpireStreamsLocked() {
  const double now = SteadyNow();
  for (auto it = streams_.begin(); it != streams_.end();) {
    if (now - it->second.last_seen > 60) it = streams_.erase(it);
    else ++it;
  }
}

void FrameStore::OpenStream(const std::string& client) {
  std::lock_guard<std::mutex> lock(mutex_);
  ExpireStreamsLocked();
  if (client.empty()) throw std::runtime_error("stream needs a client id");
  if (streams_.count(client)) {
    streams_.at(client).last_seen = SteadyNow();
    return;
  }
  if (streams_.size() >= 256) throw std::runtime_error("too many streams");
  Stream s;
  s.next_seq = next_seq_;
  s.last_seen = SteadyNow();
  streams_[client] = s;
}

void FrameStore::CloseStream(const std::string& client) {
  std::lock_guard<std::mutex> lock(mutex_);
  streams_.erase(client);
}

Json FrameStore::ReadStream(const std::string& client, std::size_t count,
                           std::vector<Frame>* out, std::uint64_t ack) {
  std::lock_guard<std::mutex> lock(mutex_);
  ExpireStreamsLocked();
  auto it = streams_.find(client);
  if (it == streams_.end()) throw std::runtime_error("stream expired or not open");
  auto& s = it->second;
  s.last_seen = SteadyNow();
  if (!s.pending_reply.is_null()) {
    if (ack != s.batch_id) {
      *out = s.pending_frames;
      return s.pending_reply;
    }
    s.pending_frames.clear();
    s.pending_reply = nullptr;
  }
  Json loss{{"count", s.dropped}, {"first_seq", nullptr},
            {"last_seq", nullptr}, {"first_timestamp", nullptr},
            {"last_timestamp", nullptr}, {"reported_at", UnixTimeNow()}};
  if (s.dropped) {
    loss["first_seq"] = s.first_seq;
    loss["last_seq"] = s.last_seq;
    loss["first_timestamp"] = s.first_timestamp;
    loss["last_timestamp"] = s.last_timestamp;
  }
  Json headers = Json::array();
  std::size_t bytes = 0;
  for (const auto& f : frames_) {
    if (f.header.seq < s.next_seq) continue;
    // Limit retained retry payloads to 16 MiB per client, with at least
    // one frame so an individual larger image can still be delivered.
    if (out->size() >= count ||
        (!out->empty() && bytes + f.data->size() > (std::size_t{16} << 20))) break;
    out->push_back(f);
    bytes += f.data->size();
    headers.push_back(f.header);
    s.next_seq = f.header.seq + 1;
  }
  s.dropped = 0;
  s.pending_frames = *out;
  s.pending_reply = Json{{"frames", headers}, {"loss", loss},
                         {"next_seq", s.next_seq}, {"batch_id", ++s.batch_id}};
  return s.pending_reply;
}

std::optional<RunInfo> FrameStore::Run(std::uint64_t run_id) const {
  std::lock_guard<std::mutex> lock(mutex_);
  auto run = runs_.find(run_id);
  if (run == runs_.end()) return std::nullopt;
  return run->second;
}

std::optional<RunInfo> FrameStore::LatestRun() const {
  std::lock_guard<std::mutex> lock(mutex_);
  if (runs_.empty()) return std::nullopt;
  return runs_.rbegin()->second;
}

std::vector<RunInfo> FrameStore::Runs() const {
  std::lock_guard<std::mutex> lock(mutex_);
  std::vector<RunInfo> runs;
  for (const auto& entry : runs_) runs.push_back(entry.second);
  return runs;
}

bool FrameStore::Get(std::uint64_t run_id, std::int64_t start,
                     std::int64_t count, std::vector<Frame>* out,
                     std::string* error) const {
  std::lock_guard<std::mutex> lock(mutex_);
  auto run = runs_.find(run_id);
  if (run == runs_.end()) {
    *error = "unknown run " + std::to_string(run_id);
    return false;
  }
  const RunInfo& info = run->second;
  if (start < 0) start = 0;
  if (count < 0) count = std::max<std::int64_t>(0, info.frames - start);
  if (start + count > info.frames) {
    *error = "run " + std::to_string(run_id) + " has only " +
             std::to_string(info.frames) + " frame(s)";
    return false;
  }
  if (count == 0) return true;
  if (frames_.empty()) {
    *error = "frames evicted from history";
    return false;
  }
  const std::uint64_t front_seq = frames_.front().header.seq;
  const std::uint64_t first = info.first_seq + static_cast<std::uint64_t>(start);
  if (first < front_seq) {
    *error = "frames " + std::to_string(start) + ".. of run " +
             std::to_string(run_id) +
             " were evicted from history (increase history_frames)";
    return false;
  }
  const std::size_t pos = static_cast<std::size_t>(first - front_seq);
  if (pos + static_cast<std::size_t>(count) > frames_.size()) {
    *error = "frames not available";
    return false;
  }
  out->reserve(out->size() + static_cast<std::size_t>(count));
  for (std::int64_t i = 0; i < count; ++i) {
    out->push_back(frames_[pos + static_cast<std::size_t>(i)]);
  }
  return true;
}

std::optional<Frame> FrameStore::Latest() const {
  std::lock_guard<std::mutex> lock(mutex_);
  if (frames_.empty()) return std::nullopt;
  return frames_.back();
}

std::size_t FrameStore::Size() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return frames_.size();
}

}  // namespace AndorEMCCDcontroller
