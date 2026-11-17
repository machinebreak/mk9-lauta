// MK9 online: the two players' own path (ICE, libjuice): straight between their PCs through their routers when it
// can (a local address, the public one STUN finds, a port the NATs open to each other), through the lobby's TURN relay
// when not. The handshake goes through the room's relay in the lobby (lobby.h, sig): the guest offers its description
// (again every second until the host answers: both may be starting up), the host answers with its own, and each sends
// its candidates as it finds them. The session's datagrams then go over the path ICE picked (transport.h).
//
// mk9-lauta (2026-11-17): a session of up to four machines has a path between each two of them. Every relayed message
// says its sender's lobby id ("from"; the host's is empty, as a message without it), and one thread hands each to the
// path it is for. Of two guests, the later one in the room offers, as a guest does to the host.
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
  // mk9-lauta (2026-11-17): the path to one machine of a session of more than two: this side's lobby id and the other's
  // (the host's: empty), and whether this side offers (the later machine in the room's order does).
  static std::unique_ptr<IceLink> CreatePair(const std::string& room, const std::string& local_id,
                                             const std::string& remote_id, bool offers);
  ~IceLink() override;

  bool HasPeer() const override;     // the path is up
  std::string PeerText() const override;
  bool Send(const void* data, size_t size) override;
  int Receive(void* buffer, size_t capacity) override;
  uint64_t LastArrivalUs() const override;
  bool Relayed() const override;
  bool WaitReadable(int timeout_ms) override;
  // The guest's side: a new exchange (a new agent and offer; the host answers it with a new agent too).
  bool Restart() override;

  bool Failed() const;
  struct Impl;

 private:
  explicit IceLink(std::unique_ptr<Impl> impl);
  std::unique_ptr<Impl> impl_;
};

}  // namespace mk9::net
