// MK9 online: the session's datagram transport (netplay.cpp), whatever carries it: a UDP socket to an address
// (net_udp.h: the tests, a LAN, a forwarded port), the internet path the lobby negotiates (ICE: direct, through the
// NATs, or relayed by TURN), and, around either, emulated network conditions for testing rollback on one machine.
//
//   mk9_net_lag_ms=<ms>      added one-way delay of every datagram sent (tests)
//   mk9_net_jitter_ms=<ms>   plus a random 0..jitter on top of it (tests)
//   mk9_net_loss_pct=<pct>   datagrams sent that are dropped (tests)

#pragma once

#include <cstddef>
#include <cstdint>
#include <deque>
#include <memory>
#include <random>
#include <string>
#include <vector>

namespace mk9::net {

class Transport {
 public:
  virtual ~Transport() = default;
  // The peer is known (datagrams can go out).
  virtual bool HasPeer() const = 0;
  virtual std::string PeerText() const = 0;
  virtual bool Send(const void* data, size_t size) = 0;
  // One datagram from the peer: its size, or -1 when nothing is waiting.
  virtual int Receive(void* buffer, size_t capacity) = 0;
  // Waits up to timeout_ms for a datagram (or for anything the transport must do, as a delayed send).
  virtual bool WaitReadable(int timeout_ms) = 0;
  // mk9-lauta (2026-11-10): finds the path again from the start (an ICE exchange anew: a path up that carries nothing);
  // false where there is nothing to redo.
  virtual bool Restart() { return false; }
};

// Emulated network conditions around another transport: each datagram sent is held for the delay (plus jitter, in
// order) and some are dropped. Created by WithTestConditions when the cvars ask for it.
class LaggedTransport : public Transport {
 public:
  LaggedTransport(std::unique_ptr<Transport> inner, int lag_ms, int jitter_ms, int loss_pct, int late_path_ms = 0,
                  int mute_ms = 0);
  // late_path_ms (tests): for that long no path to send on (HasPeer false, sends dropped) while datagrams still come
  // in, as an ICE link before its pair is chosen
  bool HasPeer() const override;
  std::string PeerText() const override;
  bool Send(const void* data, size_t size) override;
  int Receive(void* buffer, size_t capacity) override;
  bool WaitReadable(int timeout_ms) override;
  bool Restart() override { return inner_->Restart(); }

 private:
  struct Held {
    uint64_t due_us;
    std::vector<uint8_t> bytes;
  };
  void Pump();
  std::unique_ptr<Transport> inner_;
  int lag_ms_, jitter_ms_, loss_pct_;
  uint64_t path_from_us_ = 0;
  uint64_t mute_until_us_ = 0;  // tests: sends dropped (the path still up) until then
  std::deque<Held> held_;
  uint64_t last_due_us_ = 0;
  std::mt19937 rng_{12345};
};

// The transport as is, or inside a LaggedTransport when mk9_net_lag_ms / mk9_net_jitter_ms / mk9_net_loss_pct are set.
std::unique_ptr<Transport> WithTestConditions(std::unique_ptr<Transport> transport);

}  // namespace mk9::net
