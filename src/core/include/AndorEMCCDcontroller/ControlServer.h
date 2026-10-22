/**
 * @file ControlServer.h
 * @brief Declaration of AndorEMCCDcontroller::ControlServer.
 */

#ifndef AndorEMCCDcontroller__ControlServer_h
#define AndorEMCCDcontroller__ControlServer_h

#include <atomic>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include <zmq.hpp>

#include <AndorEMCCDcontroller/AcquisitionEngine.h>

namespace AndorEMCCDcontroller {

/**
 * @brief Request/reply server on a ROUTER socket.
 *
 * Requests are [empty, json] (REQ compatible); a DEALER client sends the
 * empty delimiter itself. "wait" requests are answered asynchronously when
 * the run reaches the requested frame count, so slow waits never block other
 * clients.
 */
class ControlServer {
 public:
  /** Commands that change the camera and need the control right. */
  static bool IsMutating(const std::string& cmd);

  ControlServer(AcquisitionEngine* engine, zmq::context_t* context,
                std::vector<std::string> endpoints, bool simulated);

  /**
   * @brief Seconds without any request from the holder after which control
   *        is released (clients send a keepalive "ping" every ~2 s).
   */
  void SetControlLease(double seconds) { lease_s_ = seconds; }

  /** Binds the sockets; returns false and sets @p error on failure. */
  bool Bind(std::string* error);

  /**
   * @brief Serves until @p stop becomes true.
   *
   * @param idle Called about every 100 ms from the serving thread.
   */
  void Serve(const std::atomic<bool>& stop,
             const std::function<void()>& idle = {});

 private:
  /** Identity sent by the client in every request ("client" object). */
  struct ClientIdentity {
    std::string id;
    std::string display;  ///< "name@ip".
  };

  struct PendingWait {
    std::vector<zmq::message_t> envelope;
    Json id;
    std::uint64_t run_id = 0;
    std::int64_t n_frames = 0;
    double deadline = 0.0;
  };

  void HandleRequest(std::vector<zmq::message_t>* parts);
  void CheckWaits(bool expire);
  void Reply(std::vector<zmq::message_t>& envelope, const Json& reply,
             const std::vector<Frame>* frames = nullptr);

  Json Dispatch(const std::string& cmd, const Json& args, const Json& id,
                std::vector<zmq::message_t>* envelope, bool* deferred,
                std::vector<Frame>* frames);
  Json Fetch(const Json& args, std::vector<Frame>* frames);
  ClientIdentity Identify(const Json& request, const zmq::message_t& body);
  Json HandleControl(const std::string& cmd, const Json& args,
                     const ClientIdentity& client);
  void CheckControlLease();
  void PublishControl();

  AcquisitionEngine* engine_;
  zmq::context_t* context_;
  std::vector<std::string> endpoints_;
  bool simulated_;
  zmq::socket_t router_;
  zmq::socket_t events_;
  std::vector<PendingWait> waits_;

  ControlInfo control_;
  double control_last_seen_ = 0.0;  ///< Steady clock.
  double lease_s_ = 10.0;
};

}  // namespace AndorEMCCDcontroller

#endif
