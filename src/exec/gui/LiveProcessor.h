/**
 * @file LiveProcessor.h
 * @brief Per-frame ROI statistics, formulas, UDP output and recording.
 *
 * Process() runs on the frame receiver thread for every frame, so analysis
 * and UDP output never skip frames even when the display does. The GUI
 * thread reads the latest result and the history with the accessors.
 */

#ifndef AndorEMCCDcontroller__gui_LiveProcessor_h
#define AndorEMCCDcontroller__gui_LiveProcessor_h

#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include <QString>

#include <AndorEMCCDcontroller/Analysis.h>
#include <AndorEMCCDcontroller/Expression.h>
#include <AndorEMCCDcontroller/Protocol.h>

namespace AndorEMCCDcontroller {

class UdpSender;

/** Analysis result of one frame. */
struct FrameResult {
  Frame frame;
  std::vector<double> values;  ///< Same order as LiveProcessor::Names().
  double receive_time = 0.0;
};

/**
 * @brief One UDP output: a single variable to one host:port.
 *
 * Each datagram carries one number, the format that qtconsole
 * (github.com/kenji0923/qtconsole) and similar single-value monitors read.
 */
struct UdpChannel {
  bool enabled = true;
  std::string variable;  ///< ROI statistic ("roi1_sum") or formula name.
  QString host = QStringLiteral("127.0.0.1");
  quint16 port = 9000;
  bool binary = false;  ///< 8-byte little-endian double instead of text.
};

struct UdpConfig {
  std::vector<UdpChannel> channels;
};

/** Datagram payload for one value: shortest round-trip text or LE double. */
std::string EncodeUdpValue(double value, bool binary);

class LiveProcessor {
 public:
  LiveProcessor();
  ~LiveProcessor();

  /** Replaces ROIs and formulas; returns per-formula compile errors. */
  std::vector<std::string> Configure(const std::vector<Roi>& rois,
                                     const std::vector<ExpressionDef>& defs);
  void SetUdp(const UdpConfig& config);

  /** Called on the receiver thread for every frame. */
  void Process(Frame frame);

  /** Variable names: "<roi>_<stat>" for enabled ROIs, then formula names. */
  std::vector<std::string> Names() const;
  std::shared_ptr<const FrameResult> Latest() const;

  /** Copies the last @p max_points (time, values) of the history. */
  void History(std::vector<double>* times,
               std::vector<std::vector<double>>* values,
               std::size_t max_points) const;
  void ClearHistory();

  /** Collects the next @p count frames; @p done runs on the receiver thread. */
  void StartRecording(std::size_t count,
                      std::function<void(std::vector<Frame>)> done);
  void CancelRecording();
  std::size_t RecordedCount() const;

  std::uint64_t UdpPacketsSent() const;
  std::string UdpError() const;

 private:
  struct UdpTarget {
    UdpChannel channel;
    int value_index = -1;  ///< Index into FrameResult::values, -1 if unknown.
    std::unique_ptr<UdpSender> sender;
  };

  void ResolveUdp();
  void SendUdp(const FrameResult& r);

  mutable std::mutex mutex_;
  std::vector<Roi> rois_;
  std::vector<ExpressionDef> defs_;
  ExpressionSet expressions_;
  std::vector<std::string> names_;
  std::shared_ptr<const FrameResult> latest_;
  std::deque<std::pair<double, std::vector<double>>> history_;
  std::size_t history_limit_ = 20000;

  UdpConfig udp_;
  bool udp_dirty_ = true;
  std::vector<UdpTarget> udp_targets_;  ///< Used on the receiver thread.
  std::string udp_error_;
  std::uint64_t udp_sent_ = 0;

  std::size_t record_target_ = 0;
  std::vector<Frame> recorded_;
  std::function<void(std::vector<Frame>)> record_done_;
};

}  // namespace AndorEMCCDcontroller

#endif
