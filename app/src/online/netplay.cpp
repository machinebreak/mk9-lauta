// MK9 online: the two-player session, in lockstep through menus and loads and with rollback in the fight (netplay.h).
//
//   mk9_net_room=<code>          the session of a lobby's room (lobby.h), over the players' own path (ice_link.h),
//   mk9_net_role=host|guest      as its host (player 1) or its guest (player 2); mk9_online_name: the player's name
//   mk9_net_host=<port>          or: open a session on this UDP port and wait for the other player (player 1; tests,
//   mk9_net_join=<host:port>     a LAN, a forwarded port), or join one (player 2)
//   mk9_net_delay=<frames>       input delay in lockstep (menus, loads); the host's counts for both
//   mk9_net_rollback=<bool>      fights with rollback (else lockstep all along)
//   mk9_net_rollback_delay=<f>   input delay in a fight with rollback (the host's counts for both): 0 = none, every
//                                late input repaired by a rollback; 1-2 trade a little delay for fewer rollbacks
//   mk9_net_sync_ranges=<lo-hi,...>   guest ranges that also go into the sync hash, every mk9_net_sync_every frames
//   transport.h: mk9_net_lag_ms / mk9_net_jitter_ms / mk9_net_loss_pct emulate a network for tests
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
// Lockstep (epoch 0, the menus): each frame, the local pad (InputSystem user 0: the controller, the keyboard or the
// test script) goes in at frame s + delay with this machine's busy flags at that moment (netplay.h: a movie playing,
// ...) and its ready flags (the readiness polls that said ready in the frame that just ended; a frame without a poll
// says not ready), every local input the peer has not acknowledged goes out in one datagram, and the frame waits until
// the peer's input for s is here. A busy poll during frame s then answers busy while this machine is, or while either
// machine was when frame s's inputs left (s - delay): when it stops being busy on both, both see it stop at the same
// frame; a readiness poll during frame s answers ready only if both machines' ready flags that left with frame s's
// inputs say so.
//
// Rollback (an epoch started by a fight's barrier): the simulation's memory is kept frame by frame (snapshot.h,
// Timeline) and a frame no longer waits for the peer: without its input, it runs on a prediction (the peer's last
// input). When the input arrives and differs, the next frame boundary rolls the memory back to that frame's start and
// the frames since run again with the right inputs, hidden (no pacing, no present: sa2_renut_pacing.h), the live frame
// last. The first kLearnFrames of the fight (its intro) still run in lockstep while the timeline learns the pages
// written with no store card; and every frame run so far must be confirmed before the game goes past something a
// rollback cannot undo: its mode changing (the fight over, the game's check tag 8 back to 0) or a package load (they
// finish at a frame's start, loading.h). Once a mode change is confirmed, the epoch goes on in lockstep. A side more
// than kMaxPrediction frames ahead of the peer's inputs waits, and the side ahead in time sleeps a frame now and then
// (time sync). Only final frames' sync hashes are compared: frames whose last run had both players' real inputs.
//
// Packets (little-endian): u16 'M9', u8 version, u8 type, then
//   HELLO    u8 player, u8 delay, u32 build, u32 seed (the host's: the generators start from it, seed.h),
//            u8 rollback (the host's mk9_net_rollback), u8 rollback delay
//   INPUT    u32 epoch, u32 ack (the peer's last frame received in order, +1), u32 first, u8 count, count x 14 bytes
//            (the guest's X_INPUT_GAMEPAD, big-endian, then the busy and the ready flags), u32 hash frame (~0: none),
//            u64 hash (of that final frame), u32 the sender's live frame (time sync)
//   BARRIER  u32 epoch (the one starting there), u32 the sender's last frame of the epoch before
//   PING / PONG  u64 microseconds (the sender's clock, echoed)
//   BYE
// The sync hash of a frame: the game's own checks during it (OnCheck: what its online compared, a draw of its random
// number generator among them) and the mk9_net_sync_ranges after it. A frame whose hashes differ is reported.

#include "online/netplay.h"

#include <algorithm>
#include <atomic>
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
#include <rex/ppc/context.h>
#include <rex/runtime.h>
#include <rex/system/kernel_state.h>

#include "online/ice_link.h"
#include "online/lobby.h"
#include "online/net_udp.h"
#include "online/quiesce.h"
#include "online/seed.h"
#include "online/sound_memory.h"
#include "online/snapshot.h"
#include "online/transport.h"
#include "profiler/sa2_cpu_profile_pc.h"
#include "renut_engine/gpu/sa2_renut_pacing.h"

#define XXH_INLINE_ALL
#include <xxhash.h>

REXCVAR_DEFINE_STRING(mk9_net_room, "", "MK9 Online", "The lobby room whose session this is (empty: none)");
REXCVAR_DEFINE_STRING(mk9_net_role, "", "MK9 Online", "mk9_net_room: this side is its host or its guest");
REXCVAR_DEFINE_STRING(mk9_online_name, "", "MK9 Online", "The player's name in the lobby");
REXCVAR_DEFINE_INT32(mk9_net_host, 0, "MK9 Online", "Host a two-player session on this UDP port (0 = off)");
REXCVAR_DEFINE_STRING(mk9_net_join, "", "MK9 Online", "Join a two-player session at host:port");
REXCVAR_DEFINE_INT32(mk9_net_delay, 2, "MK9 Online",
                     "Input delay in frames in lockstep (menus, loads; the host's is used by both)");
REXCVAR_DEFINE_BOOL(mk9_net_rollback, true, "MK9 Online", "Fights with rollback (the host's is used by both)");
REXCVAR_DEFINE_INT32(mk9_net_rollback_delay, 1, "MK9 Online",
                     "Input delay in frames in a fight with rollback (0 = none; the host's is used by both)");
REXCVAR_DEFINE_INT32(mk9_net_timeout_s, 20, "MK9 Online", "Seconds without the peer before the session ends");
REXCVAR_DEFINE_STRING(mk9_net_sync_ranges, "", "MK9 Online",
                      "Guest ranges (hex lo-hi, comma-separated) that also go into the sync hash both players compare");
REXCVAR_DEFINE_INT32(mk9_net_sync_every, 1, "MK9 Online", "Hash mk9_net_sync_ranges every this many frames");

namespace mk9::netplay {
namespace {

constexpr uint16_t kMagic = 0x394D;  // "M9"
constexpr uint8_t kVersion = 6;
constexpr uint32_t kBuild = 1;
enum : uint8_t { kHello = 1, kInput = 2, kPing = 3, kPong = 4, kBye = 5, kBarrier = 6 };
constexpr int kMaxPerPacket = 64;
constexpr uint32_t kNoHash = 0xFFFFFFFFu;
constexpr uint32_t kErrorDeviceNotConnected = 0x48F;
constexpr int64_t kLearnFrames = 120;   // a fight's first frames, in lockstep while the timeline learns its pages
constexpr int64_t kMaxPrediction = 8;   // frames run on predictions at most
constexpr uint32_t kAsyncPackagesNum = 0x834162A4 + 4;  // GObjAsyncPackages.Num (loading.h)

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
  std::string room;  // a lobby room's session (else a direct UDP one)
  int player = 0;  // 0 = player 1 (the host), 1 = player 2
  int lockstep_delay = 2, rollback_delay = 1;
  bool rollback_wanted = true;
  int delay = 2;                     // this epoch's
  std::unique_ptr<net::Transport> link;
  uint16_t local_port = 0;
  bool heard_peer = false, peer_hello = false;
  int peer_delay = -1, peer_rollback = -1, peer_rollback_delay = -1;
  uint32_t seed = 0;                 // the host's
  int64_t peer_seed = -1;
  bool seed_pending = false;         // for TakeSessionSeed (epoch 0; barriers reseed on the spot)
  // epochs
  uint32_t epoch = 0;
  bool at_barrier = false;           // this side reached the next epoch's barrier during the frame running
  uint32_t peer_barrier = 0;         // the newest epoch the peer has reached the barrier of
  int64_t peer_last_frame = -1;      // ... and its last frame of the epoch before (later remote pads: neutral)
  uint64_t start_game_frame = 0;     // the game frame of this epoch's frame 0
  int64_t frame = 0;                 // the epoch's frame about to run
  std::vector<Pad> local, remote;    // by frame
  std::vector<uint8_t> have_remote;
  int64_t remote_upto = -1;          // every remote frame <= this is here
  int64_t peer_acked = -1;           // the peer has every local frame <= this
  std::vector<uint64_t> local_hash, remote_hash;  // by frame; 0 = not known yet
  int64_t compared_upto = -1;        // final frames compared so far
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
  // the pads each frame ran with and their packet numbers (X_INPUT_STATE's, counted per change): by frame, so a frame
  // run again gets the same ones
  std::vector<Pad> used[2];
  std::vector<uint32_t> packet[2];
  uint32_t packet_base[2] = {};      // the packet numbers at the epoch's start
  // rollback (a fight's epoch)
  bool rollback = false;             // this epoch runs with rollback
  bool speculating = false;          // past the learning frames: frames may run on predicted inputs
  snapshot::Timeline timeline;
  int64_t latest = 0;                // the live edge: the newest frame started + 1 (frame < latest while replaying)
  int64_t rollback_to = -1;          // the earliest frame that ran on a prediction that turned out wrong
  int64_t mode_change_at = -1;       // a frame whose checks showed the game's mode change (tag 8 back to 0)
  int64_t peer_latest = -1;          // the peer's live frame, from its packets
  double advantage = 0;              // time sync: frames ahead of the peer, averaged
  uint64_t slept_at_frame = 0;
  // statistics
  uint64_t last_heard_us = 0, last_sent_us = 0, last_ping_us = 0, log_at_us = 0;
  double rtt_ms = 0, wait_ms_window = 0, wait_ms_max = 0;
  uint64_t frames_window = 0, stalls_window = 0, sent = 0, received = 0;
  uint64_t rollbacks = 0, resimulated = 0, max_depth = 0, predicted = 0, mispredicted = 0, syncs_slept = 0;
  double ms_keep = 0, ms_restore = 0;
  uint64_t keeps = 0;
};

Session g;

// A session whose game thread stops starting frames for 3 s gets every thread's stack dumped once (logs/pilas_<pid>_N.txt,
// tools/online/stacks_report.py): a hang's evidence.
std::atomic<uint64_t> g_frame_started_us{0};

void WatchFrames() {
  static const bool started = [] {
    std::thread([] {
      bool dumped = false;
      for (;;) {
        std::this_thread::sleep_for(std::chrono::milliseconds(250));
        const uint64_t at = g_frame_started_us.load();
        const uint64_t now = uint64_t(std::chrono::duration_cast<std::chrono::microseconds>(
                                          std::chrono::steady_clock::now().time_since_epoch())
                                          .count());
        if (at && !dumped && now - at > 3000000) {
          dumped = true;
          REXLOG_ERROR("[net] no frame started for {} ms: every thread's stack goes to logs/pilas_<pid>_N.txt", (now - at) / 1000);
          nfsmw::perfil_pc::VolcarPilas("netplay: the game thread stopped");
        }
      }
    }).detach();
    return true;
  }();
  (void)started;
}

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

uint32_t Load32(const uint8_t* base, uint32_t addr) {
  const uint8_t* p = base + addr;
  return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3];
}

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
    for (int p = 0; p < 2; ++p) {
      g.used[p].resize(grow);
      g.packet[p].resize(grow, 0);
    }
  }
}

// The newest frame whose last run had both players' real inputs: every frame up to it is final.
int64_t FinalUpto() {
  int64_t upto = std::min(g.remote_upto, g.frame - 1);
  if (g.rollback_to >= 0) upto = std::min(upto, g.rollback_to - 1);
  return upto;
}

// Both sides' sync hashes of the final frames not compared yet.
void CompareFinalHashes() {
  const int64_t upto = FinalUpto();
  while (g.compared_upto < upto) {
    const int64_t frame = g.compared_upto + 1;
    const uint64_t local = g.local_hash[size_t(frame)], remote = g.remote_hash[size_t(frame)];
    if (!remote) break;  // the peer's hash of it has not come (or the peer has not finished it)
    g.compared_upto = frame;
    if (!local || local == remote) continue;
    if (g.desync_at < 0) g.desync_at = frame;
    if (++g.desync_frames <= 10) {
      REXLOG_ERROR("[net] DESYNC in epoch {} at frame {} (game frame {}): local {:016x}, peer {:016x}", g.epoch,
                   frame, g.start_game_frame + frame, local, remote);
    }
  }
}

void Send(const std::vector<uint8_t>& p) {
  g.link->Send(p.data(), p.size());
  g.last_sent_us = NowUs();
}

void SendHello() {
  auto p = Header(kHello);
  p.push_back(uint8_t(g.player));
  p.push_back(uint8_t(g.lockstep_delay));
  Put32(p, kBuild);
  Put32(p, g.seed);
  p.push_back(uint8_t(g.rollback_wanted ? 1 : 0));
  p.push_back(uint8_t(g.rollback_delay));
  Send(p);
}

void SendInputs() {
  const int64_t newest = (g.rollback ? g.latest : g.frame) + g.delay;
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
  // the newest final frame's sync hash
  const int64_t final_frame = std::min(FinalUpto(), g.frame - 1);
  if (final_frame >= 0 && g.local_hash[size_t(final_frame)]) {
    Put32(p, uint32_t(final_frame));
    Put64(p, g.local_hash[size_t(final_frame)]);
  } else {
    Put32(p, kNoHash);
    Put64(p, 0);
  }
  Put32(p, uint32_t(g.rollback ? g.latest : g.frame));
  Send(p);
  ++g.sent;
}

void SendBarrier(uint32_t epoch, int64_t last_frame) {
  auto p = Header(kBarrier);
  Put32(p, epoch);
  Put32(p, uint32_t(last_frame));
  Send(p);
}

void Receive() {
  uint8_t buf[2048];
  for (;;) {
    const int n = g.link->Receive(buf, sizeof(buf));
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
    if (type == kHello && left >= 12) {
      g.peer_hello = true;
      g.peer_delay = p[1];
      g.peer_seed = Get32(p + 6);
      g.peer_rollback = p[10];
      g.peer_rollback_delay = p[11];
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
      if (left < 13 + count * kPadBytes + 16) continue;
      g.peer_acked = std::max(g.peer_acked, ack);
      for (int i = 0; i < count; ++i) {
        const int64_t f = first + i;
        Ensure(f);
        if (g.have_remote[size_t(f)]) continue;
        Pad& pad = g.remote[size_t(f)];
        std::memcpy(pad.bytes, p + 9 + i * kPadBytes, 12);
        pad.busy = p[9 + i * kPadBytes + 12];
        pad.ready = p[9 + i * kPadBytes + 13];
        g.have_remote[size_t(f)] = 1;
        // a frame that ran on a prediction this input contradicts: the rollback goes back to it
        if (g.rollback && f < g.frame && !(g.used[1 - g.player][size_t(f)] == pad)) {
          ++g.mispredicted;
          g.rollback_to = g.rollback_to < 0 ? f : std::min(g.rollback_to, f);
        }
      }
      while (size_t(g.remote_upto + 1) < g.have_remote.size() && g.have_remote[size_t(g.remote_upto + 1)]) {
        ++g.remote_upto;
      }
      const uint8_t* h = p + 9 + count * kPadBytes;
      const uint32_t hash_frame = Get32(h);
      if (hash_frame != kNoHash && Get64(h + 4) != 0) {
        Ensure(hash_frame);
        g.remote_hash[hash_frame] = Get64(h + 4);
      }
      g.peer_latest = std::max<int64_t>(g.peer_latest, int64_t(Get32(h + 12)));
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
      Send(r);
    } else if (type == kPong && left >= 8) {
      const double rtt = (NowUs() - Get64(p)) / 1000.0;
      g.rtt_ms = g.rtt_ms == 0 ? rtt : g.rtt_ms * 0.8 + rtt * 0.2;
    } else if (type == kBye) {
      REXLOG_INFO("[net] the other player left");
      g.ended = true;
    }
  }
}

void Ping() {
  if (NowUs() - g.last_ping_us > 500000) {
    auto p = Header(kPing);
    Put64(p, NowUs());
    Send(p);
    g.last_ping_us = NowUs();
  }
}

// Waits until done() (receiving, resending this side's inputs every 4 ms) or the peer goes quiet for the timeout.
template <typename Done>
void WaitFor(Done done) {
  if (done()) return;
  const uint64_t wait_start = NowUs();
  while (!done() && !g.ended) {
    g.link->WaitReadable(1);
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

// A lobby room's session: the lobby as the player's name, the room hosted (closed: the guest is in it) or joined, and
// the players' own path through ICE.
bool ConfigureRoom(const std::string& room, const std::string& role) {
  g.host = role == "host";
  if (!g.host && role != "guest") {
    REXLOG_ERROR("[net] mk9_net_role must be host or guest, not '{}'", role);
    g.failed = true;
    return false;
  }
  std::string name = REXCVAR_GET(mk9_online_name);
  if (name.empty()) name = g.host ? "Host" : "Guest";
  lobby::Start(name);
  std::string id;
  if (g.host) {
    lobby::Host(room, name, false, true);
  } else {
    std::random_device rd;
    id = fmt::format("{:08x}{:08x}", rd(), rd());
    lobby::Join(room, id);
  }
  auto link = net::IceLink::Create(room, g.host, id);
  if (!link) {
    REXLOG_ERROR("[net] room {}: the players' path could not be set up", room);
    g.failed = true;
    return false;
  }
  g.link = net::WithTestConditions(std::move(link));
  g.room = room;
  return true;
}

bool Configure() {
  g.configured = true;
  const int host_port = REXCVAR_GET(mk9_net_host);
  const std::string join = REXCVAR_GET(mk9_net_join);
  const std::string room = REXCVAR_GET(mk9_net_room);
  if (host_port <= 0 && join.empty() && room.empty()) return false;
  g.player = 0;
  g.lockstep_delay = std::clamp(REXCVAR_GET(mk9_net_delay), 0, 15);
  g.rollback_delay = std::clamp(REXCVAR_GET(mk9_net_rollback_delay), 0, 6);
  g.rollback_wanted = REXCVAR_GET(mk9_net_rollback);
  g.delay = g.lockstep_delay;
  if (!room.empty()) {
    if (!ConfigureRoom(room, REXCVAR_GET(mk9_net_role))) return false;
    g.player = g.host ? 0 : 1;
    if (g.host && !seed::Configured(&g.seed)) g.seed = std::random_device{}();
    ParseRanges();
    REXLOG_INFO("[net] room {} as its {}; lockstep delay {}, rollback {} (delay {}); the host decides", room,
                g.host ? "host" : "guest", g.lockstep_delay, g.rollback_wanted ? "on" : "off", g.rollback_delay);
    return true;
  }
  g.host = host_port > 0;
  g.player = g.host ? 0 : 1;
  g.lockstep_delay = std::clamp(REXCVAR_GET(mk9_net_delay), 0, 15);
  g.rollback_delay = std::clamp(REXCVAR_GET(mk9_net_rollback_delay), 0, 6);
  g.rollback_wanted = REXCVAR_GET(mk9_net_rollback);
  g.delay = g.lockstep_delay;
  if (g.host && !seed::Configured(&g.seed)) g.seed = std::random_device{}();
  auto udp = std::make_unique<net::UdpSocket>();
  if (!udp->Open(g.host ? uint16_t(host_port) : 0) || (!g.host && !udp->SetPeer(join))) {
    g.failed = true;
    return false;
  }
  g.local_port = udp->LocalPort();
  g.link = net::WithTestConditions(std::move(udp));
  ParseRanges();
  REXLOG_INFO("[net] {} on UDP port {}{}; lockstep delay {}, rollback {} (delay {}); the host decides; {} sync range(s)",
              g.host ? "hosting" : "joining", g.local_port, g.host ? "" : " -> " + join, g.lockstep_delay,
              g.rollback_wanted ? "on" : "off", g.rollback_delay, g.sync_ranges.size());
  return true;
}

// Blocks until both sides have heard each other; the guest takes the host's delays, rollback choice and seed.
bool Handshake() {
  const uint64_t timeout = TimeoutUs() * 6;
  const uint64_t start = NowUs();
  uint64_t hello_at = 0;
  while (NowUs() - start < timeout) {
    if (g.link->HasPeer() && NowUs() - hello_at > 100000) {
      SendHello();
      hello_at = NowUs();
    }
    g.link->WaitReadable(10);
    Receive();
    if (g.heard_peer && (g.host || g.peer_hello)) {
      if (!g.host && g.peer_delay >= 0) {
        g.lockstep_delay = g.peer_delay;
        g.seed = uint32_t(g.peer_seed);
        g.rollback_wanted = g.peer_rollback != 0;
        g.rollback_delay = g.peer_rollback_delay;
      }
      g.delay = g.lockstep_delay;
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
    g.link->WaitReadable(2);
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

void StartEpoch(uint64_t game_frame, uint32_t epoch, bool rollback) {
  // the packet numbers go on from where the last frame left them
  for (int p = 0; p < 2; ++p) {
    if (g.frame > 0 && size_t(g.frame - 1) < g.packet[p].size()) g.packet_base[p] = g.packet[p][size_t(g.frame - 1)];
  }
  g.epoch = epoch;
  g.start_game_frame = game_frame;
  g.frame = 0;
  g.latest = 0;
  g.local.clear();
  g.remote.clear();
  g.have_remote.clear();
  g.local_hash.clear();
  g.remote_hash.clear();
  for (int p = 0; p < 2; ++p) {
    g.used[p].clear();
    g.packet[p].clear();
  }
  g.compared_upto = -1;
  g.remote_upto = -1;
  g.peer_acked = -1;
  g.peer_last_frame = -1;
  g.peer_latest = -1;
  g.check_hash = 0;
  g.checks = 0;
  g.desync_at = -1;
  g.desync_frames = 0;
  g.rollback = rollback;
  g.speculating = false;
  g.rollback_to = -1;
  g.mode_change_at = -1;
  g.advantage = 0;
  g.delay = rollback ? g.rollback_delay : g.lockstep_delay;
  Ensure(g.delay + 1);
  g.last_heard_us = NowUs();
}

// The sync hash of the frame that just ran (its last run), from the game's checks during it.
void FinishFrameHash() {
  if (g.frame <= 0) return;
  uint64_t h = XXH3_64bits_withSeed(&g.checks, sizeof(g.checks), g.check_hash);
  if (!g.sync_ranges.empty() && g.frame % std::max(1, REXCVAR_GET(mk9_net_sync_every)) == 0) {
    auto* memory = rex::system::kernel_memory();
    for (const auto& [lo, hi] : g.sync_ranges) {
      h = XXH3_64bits_withSeed(memory->TranslateVirtual<const uint8_t*>(lo), hi - lo, h);
    }
  }
  g.local_hash[size_t(g.frame - 1)] = h ? h : 1;
  g.check_hash = 0;
  g.checks = 0;
}

// The pads frame g.frame runs with, and their packet numbers (one more for each change since the frame before).
void SetFramePads(const Pad& local, const Pad& remote) {
  const Pad pads[2] = {g.player == 0 ? local : remote, g.player == 0 ? remote : local};
  for (int p = 0; p < 2; ++p) {
    const size_t f = size_t(g.frame);
    const uint32_t before = f ? g.packet[p][f - 1] : g.packet_base[p];
    const bool changed = f ? !(g.used[p][f - 1] == pads[p]) : !(pads[p] == Pad{});
    g.used[p][f] = pads[p];
    g.packet[p][f] = before + (changed ? 1 : 0);
    g.frame_pads[p] = pads[p];
  }
}

void LogStats() {
  const uint64_t now = NowUs();
  if (now < g.log_at_us) return;
  if (g.log_at_us) {
    std::string rb;
    if (g.rollback) {
      const quiesce::Stats q = quiesce::Totals();
      rb = fmt::format("; rollback{}: {} rollbacks ({} frames run again, deepest {}), {} frames predicted, {} wrong; "
                       "keeping a frame {:.2f} ms, a rollback's restore {:.2f} ms; at rest {:.2f} ms a boundary "
                       "(render {:.2f}, pools and queries {:.2f}, park {:.2f}; longest {:.1f}, {} unparked, {} query "
                       "batches collected, {} pool waits); advantage {:.1f}, {} sleeps",
                       g.speculating ? "" : " (learning)", g.rollbacks, g.resimulated, g.max_depth, g.predicted,
                       g.mispredicted, g.keeps ? g.ms_keep / double(g.keeps) : 0.0,
                       g.rollbacks ? g.ms_restore / double(g.rollbacks) : 0.0,
                       q.settles ? q.ms_total / double(q.settles) : 0.0,
                       q.settles ? q.ms_flush / double(q.settles) : 0.0,
                       q.settles ? q.ms_drain / double(q.settles) : 0.0,
                       q.settles ? q.ms_park / double(q.settles) : 0.0, q.ms_max, q.unparked, q.collected,
                       q.pool_waits, g.advantage, g.syncs_slept);
    }
    const sound::HeapStats sound = sound::Stats();
    rb += fmt::format("; sound's arena {:.1f} MB in use (peak {:.1f}), {} blocks", sound.in_use / 1048576.0,
                      sound.peak / 1048576.0, sound.blocks);
    REXLOG_INFO("[net] epoch {} frame {} (game {}), rtt {:.1f} ms, delay {}, waited {:.1f} ms in total ({} stalls, "
                "longest {:.1f} ms), peer inputs to {}; sent {}, received {}{}{}",
                g.epoch, g.frame, g.start_game_frame + g.frame, g.rtt_ms, g.delay, g.wait_ms_window, g.stalls_window,
                g.wait_ms_max, g.remote_upto, g.sent, g.received, rb,
                g.desync_at >= 0 ? fmt::format("; {} frame(s) out of sync, the first {}", g.desync_frames, g.desync_at)
                                 : "");
  }
  g.wait_ms_window = g.wait_ms_max = 0;
  g.stalls_window = 0;
  g.log_at_us = now + 10'000'000;
}

// One frame of a lockstep epoch (the menus, the loads, a fight without rollback).
void LockstepFrame(uint8_t* base) {
  // this frame's local input goes in at frame + delay; the first delay frames are neutral on both sides
  Ensure(g.frame + g.delay + 1);
  Pad& mine = g.local[size_t(g.frame + g.delay)];
  mine = ReadLocalPad();
  mine.busy = SampleBusy(base);
  mine.ready = g.ready_seen;
  g.ready_seen = 0;
  FinishFrameHash();
  SendInputs();
  Ping();
  // the peer waits at the next barrier and sends nothing past its last frame: this side plays on to its own barrier
  // with neutral pads for both
  Receive();
  auto gap = [] { return g.peer_barrier > g.epoch && g.frame > g.peer_last_frame; };
  WaitFor([&] { return g.remote_upto >= g.frame || gap(); });
  if (g.ended) return;
  CompareFinalHashes();
  if (gap()) {
    SetFramePads(Pad{}, Pad{});
    g.frame_busy = g.local[size_t(g.frame)].busy;
    g.frame_ready = g.local[size_t(g.frame)].ready;
  } else {
    SetFramePads(g.local[size_t(g.frame)], g.remote[size_t(g.frame)]);
    g.frame_busy = g.local[size_t(g.frame)].busy | g.remote[size_t(g.frame)].busy;
    g.frame_ready = g.local[size_t(g.frame)].ready & g.remote[size_t(g.frame)].ready;
  }
  ++g.frames_window;
  LogStats();
  ++g.frame;
  g.latest = g.frame;
}

// One frame of a fight with rollback.
void RollbackFrame(PPCContext& ctx, uint8_t* base) {
  Receive();
  const bool live = g.frame == g.latest;
  if (live) {
    Ensure(g.latest + g.delay + 1);
    g.local[size_t(g.latest + g.delay)] = ReadLocalPad();
    SendInputs();
    Ping();
  }
  // when this side waits for the peer's inputs (the other threads run on meanwhile): while learning (lockstep); more
  // than kMaxPrediction frames ahead of them; and before going past a mode change or into a package load, until every
  // frame run so far is confirmed
  const bool loads = Load32(base, kAsyncPackagesNum) != 0;
  WaitFor([&] {
    if (!g.speculating) return g.remote_upto >= g.frame;
    if (g.mode_change_at >= 0 || loads) return g.remote_upto >= g.frame - 1;
    return g.frame - g.remote_upto <= kMaxPrediction;
  });
  if (g.ended) return;
  // the boundary at rest (quiesce.h): the frame that just ran into the timeline with its sync hash, and a wrong
  // prediction undone, with no other thread halfway through anything
  quiesce::Settle(ctx, base);
  if (g.frame > 0) {
    const uint64_t t0 = NowUs();
    g.timeline.Advance(base, uint64_t(g.frame));
    g.ms_keep += (NowUs() - t0) / 1000.0;
    ++g.keeps;
    FinishFrameHash();
    if (!g.speculating) {
      g.timeline.Verify(base);  // learning the pages written with no card (the fight's intro)
      if (g.frame >= kLearnFrames) {
        g.speculating = true;
        REXLOG_INFO("[net] rollback on from frame {} of the fight ({} pages written with no card learnt)", g.frame,
                    g.timeline.suspect_pages());
      }
    }
  }
  Receive();  // the newest inputs: a prediction they contradict is undone now
  // a prediction that turned out wrong: back to that frame
  if (g.rollback_to >= 0 && g.rollback_to < g.frame) {
    const int64_t to = g.rollback_to;
    const uint64_t t0 = NowUs();
    if (!g.timeline.RollBack(base, uint64_t(to))) {
      quiesce::Release();
      REXLOG_ERROR("[net] frame {} is out of the kept frames (now {}): session ended", to, g.frame);
      g.ended = true;
      return;
    }
    g.ms_restore += (NowUs() - t0) / 1000.0;
    ++g.rollbacks;
    if (g.rollbacks <= 20) {
      REXLOG_INFO("[net] rollback {}: back from frame {} to {} ({} frames), peer inputs to {}", g.rollbacks, g.frame,
                  to, g.frame - to, g.remote_upto);
    }
    g.resimulated += uint64_t(g.frame - to);
    g.max_depth = std::max<uint64_t>(g.max_depth, uint64_t(g.frame - to));
    g.frame = to;
    if (g.mode_change_at >= to) g.mode_change_at = -1;
  }
  quiesce::Release();
  g.rollback_to = -1;
  CompareFinalHashes();
  // a mode change every frame up to which is final: the rest of the epoch in lockstep
  if (g.mode_change_at >= 0 && g.mode_change_at < g.frame && FinalUpto() >= g.mode_change_at) {
    REXLOG_INFO("[net] the fight's mode ended at frame {}: lockstep from frame {} ({} rollbacks, {} frames run again)",
                g.mode_change_at, g.frame, g.rollbacks, g.resimulated);
    g.rollback = false;
    g.mode_change_at = -1;
    g.timeline = snapshot::Timeline();
    sa2::renut::SetFrameHidden(false);
    // this frame, as lockstep does it: its local input is the one this side already sent
    WaitFor([&] { return g.remote_upto >= g.frame; });
    if (g.ended) return;
    SetFramePads(g.local[size_t(g.frame)], g.remote[size_t(g.frame)]);
    ++g.frame;
    g.latest = g.frame;
    LogStats();
    return;
  }
  // this frame's pads: the peer's input, or its last one as the prediction
  const bool confirmed = g.have_remote[size_t(g.frame)] != 0;
  Pad remote;
  if (confirmed) {
    remote = g.remote[size_t(g.frame)];
  } else {
    if (g.remote_upto >= 0) remote = g.remote[size_t(g.remote_upto)];
    ++g.predicted;
  }
  SetFramePads(g.local[size_t(g.frame)], remote);
  g.frame_busy = 0;
  g.frame_ready = 0xFF;
  // a frame run again is not shown and not paced
  sa2::renut::SetFrameHidden(g.frame < g.latest);
  ++g.frame;
  ++g.frames_window;
  if (g.frame > g.latest) g.latest = g.frame;
  // time sync: the peer's live frame lags by about half the round trip; a side ahead by two frames or more on average
  // sleeps one frame, at most once a second
  if (g.speculating && g.frame == g.latest && g.peer_latest >= 0) {
    const double lag = g.rtt_ms / 2.0 / (1000.0 / 60.0);
    const double ahead = double(g.latest) - (double(g.peer_latest) + lag);
    g.advantage = g.advantage * 0.95 + ahead * 0.05;
    if (g.advantage >= 2.0 && uint64_t(g.frame) > g.slept_at_frame + 60) {
      std::this_thread::sleep_for(std::chrono::microseconds(16667));
      g.slept_at_frame = uint64_t(g.frame);
      g.advantage -= 1.0;
      ++g.syncs_slept;
    }
  }
  LogStats();
}

}  // namespace

bool Active() {
  if (!g.configured) Configure();
  // a host waits for its guest; a room's guest waits for the path to the host too
  return g.link && (g.link->HasPeer() || g.host || !g.room.empty()) && !g.failed && !g.ended;
}

void OnFrame(PPCContext& ctx, uint8_t* base, uint64_t game_frame) {
  if (!Active()) {
    if (g.started) sa2::renut::SetFrameHidden(false);  // a session that ended shows every frame again
    return;
  }
  g.game_thread = std::this_thread::get_id();
  WatchFrames();
  g_frame_started_us = NowUs();
  if (!g.started) {
    if (!Handshake()) {
      g.failed = true;
      return;
    }
    g.started = true;
    StartEpoch(game_frame, 0, false);
    g.seed_pending = true;
    REXLOG_INFO("[net] session started at game frame {} with {} as the peer; you are player {}; lockstep delay {}, "
                "fights with rollback {} (delay {}); seed {}",
                game_frame, g.link->PeerText(), g.player + 1, g.lockstep_delay, g.rollback_wanted ? "on" : "off",
                g.rollback_delay, g.seed);
  } else if (g.at_barrier) {
    g.at_barrier = false;
    if (!WaitAtBarrier()) return;
    StartEpoch(game_frame, g.epoch + 1, g.rollback_wanted);
    if (g.rollback) {
      quiesce::Settle(ctx, base);
      g.timeline.Start(base, 0, int(kMaxPrediction) + 2, false);
      quiesce::Release();
    }
    REXLOG_INFO("[net] epoch {} started at game frame {} ({})", g.epoch, game_frame,
                g.rollback ? fmt::format("rollback, delay {}", g.delay) : "lockstep");
  }
  if (g.rollback) {
    RollbackFrame(ctx, base);
  } else {
    LockstepFrame(base);
  }
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
  if (g.rollback) return local;  // a fight polls none of these (a mode change goes back to lockstep first)
  int bit = 0;
  while (bit < 7 && !(flag & (1u << bit))) ++bit;
  if (local) {
    g.ready_seen |= flag;
    g.ready_value[bit] = local;
  }
  return (g.frame_ready & flag) ? (g.ready_value[bit] ? g.ready_value[bit] : 1u) : 0u;
}

void Barrier(uint8_t* base, const char* what) {
  if (!g.started || g.ended || g.failed || g.at_barrier || g.rollback) return;
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
  // the game's mode changing (the frames-since-mode counter back to 0) in a fight with rollback: see RollbackFrame
  if (g.rollback && (tag & 0xFFFF) == 8 && value == 0 && g.frame > 0 && g.mode_change_at < 0) {
    g.mode_change_at = g.frame - 1;
  }
}

bool TakeSessionSeed(uint32_t* seed) {
  if (!g.seed_pending) return false;
  g.seed_pending = false;
  *seed = g.seed;
  return true;
}

SessionInfo Info() {
  SessionInfo info;
  info.active = g.started && !g.ended && !g.failed;
  info.player = g.player;
  info.rtt_ms = g.rtt_ms;
  info.delay = g.delay;
  info.rollback = g.rollback;
  info.speculating = g.speculating;
  info.rollbacks = g.rollbacks;
  info.resimulated = g.resimulated;
  info.desynced = g.desync_at >= 0;
  return info;
}

void OverrideBlock(uint8_t* base, uint32_t tag, uint32_t data, uint32_t size, uint32_t user, uint32_t r1) {
  if (!g.started || g.ended || g.failed || g.frame <= 0) return;
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
      put_be32(out, g.packet[user][size_t(g.frame - 1)]);
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
