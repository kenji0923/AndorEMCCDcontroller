/**
 * @file ControlServer.cc
 * @brief Implementation of AndorEMCCDcontroller::ControlServer.
 */

#include <AndorEMCCDcontroller/ControlServer.h>

#include <algorithm>
#include <chrono>

#include <arpa/inet.h>

#include <spdlog/spdlog.h>

namespace AndorEMCCDcontroller {

namespace {

constexpr std::size_t kMaxBenchBytes = std::size_t{256} << 20;
constexpr std::int64_t kMaxFetchFrames = 100000;

/** True for a numeric IPv4/IPv6 address that is not a loopback address. */
bool IsRoutableAddress(const std::string& text) {
  unsigned char buf[16];
  if (inet_pton(AF_INET, text.c_str(), buf) == 1) return buf[0] != 127;
  if (inet_pton(AF_INET6, text.c_str(), buf) == 1) return text != "::1";
  return false;
}

double SteadyNow() {
  using namespace std::chrono;
  return duration<double>(steady_clock::now().time_since_epoch()).count();
}

/** Payload buffer for bench_payload, shared by all replies. */
const std::shared_ptr<std::vector<std::uint8_t>>& BenchBuffer(
    std::size_t bytes) {
  static std::shared_ptr<std::vector<std::uint8_t>> buffer =
      std::make_shared<std::vector<std::uint8_t>>();
  if (buffer->size() != bytes) {
    buffer = std::make_shared<std::vector<std::uint8_t>>(bytes);
    for (std::size_t i = 0; i < bytes; ++i) {
      (*buffer)[i] = static_cast<std::uint8_t>(i * 31u);
    }
  }
  return buffer;
}

/** Thrown to reply with ok=false and a machine-readable code. */
class CodedError : public std::runtime_error {
 public:
  CodedError(std::string code, const std::string& what, Json result)
      : std::runtime_error(what),
        code_(std::move(code)),
        result_(std::move(result)) {}
  const std::string& code() const { return code_; }
  const Json& result() const { return result_; }

 private:
  std::string code_;
  Json result_;
};

}  // namespace

bool ControlServer::IsMutating(const std::string& cmd) {
  return cmd == "set_settings" || cmd == "start" || cmd == "arm" ||
         cmd == "stop" || cmd == "software_trigger";
}

ControlServer::ControlServer(AcquisitionEngine* engine,
                             zmq::context_t* context,
                             std::vector<std::string> endpoints,
                             bool simulated)
    : engine_(engine),
      context_(context),
      endpoints_(std::move(endpoints)),
      simulated_(simulated),
      router_(*context, zmq::socket_type::router),
      events_(*context, zmq::socket_type::pull) {}

bool ControlServer::Bind(std::string* error) {
  router_.set(zmq::sockopt::linger, 0);
  router_.set(zmq::sockopt::router_mandatory, false);
  for (const auto& endpoint : endpoints_) {
    try {
      router_.bind(endpoint);
      spdlog::info("control channel listening on {}", endpoint);
    } catch (const zmq::error_t& e) {
      *error = "bind " + endpoint + ": " + e.what();
      return false;
    }
  }
  events_.bind(kEngineEventsEndpoint);
  return true;
}

void ControlServer::Serve(const std::atomic<bool>& stop,
                          const std::function<void()>& idle) {
  double next_idle = 0.0;
  zmq::pollitem_t items[] = {
      {static_cast<void*>(router_), 0, ZMQ_POLLIN, 0},
      {static_cast<void*>(events_), 0, ZMQ_POLLIN, 0}};
  while (!stop) {
    try {
      zmq::poll(items, 2, std::chrono::milliseconds(100));
    } catch (const zmq::error_t& e) {
      if (e.num() == EINTR) continue;
      throw;
    }
    if (items[1].revents & ZMQ_POLLIN) {
      zmq::message_t m;
      while (events_.recv(m, zmq::recv_flags::dontwait)) {
      }
      CheckWaits(false);
    }
    if (items[0].revents & ZMQ_POLLIN) {
      for (;;) {
        std::vector<zmq::message_t> parts;
        zmq::message_t part;
        if (!router_.recv(part, zmq::recv_flags::dontwait)) break;
        parts.push_back(std::move(part));
        while (parts.back().more()) {
          zmq::message_t next;
          (void)router_.recv(next);
          parts.push_back(std::move(next));
        }
        HandleRequest(&parts);
      }
    }
    CheckWaits(true);
    CheckControlLease();
    if (idle && SteadyNow() >= next_idle) {
      next_idle = SteadyNow() + 0.1;
      idle();
    }
  }
}

void ControlServer::HandleRequest(std::vector<zmq::message_t>* parts) {
  // [identity, empty, json]; anything after the json is ignored.
  auto delimiter = std::find_if(parts->begin(), parts->end(),
                                [](const zmq::message_t& m) {
                                  return m.size() == 0;
                                });
  if (delimiter == parts->end() || delimiter + 1 == parts->end()) {
    spdlog::warn("dropping malformed request ({} parts)", parts->size());
    return;
  }
  std::vector<zmq::message_t> envelope;
  for (auto it = parts->begin(); it != delimiter; ++it) {
    envelope.push_back(std::move(*it));
  }
  const zmq::message_t& body = *(delimiter + 1);

  Json id = nullptr;
  Json reply;
  std::vector<Frame> frames;
  bool deferred = false;
  try {
    const Json request = Json::parse(body.to_string_view());
    id = request.value("id", Json(nullptr));
    const std::string cmd = request.at("cmd").get<std::string>();
    const Json args = request.value("args", Json::object());
    const ClientIdentity client = Identify(request, body);
    Json result;
    if (cmd == "stream_open") {
      engine_->Store().OpenStream(client.id);
      result = Json{{"lease_s", 60}};
    } else if (cmd == "stream_close") {
      engine_->Store().CloseStream(client.id);
      result = Json::object();
    } else if (cmd == "stream_read") {
      const int count = args.value("count", 8);
      if (count < 1 || count > 256) throw std::runtime_error("count must be 1..256");
      result = engine_->Store().ReadStream(
          client.id, count, &frames, args.value("ack", std::uint64_t{0}));
    } else if (cmd == "acquire_control" || cmd == "release_control" ||
        cmd == "get_control") {
      result = HandleControl(cmd, args, client);
    } else {
      if (IsMutating(cmd) && client.id != control_.holder_id) {
        const std::string holder =
            control_.holder_id.empty() ? "nobody" : control_.holder;
        throw CodedError("not_controller",
                         cmd + " rejected: settings control is held by " +
                             holder + "; request control first",
                         Json(control_));
      }
      result = Dispatch(cmd, args, id, &envelope, &deferred, &frames);
    }
    if (deferred) return;
    reply = Json{{"id", id}, {"ok", true}, {"result", result}};
  } catch (const CodedError& e) {
    reply = Json{{"id", id},
                 {"ok", false},
                 {"code", e.code()},
                 {"error", e.what()},
                 {"result", e.result()}};
    frames.clear();
  } catch (const std::exception& e) {
    reply = Json{{"id", id}, {"ok", false}, {"error", e.what()}};
    frames.clear();
  }
  Reply(envelope, reply, frames.empty() ? nullptr : &frames);
}

Json ControlServer::Dispatch(const std::string& cmd, const Json& args,
                             const Json& id,
                             std::vector<zmq::message_t>* envelope,
                             bool* deferred, std::vector<Frame>* frames) {
  if (cmd == "ping") {
    return Json{{"protocol", kProtocolVersion},
                {"server_time", UnixTimeNow()},
                {"simulated", simulated_}};
  }
  if (cmd == "get_info") return engine_->GetInfo();
  if (cmd == "get_settings") return engine_->GetSettings();
  if (cmd == "set_settings") {
    return engine_->SetSettings(args.contains("settings") ? args.at("settings")
                                                          : args);
  }
  if (cmd == "get_status") {
    CameraStatus status = engine_->Status();
    status.control = control_;
    return Json(status);
  }
  if (cmd == "start") return engine_->StartRun(args);
  if (cmd == "arm") return engine_->Arm(args);
  if (cmd == "stop") return engine_->StopRun();
  if (cmd == "software_trigger") return engine_->SoftwareTrigger();
  if (cmd == "get_runs") return Json(engine_->Store().Runs());
  if (cmd == "fetch") return Fetch(args, frames);
  if (cmd == "fetch_latest") {
    const auto latest = engine_->Store().Latest();
    const std::uint64_t after = args.value("after_seq", std::uint64_t{0});
    const bool has_after = args.contains("after_seq");
    const bool has_run = args.contains("run_id") && !args["run_id"].is_null();
    Json headers = Json::array();
    if (latest && (!has_after || latest->header.seq > after) &&
        (!has_run ||
         latest->header.run_id == args["run_id"].get<std::uint64_t>())) {
      headers.push_back(latest->header);
      frames->push_back(*latest);
    }
    return Json{{"frames", headers}};
  }
  if (cmd == "wait") {
    PendingWait wait;
    const auto latest = engine_->Store().LatestRun();
    wait.run_id = args.value("run_id", latest ? latest->run_id : 0);
    const auto run = engine_->Store().Run(wait.run_id);
    if (!run) throw std::runtime_error("no such run");
    wait.n_frames = args.value(
        "n_frames", run->expected > 0 ? run->expected : run->frames + 1);
    wait.deadline = SteadyNow() + args.value("timeout_s", 10.0);
    wait.id = id;
    wait.envelope = std::move(*envelope);
    waits_.push_back(std::move(wait));
    *deferred = true;
    CheckWaits(false);
    return nullptr;
  }
  if (cmd == "bench_payload") {
    const std::size_t bytes = std::min<std::size_t>(
        args.value("bytes", std::size_t{1} << 20), kMaxBenchBytes);
    Frame frame;
    frame.data = BenchBuffer(bytes);
    frames->push_back(std::move(frame));
    return Json{{"bytes", bytes}};
  }
  throw std::runtime_error("unknown command: " + cmd);
}

Json ControlServer::Fetch(const Json& args, std::vector<Frame>* frames) {
  const FrameStore& store = engine_->Store();
  std::uint64_t run_id = 0;
  if (args.contains("run_id") && !args.at("run_id").is_null()) {
    run_id = args.at("run_id").get<std::uint64_t>();
  } else {
    const auto latest = store.LatestRun();
    if (!latest) throw std::runtime_error("no run yet");
    run_id = latest->run_id;
  }
  const std::int64_t start = args.value("start", std::int64_t{0});
  std::int64_t count = args.value("count", std::int64_t{-1});
  const auto run = store.Run(run_id);
  if (!run) throw std::runtime_error("unknown run " + std::to_string(run_id));
  if (count < 0) count = std::max<std::int64_t>(0, run->frames - start);
  count = std::min(count, kMaxFetchFrames);
  std::string error;
  if (!store.Get(run_id, start, count, frames, &error)) {
    throw std::runtime_error(error);
  }
  Json headers = Json::array();
  for (const auto& f : *frames) headers.push_back(f.header);
  return Json{{"run", *run}, {"frames", headers}};
}

ControlServer::ClientIdentity ControlServer::Identify(
    const Json& request, const zmq::message_t& body) {
  ClientIdentity c;
  const Json info = request.value("client", Json::object());
  c.id = info.value("id", std::string());
  std::string name = info.value("name", std::string());
  if (name.empty()) name = "client";
  // Remote TCP peers: the address the backend sees. IPC and loopback
  // peers (same host): the LAN address the client reports.
  std::string ip;
  try {
    const char* peer = const_cast<zmq::message_t&>(body).gets("Peer-Address");
    if (peer && IsRoutableAddress(peer)) ip = peer;
  } catch (const zmq::error_t&) {
  }
  if (ip.empty()) ip = info.value("host", std::string());
  if (ip.empty()) ip = "localhost";
  c.display = name + "@" + ip;
  if (!c.id.empty() && c.id == control_.holder_id) {
    control_last_seen_ = SteadyNow();
  }
  return c;
}

Json ControlServer::HandleControl(const std::string& cmd, const Json& args,
                                  const ClientIdentity& client) {
  if (cmd == "get_control") return Json(control_);
  if (client.id.empty()) {
    throw std::runtime_error("request has no client id");
  }
  if (cmd == "release_control") {
    if (client.id == control_.holder_id) {
      spdlog::info("control released by {}", control_.holder);
      control_ = ControlInfo();
      PublishControl();
    }
    return Json(control_);
  }
  // acquire_control
  const bool free = control_.holder_id.empty();
  const bool mine = client.id == control_.holder_id;
  const bool force = args.value("force", false);
  if (!free && !mine && !force) {
    throw CodedError("control_denied",
                     "control is held by " + control_.holder, Json(control_));
  }
  if (!mine) {
    if (!free) {
      spdlog::warn("control taken over by {} from {}", client.display,
                   control_.holder);
    } else {
      spdlog::info("control granted to {}", client.display);
    }
    control_.holder_id = client.id;
    control_.since = UnixTimeNow();
  }
  control_.holder = client.display;
  control_last_seen_ = SteadyNow();
  PublishControl();
  return Json(control_);
}

void ControlServer::CheckControlLease() {
  if (control_.holder_id.empty() || lease_s_ <= 0.0) return;
  if (SteadyNow() - control_last_seen_ > lease_s_) {
    spdlog::warn("control of {} expired (no request for {:.0f} s)",
                 control_.holder, lease_s_);
    control_ = ControlInfo();
    PublishControl();
  }
}

void ControlServer::PublishControl() { engine_->SetControl(control_); }

void ControlServer::CheckWaits(bool expire) {
  if (waits_.empty()) return;
  const double now = SteadyNow();
  const FrameStore& store = engine_->Store();
  for (auto it = waits_.begin(); it != waits_.end();) {
    const auto run = store.Run(it->run_id);
    const bool done =
        run && (run->frames >= it->n_frames || run->complete);
    const bool timed_out = expire && now >= it->deadline;
    if (!done && !timed_out) {
      ++it;
      continue;
    }
    Json reply;
    if (done) {
      reply = Json{{"id", it->id}, {"ok", true}, {"result", *run}};
    } else {
      reply = Json{{"id", it->id},
                   {"ok", false},
                   {"code", "timeout"},
                   {"error", "timeout waiting for " +
                                 std::to_string(it->n_frames) +
                                 " frame(s); got " +
                                 std::to_string(run ? run->frames : 0)},
                   {"result", run ? Json(*run) : Json(nullptr)}};
    }
    Reply(it->envelope, reply);
    it = waits_.erase(it);
  }
}

void ControlServer::Reply(std::vector<zmq::message_t>& envelope,
                          const Json& reply,
                          const std::vector<Frame>* frames) {
  for (auto& part : envelope) {
    router_.send(part, zmq::send_flags::sndmore);
  }
  router_.send(zmq::message_t(), zmq::send_flags::sndmore);
  const std::string body = reply.dump();
  const bool more = frames && !frames->empty();
  router_.send(zmq::buffer(body),
               more ? zmq::send_flags::sndmore : zmq::send_flags::none);
  if (!more) return;
  for (std::size_t i = 0; i < frames->size(); ++i) {
    const Frame& frame = (*frames)[i];
    auto* hold =
        new std::shared_ptr<const std::vector<std::uint8_t>>(frame.data);
    zmq::message_t payload(
        const_cast<std::uint8_t*>(frame.data->data()), frame.data->size(),
        [](void*, void* hint) {
          delete static_cast<
              std::shared_ptr<const std::vector<std::uint8_t>>*>(hint);
        },
        hold);
    router_.send(payload, i + 1 < frames->size() ? zmq::send_flags::sndmore
                                                 : zmq::send_flags::none);
  }
}

}  // namespace AndorEMCCDcontroller
