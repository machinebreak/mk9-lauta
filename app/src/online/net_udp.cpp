// MK9 online: the session's UDP socket (net_udp.h).

#include "online/net_udp.h"

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#else
// mk9-lauta (2026-11-13): Linux: POSIX sockets, under Winsock's names where they are the same calls
#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
using SOCKET = int;
constexpr int INVALID_SOCKET = -1;
inline int closesocket(int s) { return ::close(s); }
inline int WSAGetLastError() { return errno; }
#endif

#include <cstring>
#include <mutex>

#include <rex/logging.h>

#if defined(_WIN32)
#pragma comment(lib, "ws2_32.lib")
#endif

namespace mk9::net {
namespace {

bool StartWinsock() {
#if defined(_WIN32)
  static const bool ok = [] {
    WSADATA data;
    return WSAStartup(MAKEWORD(2, 2), &data) == 0;
  }();
  return ok;
#else
  return true;
#endif
}

}  // namespace

UdpSocket::~UdpSocket() { Close(); }

bool UdpSocket::Open(uint16_t port) {
  if (!StartWinsock()) {
    REXLOG_ERROR("[net] Winsock did not start");
    return false;
  }
  SOCKET s = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
  if (s == INVALID_SOCKET) {
    REXLOG_ERROR("[net] socket: error {}", WSAGetLastError());
    return false;
  }
#if defined(_WIN32)
  u_long nonblocking = 1;
  ioctlsocket(s, FIONBIO, &nonblocking);
  // A datagram the peer could not take must not end the socket (Windows reports ICMP port unreachable as an error on
  // the next receive): SIO_UDP_CONNRESET off.
  BOOL off = FALSE;
  DWORD returned = 0;
  WSAIoctl(s, _WSAIOW(IOC_VENDOR, 12), &off, sizeof(off), nullptr, 0, &returned, nullptr, nullptr);
#else
  fcntl(s, F_SETFL, fcntl(s, F_GETFL, 0) | O_NONBLOCK);
  fcntl(s, F_SETFD, FD_CLOEXEC);  // not into a game started from this one
#endif
  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_ANY);
  addr.sin_port = htons(port);
  if (::bind(s, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
    REXLOG_ERROR("[net] bind to port {}: error {}", port, WSAGetLastError());
    closesocket(s);
    return false;
  }
#if defined(_WIN32)
  int len = sizeof(addr);
#else
  socklen_t len = sizeof(addr);
#endif
  getsockname(s, reinterpret_cast<sockaddr*>(&addr), &len);
  local_port_ = ntohs(addr.sin_port);
  socket_ = static_cast<uintptr_t>(s);
  return true;
}

void UdpSocket::Close() {
  if (socket_ != ~uintptr_t(0)) {
    closesocket(static_cast<SOCKET>(socket_));
    socket_ = ~uintptr_t(0);
  }
  has_peer_ = false;
}

bool UdpSocket::SetPeer(const std::string& host_port) {
  if (!StartWinsock()) return false;
  const size_t colon = host_port.rfind(':');
  if (colon == std::string::npos) return false;
  const std::string host = host_port.substr(0, colon), port = host_port.substr(colon + 1);
  addrinfo hints{}, *result = nullptr;
  hints.ai_family = AF_INET;
  hints.ai_socktype = SOCK_DGRAM;
  if (getaddrinfo(host.c_str(), port.c_str(), &hints, &result) != 0 || !result) {
    REXLOG_ERROR("[net] cannot resolve {}", host_port);
    return false;
  }
  std::memcpy(peer_, result->ai_addr, sizeof(sockaddr_in));
  freeaddrinfo(result);
  has_peer_ = true;
  return true;
}

std::string UdpSocket::PeerText() const {
  if (!has_peer_) return "(none)";
  const auto* a = reinterpret_cast<const sockaddr_in*>(peer_);
  char text[64];
  inet_ntop(AF_INET, &a->sin_addr, text, sizeof(text));
  return std::string(text) + ":" + std::to_string(ntohs(a->sin_port));
}

bool UdpSocket::Send(const void* data, size_t size) {
  if (socket_ == ~uintptr_t(0) || !has_peer_) return false;
  const int sent = ::sendto(static_cast<SOCKET>(socket_), static_cast<const char*>(data), int(size), 0,
                            reinterpret_cast<const sockaddr*>(peer_), sizeof(sockaddr_in));
  return sent == int(size);
}

int UdpSocket::Receive(void* buffer, size_t capacity) {
  if (socket_ == ~uintptr_t(0)) return -1;
  for (;;) {
    sockaddr_in from{};
#if defined(_WIN32)
    int len = sizeof(from);
#else
    socklen_t len = sizeof(from);
#endif
    const int got = ::recvfrom(static_cast<SOCKET>(socket_), static_cast<char*>(buffer), int(capacity), 0,
                               reinterpret_cast<sockaddr*>(&from), &len);
    if (got < 0) return -1;
    if (!has_peer_) {
      std::memcpy(peer_, &from, sizeof(from));
      has_peer_ = true;
      return got;
    }
    const auto* peer = reinterpret_cast<const sockaddr_in*>(peer_);
    if (from.sin_addr.s_addr == peer->sin_addr.s_addr && from.sin_port == peer->sin_port) return got;
    // someone else's datagram: dropped, the next one may be ours
  }
}

bool UdpSocket::WaitReadable(int timeout_ms) {
  if (socket_ == ~uintptr_t(0)) return false;
  fd_set set;
  FD_ZERO(&set);
  FD_SET(static_cast<SOCKET>(socket_), &set);
  timeval tv{timeout_ms / 1000, (timeout_ms % 1000) * 1000};
#if defined(_WIN32)
  return ::select(0, &set, nullptr, nullptr, &tv) > 0;
#else
  return ::select(static_cast<SOCKET>(socket_) + 1, &set, nullptr, nullptr, &tv) > 0;
#endif
}

}  // namespace mk9::net
