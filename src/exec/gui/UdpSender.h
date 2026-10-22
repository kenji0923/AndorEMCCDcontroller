/**
 * @file UdpSender.h
 * @brief Minimal thread-agnostic UDP sender (BSD sockets / Winsock).
 */

#ifndef AndorEMCCDcontroller__gui_UdpSender_h
#define AndorEMCCDcontroller__gui_UdpSender_h

#include <cstdint>
#include <string>

namespace AndorEMCCDcontroller {

/**
 * @brief Sends datagrams to one host:port. Usable from any single thread.
 *
 * Unlike QUdpSocket it needs no Qt event loop, so it works on the frame
 * receiver thread.
 */
class UdpSender {
 public:
  UdpSender();
  ~UdpSender();
  UdpSender(const UdpSender&) = delete;
  UdpSender& operator=(const UdpSender&) = delete;

  /** Resolves @p host (name or IPv4). Returns false if it cannot. */
  bool SetTarget(const std::string& host, std::uint16_t port,
                 std::string* error);
  bool Send(const std::string& payload);

 private:
  void Close();

  std::intptr_t socket_ = -1;
  unsigned char address_[16] = {0};  ///< sockaddr_in.
  bool has_target_ = false;
};

}  // namespace AndorEMCCDcontroller

#endif
