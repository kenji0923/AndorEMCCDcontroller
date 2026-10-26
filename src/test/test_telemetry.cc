/**
 * @file test_telemetry.cc
 * @brief Thermal watch, line protocol and the HTTP upload to Telegraf.
 */

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <gtest/gtest.h>
#include <zmq.hpp>

#include <AndorEMCCDcontroller/AcquisitionEngine.h>
#include <AndorEMCCDcontroller/SimCamera.h>
#include <AndorEMCCDcontroller/Telemetry.h>
#include <AndorEMCCDcontroller/ThermalWatch.h>

namespace aec = AndorEMCCDcontroller;

namespace {

aec::TemperatureReading Reading(double t, aec::TemperatureStatus status,
                                bool cooler_on = true) {
  aec::TemperatureReading r;
  r.temperature_c = t;
  r.status = status;
  r.cooler_on = cooler_on;
  return r;
}

/**
 * Minimal HTTP server on 127.0.0.1: stores each request and answers the
 * first @p fail_first with 503, then with @p status_line. After
 * Accept(false) connections wait unanswered in the backlog.
 */
class TestHttpServer {
 public:
  explicit TestHttpServer(std::string status_line = "204 No Content",
                          std::string reply_body = "", int fail_first = 0)
      : status_line_(std::move(status_line)),
        reply_body_(std::move(reply_body)),
        fail_first_(fail_first) {
    fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    ::bind(fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
    ::listen(fd_, 16);
    socklen_t length = sizeof(addr);
    ::getsockname(fd_, reinterpret_cast<sockaddr*>(&addr), &length);
    port_ = ntohs(addr.sin_port);
    thread_ = std::thread([this] { Run(); });
  }

  ~TestHttpServer() {
    stop_ = true;
    thread_.join();
    ::close(fd_);
  }

  std::string Url(const std::string& path = "/telegraf") const {
    return "http://127.0.0.1:" + std::to_string(port_) + path;
  }
  void Accept(bool on) { accepting_ = on; }

  std::vector<std::string> Requests() {
    std::lock_guard<std::mutex> lock(mutex_);
    return requests_;
  }

  /** Waits until @p n requests arrived; false on timeout. */
  bool WaitFor(std::size_t n, double timeout_s = 5.0) {
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::duration<double>(timeout_s);
    while (std::chrono::steady_clock::now() < deadline) {
      if (Requests().size() >= n) return true;
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return false;
  }

 private:
  void Run() {
    while (!stop_) {
      pollfd p{fd_, POLLIN, 0};
      if (!accepting_ || ::poll(&p, 1, 20) <= 0) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
        continue;
      }
      const int client = ::accept(fd_, nullptr, nullptr);
      if (client < 0) continue;
      std::string request;
      char buffer[4096];
      std::size_t expected = std::string::npos;
      while (request.size() < expected) {
        const ssize_t n = ::recv(client, buffer, sizeof(buffer), 0);
        if (n <= 0) break;
        request.append(buffer, static_cast<std::size_t>(n));
        const std::size_t end = request.find("\r\n\r\n");
        if (end != std::string::npos && expected == std::string::npos) {
          const std::size_t cl = request.find("Content-Length: ");
          expected = end + 4 +
                     (cl == std::string::npos
                          ? 0
                          : std::strtoul(request.c_str() + cl + 16, nullptr,
                                         10));
        }
      }
      std::size_t n_requests;
      {
        std::lock_guard<std::mutex> lock(mutex_);
        requests_.push_back(request);
        n_requests = requests_.size();
      }
      const bool fail = static_cast<int>(n_requests) <= fail_first_;
      const std::string body = fail ? "" : reply_body_;
      const std::string reply =
          "HTTP/1.1 " + (fail ? "503 Service Unavailable" : status_line_) +
          "\r\nContent-Length: " + std::to_string(body.size()) +
          "\r\nConnection: close\r\n\r\n" + body;
      ::send(client, reply.data(), reply.size(), MSG_NOSIGNAL);
      ::close(client);
    }
  }

  std::string status_line_;
  std::string reply_body_;
  int fail_first_ = 0;
  int fd_ = -1;
  int port_ = 0;
  std::atomic<bool> stop_{false};
  std::atomic<bool> accepting_{true};
  std::mutex mutex_;
  std::vector<std::string> requests_;
  std::thread thread_;
};

std::string Body(const std::string& request) {
  return request.substr(request.find("\r\n\r\n") + 4);
}

std::size_t CountLines(const std::string& text) {
  std::size_t n = 0;
  for (const char c : text) n += c == '\n';
  return n;
}

}  // namespace

TEST(ThermalWatch, CoolDownIsQuietAndCoolingLossAlarms) {
  aec::ThermalWatch watch(5.0);
  using S = aec::TemperatureStatus;
  // Cooling down from room temperature: far above the set point, no alarm.
  EXPECT_FALSE(watch.Update(Reading(20.0, S::kNotReached), -80));
  EXPECT_FALSE(watch.Update(Reading(-40.0, S::kNotReached), -80));
  EXPECT_EQ(watch.Level(), aec::ThermalAlarm::kOk);
  // Reached (overshoot below the set point counts), then stabilized.
  watch.Update(Reading(-87.7, S::kNotStabilized), -80);
  watch.Update(Reading(-79.3, S::kStabilized), -80);
  EXPECT_EQ(watch.Level(), aec::ThermalAlarm::kOk);
  // The SDK reports a drift first: warning.
  EXPECT_TRUE(watch.Update(Reading(-77.0, S::kDrift), -80));
  EXPECT_EQ(watch.Level(), aec::ThermalAlarm::kWarning);
  // More than 5 C above: cooling lost. Logged once, counted once.
  EXPECT_TRUE(watch.Update(Reading(-74.5, S::kDrift), -80));
  EXPECT_EQ(watch.Level(), aec::ThermalAlarm::kAlarm);
  EXPECT_NE(watch.Reason().find("cooling lost"), std::string::npos);
  EXPECT_FALSE(watch.Update(Reading(-60.0, S::kDrift), -80));
  EXPECT_NE(watch.Reason().find("-60.0 C"), std::string::npos);
  EXPECT_EQ(watch.AlarmCount(), 1u);
  // Recovered; a second episode is counted again.
  EXPECT_TRUE(watch.Update(Reading(-79.5, S::kStabilized), -80));
  EXPECT_EQ(watch.Level(), aec::ThermalAlarm::kOk);
  EXPECT_TRUE(watch.Reason().empty());
  watch.Update(Reading(-70.0, S::kDrift), -80);
  EXPECT_EQ(watch.AlarmCount(), 2u);
}

TEST(ThermalWatch, SetPointChangesAndCoolerOffDoNotAlarm) {
  aec::ThermalWatch watch(5.0);
  using S = aec::TemperatureStatus;
  watch.Update(Reading(-80.0, S::kStabilized), -80);
  // Raising the set point: the sensor warms on purpose.
  watch.Update(Reading(-79.0, S::kNotReached), -40);
  watch.Update(Reading(-45.0, S::kNotReached), -40);
  EXPECT_EQ(watch.Level(), aec::ThermalAlarm::kOk);
  // Lowering it from a warm sensor: not reached yet, so no alarm.
  watch.Update(Reading(-40.0, S::kNotReached), -80);
  EXPECT_EQ(watch.Level(), aec::ThermalAlarm::kOk);
  // Cooler switched off: warming is expected.
  watch.Update(Reading(-80.0, S::kStabilized), -80);
  watch.Update(Reading(-60.0, S::kOff, false), -80);
  EXPECT_EQ(watch.Level(), aec::ThermalAlarm::kOk);
  EXPECT_EQ(watch.AlarmCount(), 0u);
}

TEST(ThermalWatch, UnreadableAndTecTrip) {
  aec::ThermalWatch watch;
  auto r = Reading(-80.0, aec::TemperatureStatus::kUnknown);
  r.error = "GetTemperatureF: DRV_ERROR_ACK";
  EXPECT_TRUE(watch.Update(r, -80));
  EXPECT_EQ(watch.Level(), aec::ThermalAlarm::kWarning);
  EXPECT_NE(watch.Reason().find("DRV_ERROR_ACK"), std::string::npos);
  r = Reading(-80.0, aec::TemperatureStatus::kStabilized);
  r.tec_overheat = 1;
  EXPECT_TRUE(watch.Update(r, -80));
  EXPECT_EQ(watch.Level(), aec::ThermalAlarm::kAlarm);
  EXPECT_EQ(watch.AlarmCount(), 1u);
}

TEST(Telemetry, LineProtocolEscapingAndTypes) {
  const std::string line = aec::FormatLine(
      "andor emccd,x",
      {{"host", "lab pc"}, {"tlg_bucket", "laserlab"}, {"note", "a=b,c"},
       {"empty", ""}},
      {{"sensor_temperature_c", -79.247, false},
       {"state", 1.0, true},
       {"weird key", 0.5, false}},
      1792789230052442368LL);
  EXPECT_EQ(line,
            "andor\\ emccd\\,x,host=lab\\ pc,tlg_bucket=laserlab,"
            "note=a\\=b\\,c sensor_temperature_c=-79.247,state=1i,"
            "weird\\ key=0.5 1792789230052442368");
}

TEST(Telemetry, FieldsLeaveOutWhatIsNotKnown) {
  aec::CameraStatus s;
  s.state = aec::CameraState::kDisconnected;
  s.temperature_c = -79.0;  // Stale.
  auto fields = aec::TelemetryFields(s);
  ASSERT_EQ(fields.size(), 1u);
  EXPECT_EQ(fields[0].name, "state");
  EXPECT_EQ(fields[0].value, 3.0);

  s.state = aec::CameraState::kIdle;
  s.temperature_status = aec::TemperatureStatus::kUnknown;
  s.thermal_alarm = aec::ThermalAlarm::kWarning;
  fields = aec::TelemetryFields(s);
  auto has = [&fields](const std::string& name) {
    for (const auto& f : fields) {
      if (f.name == name) return true;
    }
    return false;
  };
  EXPECT_FALSE(has("sensor_temperature_c"));
  EXPECT_FALSE(has("ambient_temperature_c"));  // NaN: not provided.
  EXPECT_FALSE(has("tec_overheat"));           // -1: not supported.
  EXPECT_TRUE(has("temperature_status"));
  EXPECT_TRUE(has("thermal_alarm"));

  s.temperature_status = aec::TemperatureStatus::kStabilized;
  s.ambient_temperature_c = 31.5;
  s.cooler_voltage_v = 6.2;
  fields = aec::TelemetryFields(s);
  EXPECT_TRUE(has("sensor_temperature_c"));
  EXPECT_TRUE(has("ambient_temperature_c"));
  EXPECT_TRUE(has("cooler_voltage_v"));
}

TEST(Telemetry, ParseHttpUrl) {
  aec::HttpUrl u;
  std::string error;
  ASSERT_TRUE(
      aec::ParseHttpUrl("http://192.168.1.5:8080/telegraf", &u, &error));
  EXPECT_EQ(u.host, "192.168.1.5");
  EXPECT_EQ(u.port, "8080");
  EXPECT_EQ(u.path, "/telegraf");
  EXPECT_EQ(u.authority, "192.168.1.5:8080");
  ASSERT_TRUE(aec::ParseHttpUrl("http://telegraf.lan", &u, &error));
  EXPECT_EQ(u.port, "80");
  EXPECT_EQ(u.path, "/");
  ASSERT_TRUE(aec::ParseHttpUrl("http://[::1]:8186/write?db=x", &u, &error));
  EXPECT_EQ(u.host, "::1");
  EXPECT_EQ(u.port, "8186");
  EXPECT_EQ(u.path, "/write?db=x");
  EXPECT_FALSE(aec::ParseHttpUrl("https://192.168.1.5/telegraf", &u, &error));
  EXPECT_FALSE(aec::ParseHttpUrl("http://:8080/x", &u, &error));
}

TEST(Telemetry, HttpPostSendsTheBodyAndReportsErrors) {
  TestHttpServer ok;
  EXPECT_EQ(aec::HttpPost(ok.Url(), "m f=1i 1\n", 2.0), "");
  ASSERT_TRUE(ok.WaitFor(1));
  const std::string request = ok.Requests()[0];
  EXPECT_EQ(request.rfind("POST /telegraf HTTP/1.1\r\n", 0), 0u);
  EXPECT_NE(request.find("Content-Length: 9\r\n"), std::string::npos);
  EXPECT_EQ(Body(request), "m f=1i 1\n");

  TestHttpServer bad("400 Bad Request", "{\"error\":\"metric parse error\"}");
  const std::string error = aec::HttpPost(bad.Url(), "bad line\n", 2.0);
  EXPECT_NE(error.find("400"), std::string::npos) << error;
  EXPECT_NE(error.find("metric parse error"), std::string::npos) << error;

  // Nothing listens on a port that was just released.
  std::string closed_url;
  {
    TestHttpServer gone;
    closed_url = gone.Url();
  }
  EXPECT_FALSE(aec::HttpPost(closed_url, "x f=1 1\n", 1.0).empty());

  // A server that never answers costs at most the timeout.
  TestHttpServer silent;
  silent.Accept(false);
  const auto start = std::chrono::steady_clock::now();
  EXPECT_NE(aec::HttpPost(silent.Url(), "x f=1 1\n", 0.3), "");
  EXPECT_LT(std::chrono::steady_clock::now() - start,
            std::chrono::milliseconds(1500));
}

TEST(Telemetry, PublisherKeepsPointsUntilDelivered) {
  TestHttpServer server("204 No Content", "", 3);  // Three 503s first.
  aec::TelemetryConfig config;
  config.url = server.Url();
  config.interval_s = 0.05;
  config.tags = {{"tlg_bucket", "laserlab"}, {"device", "emccd"}};
  {
    aec::TelemetryPublisher publisher(config);
    aec::CameraStatus s;
    s.state = aec::CameraState::kIdle;
    s.temperature_status = aec::TemperatureStatus::kStabilized;
    for (int i = 0; i < 5; ++i) {
      s.timestamp = 1000.0 + i;
      s.temperature_c = -80.0 + i;
      publisher.Offer(s);
      ASSERT_TRUE(server.WaitFor(i + 1)) << i;
      std::this_thread::sleep_for(std::chrono::milliseconds(80));
    }
  }
  const auto requests = server.Requests();
  ASSERT_EQ(requests.size(), 5u);
  // Each failed POST is retried with the next point; the first accepted
  // one carries all four, the next only the new point.
  EXPECT_EQ(CountLines(Body(requests[2])), 3u);
  const std::string delivered = Body(requests[3]);
  EXPECT_EQ(CountLines(delivered), 4u) << delivered;
  for (int i = 0; i < 4; ++i) {
    EXPECT_NE(delivered.find(" " + std::to_string(1000 + i) + "000000000\n"),
              std::string::npos)
        << i << ": " << delivered;
  }
  EXPECT_EQ(CountLines(Body(requests[4])), 1u);
  EXPECT_NE(Body(requests[4]).find(" 1004000000000\n"), std::string::npos);
  EXPECT_EQ(delivered.rfind("andor_emccd,host=", 0), 0u) << delivered;
  EXPECT_NE(delivered.find(",tlg_bucket=laserlab,device=emccd "),
            std::string::npos);
  EXPECT_NE(delivered.find("sensor_temperature_c=-80,"), std::string::npos);
}

TEST(Engine, StatusAndTelemetryCarryThermalReadouts) {
  TestHttpServer server;
  aec::TelemetryConfig tc;
  tc.url = server.Url();
  tc.interval_s = 0.05;
  tc.tags = {{"tlg_bucket", "laserlab"}};
  aec::TelemetryPublisher telemetry(tc);

  zmq::context_t context(1);
  aec::EngineConfig config;
  config.data_endpoints = {"inproc://test-telemetry"};
  config.warmup_on_exit = false;
  config.status_interval_s = 0.02;
  config.telemetry = &telemetry;
  auto camera = std::make_unique<aec::SimCamera>();
  aec::SimCamera* sim = camera.get();
  aec::AcquisitionEngine engine(std::move(camera), &context, config);
  std::string error;
  ASSERT_TRUE(engine.BindSockets(&error)) << error;
  ASSERT_TRUE(engine.Start(&error)) << error;
  engine.SetSettings({{"cooler_on", true}, {"target_temperature_c", -60}});
  std::this_thread::sleep_for(std::chrono::milliseconds(200));

  aec::CameraStatus s = engine.Status();
  EXPECT_EQ(s.target_temperature_c, -60);
  EXPECT_GT(s.cooler_voltage_v, 0.0);
  EXPECT_FALSE(std::isnan(s.ambient_temperature_c));
  const aec::Json j = s;
  EXPECT_EQ(j["thermal_alarm"], "ok");
  EXPECT_TRUE(j["tec_overheat"].is_null());
  EXPECT_EQ(j["fan_mode"], "full");

  // A heat-sink trip cuts the cooler: no drive voltage, hot head.
  sim->SimulateHeatSinkTrip(true);
  std::this_thread::sleep_for(std::chrono::milliseconds(200));
  s = engine.Status();
  EXPECT_EQ(s.cooler_voltage_v, 0.0);
  EXPECT_TRUE(s.cooler_on);  // The SDK still reports the cooler as on.
  engine.Stop();

  ASSERT_TRUE(server.WaitFor(2));
  const std::string body = Body(server.Requests().back());
  EXPECT_NE(body.find("andor_emccd,host="), std::string::npos) << body;
  EXPECT_NE(body.find("tlg_bucket=laserlab"), std::string::npos) << body;
  for (const char* field :
       {"sensor_temperature_c=", "target_temperature_c=-60i", "cooler_on=1i",
        "ambient_temperature_c=", "cooler_voltage_v=", "thermal_alarm=0i",
        "state="}) {
    EXPECT_NE(body.find(field), std::string::npos) << field << ": " << body;
  }
}
