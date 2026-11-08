// MK9 online: a UDP socket for the session's datagrams (Winsock, non-blocking). The peer is set by the joining side
// (mk9_net_join=ip:port) and learned by the host from the first datagram that arrives.

#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

#include "online/transport.h"

namespace mk9::net {

class UdpSocket : public Transport {
 public:
  UdpSocket() = default;
  ~UdpSocket();
  UdpSocket(const UdpSocket&) = delete;
  UdpSocket& operator=(const UdpSocket&) = delete;

  // Binds 0.0.0.0:port (0: any free port). False and a log line on failure.
  bool Open(uint16_t port);
  void Close();
  // "host:port" (a name or an IPv4 address).
  bool SetPeer(const std::string& host_port);
  bool HasPeer() const override { return has_peer_; }
  std::string PeerText() const override;
  uint16_t LocalPort() const { return local_port_; }

  bool Send(const void* data, size_t size) override;
  // One datagram from the peer (or, while no peer is set, from anyone, who then becomes the peer): its size, or -1
  // when nothing is waiting.
  int Receive(void* buffer, size_t capacity) override;
  // Waits up to timeout_ms for a datagram to arrive.
  bool WaitReadable(int timeout_ms) override;

 private:
  uintptr_t socket_ = ~uintptr_t(0);
  bool has_peer_ = false;
  uint8_t peer_[16] = {};  // sockaddr_in
  uint16_t local_port_ = 0;
};

}  // namespace mk9::net
