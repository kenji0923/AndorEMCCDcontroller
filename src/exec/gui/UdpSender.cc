/**
 * @file UdpSender.cc
 * @brief Implementation of AndorEMCCDcontroller::UdpSender.
 */

#include "UdpSender.h"

#include <cstring>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace AndorEMCCDcontroller {

namespace {

#ifdef _WIN32
using SocketHandle = SOCKET;
#else
using SocketHandle = int;
#endif

#ifdef _WIN32
struct WinsockInit {
  WinsockInit() {
    WSADATA data;
    WSAStartup(MAKEWORD(2, 2), &data);
  }
  ~WinsockInit() { WSACleanup(); }
};
#endif

}  // namespace

UdpSender::UdpSender() {
#ifdef _WIN32
  static WinsockInit init;
#endif
  static_assert(sizeof(address_) >= sizeof(sockaddr_in), "address too small");
}

UdpSender::~UdpSender() { Close(); }

void UdpSender::Close() {
  if (socket_ < 0) return;
#ifdef _WIN32
  closesocket(static_cast<SOCKET>(socket_));
#else
  close(static_cast<int>(socket_));
#endif
  socket_ = -1;
}

bool UdpSender::SetTarget(const std::string& host, std::uint16_t port,
                          std::string* error) {
  has_target_ = false;
  addrinfo hints{};
  hints.ai_family = AF_INET;
  hints.ai_socktype = SOCK_DGRAM;
  addrinfo* result = nullptr;
  if (getaddrinfo(host.c_str(), nullptr, &hints, &result) != 0 || !result) {
    *error = "cannot resolve " + host;
    return false;
  }
  sockaddr_in addr{};
  std::memcpy(&addr, result->ai_addr, sizeof(addr));
  freeaddrinfo(result);
  addr.sin_port = htons(port);
  std::memcpy(address_, &addr, sizeof(addr));

  if (socket_ < 0) {
    socket_ = static_cast<std::intptr_t>(socket(AF_INET, SOCK_DGRAM, 0));
    if (socket_ < 0) {
      *error = "cannot create UDP socket";
      return false;
    }
    // Allow sending to a broadcast address (e.g. 192.168.1.255).
    int yes = 1;
    setsockopt(static_cast<SocketHandle>(socket_), SOL_SOCKET,
               SO_BROADCAST, reinterpret_cast<const char*>(&yes), sizeof(yes));
  }
  has_target_ = true;
  return true;
}

bool UdpSender::Send(const std::string& payload) {
  if (!has_target_ || socket_ < 0) return false;
  const auto sent = sendto(
      static_cast<SocketHandle>(socket_), payload.data(),
      static_cast<int>(payload.size()), 0,
      reinterpret_cast<const sockaddr*>(address_), sizeof(sockaddr_in));
  return sent >= 0 && static_cast<std::size_t>(sent) == payload.size();
}

}  // namespace AndorEMCCDcontroller
