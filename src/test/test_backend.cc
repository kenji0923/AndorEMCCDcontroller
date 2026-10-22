/**
 * @file test_backend.cc
 * @brief Engine, frame store and control server with the simulated camera.
 */

#include <atomic>
#include <chrono>
#include <thread>

#include <gtest/gtest.h>
#include <zmq.hpp>

#include <AndorEMCCDcontroller/AcquisitionEngine.h>
#include <AndorEMCCDcontroller/ControlServer.h>
#include <AndorEMCCDcontroller/FrameStore.h>
#include <AndorEMCCDcontroller/SimCamera.h>
#ifdef AEC_TEST_CLIENT
#include <AndorEMCCDcontroller/CameraClient.h>
#endif

namespace aec = AndorEMCCDcontroller;

namespace {

aec::Frame FrameWithSeq(std::uint64_t run, std::int64_t index,
                        std::uint64_t seq) {
  aec::Frame f;
  f.header.run_id = run;
  f.header.index = index;
  f.header.seq = seq;
  f.header.width = 2;
  f.header.height = 2;
  f.data = std::make_shared<std::vector<std::uint8_t>>(8, 0);
  return f;
}

}  // namespace

TEST(FrameStore, GetAndEviction) {
  aec::FrameStore store(4, 0);
  aec::RunInfo run;
  run.run_id = 1;
  run.expected = 6;
  run.first_seq = 0;
  store.BeginRun(run);
  for (int i = 0; i < 6; ++i) store.Append(FrameWithSeq(1, i, i));
  std::vector<aec::Frame> out;
  std::string error;
  EXPECT_FALSE(store.Get(1, 0, 2, &out, &error));  // Evicted.
  ASSERT_TRUE(store.Get(1, 2, 4, &out, &error)) << error;
  ASSERT_EQ(out.size(), 4u);
  EXPECT_EQ(out[0].header.index, 2);
  EXPECT_FALSE(store.Get(1, 5, 2, &out, &error));  // Not acquired yet.
  EXPECT_EQ(store.Run(1)->frames, 6);
}

TEST(SimCamera, KineticSeriesProducesAllFrames) {
  aec::SimCamera camera;
  aec::CameraSettings s;
  s.acquisition_mode = aec::AcquisitionMode::kKinetic;
  s.number_kinetics = 3;
  s.exposure_time_s = 0.001;
  s.hbin = 2;
  s.vbin = 4;
  EXPECT_TRUE(camera.ApplyAcquisitionSettings(&s).empty());
  EXPECT_EQ(camera.Geometry().width, 256);
  EXPECT_EQ(camera.Geometry().height, 128);
  std::string error;
  ASSERT_TRUE(camera.StartAcquisition(&error));
  std::vector<aec::Frame> frames;
  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds(5);
  while (camera.IsAcquiring() && std::chrono::steady_clock::now() < deadline) {
    camera.WaitForData(50);
    camera.RetrieveNewFrames(&frames, &error);
  }
  ASSERT_EQ(frames.size(), 3u);
  EXPECT_EQ(frames[0].data->size(), 256u * 128u * 2u);
}

TEST(SimCamera, InvalidIndicesAreCorrected) {
  aec::SimCamera camera;
  aec::CameraSettings s;
  s.hs_speed_index = 9;
  s.em_gain = 4000;
  const auto problems = camera.ApplyAcquisitionSettings(&s);
  EXPECT_EQ(problems.size(), 2u);
  EXPECT_EQ(s.hs_speed_index, 0);
  EXPECT_EQ(s.em_gain, 255);
}

TEST(Engine, EmGainSwitchKeepsSetPoint) {
  zmq::context_t context(1);
  aec::EngineConfig config;
  config.data_endpoints = {"inproc://test-em-gain"};
  config.warmup_on_exit = false;
  aec::AcquisitionEngine engine(aec::MakeSimulatedCamera(), &context, config);
  std::string error;
  ASSERT_TRUE(engine.BindSockets(&error)) << error;
  ASSERT_TRUE(engine.Start(&error)) << error;

  // Off by default; a gain value alone is stored but not applied.
  aec::Json r = engine.SetSettings({{"em_gain", 100}});
  EXPECT_FALSE(r["settings"]["em_gain_enabled"].get<bool>());
  ASSERT_EQ(r["warnings"].size(), 1u);
  EXPECT_NE(r["warnings"][0].get<std::string>().find("EM gain is off"),
            std::string::npos);
  EXPECT_EQ(engine.Status().em_gain, 0);

  r = engine.SetSettings({{"em_gain_enabled", true}});
  EXPECT_TRUE(r["warnings"].empty());
  EXPECT_EQ(engine.Status().em_gain, 100);

  engine.SetSettings({{"em_gain_enabled", false}});
  EXPECT_EQ(engine.Status().em_gain, 0);
  r = engine.SetSettings({{"em_gain_enabled", true}});
  EXPECT_EQ(r["settings"]["em_gain"], 100);  // Set point survived.
  EXPECT_EQ(engine.Status().em_gain, 100);

  engine.SetSettings({{"output_amplifier", 1}});  // Conventional.
  EXPECT_EQ(engine.Status().em_gain, 0);
  engine.Stop();
}

#ifdef AEC_TEST_CLIENT
TEST(Backend, ArmWaitFetchAndLiveStream) {
  zmq::context_t context(1);
  aec::EngineConfig config;
  config.data_endpoints = {"inproc://test-data"};
  config.warmup_on_exit = false;
  aec::AcquisitionEngine engine(aec::MakeSimulatedCamera(), &context, config);
  aec::ControlServer server(&engine, &context, {"tcp://127.0.0.1:47195"},
                            true);
  std::string error;
  ASSERT_TRUE(server.Bind(&error)) << error;
  ASSERT_TRUE(engine.BindSockets(&error)) << error;
  ASSERT_TRUE(engine.Start(&error)) << error;
  std::atomic<bool> stop{false};
  std::thread serving([&] { server.Serve(stop); });

  std::vector<aec::Frame> live;
  std::mutex live_mutex;
  aec::FrameSubscriber subscriber(
      &context, "inproc://test-data",
      [&](aec::Frame f) {
        std::lock_guard<std::mutex> lock(live_mutex);
        live.push_back(std::move(f));
      },
      nullptr);
  subscriber.Start();
  std::this_thread::sleep_for(std::chrono::milliseconds(200));

  aec::ControlClient client(&context, "tcp://127.0.0.1:47195");
  client.SetIdentity("tester", "");
  EXPECT_TRUE(client.Request("ping")["simulated"].get<bool>());
  // Mutating commands need the control right.
  try {
    client.Request("stop");
    ADD_FAILURE() << "stop without control was accepted";
  } catch (const aec::ClientError& e) {
    EXPECT_EQ(e.code(), "not_controller");
  }
  const aec::Json granted = client.Request("acquire_control");
  EXPECT_EQ(granted["holder"], "tester@localhost");
  EXPECT_THROW(client.Request("set_settings", {{"settings", {{"x", 1}}}}),
               aec::ClientError);

  const aec::Json armed = client.Request(
      "arm", {{"n_frames", 4},
              {"settings", {{"exposure_time_s", 0.001}, {"crop_width", 64},
                            {"crop_height", 32}, {"isolated_crop", true}}}});
  const auto run_id = armed["run_id"].get<std::uint64_t>();
  const aec::Json done =
      client.Request("wait", {{"run_id", run_id}, {"timeout_s", 5.0}}, 10000);
  EXPECT_EQ(done["frames"], 4);
  // wait() promises the frame count, which can precede EndRun().
  const auto complete_deadline = std::chrono::steady_clock::now() +
                                 std::chrono::seconds(2);
  while (!engine.Store().Run(run_id)->complete &&
         std::chrono::steady_clock::now() < complete_deadline) {
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  EXPECT_TRUE(engine.Store().Run(run_id)->complete);

  std::vector<aec::Frame> frames;
  const aec::Json fetched =
      client.Request("fetch", {{"run_id", run_id}}, 5000, &frames);
  ASSERT_EQ(frames.size(), 4u);
  EXPECT_EQ(frames[3].header.index, 3);
  EXPECT_EQ(frames[0].header.width, 64);
  EXPECT_EQ(frames[0].header.height, 32);
  EXPECT_EQ(frames[0].data->size(), 64u * 32u * 2u);

  // Software triggers need a continuous run; a wait that cannot complete
  // times out with code "timeout".
  client.Request("start", {{"settings", {{"trigger_mode", "software"},
                                         {"acquisition_mode", "continuous"}}}});
  try {
    client.Request("wait", {{"n_frames", 1}, {"timeout_s", 0.2}}, 2000);
    ADD_FAILURE() << "wait without trigger returned";
  } catch (const aec::ClientError& e) {
    EXPECT_EQ(e.code(), "timeout");
  }
  client.Request("software_trigger");
  client.Request("software_trigger");
  EXPECT_EQ(client.Request("wait", {{"n_frames", 2}, {"timeout_s", 2.0}},
                           5000)["frames"],
            2);
  client.Request("stop");

  // The wait reply can arrive before the independent receiver callback.
  const auto live_deadline = std::chrono::steady_clock::now() +
                             std::chrono::seconds(2);
  while (std::chrono::steady_clock::now() < live_deadline) {
    { std::lock_guard<std::mutex> lock(live_mutex);
      if (live.size() >= 6) break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  {
    std::lock_guard<std::mutex> lock(live_mutex);
    EXPECT_EQ(live.size(), 6u);
  }
  subscriber.Stop();
  stop = true;
  serving.join();
  engine.Stop();
}
#endif

#ifdef AEC_TEST_CLIENT
TEST(Backend, ControlRightIsExclusive) {
  zmq::context_t context(1);
  aec::EngineConfig config;
  config.data_endpoints = {"inproc://test-data2"};
  config.warmup_on_exit = false;
  aec::AcquisitionEngine engine(aec::MakeSimulatedCamera(), &context, config);
  aec::ControlServer server(&engine, &context, {"tcp://127.0.0.1:47196"},
                            true);
  server.SetControlLease(0.5);
  std::string error;
  ASSERT_TRUE(server.Bind(&error)) << error;
  ASSERT_TRUE(engine.BindSockets(&error)) << error;
  ASSERT_TRUE(engine.Start(&error)) << error;
  std::atomic<bool> stop{false};
  std::thread serving([&] { server.Serve(stop); });

  aec::ControlClient a(&context, "tcp://127.0.0.1:47196");
  aec::ControlClient b(&context, "tcp://127.0.0.1:47196");
  a.SetIdentity("alice", "10.0.0.7");
  b.SetIdentity("", "10.0.0.8");
  a.Request("acquire_control");
  a.Request("set_settings", {{"settings", {{"em_gain", 10}}}});

  // b is rejected while a holds control; it sees who holds it.
  try {
    b.Request("acquire_control");
    ADD_FAILURE() << "second client got control";
  } catch (const aec::ClientError& e) {
    EXPECT_EQ(e.code(), "control_denied");
    EXPECT_EQ(e.result()["holder"], "alice@10.0.0.7");
  }
  EXPECT_THROW(b.Request("set_settings", {{"settings", {{"em_gain", 20}}}}),
               aec::ClientError);
  EXPECT_EQ(b.Request("get_settings")["em_gain"], 10);  // Reading is free.
  EXPECT_EQ(b.Request("get_status")["control"]["holder"], "alice@10.0.0.7");

  // Forced takeover, then release.
  EXPECT_EQ(b.Request("acquire_control", {{"force", true}})["holder"],
            "client@10.0.0.8");
  EXPECT_THROW(a.Request("set_settings", {{"settings", {{"em_gain", 30}}}}),
               aec::ClientError);
  b.Request("release_control");
  EXPECT_TRUE(b.Request("get_control").is_null());

  // The lease expires without requests from the holder.
  a.Request("acquire_control");
  std::this_thread::sleep_for(std::chrono::milliseconds(800));
  EXPECT_TRUE(b.Request("get_control").is_null());

  stop = true;
  serving.join();
  engine.Stop();
}
#endif

TEST(FrameStore, IndependentStreamsLossTimesAndRetry) {
  aec::FrameStore store(3, 0);
  store.OpenStream("fast");
  store.OpenStream("slow");
  for (int i = 0; i < 3; ++i) {
    auto f = FrameWithSeq(1, i, i);
    f.header.timestamp = 1000 + i;
    store.Append(f);
  }
  std::vector<aec::Frame> out;
  auto first = store.ReadStream("fast", 3, &out);
  ASSERT_EQ(out.size(), 3u);
  EXPECT_EQ(first["loss"]["count"], 0);
  for (int i = 3; i < 6; ++i) {
    auto f = FrameWithSeq(2, i - 3, i);
    f.header.timestamp = 1000 + i;
    store.Append(f);
  }
  out.clear();
  auto replay = store.ReadStream("fast", 3, &out);
  EXPECT_EQ(replay, first);
  EXPECT_EQ(out.front().header.seq, 0u);  // Retry survives history eviction.
  out.clear();
  auto fast = store.ReadStream("fast", 3, &out, first["batch_id"]);
  EXPECT_EQ(fast["loss"]["count"], 0);
  ASSERT_EQ(out.size(), 3u);
  EXPECT_EQ(out.front().header.seq, 3u);
  out.clear();
  auto slow = store.ReadStream("slow", 3, &out);
  EXPECT_EQ(slow["loss"]["count"], 3);
  EXPECT_EQ(slow["loss"]["first_seq"], 0);
  EXPECT_EQ(slow["loss"]["last_seq"], 2);
  EXPECT_EQ(slow["loss"]["first_timestamp"], 1000);
  EXPECT_EQ(slow["loss"]["last_timestamp"], 1002);
  out.clear();
  auto empty = store.ReadStream("slow", 3, &out, slow["batch_id"]);
  EXPECT_EQ(empty["loss"]["count"], 0);
  EXPECT_TRUE(out.empty());
  store.CloseStream("slow");
  EXPECT_THROW(store.ReadStream("slow", 1, &out), std::runtime_error);
}

TEST(FrameStore, StreamByteEvictionAndLateJoin) {
  aec::FrameStore store(100, 16);
  store.Append(FrameWithSeq(1, 0, 0));
  store.OpenStream("late");
  for (int i = 1; i < 5; ++i) store.Append(FrameWithSeq(1, i, i));
  std::vector<aec::Frame> out;
  auto result = store.ReadStream("late", 10, &out);
  EXPECT_EQ(result["loss"]["count"], 2);  // Only 1 and 2, not pre-join 0.
  ASSERT_EQ(out.size(), 2u);
  EXPECT_EQ(out.front().header.seq, 3u);
}
