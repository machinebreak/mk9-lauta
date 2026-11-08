// MK9 online: the two players' own path (ICE, libjuice): straight between their PCs through their routers when it
// can (a local address, the public one STUN finds, a port the NATs open to each other), through the lobby's TURN relay
// when not. The handshake goes through the room's relay in the lobby (lobby.h, sig): the guest offers its description
// (again every second until the host answers: both may be starting up), the host answers with its own, and each sends
// its candidates as it finds them. The session's datagrams then go over the path ICE picked (transport.h).
//
//   mk9_ice_relay_only=<bool>   tests: take only TURN's relayed path

#pragma once

#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "online/transport.h"

namespace mk9::net {

class IceLink : public Transport {
 public:
  // room: the room's code; host: this side hosts it; id: the guest's id (on the guest's side; the host learns it).
  static std::unique_ptr<IceLink> Create(const std::string& room, bool host, const std::string& id);
  ~IceLink() override;

  bool HasPeer() const override;     // the path is up
  std::string PeerText() const override;
  bool Send(const void* data, size_t size) override;
  int Receive(void* buffer, size_t capacity) override;
  bool WaitReadable(int timeout_ms) override;

  bool Failed() const;
  struct Impl;

 private:
  explicit IceLink(std::unique_ptr<Impl> impl);
  std::unique_ptr<Impl> impl_;
};

}  // namespace mk9::net
