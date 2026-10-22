/** @file test_stream_client.cc
 * Cross-platform client integration against an isolated simulated backend.
 * AEC_STREAM_CONTROL=tcp://localhost:47400 ctest -R StreamClient
 */
#include <AndorEMCCDcontroller/CameraClient.h>
#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <mutex>
#include <thread>

namespace aec = AndorEMCCDcontroller;
namespace {
using namespace std::chrono_literals;

aec::Json Settings() {
  return {{"trigger_mode", "internal"},
          {"exposure_time_s", 0.01},
          {"kinetic_cycle_time_s", 0.02},
          {"hbin", 16},
          {"vbin", 16}};
}

struct ReleaseControl {
  aec::ControlClient* client;
  ~ReleaseControl() {
    try {
      client->Request("release_control");
    } catch (...) {
    }
  }
};
}  // namespace

TEST(StreamClient, IndependentCursorsAndRetry) {
  const char* endpoint = std::getenv("AEC_STREAM_CONTROL");
  if (!endpoint)
    GTEST_SKIP() << "Set AEC_STREAM_CONTROL to isolated sim backend";
  zmq::context_t context(1);
  aec::ControlClient owner(&context, endpoint);
  ASSERT_TRUE(owner.Request("ping")["simulated"].get<bool>());
  owner.Request("acquire_control", {{"force", true}});
  ReleaseControl release{&owner};
  aec::ControlClient fast(&context, endpoint), slow(&context, endpoint);
  fast.Request("stream_open");
  slow.Request("stream_open");
  auto acquire = [&] {
    auto run =
        owner.Request("arm", {{"n_frames", 3}, {"settings", Settings()}});
    owner.Request("wait", {{"run_id", run["run_id"]}});
  };
  acquire();
  std::vector<aec::Frame> frames;
  const auto first =
      fast.Request("stream_read", aec::Json::object(), 5000, &frames);
  ASSERT_EQ(frames.size(), 3u);
  EXPECT_EQ(first["loss"]["count"], 0);
  const auto first_seq = frames.front().header.seq;
  const auto first_timestamp = frames.front().header.timestamp;
  acquire();
  frames.clear();
  EXPECT_EQ(fast.Request("stream_read", aec::Json::object(), 5000, &frames),
            first);
  EXPECT_EQ(frames.front().header.seq, first_seq);
  frames.clear();
  const auto normal =
      fast.Request("stream_read", {{"ack", first["batch_id"]}}, 5000, &frames);
  EXPECT_EQ(normal["loss"]["count"], 0);
  EXPECT_EQ(frames.size(), 3u);
  frames.clear();
  auto delayed =
      slow.Request("stream_read", aec::Json::object(), 5000, &frames);
  EXPECT_EQ(delayed["loss"]["count"], 2);
  EXPECT_EQ(delayed["loss"]["first_seq"], first_seq);
  EXPECT_EQ(delayed["loss"]["first_timestamp"], first_timestamp);
  EXPECT_EQ(frames.size(), 4u);
  fast.Request("stream_close");
  slow.Request("stream_close");
}

TEST(StreamClient, ReceiverRingOverwritesOldest) {
  const char* endpoint = std::getenv("AEC_STREAM_CONTROL");
  if (!endpoint)
    GTEST_SKIP() << "Set AEC_STREAM_CONTROL to isolated sim backend";
  zmq::context_t context(1);
  aec::ControlClient owner(&context, endpoint);
  ASSERT_TRUE(owner.Request("ping")["simulated"].get<bool>());
  owner.Request("acquire_control", {{"force", true}});
  ReleaseControl release{&owner};
  std::atomic<bool> ready{false};
  std::atomic<int> received{0};
  std::atomic<bool> last{false};
  std::atomic<std::uint64_t> client_loss{0}, server_loss{0};
  aec::FrameSubscriber sub(
      &context, "",
      [&](aec::Frame f) {
        ++received;
        client_loss += f.delivery.at("client_loss").value("count", 0u);
        server_loss += f.delivery.at("server_loss").value("count", 0u);
        if (f.header.index == 19) last = true;
        std::this_thread::sleep_for(80ms);
      },
      [&](const aec::Json& status) {
        if (status.contains("state")) ready = true;
      },
      2, endpoint);
  sub.Start();
  auto deadline = std::chrono::steady_clock::now() + 3s;
  while (!ready && std::chrono::steady_clock::now() < deadline)
    std::this_thread::sleep_for(10ms);
  if (!ready) {
    sub.Stop();
    FAIL() << "subscriber did not register";
  }
  auto run = owner.Request("arm", {{"n_frames", 20}, {"settings", Settings()}});
  owner.Request("wait", {{"run_id", run["run_id"]}});
  deadline = std::chrono::steady_clock::now() + 3s;
  while (!last && std::chrono::steady_clock::now() < deadline)
    std::this_thread::sleep_for(10ms);
  sub.Stop();
  EXPECT_TRUE(last);
  EXPECT_GT(client_loss.load(), 0u);
  EXPECT_EQ(server_loss.load(), 0u);
  EXPECT_EQ(client_loss.load() + received.load(), 20u);
  EXPECT_EQ(sub.Dropped(), client_loss.load());
}
