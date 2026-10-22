/**
 * @file CameraClient.cc
 * @brief Implementation of AndorEMCCDcontroller::CameraClient.
 */

#include <AndorEMCCDcontroller/CameraClient.h>

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <random>

namespace AndorEMCCDcontroller {

namespace {

bool IsLocalHost(const std::string& host) {
  return host.empty() || host == "localhost" || host == "127.0.0.1" ||
         host == "::1";
}

}  // namespace

void ResolveEndpoints(const ServerAddress& address, std::string* control,
                      std::string* data) {
  bool ipc = address.use_ipc == 1;
#ifndef _WIN32
  if (address.use_ipc < 0 && IsLocalHost(address.host)) {
    std::error_code ec;
    ipc = std::filesystem::exists(
        std::filesystem::path(address.ipc_dir) / kControlIpcName, ec);
  }
#endif
  if (ipc) {
    *control = IpcEndpoint(address.ipc_dir, kControlIpcName);
    *data = IpcEndpoint(address.ipc_dir, kDataIpcName);
  } else {
    *control = TcpEndpoint(address.host, address.control_port);
    *data = TcpEndpoint(address.host, address.data_port);
  }
}

std::string RandomClientId() {
  std::random_device rd;
  std::mt19937_64 rng((static_cast<std::uint64_t>(rd()) << 32) ^ rd() ^
                      static_cast<std::uint64_t>(
                          std::chrono::steady_clock::now()
                              .time_since_epoch()
                              .count()));
  char buf[40];
  std::snprintf(buf, sizeof(buf), "%016llx%016llx",
                static_cast<unsigned long long>(rng()),
                static_cast<unsigned long long>(rng()));
  return buf;
}

ControlClient::ControlClient(zmq::context_t* context, std::string endpoint)
    : context_(context),
      endpoint_(std::move(endpoint)),
      client_id_(RandomClientId()) {
  Connect();
}

void ControlClient::SetIdentity(const std::string& name,
                                const std::string& host,
                                const std::string& id) {
  std::lock_guard<std::mutex> lock(mutex_);
  client_name_ = name;
  client_host_ = host;
  if (!id.empty()) client_id_ = id;
}

void ControlClient::Connect() {
  socket_ = std::make_unique<zmq::socket_t>(*context_, zmq::socket_type::dealer);
  socket_->set(zmq::sockopt::linger, 0);
  socket_->set(zmq::sockopt::rcvhwm, 0);
  socket_->connect(endpoint_);
}

Json ControlClient::Request(const std::string& cmd, const Json& args,
                            int timeout_ms, std::vector<Frame>* frames) {
  std::lock_guard<std::mutex> lock(mutex_);
  const std::uint64_t id = next_id_++;
  const std::string body =
      Json{{"id", id},
           {"cmd", cmd},
           {"args", args},
           {"client",
            {{"id", client_id_}, {"name", client_name_}, {"host", client_host_}}}}
          .dump();
  socket_->send(zmq::message_t(), zmq::send_flags::sndmore);
  socket_->send(zmq::buffer(body), zmq::send_flags::none);

  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
  for (;;) {
    const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
        deadline - std::chrono::steady_clock::now());
    zmq::pollitem_t item{static_cast<void*>(*socket_), 0, ZMQ_POLLIN, 0};
    if (remaining.count() <= 0 ||
        zmq::poll(&item, 1, remaining) == 0) {
      // A late reply must not be taken as the answer to the next request.
      Connect();
      throw ClientError("no reply to '" + cmd + "' from " + endpoint_);
    }
    std::vector<zmq::message_t> parts;
    do {
      zmq::message_t part;
      (void)socket_->recv(part);
      parts.push_back(std::move(part));
    } while (parts.back().more());
    if (parts.size() < 2) continue;
    const Json reply = Json::parse(parts[1].to_string_view());
    if (reply.value("id", std::uint64_t{0}) != id) continue;
    if (!reply.value("ok", false)) {
      throw ClientError(reply.value("error", std::string("request failed")),
                        reply.value("code", std::string()),
                        reply.value("result", Json()));
    }
    Json result = reply.value("result", Json());
    if (frames) {
      const Json headers =
          result.is_object() && result.contains("frames") ? result["frames"]
                                                          : Json::array();
      if (result.contains("frames") && headers.size() + 2 != parts.size()) {
        throw ClientError("frame payload count mismatch");
      }
      for (std::size_t i = 0; i < headers.size() && i + 2 < parts.size(); ++i) {
        Frame f;
        f.header = headers[i].get<FrameHeader>();
        if (parts[i + 2].size() != f.header.PayloadBytes()) {
          throw ClientError("frame payload size mismatch");
        }
        const auto* p = static_cast<const std::uint8_t*>(parts[i + 2].data());
        f.data = std::make_shared<std::vector<std::uint8_t>>(
            p, p + parts[i + 2].size());
        frames->push_back(std::move(f));
      }
    }
    return result;
  }
}

FrameSubscriber::FrameSubscriber(zmq::context_t* context, std::string endpoint,
                                 FrameCallback on_frame,
                                 StatusCallback on_status, int hwm_frames,
                                 std::string control_endpoint)
    : display_ring_(hwm_frames > 0 ? hwm_frames : 1),
      control_endpoint_(std::move(control_endpoint)),
      context_(context),
      endpoint_(std::move(endpoint)),
      on_frame_(std::move(on_frame)),
      on_status_(std::move(on_status)),
      hwm_frames_(hwm_frames) {
  if (hwm_frames < 1) throw std::invalid_argument("buffer frames must be positive");
}

FrameSubscriber::~FrameSubscriber() { Stop(); }

void FrameSubscriber::Start() {
  if (thread_.joinable()) return;
  stop_ = false;
  dropped_ = 0;
  display_ring_.Clear();
  {
    std::lock_guard<std::mutex> lock(queue_mutex_);
    queue_.clear();
    pending_server_loss_ = {{"count", 0}};
    pending_client_loss_ = {{"count", 0}};
  }
  consumer_ = std::thread(&FrameSubscriber::Consume, this);
  thread_ = std::thread(&FrameSubscriber::ThreadMain, this);
}

void FrameSubscriber::Stop() {
  stop_ = true;
  queue_cv_.notify_all();
  if (thread_.joinable()) thread_.join();
  if (consumer_.joinable()) consumer_.join();
}

namespace {
void MergeLoss(Json* target, const Json& loss) {
  const auto count = loss.value("count", std::uint64_t{0});
  if (!count) return;
  const auto previous = target->value("count", std::uint64_t{0});
  if (!previous) {
    *target = loss;
  } else {
    (*target)["last_seq"] = loss.at("last_seq");
    (*target)["last_timestamp"] = loss.at("last_timestamp");
  }
  (*target)["count"] = previous + count;
}
}  // namespace

void FrameSubscriber::ReceiveStream() {
  ControlClient client(context_, control_endpoint_);
  try {
    client.Request("stream_open", Json::object(), 2000);
    double next_status = 0;
    std::uint64_t ack = 0;
    while (!stop_) {
      std::vector<Frame> frames;
      Json result = client.Request("stream_read", {{"count", 8}, {"ack", ack}},
                                   2000, &frames);
      ack = result.at("batch_id").get<std::uint64_t>();
      {
        std::lock_guard<std::mutex> lock(queue_mutex_);
        MergeLoss(&pending_server_loss_, result.at("loss"));
        dropped_ += result.at("loss").value("count", std::uint64_t{0});
        for (auto& frame : frames) {
          display_ring_.Append(frame);
          if (queue_.size() >= static_cast<std::size_t>(hwm_frames_)) {
            const auto& h = queue_.front().header;
            MergeLoss(&pending_client_loss_,
                      {{"count", 1}, {"first_seq", h.seq}, {"last_seq", h.seq},
                       {"first_timestamp", h.timestamp},
                       {"last_timestamp", h.timestamp}});
            queue_.pop_front();
            ++dropped_;
          }
          queue_.push_back(std::move(frame));
        }
      }
      queue_cv_.notify_one();
      if (on_status_ && UnixTimeNow() >= next_status) {
        on_status_(client.Request("get_status", Json::object(), 2000));
        next_status = UnixTimeNow() + 0.5;
      }
      if (frames.empty()) std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    client.Request("stream_close", Json::object(), 1000);
  } catch (const std::exception& e) {
    if (on_status_) on_status_({{"stream_error", e.what()}});
  }
}

void FrameSubscriber::Consume() {
  while (!stop_) {
    Frame frame;
    {
      std::unique_lock<std::mutex> lock(queue_mutex_);
      queue_cv_.wait(lock, [this] { return stop_ || !queue_.empty(); });
      if (stop_) break;
      frame = std::move(queue_.front());
      queue_.pop_front();
      frame.delivery = {{"server_loss", pending_server_loss_},
                        {"client_loss", pending_client_loss_},
                        {"reported_at", UnixTimeNow()}};
      pending_server_loss_ = {{"count", 0}};
      pending_client_loss_ = {{"count", 0}};
    }
    try {
      if (on_frame_) on_frame_(std::move(frame));
    } catch (const std::exception& e) {
      if (on_status_) on_status_({{"stream_error", e.what()}});
    }
  }
}

void FrameSubscriber::ThreadMain() {
  if (!control_endpoint_.empty()) {
    ReceiveStream();
    return;
  }
  zmq::socket_t sub(*context_, zmq::socket_type::sub);
  sub.set(zmq::sockopt::rcvhwm, hwm_frames_ * 3);
  sub.set(zmq::sockopt::linger, 0);
  if (on_frame_) sub.set(zmq::sockopt::subscribe, kTopicFrame);
  if (on_status_) sub.set(zmq::sockopt::subscribe, kTopicStatus);
  sub.connect(endpoint_);

  std::uint64_t last_seq = 0;
  bool have_seq = false;
  while (!stop_) {
    zmq::pollitem_t item{static_cast<void*>(sub), 0, ZMQ_POLLIN, 0};
    if (zmq::poll(&item, 1, std::chrono::milliseconds(100)) == 0) continue;
    std::vector<zmq::message_t> parts;
    do {
      zmq::message_t part;
      (void)sub.recv(part);
      parts.push_back(std::move(part));
    } while (parts.back().more());
    if (parts.size() < 2) continue;
    const std::string_view topic = parts[0].to_string_view();
    try {
      if (topic == kTopicFrame && parts.size() >= 3 && on_frame_) {
        Frame f;
        f.header = Json::parse(parts[1].to_string_view()).get<FrameHeader>();
        if (parts[2].size() < f.header.PayloadBytes()) continue;
        if (have_seq && f.header.seq > last_seq + 1) {
          const auto count = f.header.seq - last_seq - 1;
          dropped_ += count;
          std::lock_guard<std::mutex> lock(queue_mutex_);
          MergeLoss(&pending_server_loss_,
                    {{"count", count}, {"first_seq", last_seq + 1},
                     {"last_seq", f.header.seq - 1},
                     {"first_timestamp", nullptr}, {"last_timestamp", nullptr}});
        }
        last_seq = f.header.seq;
        have_seq = true;
        const auto* p = static_cast<const std::uint8_t*>(parts[2].data());
        f.data = std::make_shared<std::vector<std::uint8_t>>(
            p, p + parts[2].size());
        {
          std::lock_guard<std::mutex> lock(queue_mutex_);
          if (queue_.size() >= static_cast<std::size_t>(hwm_frames_)) {
            const auto& h = queue_.front().header;
            MergeLoss(&pending_client_loss_,
                      {{"count", 1}, {"first_seq", h.seq}, {"last_seq", h.seq},
                       {"first_timestamp", h.timestamp},
                       {"last_timestamp", h.timestamp}});
            queue_.pop_front();
            ++dropped_;
          }
          display_ring_.Append(f);
          queue_.push_back(std::move(f));
        }
        queue_cv_.notify_one();
      } else if (topic == kTopicStatus && on_status_) {
        on_status_(Json::parse(parts[1].to_string_view()));
      }
    } catch (const std::exception&) {
      // Malformed message: skip it.
    }
  }
}

}  // namespace AndorEMCCDcontroller
