// MK9 online: the two-player session, in lockstep through menus and loads and with rollback in the fight (netplay.h).
//
//   mk9_net_room=<code>          the session of a lobby's room (lobby.h), over the players' own path (ice_link.h),
//   mk9_net_role=host|guest      as its host (player 1) or its guest (player 2); mk9_online_name: the player's name
//   mk9_net_host=<port>          or: open a session on this UDP port and wait for the other player (player 1; tests,
//   mk9_net_join=<host:port>     a LAN, a forwarded port), or join one (player 2)
//   mk9_net_delay=<frames>       input delay in lockstep (menus, loads); the host's counts for both
//   mk9_net_rollback=<bool>      fights with rollback (else lockstep all along)
//   mk9_net_rollback_delay=<f>   this player's input delay in a fight with rollback (each side its own): -1 = automatic
//                                (follows the round trip), 0 = none (every late input repaired by a rollback), 1-7 fixed
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
// The sounds' callbacks (a sync point, an end: FMOD's, on each machine's own clock) reach the game in lockstep only
// once both machines' inputs have announced them, at the same frame on both (sound_sync.h).
//
// The lockstep delay follows the round trip: in a lockstep stretch the frame waits for the peer's input of that frame,
// which left the peer delay frames before, so the two delays have to cover the round trip or every frame waits (the
// menus after a fight at 38 fps with 116 ms and delay 1). Every 2 s, and when a fight's mode ends, each side sets its
// own delay to what the round trip needs (LockstepDelayFor, from the floor of the last pings: a pong answered after a
// long frame says nothing of the network): up at once, the frames in between getting its last input again; down after
// three checks in a row, its next inputs waiting until the frames catch up. Each side's inputs stay one per frame, so
// the delays need not be the same on both.
//
// Rollback (an epoch started by a fight's barrier): the simulation's memory is kept frame by frame (snapshot.h,
// Timeline) and a frame no longer waits for the peer: without its input, it runs on a prediction (the peer's last
// input). When the input arrives and differs, the next frame boundary rolls the memory back to that frame's start and
// the frames since run again with the right inputs, hidden (no pacing, no present: sa2_renut_pacing.h), the live frame
// last. The first kLearnFrames of the fight (its intro) still run in lockstep while the timeline learns the pages
// written with no store card; and every frame run so far must be confirmed before the game goes past something a
// rollback cannot undo: its mode changing (the fight over, the game's check tag 8 back to 0) or a package load (they
// finish at a frame's start, loading.h). Once a mode change is confirmed, the epoch goes on in lockstep. The fight
// decided (its first package load: FINISH HIM's FatalityArt; AboutToLoad) counts as one: what follows (the victory,
// the profile's save, the menus, the music, the sound's teardown) cannot be undone, so it goes in lockstep too; the
// music changing waits for its frame to be confirmed (ConfirmFrame). START does not pause a fight with rollback
// (mk9_net_pause_in_fights). A side more than kMaxPrediction frames ahead of the peer's inputs waits, and the side
// ahead in time sleeps a frame now and then (time sync). Only final frames' sync hashes are compared: frames whose
// last run had both players' real inputs.
//
// Packets (little-endian): u16 'M9', u8 version, u8 type, then
//   HELLO    u8 player, u8 delay, u32 build, u32 seed (the host's: the generators start from it, seed.h),
//            u8 rollback (the host's mk9_net_rollback), u8 rollback delay
//   INPUT    u32 epoch, u32 ack (the peer's last frame received in order, +1), u32 first, u8 count, count x 18 bytes
//            (the guest's X_INPUT_GAMEPAD, big-endian, then the busy and the ready flags, then u32 a sound callback's
//            announcement, sound_sync.h), u32 hash frame (~0: none),
//            u64 hash (of that final frame), u32 the sender's live frame (time sync)
//   BARRIER  u32 epoch (the one starting there), u32 the sender's last frame of the epoch before
//   PING / PONG  u64 microseconds (the sender's clock, echoed)
//   BYE      u8 why (netplay.h, EndReason: the sender's player left, or its game found the two out of sync)
// The sync hash of a frame: the game's own checks during it (OnCheck: what its online compared, a draw of its random
// number generator among them) and the mk9_net_sync_ranges after it. A frame whose hashes differ is reported.

#include "online/netplay.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <algorithm>
#include <cmath>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <ctime>
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

#include "online/heap_undo.h"
#include "online/ice_link.h"
#include "online/lobby.h"
#include "online/net_udp.h"
#include "online/room.h"
#include "online/quiesce.h"
#include "online/seed.h"
#include "online/sound_memory.h"
#include "online/sound_replay.h"
#include "online/sound_sync.h"
#include "online/snapshot.h"
#include "online/transport.h"
#include "profiler/sa2_cpu_profile_pc.h"
#include "renut_engine/gpu/backend/render_thread.h"
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
REXCVAR_DEFINE_INT32(mk9_net_rollback_delay, -1, "MK9 Online",
                     "Your input delay in a fight with rollback: -1 = automatic (1 frame, more with a long round trip, "
                     "all fight long), 0-7 = that many frames always. Each player's own")
    .range(-1, 7);
REXCVAR_DEFINE_INT32(mk9_net_timeout_s, 20, "MK9 Online", "Seconds without the peer before the session ends");
REXCVAR_DEFINE_BOOL(mk9_net_skip_logos, true, "MK9 Online",
                    "A session's start skips the boot's logo movies (WB, NetherRealm: 16 s): the host's pad taps START "
                    "while one plays (an input, so both machines see it at the same frame)");
REXCVAR_DEFINE_BOOL(mk9_net_pause_in_fights, false, "MK9 Online",
                    "START pauses a fight with rollback too (off: no pause in online fights; menus keep it)");
REXCVAR_DEFINE_BOOL(mk9_net_light_resim, true, "MK9 Online",
                    "A frame a rollback runs again with both players' inputs already known starts without a snapshot or "
                    "a rest (no rollback can come back to it): rollbacks cost about half (tests: false = every frame "
                    "rests)");
REXCVAR_DEFINE_BOOL(mk9_net_auto_delay, true, "MK9 Online",
                    "A fight's input delay follows the round trip all fight long (from the host's "
                    "mk9_net_rollback_delay up to 7 frames) so rollbacks stay about 5 frames deep: steadier characters "
                    "with 150-400 ms of ping");
REXCVAR_DEFINE_BOOL(mk9_net_record, true, "MK9 Online",
                    "Each session's final frames (both players' inputs and the sync hash) go to "
                    "logs/session_<date>_<player>.mk9net: the two players' files say where the games parted "
                    "(tools/online/session_report.py); the newest 10 are kept");
REXCVAR_DEFINE_BOOL(mk9_net_end_on_desync, false, "MK9 Online",
                    "The session ends when the two games' sync hashes differ (false: it goes on and the log says so). "
                    "Off since the hashes are compared all along (2026-11-09): what they catch most is the game's random "
                    "draws in another order when a script waits on a sound, whose end comes on each machine's own "
                    "mixer (sound_sync.h), not always a fight that goes another way");
REXCVAR_DEFINE_BOOL(mk9_sound_sync_fights, false, "MK9 Online",
                    "Experimental: a fight's sounds' callbacks at the same frame on both machines by frame numbers "
                    "(sound_sync.h, Mode::kFrames); off: as they come, as before. Not enough yet: FMOD starts some "
                    "sounds a frame or two apart on the two machines (its instances and loads on its own clock)");
REXCVAR_DEFINE_STRING(mk9_net_sync_ranges, "", "MK9 Online",
                      "Guest ranges (hex lo-hi, comma-separated) that also go into the sync hash both players compare");
REXCVAR_DEFINE_INT32(mk9_net_sync_every, 1, "MK9 Online", "Hash mk9_net_sync_ranges every this many frames");
REXCVAR_DEFINE_INT32(mk9_net_test_protocol, 0, "MK9 Online",
                     "Tests: speak this session protocol version instead of the build's (7: as 0.1.0 and 0.2.0 do), "
                     "to see the other-version handshake")
    .range(0, 255);

#ifndef MK9_LAUTA_VERSION
#define MK9_LAUTA_VERSION "0.0.0"
#endif

namespace mk9::netplay {
namespace {

constexpr uint16_t kMagic = 0x394D;  // "M9"
// mk9-lauta (2026-11-10): 8 from 0.2.1 on. 0.1.0 and 0.2.0 both said 7 and played each other, with a different
// netcode on each side; a HELLO of another version now ends the handshake at once on this side, with the reason
// (EndReason::kOtherVersion), and the other side gets a goodbye in its own version's words. From 8 on a HELLO also
// says the release ("0.2.1").
constexpr uint8_t kVersion = 8;
constexpr uint32_t kBuild = 1;
enum : uint8_t { kHello = 1, kInput = 2, kPing = 3, kPong = 4, kBye = 5, kBarrier = 6 };
constexpr int kMaxPerPacket = 64;
constexpr uint32_t kNoHash = 0xFFFFFFFFu;
constexpr uint32_t kErrorDeviceNotConnected = 0x48F;
constexpr int64_t kLearnFrames = 120;   // a fight's first frames, in lockstep while the timeline learns its pages
constexpr int64_t kMaxPrediction = 10;  // frames run on predictions at most (the timeline keeps 2 more)
// mk9-lauta (2026-11-10): the fight's own input delay grows with the round trip so a rollback goes back about this
// many frames at most (the 0.1.0 testers, Argentina-Venezuela over TURN, 140-220 ms: rollbacks of 5-7 frames with
// delay 1, the characters jumping)
constexpr int kTargetRollbackDepth = 5;
constexpr uint32_t kAsyncPackagesNum = 0x834162A4 + 4;  // GObjAsyncPackages.Num (loading.h)

using Clock = std::chrono::steady_clock;
uint64_t NowUs() {
  return uint64_t(std::chrono::duration_cast<std::chrono::microseconds>(Clock::now().time_since_epoch()).count());
}

struct Pad {
  uint8_t bytes[12] = {};  // X_INPUT_GAMEPAD as the guest stores it (big-endian)
  uint8_t busy = 0;        // BusyFlag bits of the machine when it was read
  uint8_t ready = 0;       // ReadyFlag bits: the polls that said ready in the frame before
  uint32_t sound = 0;      // a sound callback this machine got (sound_sync.h Announcement; 0: none)
  bool operator==(const Pad& o) const { return std::memcmp(bytes, o.bytes, 12) == 0; }
};
constexpr int kPadBytes = 18;

struct Session {
  bool configured = false, failed = false, started = false, ended = false;
  bool host = false;
  std::string room;  // a lobby room's session (else a direct UDP one)
  int player = 0;  // 0 = player 1 (the host), 1 = player 2
  int lockstep_delay = 2, rollback_delay = 1;
  bool rollback_wanted = true;
  int delay = 2;                     // this epoch's (a lockstep stretch raises it with the round trip)
  int64_t delay_check_at = 0;        // the lockstep frame that looks at the round trip next
  int lower_votes = 0;               // checks in a row that found the delay longer than needed
  int64_t local_made_upto = -1;      // the newest frame with a local input
  std::unique_ptr<net::Transport> link;
  uint16_t local_port = 0;
  bool heard_peer = false, peer_hello = false;
  int peer_delay = -1, peer_rollback = -1, peer_rollback_delay = -1;
  int peer_version = -1;             // the protocol version of a HELLO that was not this one's (another release)
  std::string peer_release;          // the other player's release, as its HELLO said it (8 and up)
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
  // the newest frame the peer sent its hash of: each packet carries only its newest final frame's, so the frames it
  // skipped (a lost packet, or several frames made final at once by a rollback) never come and are not waited for
  int64_t remote_hash_newest = -1;
  uint64_t hashes_compared = 0;
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
  bool frames_sound = false;         // this epoch started with rollback: its sounds' callbacks by frame (sound_sync.h)
  bool speculating = false;          // past the learning frames: frames may run on predicted inputs
  uint64_t light_boundaries = 0;     // frames run again that started with no snapshot (mk9_net_light_resim)
  uint64_t delay_changes = 0;        // a fight's delay changed for the round trip (AdjustFightDelay)
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
  double rtt_samples[8] = {};        // the last pongs' round trips (their floor sets the lockstep delay)
  int rtt_count = 0;
  // mk9-lauta (2026-11-10): the last pings sent (their send time) and whether a pong came back (loss, the indicator)
  struct PingSent {
    uint64_t at_us = 0;
    bool answered = false;
  };
  PingSent pings[32] = {};
  int ping_next = 0;
  std::atomic<bool> relayed{false};  // the path goes through TURN (checked every few seconds on the game thread)
  uint64_t relayed_checked_us = 0;
  uint64_t frames_window = 0, stalls_window = 0, sent = 0, received = 0;
  uint64_t rollbacks = 0, resimulated = 0, max_depth = 0, predicted = 0, mispredicted = 0, syncs_slept = 0;
  uint64_t confirms = 0, confirms_doomed = 0;  // ConfirmFrame: actions a rollback cannot undo, and those left to a rerun
  double ms_keep = 0, ms_restore = 0;
  uint64_t keeps = 0;
};

Session g;

// mk9_net_record: the session's final frames, to a file in logs. Records (little-endian):
//   header   "MK9NET1\0", u32 version 1, u8 player, u32 seed, u8 rollback, u8 lockstep delay, u8 rollback delay
//   'E'      u32 epoch, u64 the game frame of its frame 0, u8 rollback
//   'F'      u32 frame, 18 bytes this player's input, 18 bytes the other's (the packets' pad record), u64 sync hash
struct Recorder {
  FILE* file = nullptr;
  std::string path;
  int64_t upto = -1;  // the epoch's frames written
  uint64_t frames = 0;
};
Recorder g_rec;

void RecordPut(const void* data, size_t size) {
  if (g_rec.file) std::fwrite(data, 1, size, g_rec.file);
}

template <typename T>
void RecordValue(T v) {
  RecordPut(&v, sizeof(v));
}

void RecordPad(const Pad& pad) {
  RecordPut(pad.bytes, 12);
  RecordValue(pad.busy);
  RecordValue(pad.ready);
  RecordValue(pad.sound);
}

void RecordOpen() {
  if (!REXCVAR_GET(mk9_net_record) || g_rec.file) return;
  wchar_t exe[MAX_PATH] = {};
  GetModuleFileNameW(nullptr, exe, MAX_PATH);
  const std::filesystem::path logs = std::filesystem::path(exe).parent_path() / L"logs";
  std::error_code ec;
  std::filesystem::create_directories(logs, ec);
  // the newest 10 stay
  std::vector<std::filesystem::path> old;
  for (const auto& entry : std::filesystem::directory_iterator(logs, ec)) {
    const std::wstring name = entry.path().filename().wstring();
    if (name.rfind(L"session_", 0) == 0 && entry.path().extension() == L".mk9net") old.push_back(entry.path());
  }
  std::sort(old.begin(), old.end());
  while (old.size() >= 10) {
    std::filesystem::remove(old.front(), ec);
    old.erase(old.begin());
  }
  const std::time_t now = std::time(nullptr);
  std::tm local{};
  localtime_s(&local, &now);
  char name[64];
  std::strftime(name, sizeof(name), "session_%Y%m%d_%H%M%S", &local);
  const std::filesystem::path path = logs / (std::string(name) + (g.player == 0 ? "_p1" : "_p2") + ".mk9net");
  g_rec.file = _wfopen(path.c_str(), L"wb");
  if (!g_rec.file) return;
  g_rec.path = path.string();
  RecordPut("MK9NET1", 8);
  RecordValue<uint32_t>(1);
  RecordValue<uint8_t>(uint8_t(g.player));
  RecordValue<uint32_t>(g.seed);
  RecordValue<uint8_t>(g.rollback_wanted ? 1 : 0);
  RecordValue<uint8_t>(uint8_t(g.lockstep_delay));
  RecordValue<uint8_t>(uint8_t(g.rollback_delay));
  REXLOG_INFO("[net] the session's inputs go to {}", g_rec.path);
}

void RecordEpoch() {
  if (!g_rec.file) return;
  RecordValue<uint8_t>('E');
  RecordValue<uint32_t>(g.epoch);
  RecordValue<uint64_t>(g.start_game_frame);
  RecordValue<uint8_t>(g.rollback ? 1 : 0);
  g_rec.upto = -1;
}

int64_t FinalUpto();

// The frames that are final now (both players' real inputs, their last run done), in order.
void RecordFinal() {
  if (!g_rec.file) return;
  const int64_t upto = std::min(FinalUpto(), g.frame - 1);
  for (int64_t f = g_rec.upto + 1; f <= upto; ++f) {
    if (!g.have_remote[size_t(f)]) break;
    RecordValue<uint8_t>('F');
    RecordValue<uint32_t>(uint32_t(f));
    RecordPad(g.local[size_t(f)]);
    RecordPad(g.remote[size_t(f)]);
    RecordValue<uint64_t>(g.local_hash[size_t(f)]);
    g_rec.upto = f;
    ++g_rec.frames;
  }
  if ((g_rec.frames & 1023) == 0) std::fflush(g_rec.file);
}

void RecordClose() {
  if (!g_rec.file) return;
  std::fclose(g_rec.file);
  g_rec.file = nullptr;
}

// The online screens' requests to the game thread (Leave, PeerLeft) and how the session ended: any thread.
std::atomic<int> g_end_request{0};             // 1: this player leaves; 2: the room says the other one left
std::atomic<uint8_t> g_end_reason{0};          // EndReason
std::atomic<uint64_t> g_waiting_since_us{0};   // WaitFor's start while it waits for the other player (0: not)

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

// The protocol version this side speaks (mk9_net_test_protocol: another one, for tests).
uint8_t Protocol() {
  static const int test = REXCVAR_GET(mk9_net_test_protocol);
  return test > 0 ? uint8_t(test) : kVersion;
}

std::vector<uint8_t> Header(uint8_t type) {
  std::vector<uint8_t> out;
  Put16(out, kMagic);
  out.push_back(Protocol());
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
    if (!remote && frame >= g.remote_hash_newest) break;  // the peer's hash of it may still come
    g.compared_upto = frame;
    if (!local || !remote) continue;  // one the peer skipped
    ++g.hashes_compared;
    if (local == remote) continue;
    if (g.desync_at < 0) {
      g.desync_at = frame;
      if (!REXCVAR_GET(mk9_net_end_on_desync)) {
        REXLOG_WARN("[net] the two games' checks differ from frame {} of epoch {} on: the session goes on "
                    "(mk9_net_end_on_desync); a fight may look different on the two screens from here",
                    frame, g.epoch);
      }
      if (g_rec.file) {
        std::fflush(g_rec.file);
        REXLOG_WARN("[net] the session's inputs are in {}: with the other player's, tools/online/session_report.py "
                    "says where the two games parted", g_rec.path);
      }
    }
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

const char* ReasonText(EndReason why) {
  switch (why) {
    case EndReason::kLeft: return "this player left";
    case EndReason::kPeerLeft: return "the other player left";
    case EndReason::kPeerLost: return "no word from the other player";
    case EndReason::kDesync: return "the two games went out of sync";
    case EndReason::kFailed: return "it could not start";
    case EndReason::kOtherVersion: return "the other player has another release of mk9-lauta";
    default: return "?";
  }
}

// The session's end (game thread). goodbye: the peer hears why at once (three datagrams, as one may be lost; the byte
// after the type says why).
void EndSession(EndReason why, bool goodbye) {
  if (g.ended) return;
  if (goodbye && g.link && g.link->HasPeer()) {
    auto p = Header(kBye);
    p.push_back(uint8_t(why));
    for (int i = 0; i < 3; ++i) g.link->Send(p.data(), p.size());
  }
  g.ended = true;
  g_end_reason.store(uint8_t(why), std::memory_order_release);
  g_frame_started_us = 0;  // frames go on without the session: no hang to watch for
  REXLOG_INFO("[net] session ended: {}", ReasonText(why));
  RecordClose();
  heap_undo::End();
}

// The online screens' requests (Leave, PeerLeft), taken on the game thread.
void TakeEndRequest() {
  switch (g_end_request.exchange(0, std::memory_order_acq_rel)) {
    case 1:
      EndSession(EndReason::kLeft, true);
      break;
    case 2:
      EndSession(EndReason::kPeerLeft, false);
      break;
    default:
      break;
  }
}

// True when it went out: an ICE link has no path to send on until its pair is chosen (IceLink::Send).
bool SendHello() {
  auto p = Header(kHello);
  p.push_back(uint8_t(g.player));
  p.push_back(uint8_t(g.lockstep_delay));
  Put32(p, kBuild);
  Put32(p, g.seed);
  p.push_back(uint8_t(g.rollback_wanted ? 1 : 0));
  p.push_back(uint8_t(g.rollback_delay));
  const std::string_view release = MK9_LAUTA_VERSION;
  p.push_back(uint8_t(std::min<size_t>(release.size(), 15)));
  p.insert(p.end(), release.begin(), release.begin() + std::min<size_t>(release.size(), 15));
  const bool sent = g.link->Send(p.data(), p.size());
  g.last_sent_us = NowUs();
  return sent;
}

// A HELLO heard after this side started: the peer is still in its handshake (it never got ours), so it gets ours
// again, at most every 50 ms.
void AnswerHello() {
  static uint64_t answered_at = 0;
  if (NowUs() - answered_at < 50000) return;
  answered_at = NowUs();
  SendHello();
}

void SendInputs() {
  // only inputs made: in a fight the next one (latest + delay) is made at the next boundary, and a wait inside a
  // frame (ConfirmFrame: a load, the music changing) resends before that; sent unmade, the peer would keep that
  // empty pad as this frame's input and drop the real one (2026-11-09)
  const int64_t newest = g.local_made_upto;
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
    Put32(p, g.local[size_t(f)].sound);
  }
  // the newest final frame's sync hash (a fight's frame sends before the frame that just ran has its hash: the one
  // before then)
  int64_t final_frame = std::min(FinalUpto(), g.frame - 1);
  for (int back = 0; back < 4 && final_frame > 0 && !g.local_hash[size_t(final_frame)]; ++back) --final_frame;
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
    if ((buf[0] | buf[1] << 8) != kMagic) continue;
    if (buf[2] != Protocol()) {
      // another release's game (the HELLO's type byte is where it always was): the handshake ends with the reason
      if (buf[3] == kHello && g.peer_version < 0) {
        g.peer_version = buf[2];
        // 8 and up say their release after the 12 bytes of 0.2.0's HELLO
        if (buf[2] >= 8 && n >= 4 + 13 && n >= 4 + 13 + buf[4 + 12]) {
          g.peer_release.assign(reinterpret_cast<const char*>(buf + 4 + 13), buf[4 + 12]);
        }
        REXLOG_WARN("[net] the other player's game speaks the session protocol {} (this one {}): another release of "
                    "mk9-lauta ({}; this one {})",
                    buf[2], Protocol(), g.peer_release.empty() ? "0.2.0 or older" : g.peer_release, MK9_LAUTA_VERSION);
      }
      continue;
    }
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
      if (left >= 13 && left >= 13 + p[12] && g.peer_release.empty()) {
        g.peer_release.assign(reinterpret_cast<const char*>(p + 13), p[12]);
        REXLOG_INFO("[net] the other player has mk9-lauta {}", g.peer_release);
      }
      if (g.started) AnswerHello();
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
        pad.sound = Get32(p + 9 + i * kPadBytes + 14);
        g.have_remote[size_t(f)] = 1;
        // a frame that ran on a prediction this input contradicts: the rollback goes back to it
        if (g.rollback && f < g.frame && !(g.used[1 - g.player][size_t(f)] == pad)) {
          ++g.mispredicted;
          g.rollback_to = g.rollback_to < 0 ? f : std::min(g.rollback_to, f);
        }
        // a sound callback both machines have now said: given at a frame that may have run without it
        if (g.frames_sound && pad.sound) {
          const int64_t due = sound_sync::RemoteAnnounced(pad.sound, f);
          if (g.rollback && due >= 0 && due < g.frame) {
            g.rollback_to = g.rollback_to < 0 ? due : std::min(g.rollback_to, due);
            sound_sync::NoteRollback();
          }
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
        g.remote_hash_newest = std::max<int64_t>(g.remote_hash_newest, hash_frame);
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
      // mk9-lauta (2026-11-10): the pong says how long the ping waited here before this side read it (the transport
      // knows when it arrived: ICE), and the round trip leaves that out, as it leaves out the pong's own wait on the
      // other side: the network's round trip, not this frame's length. A 0.2.0 fight on a busy GPU measured 400-500
      // ms of "round trip" over a 160 ms path, and its delay went up to 7 for half a minute.
      auto r = Header(kPong);
      r.insert(r.end(), p, p + 8);
      const uint64_t arrived = g.link->LastArrivalUs(), now = NowUs();
      Put32(r, arrived && now > arrived ? uint32_t(std::min<uint64_t>(now - arrived, 0xFFFFFFFFull)) : 0);
      Send(r);
    } else if (type == kPong && left >= 8) {
      const uint64_t arrived = g.link->LastArrivalUs(), now = NowUs();
      const uint64_t at = arrived && arrived <= now ? arrived : now;
      const uint64_t held_there = left >= 12 ? Get32(p + 8) : 0;
      const uint64_t sent = Get64(p);
      const double rtt = std::max(0.1, (double(at > sent ? at - sent : 0) - double(held_there)) / 1000.0);
      g.rtt_ms = g.rtt_ms == 0 ? rtt : g.rtt_ms * 0.8 + rtt * 0.2;
      g.rtt_samples[g.rtt_count++ % 8] = rtt;
      for (auto& ping : g.pings) {
        if (ping.at_us == sent) ping.answered = true;
      }
    } else if (type == kBye) {
      // its game found the two out of sync, or its player left
      const bool desync = left >= 1 && p[0] == uint8_t(EndReason::kDesync);
      if (!g.ended) REXLOG_INFO("[net] the other player's goodbye{}", desync ? " (out of sync)" : "");
      EndSession(desync ? EndReason::kDesync : EndReason::kPeerLeft, false);
    }
  }
}

void Ping() {
  if (NowUs() - g.last_ping_us > 500000) {
    const uint64_t now = NowUs();
    auto p = Header(kPing);
    Put64(p, now);
    Send(p);
    g.last_ping_us = now;
    g.pings[g.ping_next++ % 32] = {now, false};
  }
  if (NowUs() - g.relayed_checked_us > 3000000) {
    g.relayed_checked_us = NowUs();
    g.relayed.store(g.link->Relayed(), std::memory_order_relaxed);
  }
}

// Waits until done() (receiving, resending this side's inputs every 4 ms) or the peer goes quiet for the timeout.
template <typename Done>
void WaitFor(Done done) {
  if (done()) return;
  const uint64_t wait_start = NowUs();
  g_waiting_since_us.store(wait_start, std::memory_order_relaxed);
  while (!done() && !g.ended) {
    g.link->WaitReadable(1);
    Receive();
    TakeEndRequest();
    if (g.ended) break;
    if (NowUs() - g.last_sent_us > 4000) SendInputs();
    if (NowUs() - g.last_heard_us > TimeoutUs()) {
      REXLOG_ERROR("[net] no word from the other player for {} s", TimeoutUs() / 1000000);
      EndSession(EndReason::kPeerLost, false);
    }
  }
  g_waiting_since_us.store(0, std::memory_order_relaxed);
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

// This player's pad: every device of this PC together (the four players' seats merged), so the keyboard and any pad
// play, whichever seat the options give them offline (mk9_controls.h).
Pad ReadLocalPad() {
  Pad pad;
  auto* runtime = REX_KERNEL_STATE() ? REX_KERNEL_STATE()->emulator() : nullptr;
  auto* is = runtime ? static_cast<rex::input::InputSystem*>(runtime->input_system()) : nullptr;
  if (!is) return pad;
  rex::input::X_INPUT_STATE merged{};
  bool any = false;
  for (uint32_t user = 0; user < rex::input::kMaxGuestUsers; ++user) {
    rex::input::X_INPUT_STATE state{};
    if (is->GetState(user, &state) != 0) continue;
    if (!any) merged = state;
    else rex::input::MergeInto(merged, state);
    any = true;
  }
  if (any) std::memcpy(pad.bytes, &merged.gamepad, 12);
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
  room::EnterSession(room, g.host, name, id);  // the room's chat goes on in the session
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
  g.rollback_delay = std::clamp(REXCVAR_GET(mk9_net_rollback_delay), -1, 7);
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
  g.rollback_delay = std::clamp(REXCVAR_GET(mk9_net_rollback_delay), -1, 7);
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
// mk9-lauta (2026-11-10): 30 s at most (it was 2 minutes of a frozen logo); every 5 s the log says what it waits for;
// and a guest whose path has been up 6 s without a word from the host starts the ICE exchange over (up to 3 times):
// the 0.2.0 sessions that froze had ICE "completed" on the guest's side and never a datagram from the host.
bool Handshake() {
  const uint64_t timeout = std::max<uint64_t>(30000000, TimeoutUs() + 10000000);
  const uint64_t start = NowUs();
  uint64_t hello_at = 0, said_at = start, path_since = 0;
  int restarts = 0, hellos_sent = 0;
  while (NowUs() - start < timeout) {
    const bool path = g.link->HasPeer();
    if (path && NowUs() - hello_at > 100000) {
      if (SendHello()) ++hellos_sent;
      hello_at = NowUs();
    }
    if (!path) {
      path_since = 0;
    } else if (!path_since) {
      path_since = NowUs();
    }
    if (!g.host && !g.heard_peer && path_since && NowUs() - path_since > 6000000 && restarts < 3) {
      REXLOG_WARN("[net] the path to the host has been up {} s with nothing from it: the ICE exchange again ({} of 3)",
                  (NowUs() - path_since) / 1000000, restarts + 1);
      if (g.link->Restart()) ++restarts;
      path_since = 0;
    }
    if (NowUs() - said_at > 5000000) {
      said_at = NowUs();
      REXLOG_INFO("[net] waiting for the other player ({} s): path {}, heard {}, its HELLO {}, {} HELLOs sent{}",
                  (NowUs() - start) / 1000000, path ? "up" : "not yet", g.heard_peer ? "yes" : "no",
                  g.peer_hello ? "yes" : "no", hellos_sent, restarts ? fmt::format(", {} new exchange(s)", restarts) : "");
    }
    g.link->WaitReadable(10);
    Receive();
    TakeEndRequest();
    if (g.ended) return false;
    if (g.peer_version >= 0) {
      // another release: no session (their netcode is not this one's). Their game hears a goodbye in its own
      // version's words (0.2.0 ends its handshake with "the other player left" instead of waiting 2 minutes).
      if (g.link->HasPeer()) {
        const uint8_t bye[5] = {uint8_t(kMagic & 0xFF), uint8_t(kMagic >> 8), uint8_t(g.peer_version), kBye,
                                uint8_t(EndReason::kLeft)};
        for (int i = 0; i < 3; ++i) g.link->Send(bye, sizeof(bye));
      }
      EndSession(EndReason::kOtherVersion, false);
      return false;
    }
    // Both sides need the other's HELLO and a path to answer on. Over ICE a HELLO can arrive before the pair this side
    // sends on is chosen: the host of 0.1.0 started on any word from the guest, its own HELLO went nowhere and the
    // guest waited for it while the host waited for the guest's inputs (the sessions that froze at their start).
    if (g.peer_hello && g.link->HasPeer()) {
      // the host's seed and its choice of rollback; the delays are each player's own (mk9-lauta, 2026-11-10: the
      // inputs carry their frames, so the two sides need not wait the same)
      if (!g.host && g.peer_delay >= 0) {
        g.seed = uint32_t(g.peer_seed);
        g.rollback_wanted = g.peer_rollback != 0;
      }
      g.delay = g.lockstep_delay;
      // the side that finished first lets the other finish too; a peer that still misses it gets it again when its
      // next HELLO arrives (AnswerHello)
      if (SendHello()) return true;
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
  g_waiting_since_us.store(start, std::memory_order_relaxed);
  while (g.peer_barrier < next && !g.ended) {
    if (NowUs() - told_at > 20000) {
      SendBarrier(next, last);
      told_at = NowUs();
    }
    g.link->WaitReadable(2);
    Receive();
    TakeEndRequest();
    if (g.ended) break;
    if (NowUs() - g.last_heard_us > TimeoutUs() || NowUs() - start > TimeoutUs() * 3) {
      REXLOG_ERROR("[net] the other player never reached the barrier of epoch {}", next);
      EndSession(EndReason::kPeerLost, false);
    }
  }
  g_waiting_since_us.store(0, std::memory_order_relaxed);
  SendBarrier(next, last);  // in case the peer has not heard it yet
  const double waited = (NowUs() - start) / 1000.0;
  REXLOG_INFO("[net] barrier of epoch {}: this side waited {:.1f} ms at its frame {}", next, waited, last);
  return !g.ended;
}

double RoundTripTypical();
int LockstepDelayFor(double rtt_ms);

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
  g.remote_hash_newest = -1;
  g.hashes_compared = 0;
  g.remote_upto = -1;
  g.peer_acked = -1;
  g.peer_last_frame = -1;
  g.peer_latest = -1;
  g.check_hash = 0;
  g.checks = 0;
  g.desync_at = -1;
  g.desync_frames = 0;
  g.rollback = rollback;
  g.frames_sound = rollback && REXCVAR_GET(mk9_sound_sync_fights);
  g.speculating = false;
  g.rollback_to = -1;
  g.mode_change_at = -1;
  g.advantage = 0;
  // the delay the round trip needs (from the pongs so far): a lockstep epoch starts with it instead of the host's
  // and goes on adjusting from its first frame; a fight's first kLearnFrames (its intro, nobody plays yet) run in
  // lockstep with it too, and the fight's own delay comes after them (FightDelay). With the host's 1 frame, 140-220
  // ms of round trip ran the intros at ~13 frames a second (the 0.1.0 testers' "it slows down when the fight starts")
  const int needed = g.rtt_count > 0 ? LockstepDelayFor(RoundTripTypical()) : g.lockstep_delay;
  g.delay = rollback ? std::max(g.rollback_delay, needed) : std::max(g.lockstep_delay, needed);
  g.delay_check_at = rollback ? 120 : 0;
  g.lower_votes = 0;
  RecordEpoch();
  g.local_made_upto = g.delay - 1;  // the epoch's first delay frames: neutral on both sides
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
                       "batches collected of {} query objects, {} pool waits); advantage {:.1f}, {} sleeps; {} frames "
                       "run again with no snapshot; the delay changed {} times",
                       g.speculating ? "" : " (learning)", g.rollbacks, g.resimulated, g.max_depth, g.predicted,
                       g.mispredicted, g.keeps ? g.ms_keep / double(g.keeps) : 0.0,
                       g.rollbacks ? g.ms_restore / double(g.rollbacks) : 0.0,
                       q.settles ? q.ms_total / double(q.settles) : 0.0,
                       q.settles ? q.ms_flush / double(q.settles) : 0.0,
                       q.settles ? q.ms_drain / double(q.settles) : 0.0,
                       q.settles ? q.ms_park / double(q.settles) : 0.0, q.ms_max, q.unparked, q.collected,
                       q.queries_seen, q.pool_waits, g.advantage, g.syncs_slept, g.light_boundaries, g.delay_changes);
    }
    const sound::HeapStats sound = sound::Stats();
    const sound_replay::Stats sounds = sound_replay::GetStats();
    rb += fmt::format("; sound's arena {:.1f} MB in use (peak {:.1f}), {} blocks, physical {:.1f} MB (peak {:.1f}"
                      "{}); sounds started {}, in frames run again {} kept playing ({} instances given back), {} new, "
                      "{} stopped",
                      sound.in_use / 1048576.0, sound.peak / 1048576.0, sound.blocks,
                      sound.physical_in_use / 1048576.0, sound.physical_peak / 1048576.0,
                      sound.physical_failed ? fmt::format(", {} did not fit", sound.physical_failed) : "",
                      sounds.starts_live,
                      sounds.starts_skipped, sounds.pops_replayed, sounds.starts_new, sounds.stopped);
    if (g.confirms) {
      rb += fmt::format("; {} action(s) a rollback cannot undo, {} left to a corrected run", g.confirms,
                        g.confirms_doomed);
    }
    const sound_sync::Stats callbacks = sound_sync::GetStats();
    rb += fmt::format("; sound callbacks: {} sources numbered, {} kept for both, {} given, {} dropped, {} waiting, "
                      "{} through at once ({} given again in frames run again), {} given at an epoch's change, {} said "
                      "in a fight, {} rollbacks for them",
                      callbacks.numbered, callbacks.deferred, callbacks.delivered, callbacks.dropped,
                      callbacks.waiting, callbacks.passed, callbacks.regiven, callbacks.flushed, callbacks.announced,
                      callbacks.rollbacks);
    REXLOG_INFO("[net] epoch {} frame {} (game {}), rtt {:.1f} ms, delay {}, waited {:.1f} ms in total ({} stalls, "
                "longest {:.1f} ms), peer inputs to {}; sent {}, received {}; {} frames' sync checked{}{}",
                g.epoch, g.frame, g.start_game_frame + g.frame, g.rtt_ms, g.delay, g.wait_ms_window, g.stalls_window,
                g.wait_ms_max, g.remote_upto, g.sent, g.received, g.hashes_compared, rb,
                g.desync_at >= 0 ? fmt::format("; {} frame(s) out of sync, the first {}", g.desync_frames, g.desync_at)
                                 : "");
  }
  g.wait_ms_window = g.wait_ms_max = 0;
  g.stalls_window = 0;
  g.log_at_us = now + 10'000'000;
}

// The round trip's floor: the lowest of the last pongs (one answered after a long frame of the peer's, a load, says
// nothing of the network). 0 until one came.
double RoundTripFloor() {
  const int n = std::min(g.rtt_count, 8);
  if (n == 0) return 0;
  return *std::min_element(g.rtt_samples, g.rtt_samples + n);
}

// The round trip most frames see: the 75th percentile of the last pongs (the third highest of 8). The floor alone set
// the delay for the best moments: with 130-180 ms (the 0.1.0 testers) the delay covered 131 ms and the menus waited
// 8 s of every 10 for inputs that came a few ms late. 0 until one came.
double RoundTripTypical() {
  const int n = std::min(g.rtt_count, 8);
  if (n == 0) return 0;
  double sorted[8];
  std::copy(g.rtt_samples, g.rtt_samples + n, sorted);
  std::sort(sorted, sorted + n);
  return sorted[(n * 3) / 4 < n ? (n * 3) / 4 : n - 1];
}

// The lockstep delay a round trip needs: the two sides' delays cover it and ~25 ms of jitter, from the host's
// mk9_net_delay up to 20 frames. mk9-lauta (2026-11-10): 12 at most covered 375 ms; a 0.2.0 player's round trip went
// 170-585 ms and the menus stopped for 200-500 ms every few seconds (a third of a second of delay in a menu is better).
int LockstepDelayFor(double rtt_ms) {
  const int needed = int(std::ceil((rtt_ms + 25.0) / (2.0 * 1000.0 / 60.0)));
  return std::clamp(needed, g.lockstep_delay, 20);
}

// Every 2 s of lockstep: this side's delay for the round trip, up at once, down after three checks in a row.
void AdjustDelayForRoundTrip() {
  const double floor_ms = RoundTripFloor();
  if (g.frame < g.delay_check_at || floor_ms <= 0) return;
  g.delay_check_at = g.frame + 120;
  const int wanted = LockstepDelayFor(RoundTripTypical());
  if (wanted < g.delay && ++g.lower_votes < 3) return;
  if (wanted >= g.delay) g.lower_votes = 0;
  if (wanted == g.delay) return;
  REXLOG_INFO("[net] lockstep delay {} -> {} frames at frame {} (round trip {:.0f} ms at least, {:.0f} typical, {:.0f} "
              "on average)",
              g.delay, wanted, g.frame, floor_ms, RoundTripTypical(), g.rtt_ms);
  g.delay = wanted;
  g.lower_votes = 0;
}

// The input delay of a fight with rollback, once its intro is over: the player's own (mk9_net_rollback_delay 0-7), or,
// automatic (-1, the default), 1 raised with a long round trip so a wrong prediction goes back about
// kTargetRollbackDepth frames:
// 150 ms -> 1, 180 ms -> 2, 220 ms -> 3, 260 ms -> 4, 300 ms -> 6, 7 at most (340 ms and more: with 4, 340 ms went back
// 9-10 frames, frames of 50-120 ms). Each side picks its own (its inputs carry their frames).
int FightDelay() {
  if (g.rollback_delay >= 0) return g.rollback_delay;  // the player's own, fixed (F2 > Online)
  const double rtt = RoundTripTypical();
  if (!REXCVAR_GET(mk9_net_auto_delay) || rtt <= 0) return 1;
  const int one_way = int(std::ceil(rtt / 2.0 / (1000.0 / 60.0)));
  return std::clamp(one_way + 1 - kTargetRollbackDepth, 1, 7);
}

// mk9-lauta (2026-11-10): every 2 s of a fight with rollback, its delay for the round trip of now: up at once, down a
// frame at a time after three checks in a row (RollbackFrame fills the frames a raise skips with the last input, and
// makes no input for a frame after a lowering). 0.2.0 chose it once, at the end of the intro: a fight that began at
// 184 ms kept 2 frames with 300-350 ms later on; its rollbacks went back 10 frames, its frames took up to 850 ms and
// it showed 27-40 frames a second.
void AdjustFightDelay() {
  if (g.rollback_delay >= 0 || !REXCVAR_GET(mk9_net_auto_delay) || g.latest < g.delay_check_at ||
      RoundTripTypical() <= 0) {
    return;
  }
  g.delay_check_at = g.latest + 120;
  const int wanted = FightDelay();
  if (wanted < g.delay && ++g.lower_votes < 3) return;
  if (wanted >= g.delay) g.lower_votes = 0;
  if (wanted == g.delay) return;
  const int next = wanted > g.delay ? wanted : g.delay - 1;
  REXLOG_INFO("[net] fight delay {} -> {} frames at frame {} (round trip {:.0f} ms typical, {:.0f} at least)", g.delay,
              next, g.latest, RoundTripTypical(), RoundTripFloor());
  g.delay = next;
  g.lower_votes = 0;
  ++g.delay_changes;
}

// One frame of a lockstep epoch (the menus, the loads, a fight without rollback).
void LockstepFrame(PPCContext& ctx, uint8_t* base) {
  AdjustDelayForRoundTrip();
  // this frame's local input goes in at frame + delay; the first delay frames are neutral on both sides. A raised
  // delay: the frames in between get the last input again (pad and busy flags; not ready, no sound announcement). A
  // lowered one: that frame has its input already, and this side makes none until the frames catch up. Either way
  // this side's inputs stay one a frame.
  const int64_t target = g.frame + g.delay;
  Ensure(target + 1);
  if (target > g.local_made_upto) {
    const Pad last = g.local_made_upto >= 0 ? g.local[size_t(g.local_made_upto)] : Pad{};
    for (int64_t f = g.local_made_upto + 1; f < target; ++f) {
      Pad& fill = g.local[size_t(f)];
      std::memcpy(fill.bytes, last.bytes, 12);
      fill.busy = last.busy;
      fill.ready = 0;
      fill.sound = 0;
    }
    Pad& mine = g.local[size_t(target)];
    mine = ReadLocalPad();
    mine.busy = SampleBusy(base);
    // the boot's logo movies, skipped: the host taps START (X_INPUT_GAMEPAD's 0x0010, the buttons' low byte) every 10
    // frames while one plays in the session's first 25 s; the tap is an input, seen by both machines at the same frame
    static const bool skip_logos = REXCVAR_GET(mk9_net_skip_logos);
    if (skip_logos && g.host && g.epoch == 0 && target < 1500 && (mine.busy & kBusyMovie) && target % 10 == 0) {
      mine.bytes[1] |= 0x10;
    }
    mine.ready = g.ready_seen;
    mine.sound = g.frames_sound ? sound_sync::AnnounceFor(target) : sound_sync::Announcement();
    g.ready_seen = 0;
    g.local_made_upto = target;
  }
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
  RecordFinal();
  if (gap()) {
    SetFramePads(Pad{}, Pad{});
    g.frame_busy = g.local[size_t(g.frame)].busy;
    g.frame_ready = g.local[size_t(g.frame)].ready;
    if (g.frames_sound) sound_sync::FrameStart(ctx, base, g.frame, FinalUpto());
  } else {
    SetFramePads(g.local[size_t(g.frame)], g.remote[size_t(g.frame)]);
    g.frame_busy = g.local[size_t(g.frame)].busy | g.remote[size_t(g.frame)].busy;
    g.frame_ready = g.local[size_t(g.frame)].ready & g.remote[size_t(g.frame)].ready;
    // the sounds' callbacks both machines' inputs have announced, before the frame (the same frame on both)
    if (g.frames_sound) {
      sound_sync::FrameStart(ctx, base, g.frame, FinalUpto());
    } else {
      sound_sync::Deliver(ctx, base, g.local[size_t(g.frame)].sound, g.remote[size_t(g.frame)].sound);
    }
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
    if (g.speculating) AdjustFightDelay();
    Ensure(g.latest + g.delay + 1);
    // the input of frame latest + delay, once: a rollback in the call that made it leaves the live frame where it
    // was, and making it again here would replace an input the peer already has (the two games would then play
    // that frame with different buttons: the desyncs of bad networks, 2026-11-09)
    if (g.latest + g.delay > g.local_made_upto) {
      // a raised delay: the frames it skipped get the last input again (this side's inputs stay one a frame)
      const Pad last = g.local_made_upto >= 0 ? g.local[size_t(g.local_made_upto)] : Pad{};
      for (int64_t f = g.local_made_upto + 1; f < g.latest + g.delay; ++f) {
        Pad& fill = g.local[size_t(f)];
        std::memcpy(fill.bytes, last.bytes, 12);
        fill.busy = last.busy;
        fill.ready = 0;
        fill.sound = sound_sync::AnnounceFor(f);
      }
      Pad pad = ReadLocalPad();
      // no pause in a fight with rollback: the pause menu's load would turn the rest of the fight to lockstep, and a
      // player could hold the other one there (START is X_INPUT_GAMEPAD's 0x0010, the buttons' low byte)
      if (!REXCVAR_GET(mk9_net_pause_in_fights)) pad.bytes[1] &= uint8_t(~0x10);
      pad.sound = sound_sync::AnnounceFor(g.latest + g.delay);
      g.local[size_t(g.latest + g.delay)] = pad;
      g.local_made_upto = g.latest + g.delay;
    }
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
  // mk9-lauta (2026-11-10): a frame a rollback runs again with both players' inputs already known cannot be predicted
  // wrong, so no rollback will ever come back to its start: its boundary takes no snapshot (the next snapshot's undo
  // record covers the frames in between, tagged with the last frame kept: a rollback to that one or earlier still
  // finds every page) and so needs no rest either. It runs on as a lockstep frame does, UE3's render thread alongside,
  // only its sync hash taken. The boundary before the shown frame (the hidden frames' drawing done first) and one that
  // undoes a prediction rest as before. A rollback whose frames' inputs had arrived costs about half.
  bool light = REXCVAR_GET(mk9_net_light_resim) && g.speculating && g.frame > 0 && g.frame < g.latest &&
               g.frame <= g.remote_upto && g.rollback_to < 0 && g.mode_change_at < 0 && !loads;
  auto rest_and_keep = [&] {
    // the boundary at rest (quiesce.h): the frame that just ran into the timeline with its sync hash, and a wrong
    // prediction undone, with no other thread halfway through anything
    quiesce::Settle(ctx, base);
    const uint64_t t0 = NowUs();
    g.timeline.Advance(base, uint64_t(g.frame));
    g.ms_keep += (NowUs() - t0) / 1000.0;
    ++g.keeps;
  };
  if (light) {
    FinishFrameHash();
    ++g.light_boundaries;
  } else if (g.frame == 0) {
    quiesce::Settle(ctx, base);
  } else {
    rest_and_keep();
    FinishFrameHash();
    if (!g.speculating) {
      g.timeline.Verify(base);  // learning the pages written with no card (the fight's intro)
      if (g.frame >= kLearnFrames) {
        g.speculating = true;
        // the fight's own delay: lower than the intro's, so this side makes no input until the frames catch up with
        // the ones it made already (the intro's last frames)
        const int intro_delay = g.delay;
        g.delay = FightDelay();
        g.delay_check_at = g.latest + 120;  // AdjustFightDelay from here on
        g.lower_votes = 0;
        REXLOG_INFO("[net] rollback on from frame {} of the fight ({} pages written with no card learnt); input delay "
                    "{} frames (the intro's {}; round trip {:.0f} ms typical)",
                    g.frame, g.timeline.suspect_pages(), g.delay, intro_delay, RoundTripTypical());
      }
    }
  }
  Receive();  // the newest inputs: a prediction they contradict is undone now
  // a prediction that turned out wrong: back to that frame
  if (g.rollback_to >= 0 && g.rollback_to < g.frame) {
    if (light) {  // (the frames before a light boundary are final: not expected) the timeline brought up to here first
      rest_and_keep();
      light = false;
    }
    const int64_t to = g.rollback_to;
    const uint64_t t0 = NowUs();
    // mk9-lauta (2026-11-10): the renderer's queued work done first. The last shown frame's draws are recorded after its
    // Swap and read their vertex streams in guest memory then, and some of those live in the memory the timeline puts
    // back (the 4 KB pages under 0x40000000): read while the restore rewrote them, they drew stretched particles in
    // green and cyan and, once, a whole frame overexposed (the "green lights" left after the drain's gate, recorded at
    // ~200 ms). The rest has the guest's render thread flushed already: what is queued ends with that Swap.
    renut::gpu::render_thread::Drain();
    heap_undo::RollBack(to);  // the allocations of the frames undone go, the frees held for them are forgotten
    if (!g.timeline.RollBack(base, uint64_t(to))) {
      quiesce::Release();
      // a wrong prediction that cannot be undone: from here the two games go their own ways
      REXLOG_ERROR("[net] frame {} is out of the kept frames (now {})", to, g.frame);
      EndSession(EndReason::kDesync, true);
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
    // the frames run again get their hashes again: the old ones (their wrong predictions' runs) must not go out as the
    // final frame's (SendInputs takes the newest final frame's hash before the frame that just ran has its new one:
    // the lone false DESYNC of a very long round trip, 2026-11-10)
    for (int64_t f = to; f <= g.latest && size_t(f) < g.local_hash.size(); ++f) g.local_hash[size_t(f)] = 0;
    g.frame = to;
    if (g.mode_change_at >= to) g.mode_change_at = -1;
  }
  quiesce::Release();
  g.rollback_to = -1;
  CompareFinalHashes();
  RecordFinal();
  heap_undo::Confirm(FinalUpto());
  // a mode change every frame up to which is final: the rest of the epoch in lockstep
  if (g.mode_change_at >= 0 && g.mode_change_at < g.frame && FinalUpto() >= g.mode_change_at) {
    REXLOG_INFO("[net] the fight's mode ended at frame {}: lockstep from frame {} ({} rollbacks, {} frames run again)",
                g.mode_change_at, g.frame, g.rollbacks, g.resimulated);
    g.rollback = false;
    g.mode_change_at = -1;
    g.delay_check_at = 0;  // the lockstep after the fight: its delay for the round trip from the next frame
    g.timeline = snapshot::Timeline();
    heap_undo::End();
    sound_replay::OnFrame(ctx, base, 0, false);
    sound_sync::PassedEnd();
    sa2::renut::SetFrameHidden(false);
    // this frame, as lockstep does it: its local input is the one this side already sent
    WaitFor([&] { return g.remote_upto >= g.frame; });
    if (g.ended) return;
    Receive();
    sound_sync::FrameStart(ctx, base, g.frame, FinalUpto());
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
  // the sounds' callbacks due at this frame (in each of its runs), and the sounds of the frames now final numbered; or,
  // as they come, those a frame run again had lost
  if (g.frames_sound) {
    sound_sync::FrameStart(ctx, base, g.frame, FinalUpto());
  } else {
    sound_sync::PassedFrameStart(ctx, base, g.frame, g.frame < g.latest, FinalUpto());
  }
  // a frame run again is not shown and not paced, and keeps the sounds its first run started (sound_replay.h)
  sound_replay::OnFrame(ctx, base, uint64_t(g.epoch) << 32 | uint64_t(g.frame + 1), g.frame < g.latest);
  sa2::renut::SetFrameHidden(g.frame < g.latest);
  heap_undo::SetFrame(g.frame);
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
  if (Active()) {
    TakeEndRequest();
    // two games out of sync go their own ways: the session ends on both (the goodbye says why)
    if (g.started && g.desync_at >= 0 && REXCVAR_GET(mk9_net_end_on_desync)) EndSession(EndReason::kDesync, true);
  }
  if (!Active()) {
    if (g.started) {
      sa2::renut::SetFrameHidden(false);  // a session that ended shows every frame again
      sound_replay::OnFrame(ctx, base, 0, false);
      sound_sync::SetMode(ctx, base, sound_sync::Mode::kOff, 0);  // the sounds' callbacks go through again
      sound_sync::PassedEnd();
    }
    return;
  }
  g.game_thread = std::this_thread::get_id();
  WatchFrames();
  g_frame_started_us = NowUs();
  if (!g.started) {
    // the handshake can wait seconds for the peer's path (ICE): not a hang for the watchdog
    g_frame_started_us = 0;
    const bool shook = Handshake();
    g_frame_started_us = NowUs();
    if (!shook) {
      g.failed = true;
      return;
    }
    g.started = true;
    RecordOpen();
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
      heap_undo::Begin();  // the game's allocations and frees in the rollback too (heap_undo.h)
      quiesce::Release();
    }
    REXLOG_INFO("[net] epoch {} started at game frame {} ({})", g.epoch, game_frame,
                g.rollback ? fmt::format("rollback, delay {}", g.delay) : "lockstep");
  }
  // the sounds' callbacks wait for both machines (sound_sync.h): in lockstep by their announcements, in a fight's
  // epoch by frame numbers (mk9_sound_sync_fights) or else as they come
  sound_sync::SetMode(ctx, base,
                      g.frames_sound ? sound_sync::Mode::kFrames
                      : g.rollback   ? sound_sync::Mode::kOff
                                     : sound_sync::Mode::kLockstep,
                      g.epoch);
  if (g.rollback) {
    RollbackFrame(ctx, base);
  } else {
    sound_replay::OnFrame(ctx, base, 0, false);
    LockstepFrame(ctx, base);
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
  if (g.rollback) {
    // a fight polls none of these (a mode change goes back to lockstep first): one that does would answer each
    // machine's own
    static int logged = 0;
    if (logged < 10) {
      ++logged;
      REXLOG_WARN("[net] readiness poll {} in a fight with rollback at frame {}: {} (each machine's own)", int(flag),
                  g.frame - 1, local);
    }
    return local;
  }
  int bit = 0;
  while (bit < 7 && !(flag & (1u << bit))) ++bit;
  if (local) {
    g.ready_seen |= flag;
    g.ready_value[bit] = local;
  }
  return (g.frame_ready & flag) ? (g.ready_value[bit] ? g.ready_value[bit] : 1u) : 0u;
}

bool ConfirmFrame(const char* what) {
  if (!g.started || g.ended || g.failed || !g.rollback || std::this_thread::get_id() != g.game_thread) return true;
  const int64_t frame = g.frame - 1;  // the frame running
  if (frame < 0) return true;
  const bool waited = g.remote_upto < frame;
  WaitFor([&] {
    Receive();
    return g.remote_upto >= frame;
  });
  if (g.ended) return true;  // the session is over: the game goes on alone
  const bool doomed = g.rollback_to >= 0 && g.rollback_to <= frame;
  ++g.confirms;
  if (doomed) ++g.confirms_doomed;
  if (g.confirms <= 20) {
    REXLOG_INFO("[net] {} at frame {}{}: {}", what, frame, waited ? " (waited for the other player's inputs)" : "",
                doomed ? "a prediction was wrong, this frame will be run again" : "confirmed");
  }
  return !doomed;
}

bool AboutToLoad() {
  if (!g.started || g.ended || g.failed || !g.rollback || !g.speculating ||
      std::this_thread::get_id() != g.game_thread) {
    return true;
  }
  if (!ConfirmFrame("a package loads")) return false;
  // the rest of the epoch in lockstep, from the frame after this one (RollbackFrame, as a mode change)
  const int64_t frame = g.frame - 1;
  if (g.mode_change_at < 0 || g.mode_change_at > frame) {
    g.mode_change_at = frame;
    REXLOG_INFO("[net] a package loads at frame {}: the fight is over, lockstep from here", frame);
  }
  return true;
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

int64_t FrameRunning(uint32_t* epoch, bool* again) {
  if (!g.started || g.ended || g.failed) return -1;
  if (epoch) *epoch = g.epoch;
  if (again) *again = g.rollback && g.frame < g.latest;
  return g.frame - 1;
}

SessionInfo Info() {
  SessionInfo info;
  info.active = g.started && !g.ended && !g.failed;
  info.ended = g.ended || g.failed;
  info.end_reason = EndReason(g_end_reason.load(std::memory_order_acquire));
  if (info.end_reason == EndReason::kNone && g.failed) info.end_reason = EndReason::kFailed;
  if (const uint64_t since = g_waiting_since_us.load(std::memory_order_relaxed); since && info.active) {
    const uint64_t now = NowUs();
    info.waiting_s = now > since ? double(now - since) / 1e6 : 0.0;
    info.waiting = info.waiting_s >= 0.5;
  }
  info.player = g.player;
  info.rtt_ms = g.rtt_ms;
  info.delay = g.delay;
  info.rollback = g.rollback;
  info.speculating = g.speculating;
  info.rollbacks = g.rollbacks;
  info.resimulated = g.resimulated;
  // two games apart for good: a lone frame that differed (and matched again) says nothing to the players
  info.desynced = g.desync_at >= 0 && g.desync_frames >= 20;
  info.peer_release = g.peer_release;
  // the connection's indicator
  const int n = std::min(g.rtt_count, 8);
  if (n >= 2) {
    double mean = 0, var = 0;
    for (int i = 0; i < n; ++i) mean += g.rtt_samples[i];
    mean /= n;
    for (int i = 0; i < n; ++i) var += (g.rtt_samples[i] - mean) * (g.rtt_samples[i] - mean);
    info.jitter_ms = std::sqrt(var / n);
  }
  const uint64_t now = NowUs();
  int sent = 0, lost = 0;
  for (const auto& ping : g.pings) {
    // the pings of the last ~14 s that have had 2 s to come back
    if (!ping.at_us || now - ping.at_us < 2000000 || now - ping.at_us > 14000000) continue;
    ++sent;
    if (!ping.answered) ++lost;
  }
  info.loss_pct = sent >= 4 ? 100.0 * lost / sent : 0.0;
  info.relayed = g.relayed.load(std::memory_order_relaxed);
  return info;
}

void Leave() { g_end_request.store(1, std::memory_order_release); }

void PeerLeft() {
  int none = 0;
  g_end_request.compare_exchange_strong(none, 2, std::memory_order_acq_rel);  // this player's leaving goes first
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
