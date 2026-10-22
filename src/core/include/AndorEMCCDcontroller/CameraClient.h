/**
 * @file CameraClient.h
 * @brief C++ client of the backend (control requests and live frames).
 */

#ifndef AndorEMCCDcontroller__CameraClient_h
#define AndorEMCCDcontroller__CameraClient_h

#include <atomic>
#include <condition_variable>
#include <deque>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include <zmq.hpp>

#include <AndorEMCCDcontroller/Protocol.h>
#include <AndorEMCCDcontroller/FrameRing.h>

namespace AndorEMCCDcontroller {

/** Where to reach a backend. */
struct ServerAddress {
  std::string host = "localhost";
  int control_port = kDefaultControlPort;
  int data_port = kDefaultDataPort;
  std::string ipc_dir = kDefaultIpcDir;
  /** -1: automatic (IPC when the host is local and the socket exists). */
  int use_ipc = -1;
  /** Shown to other clients as "name@ip" ("client" when empty). */
  int buffer_frames = 64;  ///< Client ring capacity.
  std::string client_name;
};

/** Control and data endpoints for @p address. */
void ResolveEndpoints(const ServerAddress& address, std::string* control,
                      std::string* data);

class ClientError : public std::runtime_error {
 public:
  explicit ClientError(const std::string& what, std::string code = {},
                       Json result = nullptr)
      : std::runtime_error(what),
        code_(std::move(code)),
        result_(std::move(result)) {}

  /** Backend error code: "timeout", "not_controller", "control_denied"... */
  const std::string& code() const { return code_; }
  const Json& result() const { return result_; }

 private:
  std::string code_;
  Json result_;
};

/**
 * @brief Request/reply client (DEALER). Thread-safe.
 */
class ControlClient {
 public:
  ControlClient(zmq::context_t* context, std::string endpoint);

  const std::string& endpoint() const { return endpoint_; }

  /**
   * @brief Identity sent with every request.
   *
   * @param name Free text; "" means "client". The backend shows
   *        "name@ip" using the TCP peer address, or @p host for IPC.
   * @param id   Unique per client instance; "" generates a random one.
   */
  void SetIdentity(const std::string& name, const std::string& host,
                   const std::string& id = {});
  const std::string& client_id() const { return client_id_; }

  /**
   * @brief Sends a request and returns its "result".
   *
   * @param frames Receives the binary frames of the reply (fetch).
   * @throws ClientError on timeout or backend error.
   */
  Json Request(const std::string& cmd, const Json& args = Json::object(),
               int timeout_ms = 5000, std::vector<Frame>* frames = nullptr);

 private:
  void Connect();

  zmq::context_t* context_;
  std::string endpoint_;
  std::mutex mutex_;
  std::unique_ptr<zmq::socket_t> socket_;
  std::uint64_t next_id_ = 1;
  std::string client_id_;
  std::string client_name_;
  std::string client_host_;
};

/** Random 128-bit hex id for SetIdentity(). */
std::string RandomClientId();

/**
 * @brief Receives live frames and status on a background thread.
 *
 * Frame callbacks run on a separate consumer thread, fed by a bounded ring.
 * Status callbacks run on the receiver thread. Pass the control endpoint to
 * use per-client server cursors and loss reports; omitting it uses legacy PUB.
 * hwm_frames is the ring capacity (oldest overwritten when full).
 */
class FrameSubscriber {
 public:
  using FrameCallback = std::function<void(Frame)>;
  using StatusCallback = std::function<void(const Json&)>;

  FrameSubscriber(zmq::context_t* context, std::string endpoint,
                  FrameCallback on_frame, StatusCallback on_status,
                  int hwm_frames = 64, std::string control_endpoint = {});
  ~FrameSubscriber();

  void Start();
  void Stop();

  Frame DisplayFrame(std::size_t count, std::size_t* used = nullptr,
                     std::uint64_t through_seq = UINT64_MAX) const {
    return display_ring_.Average(count, used, through_seq);
  }

  /** Total server and client ring losses since Start(). */
  std::uint64_t Dropped() const { return dropped_; }

 private:
  void ThreadMain();
  void ReceiveStream();
  void Consume();
  FrameRing display_ring_;
  std::string control_endpoint_;
  std::thread consumer_;
  std::mutex queue_mutex_;
  std::condition_variable queue_cv_;
  std::deque<Frame> queue_;
  Json pending_server_loss_ = {{"count", 0}};
  Json pending_client_loss_ = {{"count", 0}};

  zmq::context_t* context_;
  std::string endpoint_;
  FrameCallback on_frame_;
  StatusCallback on_status_;
  int hwm_frames_;
  std::thread thread_;
  std::atomic<bool> stop_{false};
  std::atomic<std::uint64_t> dropped_{0};
};

}  // namespace AndorEMCCDcontroller

#endif
