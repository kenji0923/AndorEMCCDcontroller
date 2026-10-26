/**
 * @file Telemetry.h
 * @brief Upload of the camera status to Telegraf, for records and alerts.
 *
 * Points are InfluxDB line protocol sent by HTTP POST (Telegraf
 * http_listener_v2 with data_format "influx"). When Telegraf writes them to
 * VictoriaMetrics, field f of measurement m becomes the metric m_f and the
 * tags become labels.
 */

#ifndef AndorEMCCDcontroller__Telemetry_h
#define AndorEMCCDcontroller__Telemetry_h

#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <AndorEMCCDcontroller/CameraSettings.h>

namespace AndorEMCCDcontroller {

using TelemetryTags = std::vector<std::pair<std::string, std::string>>;

struct TelemetryConfig {
  std::string url;  ///< "http://host:port/path"; empty disables telemetry.
  double interval_s = 5.0;
  double timeout_s = 3.0;  ///< Per POST.
  std::string measurement = "andor_emccd";
  /** Tags of every point, after host=<hostname> (e.g. tlg_bucket). */
  TelemetryTags tags;
  /** Points kept while Telegraf is unreachable (6 h at 5 s). */
  std::size_t max_queued_points = 4320;
};

/** One field; integers are written with the line protocol "i" suffix. */
struct TelemetryField {
  std::string name;
  double value = 0.0;
  bool integer = false;
};

/**
 * @brief Numeric fields for @p s.
 *
 * Enumerations are sent as their codes. Readouts the camera does not
 * provide, an unreadable sensor temperature and, while the camera is
 * disconnected, everything but the state are left out rather than sent
 * stale.
 */
std::vector<TelemetryField> TelemetryFields(const CameraStatus& s);

/** One line of InfluxDB line protocol (escaped); timestamp in ns. */
std::string FormatLine(const std::string& measurement,
                       const TelemetryTags& tags,
                       const std::vector<TelemetryField>& fields,
                       std::int64_t timestamp_ns);

struct HttpUrl {
  std::string host;
  std::string port = "80";
  std::string path = "/";
  std::string authority;  ///< "host[:port]" as written, for the Host header.
};

/** Splits "http://host[:port][/path]"; false with @p error otherwise. */
bool ParseHttpUrl(const std::string& url, HttpUrl* out, std::string* error);

/**
 * @brief POSTs @p body (text/plain) to an http:// URL.
 *
 * @return "" on a 2xx reply, otherwise what went wrong.
 */
std::string HttpPost(const std::string& url, const std::string& body,
                     double timeout_s);

/**
 * @brief Sends a status point every interval from its own thread.
 *
 * Offer() is cheap and thread-safe; it keeps a point when the interval has
 * passed since the previous one. Points that cannot be delivered stay
 * queued, with their timestamps, and go with the next POST.
 */
class TelemetryPublisher {
 public:
  explicit TelemetryPublisher(TelemetryConfig config);
  ~TelemetryPublisher();

  TelemetryPublisher(const TelemetryPublisher&) = delete;
  TelemetryPublisher& operator=(const TelemetryPublisher&) = delete;

  void Offer(const CameraStatus& s);

 private:
  void ThreadMain();
  /** Posts the oldest queued points; call with the lock held. */
  bool SendBatch(std::unique_lock<std::mutex>& lock);

  TelemetryConfig config_;
  std::mutex mutex_;
  std::condition_variable cv_;
  std::deque<std::pair<std::uint64_t, std::string>> queue_;  ///< (seq, line)
  std::uint64_t next_seq_ = 1;
  double next_point_ = 0.0;  ///< Steady-clock time the next point is due.
  bool stop_ = false;
  bool failing_ = false;
  double next_failure_log_ = 0.0;
  std::uint64_t dropped_ = 0;
  std::thread thread_;
};

}  // namespace AndorEMCCDcontroller

#endif
