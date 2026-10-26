/**
 * @file LiveProcessor.cc
 * @brief Implementation of AndorEMCCDcontroller::LiveProcessor.
 */

#include "LiveProcessor.h"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstring>

#include "UdpSender.h"

namespace AndorEMCCDcontroller {

std::string EncodeUdpValue(double value, bool binary) {
  if (binary) {
    std::uint64_t bits = 0;
    std::memcpy(&bits, &value, sizeof(bits));
    std::string out(8, '\0');
    for (int i = 0; i < 8; ++i) {
      out[i] = static_cast<char>((bits >> (8 * i)) & 0xff);
    }
    return out;
  }
  // to_chars ignores the C locale (Qt sets it from the environment, which
  // could make printf write a decimal comma).
  char buffer[32];
  const auto r = std::to_chars(buffer, buffer + sizeof(buffer), value);
  return std::string(buffer, r.ptr);
}

LiveProcessor::LiveProcessor() = default;
LiveProcessor::~LiveProcessor() = default;

std::vector<std::string> LiveProcessor::Configure(
    const std::vector<Roi>& rois, const std::vector<ExpressionDef>& defs) {
  std::vector<std::string> inputs;
  for (const auto& roi : rois) {
    if (!roi.enabled) continue;
    for (const auto& stat : StatNames()) inputs.push_back(roi.name + "_" + stat);
  }
  ExpressionSet set;
  std::vector<std::string> errors;
  set.Compile(inputs, defs, &errors);

  std::lock_guard<std::mutex> lock(mutex_);
  rois_ = rois;
  defs_ = defs;
  expressions_ = std::move(set);
  names_ = inputs;
  for (const auto& def : defs) names_.push_back(def.name);
  history_.clear();
  udp_dirty_ = true;  // Variable indices may have moved.
  return errors;
}

void LiveProcessor::SetUdp(const UdpConfig& config) {
  std::lock_guard<std::mutex> lock(mutex_);
  udp_ = config;
  udp_dirty_ = true;  // Resolved on the receiver thread at the next frame.
}

Frame LiveProcessor::Process(Frame frame) {
  auto result = std::make_shared<FrameResult>();
  result->receive_time = UnixTimeNow();

  std::lock_guard<std::mutex> lock(mutex_);
  frame.analysis["background"] = MeasureBackground(frame, rois_);
  result->frame = frame;  // Shares the unmodified raw payload.
  std::vector<double> inputs;
  for (const auto& roi : rois_) {
    if (!roi.enabled) continue;
    const RoiStats s = ComputeRoiStats(frame.header, frame.data->data(), roi);
    const auto v = StatValues(s);
    inputs.insert(inputs.end(), v.begin(), v.end());
  }
  const std::vector<double> derived = expressions_.Evaluate(inputs);
  result->values = inputs;
  result->values.insert(result->values.end(), derived.begin(), derived.end());

  history_.emplace_back(frame.header.timestamp, result->values);
  while (history_.size() > history_limit_) history_.pop_front();

  if (!udp_.channels.empty()) SendUdp(*result);

  if (record_target_ > 0) {
    recorded_.push_back(frame);
    if (recorded_.size() >= record_target_) {
      record_target_ = 0;
      auto done = std::move(record_done_);
      auto frames = std::move(recorded_);
      recorded_.clear();
      if (done) done(std::move(frames));
    }
  }

  latest_ = std::move(result);
  return frame;
}

void LiveProcessor::ResolveUdp() {
  udp_dirty_ = false;
  udp_error_.clear();
  udp_targets_.clear();
  for (std::size_t i = 0; i < udp_.channels.size(); ++i) {
    const UdpChannel& c = udp_.channels[i];
    if (!c.enabled) continue;
    const std::string row = "row " + std::to_string(i + 1) + ": ";
    UdpTarget t;
    t.channel = c;
    const auto it = std::find(names_.begin(), names_.end(), c.variable);
    if (it == names_.end()) {
      udp_error_ += row + "unknown variable '" + c.variable + "'\n";
      continue;
    }
    t.value_index = static_cast<int>(it - names_.begin());
    t.sender = std::make_unique<UdpSender>();
    std::string error;
    if (!t.sender->SetTarget(c.host.toStdString(), c.port, &error)) {
      udp_error_ += row + error + "\n";
      continue;
    }
    udp_targets_.push_back(std::move(t));
  }
  if (!udp_error_.empty()) udp_error_.pop_back();
}

void LiveProcessor::SendUdp(const FrameResult& r) {
  if (udp_dirty_) ResolveUdp();
  for (const auto& t : udp_targets_) {
    const auto i = static_cast<std::size_t>(t.value_index);
    if (i >= r.values.size()) continue;
    const double v = r.values[i];
    if (!std::isfinite(v)) continue;  // e.g. an empty ROI; nothing to plot.
    if (t.sender->Send(EncodeUdpValue(v, t.channel.binary))) ++udp_sent_;
  }
}

std::vector<std::string> LiveProcessor::Names() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return names_;
}

std::shared_ptr<const FrameResult> LiveProcessor::Latest() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return latest_;
}

void LiveProcessor::History(std::vector<double>* times,
                            std::vector<std::vector<double>>* values,
                            std::size_t max_points) const {
  std::lock_guard<std::mutex> lock(mutex_);
  const std::size_t n = std::min(max_points, history_.size());
  times->clear();
  values->clear();
  times->reserve(n);
  values->reserve(n);
  for (std::size_t i = history_.size() - n; i < history_.size(); ++i) {
    times->push_back(history_[i].first);
    values->push_back(history_[i].second);
  }
}

void LiveProcessor::ClearHistory() {
  std::lock_guard<std::mutex> lock(mutex_);
  history_.clear();
}

void LiveProcessor::StartRecording(
    std::size_t count, std::function<void(std::vector<Frame>)> done) {
  std::lock_guard<std::mutex> lock(mutex_);
  recorded_.clear();
  recorded_.reserve(count);
  record_target_ = count;
  record_done_ = std::move(done);
}

void LiveProcessor::CancelRecording() {
  std::lock_guard<std::mutex> lock(mutex_);
  record_target_ = 0;
  recorded_.clear();
  record_done_ = nullptr;
}

std::size_t LiveProcessor::RecordedCount() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return recorded_.size();
}

std::string LiveProcessor::UdpError() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return udp_error_;
}

std::uint64_t LiveProcessor::UdpPacketsSent() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return udp_sent_;
}

}  // namespace AndorEMCCDcontroller
