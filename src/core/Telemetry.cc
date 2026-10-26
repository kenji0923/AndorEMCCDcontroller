/**
 * @file Telemetry.cc
 * @brief Implementation of AndorEMCCDcontroller::Telemetry.
 */

#include <AndorEMCCDcontroller/Telemetry.h>

#include <algorithm>
#include <cerrno>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <memory>

#include <netdb.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#include <spdlog/spdlog.h>

namespace AndorEMCCDcontroller {

namespace {

// A long outage is logged again after this long, not on every attempt.
constexpr double kFailureLogPeriodS = 600.0;
constexpr std::size_t kMaxBatchPoints = 1000;
constexpr std::size_t kMaxReplyBytes = 4096;

double SteadyNow() {
  using namespace std::chrono;
  return duration<double>(steady_clock::now().time_since_epoch()).count();
}

/** Backslash-escapes the characters in @p special (line protocol). */
std::string Escape(const std::string& s, const char* special) {
  std::string out;
  for (const char c : s) {
    if (c != '\0' && std::strchr(special, c)) out += '\\';
    out += c;
  }
  return out;
}

/** Closes a socket on scope exit. */
class Socket {
 public:
  explicit Socket(int fd) : fd_(fd) {}
  ~Socket() {
    if (fd_ >= 0) ::close(fd_);
  }
  Socket(const Socket&) = delete;
  Socket& operator=(const Socket&) = delete;
  int fd() const { return fd_; }

 private:
  int fd_;
};

/** Waits for @p events on @p fd until @p deadline; false on timeout. */
bool WaitFor(int fd, short events, double deadline) {
  for (;;) {
    const int ms =
        static_cast<int>(std::ceil((deadline - SteadyNow()) * 1e3));
    if (ms <= 0) return false;
    pollfd p{fd, events, 0};
    const int rc = ::poll(&p, 1, ms);
    if (rc > 0) return true;
    if (rc == 0 || errno != EINTR) return false;
  }
}

}  // namespace

std::vector<TelemetryField> TelemetryFields(const CameraStatus& s) {
  std::vector<TelemetryField> f;
  const auto integer = [&f](const char* name, double v) {
    f.push_back({name, v, true});
  };
  const auto real = [&f](const char* name, double v) {
    if (std::isfinite(v)) f.push_back({name, v, false});
  };
  integer("state", static_cast<int>(s.state));
  if (s.state == CameraState::kDisconnected) return f;
  if (s.temperature_status != TemperatureStatus::kUnknown) {
    real("sensor_temperature_c", s.temperature_c);
  }
  integer("temperature_status", static_cast<int>(s.temperature_status));
  integer("target_temperature_c", s.target_temperature_c);
  integer("cooler_on", s.cooler_on ? 1 : 0);
  integer("fan_mode", static_cast<int>(s.fan_mode));
  real("ambient_temperature_c", s.ambient_temperature_c);
  real("cooler_voltage_v", s.cooler_voltage_v);
  if (s.tec_overheat >= 0) integer("tec_overheat", s.tec_overheat);
  integer("thermal_alarm", static_cast<int>(s.thermal_alarm));
  integer("thermal_alarm_count", static_cast<double>(s.thermal_alarm_count));
  integer("em_gain", s.em_gain);
  real("fps", s.fps);
  return f;
}

std::string FormatLine(const std::string& measurement,
                       const TelemetryTags& tags,
                       const std::vector<TelemetryField>& fields,
                       std::int64_t timestamp_ns) {
  std::string line = Escape(measurement, ", ");
  for (const auto& [key, value] : tags) {
    if (key.empty() || value.empty()) continue;  // Not allowed by the format.
    line += ',' + Escape(key, ",= ") + '=' + Escape(value, ",= ");
  }
  char separator = ' ';
  for (const auto& field : fields) {
    line += separator;
    separator = ',';
    line += Escape(field.name, ",= ") + '=';
    if (field.integer) {
      line += std::to_string(std::llround(field.value)) + 'i';
    } else {
      // Shortest text that reads back as the same double.
      char buffer[32];
      const auto r =
          std::to_chars(buffer, buffer + sizeof(buffer), field.value);
      line.append(buffer, r.ptr);
    }
  }
  line += ' ' + std::to_string(timestamp_ns);
  return line;
}

bool ParseHttpUrl(const std::string& url, HttpUrl* out, std::string* error) {
  const std::string scheme = "http://";
  if (url.compare(0, scheme.size(), scheme) != 0) {
    *error = "only http:// URLs are supported: " + url;
    return false;
  }
  const std::string rest = url.substr(scheme.size());
  const std::size_t slash = rest.find('/');
  HttpUrl u;
  u.authority = rest.substr(0, slash);
  if (slash != std::string::npos) u.path = rest.substr(slash);
  u.host = u.authority;
  if (!u.host.empty() && u.host.front() == '[') {  // [IPv6]:port
    const std::size_t close = u.host.find(']');
    if (close == std::string::npos) {
      *error = "bad host in " + url;
      return false;
    }
    if (close + 1 < u.host.size() && u.host[close + 1] == ':') {
      u.port = u.host.substr(close + 2);
    }
    u.host = u.host.substr(1, close - 1);
  } else if (const std::size_t colon = u.host.rfind(':');
             colon != std::string::npos) {
    u.port = u.host.substr(colon + 1);
    u.host = u.host.substr(0, colon);
  }
  if (u.host.empty() || u.port.empty()) {
    *error = "no host or port in " + url;
    return false;
  }
  *out = u;
  return true;
}

std::string HttpPost(const std::string& url, const std::string& body,
                     double timeout_s) {
  HttpUrl u;
  std::string error;
  if (!ParseHttpUrl(url, &u, &error)) return error;
  const double deadline = SteadyNow() + timeout_s;

  addrinfo hints{};
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_STREAM;
  addrinfo* found = nullptr;
  const int gai = ::getaddrinfo(u.host.c_str(), u.port.c_str(), &hints, &found);
  if (gai != 0) return "resolve " + u.host + ": " + ::gai_strerror(gai);
  const std::unique_ptr<addrinfo, void (*)(addrinfo*)> addresses(
      found, ::freeaddrinfo);

  const std::string request =
      "POST " + u.path + " HTTP/1.1\r\nHost: " + u.authority +
      "\r\nUser-Agent: andor_emccd_backend/" AEC_VERSION
      "\r\nContent-Type: text/plain; charset=utf-8\r\nContent-Length: " +
      std::to_string(body.size()) + "\r\nConnection: close\r\n\r\n" + body;

  for (const addrinfo* a = found; a; a = a->ai_next) {
    const Socket socket(
        ::socket(a->ai_family, a->ai_socktype | SOCK_NONBLOCK | SOCK_CLOEXEC,
                 a->ai_protocol));
    const int fd = socket.fd();
    if (fd < 0) {
      error = std::string("socket: ") + std::strerror(errno);
      continue;
    }
    if (::connect(fd, a->ai_addr, a->ai_addrlen) != 0) {
      if (errno != EINPROGRESS) {
        error = "connect " + u.authority + ": " + std::strerror(errno);
        continue;
      }
      if (!WaitFor(fd, POLLOUT, deadline)) {
        error = "connect " + u.authority + ": timed out";
        continue;
      }
      int so_error = 0;
      socklen_t length = sizeof(so_error);
      ::getsockopt(fd, SOL_SOCKET, SO_ERROR, &so_error, &length);
      if (so_error != 0) {
        error = "connect " + u.authority + ": " + std::strerror(so_error);
        continue;
      }
    }

    for (std::size_t sent = 0; sent < request.size();) {
      const ssize_t n = ::send(fd, request.data() + sent,
                               request.size() - sent, MSG_NOSIGNAL);
      if (n > 0) {
        sent += static_cast<std::size_t>(n);
      } else if (n < 0 && errno == EINTR) {
        continue;
      } else if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
        if (!WaitFor(fd, POLLOUT, deadline)) {
          return "send to " + u.authority + ": timed out";
        }
      } else {
        return "send to " + u.authority + ": " + std::strerror(errno);
      }
    }

    // Read the status line; for an error keep some of the body (Telegraf
    // explains parse errors there). "Connection: close" ends the reply.
    std::string reply;
    int code = 0;
    while (reply.size() < kMaxReplyBytes) {
      char buffer[1024];
      const ssize_t n = ::recv(fd, buffer, sizeof(buffer), 0);
      if (n > 0) {
        reply.append(buffer, static_cast<std::size_t>(n));
        const std::size_t space = reply.find(' ');
        if (code == 0 && reply.find("\r\n") != std::string::npos &&
            space != std::string::npos) {
          code = std::atoi(reply.c_str() + space + 1);
        }
        if (code >= 200 && code < 300) break;
      } else if (n == 0) {
        break;
      } else if (errno == EINTR) {
        continue;
      } else if ((errno == EAGAIN || errno == EWOULDBLOCK) &&
                 WaitFor(fd, POLLIN, deadline)) {
        continue;
      } else {
        break;
      }
    }
    if (reply.compare(0, 5, "HTTP/") != 0 || code == 0) {
      return reply.empty() ? "no reply from " + u.authority
                           : "unexpected reply from " + u.authority;
    }
    if (code >= 200 && code < 300) return "";
    std::string status_line = reply.substr(0, reply.find("\r\n"));
    const std::size_t body_start = reply.find("\r\n\r\n");
    std::string detail = body_start == std::string::npos
                             ? std::string()
                             : reply.substr(body_start + 4, 200);
    for (char& c : detail) {
      if (c == '\r' || c == '\n') c = ' ';
    }
    return u.authority + " replied " + status_line +
           (detail.empty() ? "" : ": " + detail);
  }
  return error;
}

TelemetryPublisher::TelemetryPublisher(TelemetryConfig config)
    : config_(std::move(config)) {
  const bool has_host =
      std::any_of(config_.tags.begin(), config_.tags.end(),
                  [](const auto& tag) { return tag.first == "host"; });
  char host[256] = {0};
  if (!has_host && ::gethostname(host, sizeof(host) - 1) == 0 && host[0]) {
    config_.tags.insert(config_.tags.begin(), {"host", host});
  }
  thread_ = std::thread(&TelemetryPublisher::ThreadMain, this);
}

TelemetryPublisher::~TelemetryPublisher() {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    stop_ = true;
  }
  cv_.notify_all();
  thread_.join();
}

void TelemetryPublisher::Offer(const CameraStatus& s) {
  const double now = SteadyNow();
  std::lock_guard<std::mutex> lock(mutex_);
  if (stop_ || now < next_point_) return;
  // Keep the cadence; after a pause start again from now.
  next_point_ = now - next_point_ < config_.interval_s
                    ? next_point_ + config_.interval_s
                    : now + config_.interval_s;
  const double timestamp = s.timestamp > 0.0 ? s.timestamp : UnixTimeNow();
  queue_.emplace_back(
      next_seq_++,
      FormatLine(config_.measurement, config_.tags, TelemetryFields(s),
                 std::llround(timestamp * 1e9)));
  if (queue_.size() > config_.max_queued_points) {
    queue_.pop_front();
    ++dropped_;
  }
  cv_.notify_all();
}

void TelemetryPublisher::ThreadMain() {
  std::unique_lock<std::mutex> lock(mutex_);
  // After a failed POST, wait for the next point before trying again.
  std::uint64_t retry_after_seq = 0;
  while (!stop_) {
    cv_.wait(lock, [&] {
      return stop_ || (!queue_.empty() && next_seq_ > retry_after_seq);
    });
    if (stop_) break;
    retry_after_seq = SendBatch(lock) ? 0 : next_seq_;
  }
  // Last chance for points held back by an outage.
  if (!queue_.empty()) SendBatch(lock);
}

bool TelemetryPublisher::SendBatch(std::unique_lock<std::mutex>& lock) {
  std::string body;
  std::uint64_t last_seq = 0;
  std::size_t count = 0;
  for (const auto& [seq, line] : queue_) {
    if (count == kMaxBatchPoints) break;
    body += line;
    body += '\n';
    last_seq = seq;
    ++count;
  }
  lock.unlock();
  const std::string error = HttpPost(config_.url, body, config_.timeout_s);
  lock.lock();
  if (error.empty()) {
    // Offer() may have dropped some of them meanwhile: go by sequence.
    while (!queue_.empty() && queue_.front().first <= last_seq) {
      queue_.pop_front();
    }
    if (failing_) {
      spdlog::info("telemetry: delivering to {} again", config_.url);
      failing_ = false;
    }
    return true;
  }
  const double now = SteadyNow();
  if (!failing_ || now >= next_failure_log_) {
    spdlog::warn("telemetry: {} ({} point(s) queued, {} dropped)", error,
                 queue_.size(), dropped_);
    next_failure_log_ = now + kFailureLogPeriodS;
  }
  failing_ = true;
  return false;
}

}  // namespace AndorEMCCDcontroller
