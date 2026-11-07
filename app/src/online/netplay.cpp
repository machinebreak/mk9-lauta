// MK9 online: the lockstep session (netplay.h).
//
//   mk9_net_host=<port>        open a session on this UDP port and wait for the other player (player 1)
//   mk9_net_join=<host:port>   join a session (player 2)
//   mk9_net_delay=<frames>     input delay (the host's counts for both)
//   mk9_net_sync_ranges=<lo-hi,...>   guest ranges that also go into the sync hash, every mk9_net_sync_every frames
//
// The session starts at the first frame boundary after both sides heard each other (each blocks there until then):
// epoch 0. A barrier (Barrier(): a game event both machines reach, such as a fight's start once it has loaded) starts
// the next epoch, the way the game's own online waits for both players and a "go" before a fight: each machine
// reseeds the game's random number generators there with the epoch's seed (seed.h) and, at the next frame boundary,
// waits until the other has reached it too. The one that gets there first stops; the other plays on to its barrier
// with neutral pads for both players (loading frames: what either player presses there counts for nothing). Then
// both count frames from 0 again, so an epoch's frame N is the same game moment on both machines even when they took
// a different number of frames to load.
//
// Each frame: the local pad (InputSystem user 0: the controller, the keyboard or the test script) goes in at frame
// s + delay with this machine's busy flags at that moment (netplay.h: a movie playing, ...) and its ready flags (the
// readiness polls that said ready in the frame that just ended; a frame without a poll says not ready), every local
// input the
// peer has not acknowledged goes out in one datagram, and the frame waits until the peer's input for s is here. A busy
// poll during frame s then answers busy while this machine is, or while either machine was when frame s's inputs left
// (s - delay): when it stops being busy on both, both see it stop at the same frame; a readiness poll during frame s
// answers ready only if both machines' ready flags that left with frame s's inputs say so. Packets (little-endian):
// u16 'M9', u8 version, u8 type, then
//   HELLO    u8 player, u8 delay, u32 build, u32 seed (the host's: the generators start from it, seed.h)
//   INPUT    u32 epoch, u32 ack (the peer's last frame received in order, +1), u32 first, u8 count, count x 14 bytes
//            (the guest's X_INPUT_GAMEPAD, big-endian, then the busy and the ready flags), u32 hash frame (~0: none),
//            u64 hash
//   BARRIER  u32 epoch (the one starting there), u32 the sender's last frame of the epoch before
//   PING / PONG  u64 microseconds (the sender's clock, echoed)
//   BYE
// The sync hash of a frame: the game's own checks during it (OnCheck: what its online compared, a draw of its random
// number generator among them) and the mk9_net_sync_ranges after it. A frame whose hashes differ is reported.

#include "online/netplay.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <random>
#include <string>
#include <thread>
#include <vector>

#include <fmt/format.h>
#include <rex/cvar.h>
#include <rex/input/input.h>
#include <rex/input/input_system.h>
#include <rex/logging.h>
#include <rex/runtime.h>
#include <rex/system/kernel_state.h>

#include "online/net_udp.h"
#include "online/seed.h"

#define XXH_INLINE_ALL
#include <xxhash.h>

REXCVAR_DEFINE_INT32(mk9_net_host, 0, "MK9 Online", "Host a two-player session on this UDP port (0 = off)");
REXCVAR_DEFINE_STRING(mk9_net_join, "", "MK9 Online", "Join a two-player session at host:port");
REXCVAR_DEFINE_INT32(mk9_net_delay, 2, "MK9 Online", "Input delay in frames (the host's is used by both)");
REXCVAR_DEFINE_INT32(mk9_net_timeout_s, 20, "MK9 Online", "Seconds without the peer before the session ends");
REXCVAR_DEFINE_STRING(mk9_net_sync_ranges, "", "MK9 Online",
                      "Guest ranges (hex lo-hi, comma-separated) that also go into the sync hash both players compare");
REXCVAR_DEFINE_INT32(mk9_net_sync_every, 1, "MK9 Online", "Hash mk9_net_sync_ranges every this many frames");

namespace mk9::netplay {
namespace {

constexpr uint16_t kMagic = 0x394D;  // "M9"
constexpr uint8_t kVersion = 5;
constexpr uint32_t kBuild = 1;
enum : uint8_t { kHello = 1, kInput = 2, kPing = 3, kPong = 4, kBye = 5, kBarrier = 6 };
constexpr int kMaxPerPacket = 64;
constexpr uint32_t kNoHash = 0xFFFFFFFFu;
constexpr uint32_t kErrorDeviceNotConnected = 0x48F;

using Clock = std::chrono::steady_clock;
uint64_t NowUs() {
  return uint64_t(std::chrono::duration_cast<std::chrono::microseconds>(Clock::now().time_since_epoch()).count());
}

struct Pad {
  uint8_t bytes[12] = {};  // X_INPUT_GAMEPAD as the guest stores it (big-endian)
  uint8_t busy = 0;        // BusyFlag bits of the machine when it was read
  uint8_t ready = 0;       // ReadyFlag bits: the polls that said ready in the frame before
  bool operator==(const Pad& o) const { return std::memcmp(bytes, o.bytes, 12) == 0; }
};
constexpr int kPadBytes = 14;

struct Session {
  bool configured = false, failed = false, started = false, ended = false;
  bool host = false;
  int player = 0;  // 0 = player 1 (the host), 1 = player 2
  int delay = 2;
  net::UdpSocket socket;
  bool heard_peer = false, peer_hello = false;
  int peer_delay = -1;
  uint32_t seed = 0;                 // the host's
  int64_t peer_seed = -1;
  bool seed_pending = false;         // for TakeSessionSeed (epoch 0; barriers reseed on the spot)
  // epochs
  uint32_t epoch = 0;
  bool at_barrier = false;           // this side reached the next epoch's barrier during the frame running
  uint32_t peer_barrier = 0;         // the newest epoch the peer has reached the barrier of
  int64_t peer_last_frame = -1;      // ... and its last frame of the epoch before (later remote pads: neutral)
  uint64_t start_game_frame = 0;     // the game frame of this epoch's frame 0
  int64_t frame = 0;                 // the epoch's frame being started
  std::vector<Pad> local, remote;    // by frame
  std::vector<uint8_t> have_remote;
  int64_t remote_upto = -1;          // every remote frame <= this is here
  int64_t peer_acked = -1;           // the peer has every local frame <= this
  std::vector<uint64_t> local_hash, remote_hash;  // by frame; 0 = not known yet
  std::vector<std::pair<uint32_t, uint32_t>> sync_ranges;
  uint64_t check_hash = 0;           // this frame's checks so far
  uint32_t checks = 0;
  int64_t desync_at = -1;            // the epoch's first frame whose hashes differed
  uint64_t desync_frames = 0;
  Pad frame_pads[2];                 // this frame's players' pads
  uint8_t frame_busy = 0;            // either machine's busy flags when this frame's inputs left
  uint32_t busy_value[8] = {};       // the last busy answer of each flag's poll here (what it says while busy)
  uint8_t frame_ready = 0;           // ready on both machines in the frame before this frame's inputs left
  uint8_t ready_seen = 0;            // the readiness polls that said ready during the frame running
  uint32_t ready_value[8] = {};      // the last ready answer of each flag's poll here
  std::thread::id game_thread;
  uint32_t packet_number[2] = {};
  Pad last_pad[2];
  // statistics
  uint64_t last_heard_us = 0, last_sent_us = 0, last_ping_us = 0, log_at_us = 0;
  double rtt_ms = 0, wait_ms_window = 0, wait_ms_max = 0;
  uint64_t frames_window = 0, stalls_window = 0, sent = 0, received = 0;
};

Session g;

void Put16(std::vector<uint8_t>& out, uint16_t v) { out.insert(out.end(), {uint8_t(v), uint8_t(v >> 8)}); }
void Put32(std::vector<uint8_t>& out, uint32_t v) {
  for (int i = 0; i < 4; ++i) out.push_back(uint8_t(v >> (8 * i)));
}
void Put64(std::vector<uint8_t>& out, uint64_t v) {
  for (int i = 0; i < 8; ++i) out.push_back(uint8_t(v >> (8 * i)));
}
uint32_t Get32(const uint8_t* p) { return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24; }
uint64_t Get64(const uint8_t* p) { return uint64_t(Get32(p)) | uint64_t(Get32(p + 4)) << 32; }

std::vector<uint8_t> Header(uint8_t type) {
  std::vector<uint8_t> out;
  Put16(out, kMagic);
  out.push_back(kVersion);
  out.push_back(type);
  return out;
}

uint64_t TimeoutUs() { return uint64_t(std::max(5, REXCVAR_GET(mk9_net_timeout_s))) * 1000000ull; }

// An epoch's seed: the host's for epoch 0, then an integer hash of it and the epoch (the same on both machines).
uint32_t EpochSeed(uint32_t epoch) {
  if (epoch == 0) return g.seed;
  uint32_t x = g.seed + 0x9E3779B9u * epoch + 0x632BE5ABu;
  x ^= x >> 16;
  x *= 0x7FEB352Du;
  x ^= x >> 15;
  x *= 0x846CA68Bu;
  x ^= x >> 16;
  return x;
}

void Ensure(int64_t frame) {
  const size_t need = size_t(frame + 1);
  if (g.local.size() < need) {
    const size_t grow = std::max(need, g.local.size() * 2 + 256);
    g.local.resize(grow);
    g.remote.resize(grow);
    g.have_remote.resize(grow, 0);
    g.local_hash.resize(grow, 0);
    g.remote_hash.resize(grow, 0);
  }
}

// Both sides' sync hashes of a frame, once both are here.
void CompareHashes(int64_t frame) {
  const uint64_t local = g.local_hash[size_t(frame)], remote = g.remote_hash[size_t(frame)];
  if (!local || !remote || local == remote) return;
  if (g.desync_at < 0) g.desync_at = frame;
  if (++g.desync_frames <= 10) {
    REXLOG_ERROR("[net] DESYNC in epoch {} at frame {} (game frame {}): local {:016x}, peer {:016x}", g.epoch, frame,
                 g.start_game_frame + frame, local, remote);
  }
}

void SendHello() {
  auto p = Header(kHello);
  p.push_back(uint8_t(g.player));
  p.push_back(uint8_t(g.delay));
  Put32(p, kBuild);
  Put32(p, g.seed);
  g.socket.Send(p.data(), p.size());
  g.last_sent_us = NowUs();
}

void SendInputs() {
  const int64_t newest = g.frame + g.delay;
  int64_t first = std::max<int64_t>(g.peer_acked + 1, 0);
  first = std::max<int64_t>(first, newest - kMaxPerPacket + 1);
  auto p = Header(kInput);
  Put32(p, g.epoch);
  Put32(p, uint32_t(g.remote_upto + 1));
  Put32(p, uint32_t(first));
  const int count = int(std::max<int64_t>(0, newest - first + 1));
  p.push_back(uint8_t(count));
  for (int64_t f = first; f < first + count; ++f) {
    p.insert(p.end(), g.local[size_t(f)].bytes, g.local[size_t(f)].bytes + 12);
    p.push_back(g.local[size_t(f)].busy);
    p.push_back(g.local[size_t(f)].ready);
  }
  // the newest sync hash this side has: the frame before g.frame
  if (g.frame > 0 && g.local_hash[size_t(g.frame - 1)]) {
    Put32(p, uint32_t(g.frame - 1));
    Put64(p, g.local_hash[size_t(g.frame - 1)]);
  } else {
    Put32(p, kNoHash);
    Put64(p, 0);
  }
  g.socket.Send(p.data(), p.size());
  g.last_sent_us = NowUs();
  ++g.sent;
}

void SendBarrier(uint32_t epoch, int64_t last_frame) {
  auto p = Header(kBarrier);
  Put32(p, epoch);
  Put32(p, uint32_t(last_frame));
  g.socket.Send(p.data(), p.size());
  g.last_sent_us = NowUs();
}

void Receive() {
  uint8_t buf[2048];
  for (;;) {
    const int n = g.socket.Receive(buf, sizeof(buf));
    if (n < 4) {
      if (n < 0) return;
      continue;
    }
    if ((buf[0] | buf[1] << 8) != kMagic || buf[2] != kVersion) continue;
    g.heard_peer = true;
    g.last_heard_us = NowUs();
    ++g.received;
    const uint8_t type = buf[3];
    const uint8_t* p = buf + 4;
    const int left = n - 4;
    if (type == kHello && left >= 10) {
      g.peer_hello = true;
      g.peer_delay = p[1];
      g.peer_seed = Get32(p + 6);
    } else if (type == kInput && left >= 13) {
      const uint32_t epoch = Get32(p);
      if (epoch != g.epoch) {
        // the peer is past a barrier this side has not finished yet: it got there
        if (epoch > g.epoch) g.peer_barrier = std::max(g.peer_barrier, epoch);
        continue;
      }
      p += 4;
      const int64_t ack = int64_t(Get32(p)) - 1;
      const int64_t first = Get32(p + 4);
      const int count = p[8];
      if (left < 13 + count * kPadBytes + 12) continue;
      g.peer_acked = std::max(g.peer_acked, ack);
      for (int i = 0; i < count; ++i) {
        const int64_t f = first + i;
        Ensure(f);
        if (!g.have_remote[size_t(f)]) {
          std::memcpy(g.remote[size_t(f)].bytes, p + 9 + i * kPadBytes, 12);
          g.remote[size_t(f)].busy = p[9 + i * kPadBytes + 12];
          g.remote[size_t(f)].ready = p[9 + i * kPadBytes + 13];
          g.have_remote[size_t(f)] = 1;
        }
      }
      while (size_t(g.remote_upto + 1) < g.have_remote.size() && g.have_remote[size_t(g.remote_upto + 1)]) {
        ++g.remote_upto;
      }
      const uint8_t* h = p + 9 + count * kPadBytes;
      const uint32_t hash_frame = Get32(h);
      if (hash_frame != kNoHash && Get64(h + 4) != 0) {
        Ensure(hash_frame);
        if (!g.remote_hash[hash_frame]) {
          g.remote_hash[hash_frame] = Get64(h + 4);
          CompareHashes(hash_frame);
        }
      }
    } else if (type == kBarrier && left >= 8) {
      const uint32_t epoch = Get32(p);
      if (epoch == g.epoch + 1 && g.peer_barrier < epoch) {
        g.peer_barrier = epoch;
        g.peer_last_frame = Get32(p + 4);
        REXLOG_INFO("[net] the other player is at the barrier of epoch {} (its last frame {}); this side is at frame {}",
                    epoch, g.peer_last_frame, g.frame);
      }
    } else if (type == kPing && left >= 8) {
      auto r = Header(kPong);
      r.insert(r.end(), p, p + 8);
      g.socket.Send(r.data(), r.size());
    } else if (type == kPong && left >= 8) {
      const double rtt = (NowUs() - Get64(p)) / 1000.0;
      g.rtt_ms = g.rtt_ms == 0 ? rtt : g.rtt_ms * 0.8 + rtt * 0.2;
    } else if (type == kBye) {
      REXLOG_INFO("[net] the other player left");
      g.ended = true;
    }
  }
}

void ParseRanges() {
  const std::string text = REXCVAR_GET(mk9_net_sync_ranges);
  size_t pos = 0;
  while (pos < text.size()) {
    const size_t end = text.find(',', pos);
    unsigned lo = 0, hi = 0;
    if (std::sscanf(text.substr(pos, end - pos).c_str(), "%x-%x", &lo, &hi) == 2 && hi > lo) {
      g.sync_ranges.emplace_back(lo, hi);
    }
    if (end == std::string::npos) break;
    pos = end + 1;
  }
}

uint32_t Load32(const uint8_t* base, uint32_t addr) {
  const uint8_t* p = base + addr;
  return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3];
}

// This machine's busy flags now. A movie: GFullScreenMovie (0x834246A8) playing (+20) with its flag at 0x834246B4,
// what IsMoviePlaying (sub_829CE600) tests.
uint8_t SampleBusy(const uint8_t* base) {
  uint8_t busy = 0;
  const uint32_t movie = Load32(base, 0x834246A8);
  if (movie >= 0x40000000u && movie < 0xF0000000u && Load32(base, movie + 20) != 0 && Load32(base, 0x834246B4) != 0) {
    busy |= kBusyMovie;
  }
  return busy;
}

Pad ReadLocalPad() {
  Pad pad;
  auto* runtime = REX_KERNEL_STATE() ? REX_KERNEL_STATE()->emulator() : nullptr;
  auto* is = runtime ? static_cast<rex::input::InputSystem*>(runtime->input_system()) : nullptr;
  if (!is) return pad;
  rex::input::X_INPUT_STATE state{};
  if (is->GetState(0, &state) == 0) std::memcpy(pad.bytes, &state.gamepad, 12);
  return pad;
}

bool Configure() {
  g.configured = true;
  const int host_port = REXCVAR_GET(mk9_net_host);
  const std::string join = REXCVAR_GET(mk9_net_join);
  if (host_port <= 0 && join.empty()) return false;
  g.host = host_port > 0;
  g.player = g.host ? 0 : 1;
  g.delay = std::clamp(REXCVAR_GET(mk9_net_delay), 0, 15);
  if (g.host && !seed::Configured(&g.seed)) g.seed = std::random_device{}();
  if (!g.socket.Open(g.host ? uint16_t(host_port) : 0) || (!g.host && !g.socket.SetPeer(join))) {
    g.failed = true;
    return false;
  }
  ParseRanges();
  REXLOG_INFO("[net] {} on UDP port {}{}; delay {} (host decides); {} sync range(s)",
              g.host ? "hosting" : "joining", g.socket.LocalPort(), g.host ? "" : " -> " + join, g.delay,
              g.sync_ranges.size());
  return true;
}

// Blocks until both sides have heard each other; the guest takes the host's delay and seed.
bool Handshake() {
  const uint64_t timeout = TimeoutUs() * 6;
  const uint64_t start = NowUs();
  uint64_t hello_at = 0;
  while (NowUs() - start < timeout) {
    if (g.socket.HasPeer() && NowUs() - hello_at > 100000) {
      SendHello();
      hello_at = NowUs();
    }
    g.socket.WaitReadable(10);
    Receive();
    if (g.heard_peer && (g.host || g.peer_hello)) {
      if (!g.host && g.peer_delay >= 0) g.delay = g.peer_delay;
      if (!g.host && g.peer_seed >= 0) g.seed = uint32_t(g.peer_seed);
      SendHello();  // the side that finished first lets the other finish too
      return true;
    }
  }
  REXLOG_ERROR("[net] nobody answered in {} s: session off", timeout / 1000000);
  return false;
}

// This side reached the next epoch's barrier during its frame g.frame - 1: tells the peer and waits for it there.
bool WaitAtBarrier() {
  const uint32_t next = g.epoch + 1;
  const int64_t last = g.frame - 1;
  const uint64_t start = NowUs();
  uint64_t told_at = 0;
  while (g.peer_barrier < next && !g.ended) {
    if (NowUs() - told_at > 20000) {
      SendBarrier(next, last);
      told_at = NowUs();
    }
    g.socket.WaitReadable(2);
    Receive();
    if (NowUs() - g.last_heard_us > TimeoutUs() || NowUs() - start > TimeoutUs() * 3) {
      REXLOG_ERROR("[net] the other player never reached the barrier of epoch {}: session ended", next);
      g.ended = true;
    }
  }
  SendBarrier(next, last);  // in case the peer has not heard it yet
  const double waited = (NowUs() - start) / 1000.0;
  REXLOG_INFO("[net] barrier of epoch {}: this side waited {:.1f} ms at its frame {}", next, waited, last);
  return !g.ended;
}

void StartEpoch(uint64_t game_frame, uint32_t epoch) {
  g.epoch = epoch;
  g.start_game_frame = game_frame;
  g.frame = 0;
  g.local.clear();
  g.remote.clear();
  g.have_remote.clear();
  g.local_hash.clear();
  g.remote_hash.clear();
  g.remote_upto = -1;
  g.peer_acked = -1;
  g.peer_last_frame = -1;
  g.check_hash = 0;
  g.checks = 0;
  g.desync_at = -1;
  g.desync_frames = 0;
  Ensure(g.delay + 1);
  g.last_heard_us = NowUs();
}

void LogStats() {
  const uint64_t now = NowUs();
  if (now < g.log_at_us) return;
  if (g.log_at_us) {
    REXLOG_INFO("[net] epoch {} frame {} (game {}), rtt {:.1f} ms, delay {}, waited {:.1f} ms in total ({} stalls, "
                "longest {:.1f} ms), peer {} frames ahead; sent {}, received {}{}",
                g.epoch, g.frame, g.start_game_frame + g.frame, g.rtt_ms, g.delay, g.wait_ms_window, g.stalls_window,
                g.wait_ms_max, g.remote_upto - (g.frame - 1 + g.delay), g.sent, g.received,
                g.desync_at >= 0 ? fmt::format("; {} frame(s) out of sync, the first {}", g.desync_frames, g.desync_at)
                                 : "");
  }
  g.wait_ms_window = g.wait_ms_max = 0;
  g.stalls_window = 0;
  g.log_at_us = now + 10'000'000;
}

}  // namespace

bool Active() {
  if (!g.configured) Configure();
  return g.socket.HasPeer() || REXCVAR_GET(mk9_net_host) > 0 ? !g.failed && !g.ended : false;
}

void OnFrame(uint8_t* base, uint64_t game_frame) {
  if (!Active()) return;
  g.game_thread = std::this_thread::get_id();
  if (!g.started) {
    if (!Handshake()) {
      g.failed = true;
      return;
    }
    g.started = true;
    StartEpoch(game_frame, 0);
    g.seed_pending = true;
    REXLOG_INFO("[net] session started at game frame {} with {} as the peer; you are player {}, delay {}, seed {}",
                game_frame, g.socket.PeerText(), g.player + 1, g.delay, g.seed);
  } else if (g.at_barrier) {
    g.at_barrier = false;
    if (!WaitAtBarrier()) return;
    StartEpoch(game_frame, g.epoch + 1);
    REXLOG_INFO("[net] epoch {} started at game frame {}", g.epoch, game_frame);
  }
  // this frame's local input goes in at frame + delay; the first delay frames are neutral on both sides
  Ensure(g.frame + g.delay + 1);
  g.local[size_t(g.frame + g.delay)] = ReadLocalPad();
  g.local[size_t(g.frame + g.delay)].busy = SampleBusy(base);
  g.local[size_t(g.frame + g.delay)].ready = g.ready_seen;
  g.ready_seen = 0;
  // the sync hash of the frame that just ended
  if (g.frame > 0) {
    uint64_t h = XXH3_64bits_withSeed(&g.checks, sizeof(g.checks), g.check_hash);
    if (!g.sync_ranges.empty() && g.frame % std::max(1, REXCVAR_GET(mk9_net_sync_every)) == 0) {
      auto* memory = rex::system::kernel_memory();
      for (const auto& [lo, hi] : g.sync_ranges) {
        h = XXH3_64bits_withSeed(memory->TranslateVirtual<const uint8_t*>(lo), hi - lo, h);
      }
    }
    g.local_hash[size_t(g.frame - 1)] = h ? h : 1;
    CompareHashes(g.frame - 1);
  }
  g.check_hash = 0;
  g.checks = 0;
  SendInputs();
  if (NowUs() - g.last_ping_us > 500000) {
    auto p = Header(kPing);
    Put64(p, NowUs());
    g.socket.Send(p.data(), p.size());
    g.last_ping_us = NowUs();
  }
  // the peer waits at the next barrier and sends nothing past its last frame: this side plays on to its own barrier
  // with neutral pads for both
  Receive();
  const bool gap = g.peer_barrier > g.epoch && g.frame > g.peer_last_frame;
  // lockstep: wait for the peer's input of this frame
  if (!gap && g.remote_upto < g.frame) {
    const uint64_t wait_start = NowUs();
    while (g.remote_upto < g.frame && !g.ended && !(g.peer_barrier > g.epoch && g.frame > g.peer_last_frame)) {
      g.socket.WaitReadable(1);
      Receive();
      if (NowUs() - g.last_sent_us > 4000) SendInputs();
      if (NowUs() - g.last_heard_us > TimeoutUs()) {
        REXLOG_ERROR("[net] no word from the other player for {} s: session ended", TimeoutUs() / 1000000);
        g.ended = true;
      }
    }
    const double waited = (NowUs() - wait_start) / 1000.0;
    g.wait_ms_window += waited;
    g.wait_ms_max = std::max(g.wait_ms_max, waited);
    ++g.stalls_window;
  }
  if (g.ended) return;
  if (g.peer_barrier > g.epoch && g.frame > g.peer_last_frame) {
    g.frame_pads[0] = g.frame_pads[1] = Pad{};
    g.frame_busy = g.local[size_t(g.frame)].busy;
    g.frame_ready = g.local[size_t(g.frame)].ready;
  } else {
    g.frame_pads[g.player] = g.local[size_t(g.frame)];
    g.frame_pads[1 - g.player] = g.remote[size_t(g.frame)];
    g.frame_busy = g.local[size_t(g.frame)].busy | g.remote[size_t(g.frame)].busy;
    g.frame_ready = g.local[size_t(g.frame)].ready & g.remote[size_t(g.frame)].ready;
  }
  for (int p = 0; p < 2; ++p) {
    if (!(g.frame_pads[p] == g.last_pad[p])) {
      ++g.packet_number[p];
      g.last_pad[p] = g.frame_pads[p];
    }
  }
  ++g.frames_window;
  LogStats();
  ++g.frame;
}

uint32_t Busy(BusyFlag flag, uint32_t local) {
  if (!g.started || g.ended || g.failed || std::this_thread::get_id() != g.game_thread) return local;
  int bit = 0;
  while (bit < 7 && !(flag & (1u << bit))) ++bit;
  if (local) {
    g.busy_value[bit] = local;
    return local;
  }
  return (g.frame_busy & flag) ? (g.busy_value[bit] ? g.busy_value[bit] : 1u) : 0u;
}

uint32_t Ready(ReadyFlag flag, uint32_t local) {
  if (!g.started || g.ended || g.failed || std::this_thread::get_id() != g.game_thread) return local;
  int bit = 0;
  while (bit < 7 && !(flag & (1u << bit))) ++bit;
  if (local) {
    g.ready_seen |= flag;
    g.ready_value[bit] = local;
  }
  return (g.frame_ready & flag) ? (g.ready_value[bit] ? g.ready_value[bit] : 1u) : 0u;
}

void Barrier(uint8_t* base, const char* what) {
  if (!g.started || g.ended || g.failed || g.at_barrier) return;
  g.at_barrier = true;
  const uint32_t seed = EpochSeed(g.epoch + 1);
  seed::Start(base, seed);
  REXLOG_INFO("[net] barrier '{}' at frame {} of epoch {} (game frame {}): epoch {} starts from seed {}", what,
              g.frame - 1, g.epoch, g.start_game_frame + g.frame - 1, g.epoch + 1, seed);
}

void OnCheck(uint32_t tag, uint32_t value) {
  if (!g.started || g.ended || g.failed) return;
  const uint32_t pair[2] = {tag, value};
  g.check_hash = XXH3_64bits_withSeed(pair, sizeof(pair), g.check_hash);
  ++g.checks;
}

bool TakeSessionSeed(uint32_t* seed) {
  if (!g.seed_pending) return false;
  g.seed_pending = false;
  *seed = g.seed;
  return true;
}

void OverrideBlock(uint8_t* base, uint32_t tag, uint32_t data, uint32_t size, uint32_t user, uint32_t r1) {
  if (!g.started || g.ended || g.failed) return;
  const Pad* pad = user == 0 ? &g.frame_pads[0] : user == 1 ? &g.frame_pads[1] : nullptr;
  uint8_t* out = base + data;
  auto put_be32 = [](uint8_t* p, uint32_t v) {
    p[0] = uint8_t(v >> 24);
    p[1] = uint8_t(v >> 16);
    p[2] = uint8_t(v >> 8);
    p[3] = uint8_t(v);
  };
  if (tag == 0x00FF0003 && size == 4 && data - r1 < 0x400) {
    // XInputGetState's result (a stack temporary of the gamepad read): connected for the two players
    put_be32(out, pad ? 0 : kErrorDeviceNotConnected);
  } else if (tag == 0x00FF0002 && size == 16) {
    // X_INPUT_STATE: packet number, then the gamepad
    std::memset(out, 0, 16);
    if (pad) {
      put_be32(out, g.packet_number[user]);
      std::memcpy(out + 4, pad->bytes, 12);
    }
  } else if (tag == 0x00FF0002 && size == 20) {
    // X_INPUT_CAPABILITIES of a standard gamepad: type 1, subtype 1, every button and axis, both motors
    std::memset(out, 0, 20);
    if (pad) {
      static const uint8_t caps[20] = {1, 1, 0, 0, 0xF3, 0xFF, 0xFF, 0xFF, 0xFF, 0xC0, 0xFF, 0xC0,
                                       0xFF, 0xC0, 0xFF, 0xC0, 0xFF, 0xFF, 0xFF, 0xFF};
      std::memcpy(out, caps, 20);
    }
  }
}

}  // namespace mk9::netplay
