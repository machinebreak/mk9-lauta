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
//            u8 rollback (the host's mk9_net_rollback), u8 rollback delay, u8 release length + the release (8 and
//            up), u8 settings (10 and up: bit 0, the game thread's clock made of frames, virtual_time.h)
//   INPUT    u32 epoch, u32 ack (the peer's last frame received in order, +1), u32 first, u8 count, count x 18 bytes
//            (the guest's X_INPUT_GAMEPAD, big-endian, then the busy and the ready flags, then u32 a sound callback's
//            announcement, sound_sync.h), u32 hash frame (~0: none),
//            u64 hash (of that final frame), u32 the sender's live frame (time sync), u32 the live frame its last
//            paced release started, u16 the time since that release in 10 us (0xFFFF: none), i16 the sender's
//            measure of the phase in 10 us (C1) and u64 the fighters' state hash of the hash frame (0: none)
//   BARRIER  u32 epoch (the one starting there), u32 the sender's last frame of the epoch before
//   PING / PONG  u64 microseconds (the sender's clock, echoed)
//   BYE      u8 why (netplay.h, EndReason: the sender's player left, or its game found the two out of sync)
// The sync hash of a frame: the game's own checks during it (OnCheck: what its online compared, a draw of its random
// number generator among them), in a fight each player's health and position after it (fight_state.h HashCore;
// protocol 10) and the mk9_net_sync_ranges. A frame whose hashes differ is reported, and 20 of them tell the players.
// Beside it, in a fight, the rest of the fighters' state after the frame (HashFighters): a diagnostic that says at
// which frame two fights parted, before the sync hash sees it, and leaves a report; it tells nobody, as what it hashes
// is chosen from dumps (2026-11-11: a load's time it had kept differed by 0.1875 s between two machines whose fights
// went on alike).

#include "mk9_kratos.h"
#include "mk9_platform.h"
#include "online/netplay.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#if defined(_WIN32)
#include <windows.h>
#endif

#include <algorithm>
#include <cmath>
#include <atomic>
#include <condition_variable>
#include <chrono>
#include <array>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <ctime>
#include <mutex>
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

#include "online/feed.h"
#include "online/fight_state.h"
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
#include "online/virtual_time.h"
#include "profiler/sa2_cpu_profile_pc.h"
#include "renut_engine/gpu/backend/render_thread.h"
#include "renut_engine/gpu/sa2_renut_pacing.h"

#define XXH_INLINE_ALL
#include <xxhash.h>

REXCVAR_DEFINE_STRING(mk9_net_room, "", "MK9 Online", "The lobby room whose session this is (empty: none)");
REXCVAR_DEFINE_STRING(mk9_net_role, "", "MK9 Online", "mk9_net_room: this side is its host or its guest");
REXCVAR_DEFINE_STRING(mk9_online_name, "", "MK9 Online", "The player's name in the lobby");
REXCVAR_DEFINE_BOOL(mk9_net_room_listed, false, "MK9 Online",
                    "mk9_net_room's host: the room stays in the lobby's list while it plays (it was a public one), for "
                    "spectators");
REXCVAR_DEFINE_STRING(mk9_net_room_title, "", "MK9 Online", "mk9_net_room_listed: the room's title in the list");
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
REXCVAR_DEFINE_INT32(mk9_net_local_players, 1, "MK9 Online",
                     "Players at this machine in a session: 2 = two against two with four players, two at each "
                     "machine (the second pad is the game's player 3 on the host, player 4 on the guest)")
    .range(1, 2);
REXCVAR_DEFINE_STRING(mk9_net_lockstep_rest, "luck", "MK9 Online",
                      "Test: a lockstep frame's boundary: luck (at rest in Test Your Luck's modes, mk9_net_settle_luck), "
                      "pools (every lockstep frame: the thread pools' work finished), full (every one at rest), off");
REXCVAR_DEFINE_BOOL(mk9_net_settle_luck, true, "MK9 Online",
                    "Test Your Luck's modes: every lockstep frame's boundary at rest (quiesce.h: the render thread "
                    "flushed, the thread pools' work finished), so work in the background ends at the same frame on "
                    "both machines (the VS screen's fighter images before the slot machine's draws)");
REXCVAR_DEFINE_BOOL(mk9_net_light_resim, true, "MK9 Online",
                    "A frame a rollback runs again with both players' inputs already known starts without a snapshot or "
                    "a rest (no rollback can come back to it): rollbacks cost about half (tests: false = every frame "
                    "rests)");
REXCVAR_DEFINE_BOOL(mk9_net_light_live, true, "MK9 Online",
                    "A live frame of a fight whose rival input has already come (and the frame before it was shown) "
                    "runs without a snapshot or a rest too: it cannot be predicted wrong (false: every live frame "
                    "rests, as in 0.2.0)");
REXCVAR_DEFINE_BOOL(mk9_net_log_ready, false, "MK9 Online", "Tests: log the readiness polls answered ready");
REXCVAR_DEFINE_STRING(mk9_net_input_replay, "", "MK9 Online",
                      "Tests: this player's buttons come from a session recording of this player's (logs/session_*_p1 "
                      "or _p2.mk9net: its own pad of each frame), by epoch and frame, instead of the devices: a session "
                      "that parted run again on the same path");
REXCVAR_DEFINE_BOOL(mk9_watch_catch_up, true, "MK9 Online",
                    "A spectator far behind the match catches up in the menus (frames hidden, fast and muted)");
REXCVAR_DEFINE_BOOL(mk9_net_phase_align, true, "MK9 Online",
                    "In a fight, a side whose frames start ahead of the other's by part of a frame (the two machines' "
                    "start of the same frame, from the packets) moves its 60 Hz grid back a little each second: the two "
                    "see each other's inputs equally late (PLAN-0.4 C1; measured 2026-11-12 at 60 fps, RTT ~120 ms: the "
                    "phase from +-13 to +-3.5 ms, the side with more frames run again 10-20 % fewer, the total the same)");
REXCVAR_DEFINE_STRING(mk9_net_replay, "", "MK9 Online",
                      "A session recording (logs\\session_*.mk9net) played back from the game's start, as a spectator "
                      "watches a match: the menus fast and muted, the fights at their pace (F1 > Replays)");
REXCVAR_DEFINE_STRING(mk9_watch_catch_up_mode, "", "MK9 Online",
                      "Tests: how a spectator catches up: \"\" hidden and unpaced; \"drawn\" drawn and shown but unpaced "
                      "(with renut_vsync=false); \"paced\" hidden at the players' speed (it never catches up)");
REXCVAR_DEFINE_INT32(mk9_watch_peek_ms, 700, "MK9 Online",
                     "A spectator or a replay catching up shows a frame this often (ms; 0: none, the screen still until "
                     "it catches up)");
REXCVAR_DEFINE_BOOL(mk9_watch_catch_up_fights, true, "MK9 Online",
                    "A spectator far behind the match catches up in the fights too (until 2026-11-11 a fight's playback "
                    "parted from the match ~430 frames in: a stage's script read FMOD's position of a sound, now made "
                    "of frames, virtual_time.h)");
REXCVAR_DEFINE_BOOL(mk9_net_auto_delay, true, "MK9 Online",
                    "A fight's input delay follows the round trip all fight long (from the host's "
                    "mk9_net_rollback_delay up to 7 frames) so rollbacks stay about 5 frames deep: steadier characters "
                    "with 150-400 ms of ping");
REXCVAR_DEFINE_BOOL(mk9_net_record, false, "MK9 Online",
                    "Replays: each session's final frames (both players' inputs and the sync hash) go to "
                    "logs/session_<date>_<player>.mk9net (F1 > Replays plays them; the two players' files say where "
                    "the games parted: tools/online/session_report.py); the newest 10 are kept. Off (the default): "
                    "nothing is written, the inputs stay in memory for a desync's report alone (F2 > Online)");
REXCVAR_DEFINE_BOOL(mk9_net_end_on_desync, false, "MK9 Online",
                    "The session ends when the two games' sync hashes differ (false: it goes on and the log says so). "
                    "Off since the hashes are compared all along (2026-11-09): what they catch most is the game's random "
                    "draws in another order when a script waits on a sound, whose end comes on each machine's own "
                    "mixer (sound_sync.h), not always a fight that goes another way");
REXCVAR_DEFINE_BOOL(mk9_sound_sync_fights, true, "MK9 Online",
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
// says the release ("0.2.1"). 9 from 0.3.0 on: an epoch's last frame (its barrier's) has a sync hash of its own (it
// went into the next epoch's first frame's), and it is recorded (spectators: feed.h). 11 from 1.1.0 on: Test Your
// Luck's fights have a barrier and rollback too (mk9_capture.cpp, native 82495b68), where 1.0.0 stays in lockstep,
// and its lockstep frames settle (mk9_net_settle_luck: the slot machine draws the same modifiers on both).
constexpr uint8_t kVersion = 11;
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
  // mk9-lauta (2026-11-13): the second player at this machine (mk9_net_local_players = 2: two against two with four
  // players, two at each machine): the game's player 3 (the host's second) or 4 (the guest's)
  uint8_t bytes2[12] = {};
  bool operator==(const Pad& o) const {
    return std::memcmp(bytes, o.bytes, 12) == 0 && std::memcmp(bytes2, o.bytes2, 12) == 0;
  }
};
constexpr int kPadBytes = 18;   // a machine's pad on the wire and in a recording ('F')
constexpr int kPadBytes2 = 30;  // ... with its second player's (a machine with two players; the recording's 'G')
int PadBytes(int players) { return players >= 2 ? kPadBytes2 : kPadBytes; }

struct Session {
  bool configured = false, failed = false, started = false, ended = false;
  bool host = false;
  int local_players = 1;  // mk9_net_local_players: this machine's (a spectator: the host's)
  int peer_players = 1;   // the other machine's, as its HELLO said (a spectator: the guest's)
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
  std::vector<uint64_t> local_fighters, remote_fighters;  // the fighters' state hash by frame; 0 = none (not a fight)
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
  uint64_t fighters_compared = 0;    // frames whose fighters' state hashes were compared
  int64_t fighters_at = -1;          // the epoch's first frame whose fighters' state differed
  uint64_t fighters_frames = 0;
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
  uint64_t light_live = 0;           // live frames that started with no snapshot (mk9_net_light_live)
  // mk9-lauta (2026-11-10): a spectator's game (mk9_net_role=watch): the host's recording played back (feed.h)
  bool watching = false;
  bool replay = false;                 // a replay (mk9_net_replay): watching a recording read from a file
  bool replay_swap = false;            // ... recorded by the guest: its frames' two inputs player 2's first
  struct WatchedFrame {
    Pad p1, p2;
    uint64_t hash = 0;
  };
  struct WatchedEpoch {
    uint64_t start = 0;  // its frame 0's game frame there
    bool rollback = false;
    int64_t mode_end = -1;  // the fight's mode ended at this frame (from then on as lockstep)
    bool frames_sound = false;  // its sounds' callbacks by frame numbers: the players' words in the recording
    int players[2] = {1, 1};    // at the host's machine and at the guest's (two: their pads carry the second's)
    std::vector<WatchedFrame> frames;
  };
  std::vector<WatchedEpoch> watched;  // by epoch
  uint64_t watch_read = 0;            // the recording's bytes parsed
  bool watch_header = false;
  bool watch_other_release = false;  // the recording is of another release's match (its header's protocol)
  bool watch_peeking = false;         // mk9-lauta (2026-11-11): catching up, this frame is one shown (SpectatorFrame)
  uint64_t watch_peek_us = 0;         // ... the last one's time
  bool watch_hidden = false;          // catching up: frames hidden, unpaced and muted
  std::atomic<int64_t> watch_behind{0};  // frames of the match beyond this one's (the game thread's count, for Info)
  std::atomic<bool> watch_in_fight{false};  // it plays a fight back (for Info)
  std::atomic<bool> watch_fight{false};  // the fight it plays back is past its intro (for Info)
  std::atomic<int> game_state{-1};       // fight_state::State at the last frame (for Info)
  uint64_t watch_diffs = 0;
  uint64_t watch_compared = 0;        // frames whose sync hash was compared with the players'
  uint8_t watch_ready_before = 0;     // the last frame's recorded ready bits (a rise waits for this machine's loads)
  int64_t watch_words_upto = -1;      // a fight's frames whose players' sound words went to sound_sync
  bool prev_hidden = true;           // the frame that ran last was one run again (not shown)
  uint64_t delay_changes = 0;        // a fight's delay changed for the round trip (AdjustFightDelay)
  snapshot::Timeline timeline;
  int64_t latest = 0;                // the live edge: the newest frame started + 1 (frame < latest while replaying)
  int64_t rollback_to = -1;          // the earliest frame that ran on a prediction that turned out wrong
  int64_t mode_change_at = -1;       // a frame whose checks showed the game's mode change (tag 8 back to 0)
  int64_t peer_latest = -1;          // the peer's live frame, from its packets
  double advantage = 0;              // time sync: frames ahead of the peer, averaged
  // mk9-lauta (2026-11-11, C1): the two machines' start of the same frame
  uint64_t release_us = 0;           // the last paced Swap's release a fight's boundary saw (sa2 LastReleaseUs)
  int64_t release_frame = 0;         // the live frame that release started (g.latest at the first boundary after it)
  std::array<std::pair<int64_t, uint64_t>, 128> starts{};  // live frame -> its release, the latest 128
  double phase = 0;                  // ms this side started the peer's frames before the peer did (the packets')
  double peer_phase = 0;             // the peer's own measure, from its packets (the other way round)
  uint64_t phase_samples = 0, phase_check_at = 0, phase_shifts = 0;
  int phase_ahead_checks = 0;        // checks running that found this side ahead past the dead band
  double phase_shifted_ms = 0;       // this side's grid moved back so far
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

// The players at the host's machine (player 0) or the guest's (1): 2 with mk9_net_local_players on that side.
int PlayersOf(int player) { return player == g.player ? g.local_players : g.peer_players; }

// mk9_net_record: the session's final frames, to a file in logs. Records (little-endian):
//   header   "MK9NET1\0", u32 version (1; from protocol 10 on, + the protocol x 256: a spectator of another release
//            leaves), u8 player, u32 seed, u8 rollback, u8 lockstep delay, u8 rollback delay
//   'E'      u32 epoch, u64 the game frame of its frame 0, u8 rollback
//   'F'      u32 frame, 18 bytes this player's input, 18 bytes the other's (the packets' pad record), u64 sync hash
//   'M'      u32 frame: the fight's mode ended there (mk9-lauta, 2026-11-10)
// mk9-lauta (2026-11-11): the replays are off by default (F2 > Online). The game thread never touches the disk: a
// frame's records are built in one buffer and handed over at once (to the spectators' feed, and to the recording);
// with mk9_net_record on, a thread of its own writes them to the file every 2 s. Off, they stay in memory (about 3 KB a
// second of session, in pieces of 1 MB: nothing there is ever copied again; 64 MB at most) for a desync's report alone,
// which writes them beside it (logs\desync_...\session.mk9net).
constexpr size_t kRecordPiece = 1u << 20;
constexpr size_t kRecordMemoryMax = 64u << 20;

struct Recorder {
  bool open = false;     // RecordOpen ran: the session's records are kept
  bool to_file = false;  // mk9_net_record: the writer's thread writes them to `path`
  bool feeding = false;  // the host: every record also goes to the spectators (feed.h)
  std::string path;      // the file's, UTF-8 (the log, the desync's summary)
  std::string record;    // the records being built (game thread)
  int64_t upto = -1;     // the epoch's frames written
  uint64_t frames = 0;
  // the game thread's records for the writer (to_file), or kept in memory (else)
  std::mutex mutex;
  std::condition_variable wake;
  std::string pending;              // to_file: not written yet
  std::vector<std::string> memory;  // !to_file: all of them, in pieces of kRecordPiece
  size_t memory_size = 0;
  bool closing = false, flush_now = false;
  std::thread writer;
};
Recorder g_rec;

void RecordPut(const void* data, size_t size) {
  if (g_rec.open || g_rec.feeding) g_rec.record.append(static_cast<const char*>(data), size);
}

template <typename T>
void RecordValue(T v) {
  RecordPut(&v, sizeof(v));
}

// The records built since the last hand-over, to the spectators and to the recording (game thread).
void RecordHandOver() {
  if (g_rec.record.empty()) return;
  if (g_rec.feeding) feed::Append(g_rec.record.data(), g_rec.record.size());
  if (g_rec.open) {
    std::lock_guard<std::mutex> lock(g_rec.mutex);
    if (g_rec.to_file) {
      g_rec.pending += g_rec.record;
    } else if (g_rec.memory_size + g_rec.record.size() <= kRecordMemoryMax) {
      if (g_rec.memory.empty() || g_rec.memory.back().size() + g_rec.record.size() > kRecordPiece) {
        g_rec.memory.emplace_back();
        g_rec.memory.back().reserve(std::max(kRecordPiece, g_rec.record.size()));
      }
      g_rec.memory.back() += g_rec.record;
      g_rec.memory_size += g_rec.record.size();
    }
  }
  g_rec.record.clear();
}

uint8_t Protocol();

void RecordHeader() {
  RecordPut("MK9NET1", 8);
  RecordValue<uint32_t>(1u | uint32_t(Protocol()) << 8);
  RecordValue<uint8_t>(uint8_t(g.player));
  RecordValue<uint32_t>(g.seed);
  RecordValue<uint8_t>(g.rollback_wanted ? 1 : 0);
  RecordValue<uint8_t>(uint8_t(g.lockstep_delay));
  RecordValue<uint8_t>(uint8_t(g.rollback_delay));
}

void RecordPad(const Pad& pad, bool second) {
  RecordPut(pad.bytes, 12);
  RecordValue(pad.busy);
  RecordValue(pad.ready);
  RecordValue(pad.sound);
  if (second) RecordPut(pad.bytes2, 12);
}

// The writer's thread (mk9_net_record): the logs' newest sessions kept, the file made, the records written every 2 s
// (at once when asked: a desync, the session's end) until the session closes.
void RecordWriter(std::filesystem::path logs, std::filesystem::path file) {
  std::error_code ec;
  std::filesystem::create_directories(logs, ec);
  std::vector<std::filesystem::path> old;  // the newest 10 stay, this one with them
  for (const auto& entry : std::filesystem::directory_iterator(logs, ec)) {
    const std::wstring name = entry.path().filename().wstring();
    if (name.rfind(L"session_", 0) == 0 && entry.path().extension() == L".mk9net") old.push_back(entry.path());
  }
  std::sort(old.begin(), old.end());
  while (old.size() >= 10) {
    std::filesystem::remove(old.front(), ec);
    old.erase(old.begin());
  }
  FILE* f = mk9::platform::OpenFile(file, "wb");
  if (!f) REXLOG_WARN("[net] the replay's file could not be made: the session is not recorded");
  std::string chunk;
  for (;;) {
    bool closing = false;
    {
      std::unique_lock<std::mutex> lock(g_rec.mutex);
      g_rec.wake.wait_for(lock, std::chrono::seconds(2), [] { return g_rec.closing || g_rec.flush_now; });
      chunk.swap(g_rec.pending);
      g_rec.flush_now = false;
      closing = g_rec.closing;
    }
    if (f && !chunk.empty()) {
      std::fwrite(chunk.data(), 1, chunk.size(), f);
      std::fflush(f);
    }
    chunk.clear();
    if (closing) break;
  }
  if (f) std::fclose(f);
}

void RecordOpen() {
  if (g_rec.open) return;
  g_rec.feeding = g.host && !g.room.empty();  // a room's host: its spectators get the same records
  g_rec.open = true;
  g_rec.to_file = REXCVAR_GET(mk9_net_record);
  g_rec.record.reserve(4096);
  if (g_rec.to_file) {
    const std::filesystem::path logs = mk9::platform::ExeDir() / "logs";
    const std::tm local = mk9::platform::LocalTime(std::time(nullptr));
    char name[64];
    std::strftime(name, sizeof(name), "session_%Y%m%d_%H%M%S", &local);
    const std::filesystem::path path = logs / (std::string(name) + (g.player == 0 ? "_p1" : "_p2") + ".mk9net");
    const std::u8string utf8 = path.u8string();
    g_rec.path.assign(reinterpret_cast<const char*>(utf8.data()), utf8.size());
    g_rec.pending.reserve(64 * 1024);
    g_rec.writer = std::thread(RecordWriter, logs, path);
    REXLOG_INFO("[net] the session's inputs go to {} (replays on: mk9_net_record)", g_rec.path);
  }
  RecordHeader();
  RecordHandOver();
}

void RecordEpoch() {
  if (!g_rec.open && !g_rec.feeding) return;
  RecordValue<uint8_t>('E');
  RecordValue<uint32_t>(g.epoch);
  RecordValue<uint64_t>(g.start_game_frame);
  // bit 0 rollback; bit 1 (mk9-lauta, 2026-11-10): its sounds' callbacks by frame numbers (sound_sync.h kFrames);
  // bits 2 and 3 (2026-11-13): two players at the host's machine, at the guest's (their frames are 'G')
  RecordValue<uint8_t>(uint8_t((g.rollback ? 1 : 0) | (g.frames_sound ? 2 : 0) | (PlayersOf(0) == 2 ? 4 : 0) |
                               (PlayersOf(1) == 2 ? 8 : 0)));
  g_rec.upto = -1;
  RecordHandOver();
}

int64_t FinalUpto();

// The frames that are final now (both players' real inputs, their last run done), in order.
void RecordFinal() {
  if (!g_rec.open && !g_rec.feeding) return;
  const int64_t upto = std::min(FinalUpto(), g.frame - 1);
  for (int64_t f = g_rec.upto + 1; f <= upto; ++f) {
    if (!g.have_remote[size_t(f)]) break;
    const bool four = g.local_players == 2 || g.peer_players == 2;
    RecordValue<uint8_t>(four ? 'G' : 'F');
    RecordValue<uint32_t>(uint32_t(f));
    RecordPad(g.local[size_t(f)], four);
    RecordPad(g.remote[size_t(f)], four);
    RecordValue<uint64_t>(g.local_hash[size_t(f)]);
    g_rec.upto = f;
    ++g_rec.frames;
  }
  RecordHandOver();
}

// mk9-lauta (2026-11-10): 'M' u32 frame: the fight's mode ended at that frame (the rest of the epoch in lockstep), for a
// spectator to play it as the players did.
void RecordModeEnd(int64_t frame) {
  if (!g_rec.open && !g_rec.feeding) return;
  RecordValue<uint8_t>('M');
  RecordValue<uint32_t>(uint32_t(frame));
  RecordHandOver();
}

// The file written now (the writer's thread; the game thread does not wait for it).
void RecordFlushSoon() {
  if (!g_rec.to_file) return;
  {
    std::lock_guard<std::mutex> lock(g_rec.mutex);
    g_rec.flush_now = true;
  }
  g_rec.wake.notify_one();
}

// The records kept in memory (the replays off), for a desync's report: a copy (any thread may write it out).
std::string RecordMemoryCopy() {
  std::lock_guard<std::mutex> lock(g_rec.mutex);
  std::string out;
  out.reserve(g_rec.memory_size);
  for (const std::string& piece : g_rec.memory) out += piece;
  return out;
}

// The session's end: the writer writes what is left and closes the file (the session is over: a moment is fine).
void RecordClose() {
  if (!g_rec.open) return;
  RecordHandOver();
  if (g_rec.writer.joinable()) {
    {
      std::lock_guard<std::mutex> lock(g_rec.mutex);
      g_rec.closing = true;
    }
    g_rec.wake.notify_one();
    g_rec.writer.join();
  }
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

double RoundTripTypical();

// mk9-lauta (2026-11-11, C1): a packet from the peer said it left `into_us` after the release that started its live
// frame `remote_frame`: that frame started on the peer at its arrival minus half the round trip minus into_us, in this
// machine's time; against when it started here (the starts kept by frame), the ms this side was ahead, averaged. The
// two grids run at 60 Hz each on its own clock, at any phase: with ~44 ms each way, 8 ms of phase made one side see
// the other's inputs 2.2 frames late and the other 3.1, and one side did nearly all the rollbacks
// (docs/online/baseline-0.3.0.md: 0 against 305). Half the round trip is a guess, and the jitter adds on both sides:
// the error is the same in the two measures, so the alignment goes by half their difference (the peer's comes in its
// packets), as NTP does. By frame, not by 1/60 s: a machine late on its grid does not throw it off.
void NotePhase(int64_t remote_frame, uint64_t into_us) {
  if (!g.rollback || g.rtt_count == 0 || remote_frame < 0) return;
  const auto& here = g.starts[size_t(remote_frame) % g.starts.size()];
  if (here.first != remote_frame || !here.second) return;  // not started here yet, or long ago
  const uint64_t now = NowUs();
  uint64_t arrived = g.link->LastArrivalUs();
  if (!arrived || arrived > now) arrived = now;
  const double there = double(arrived) - RoundTripTypical() * 500.0 - double(into_us);
  const double ahead_ms = (there - double(here.second)) / 1000.0;
  if (std::abs(ahead_ms) > 300.0) return;  // nonsense
  g.phase = g.phase_samples ? g.phase * 0.95 + ahead_ms * 0.05 : ahead_ms;
  ++g.phase_samples;
}

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
    g.local_fighters.resize(grow, 0);
    g.remote_fighters.resize(grow, 0);
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
// mk9-lauta (2026-11-11, A3): the first frame whose hashes differ leaves a report in logs\desync_<date>_p<N>: the
// fighters' hashed bytes of the frames up to it (fight_state.h WriteFighters) and what the session was then
// (summary.txt); tools/online/desync_report.py with the two players' folders says the first field that differs. The
// session's inputs are in its .mk9net (named in the summary).
std::string g_desync_report;  // the report's folder (the online screens offer it)
std::mutex g_desync_report_mutex;
double RoundTripTypical();

void WriteDesyncReport(int64_t frame, uint64_t local, uint64_t remote, bool fighters_only) {
  const std::tm when = mk9::platform::LocalTime(std::time(nullptr));
  char name[64];
  std::strftime(name, sizeof(name), "desync_%Y%m%d_%H%M%S", &when);
  const std::filesystem::path dir = mk9::platform::ExeDir() / "logs" /
                                    (std::string(name) + (g.player == 0 ? "_p1" : "_p2") +
                                     (fighters_only ? "_fighters" : ""));
  std::error_code ec;
  std::filesystem::create_directories(dir, ec);
  const bool fighters =
      fight_state::WriteFighters(dir / L"fighters.bin", uint64_t(g.epoch) << 32 | uint64_t(frame));
  if (FILE* f = mk9::platform::OpenFile(dir / "summary.txt", "w")) {
    std::fprintf(f, "mk9-lauta %s, session protocol %u, player %d\n", MK9_LAUTA_VERSION, unsigned(Protocol()),
                 g.player + 1);
    std::fprintf(f, "first frame whose hashes differ: epoch %u frame %lld (game frame %llu)\n", g.epoch,
                 static_cast<long long>(frame), static_cast<unsigned long long>(g.start_game_frame + frame));
    std::fprintf(f, "what differs: %s\n",
                 fighters_only ? "the fighters' state only (the sync hash agrees there: the game's checks, the "
                                 "health and the positions): a diagnostic"
                               : "the sync hash (the game's checks, or the health or a position: the two games apart)");
    std::fprintf(f, "hash here %016llx, the other's %016llx\n", static_cast<unsigned long long>(local),
                 static_cast<unsigned long long>(remote));
    std::fprintf(f, "epoch %s, input delay %d, round trip %.0f ms typical, %llu rollbacks so far\n",
                 g.rollback ? "with rollback (a fight)" : "lockstep", g.delay, RoundTripTypical(),
                 static_cast<unsigned long long>(g.rollbacks));
    std::fprintf(f, "the fighters' bytes: %s\n", fighters ? "fighters.bin" : "(not written)");
    std::fprintf(f, "the session's inputs: %s\n",
                 g_rec.to_file ? g_rec.path.c_str()
                 : g_rec.open  ? "session.mk9net (in this folder: the replays are off, they were kept in memory)"
                               : "(not recorded)");
    std::fclose(f);
  }
  // mk9-lauta (2026-11-11): the replays off, the inputs kept in memory go beside the report (a copy now, the file on a
  // thread of its own: the game thread does not wait on the disk)
  if (g_rec.open && !g_rec.to_file && !fighters_only) {
    std::thread([inputs = RecordMemoryCopy(), file = dir / L"session.mk9net"] {
      if (FILE* out = mk9::platform::OpenFile(file, "wb")) {
        std::fwrite(inputs.data(), 1, inputs.size(), out);
        std::fclose(out);
      }
    }).detach();
  }
  const std::u8string utf8 = dir.u8string();
  const std::string path(reinterpret_cast<const char*>(utf8.c_str()), utf8.size());
  if (!fighters_only) {  // the one the online screens offer: the players were told of this one
    std::lock_guard lock(g_desync_report_mutex);
    g_desync_report = path;
  }
  REXLOG_WARN("[net] the {} report is in {} (tools/online/desync_report.py with the other player's)",
              fighters_only ? "fighters' state" : "desync's", path);
}

// mk9-lauta (2026-11-11): the fighters' state hashes of a final frame, a diagnostic (the head of this file): its first
// difference in an epoch is logged, and leaves a report when the sync hash still agrees there (a frame where both
// differ leaves the desync's own).
void CompareFighters(int64_t frame, bool checks_agree) {
  const uint64_t local = g.local_fighters[size_t(frame)], remote = g.remote_fighters[size_t(frame)];
  if (!local || !remote) return;
  ++g.fighters_compared;
  if (local == remote) return;
  ++g.fighters_frames;
  if (g.fighters_at >= 0) return;
  g.fighters_at = frame;
  REXLOG_WARN("[net] the fighters' state differs from frame {} of epoch {} on (game frame {}; {:016x}, the peer's "
              "{:016x}){}",
              frame, g.epoch, g.start_game_frame + frame, local, remote,
              checks_agree ? ": the sync hash agrees there, the session goes on as it was (a diagnostic)" : "");
  if (checks_agree) WriteDesyncReport(frame, local, remote, true);
}

void CompareFinalHashes() {
  const int64_t upto = FinalUpto();
  while (g.compared_upto < upto) {
    const int64_t frame = g.compared_upto + 1;
    const uint64_t local = g.local_hash[size_t(frame)], remote = g.remote_hash[size_t(frame)];
    if (!remote && frame >= g.remote_hash_newest) break;  // the peer's hash of it may still come
    g.compared_upto = frame;
    if (!local || !remote) continue;  // one the peer skipped
    ++g.hashes_compared;
    CompareFighters(frame, local == remote);
    if (local == remote) continue;
    if (g.desync_at < 0) {
      g.desync_at = frame;
      if (!REXCVAR_GET(mk9_net_end_on_desync)) {
        REXLOG_WARN("[net] the two games differ from frame {} of epoch {} on (the sync hash: the game's checks, the "
                    "health or a position): the session goes on (mk9_net_end_on_desync); a fight may look different "
                    "on the two screens from here",
                    frame, g.epoch);
      }
      if (g_rec.to_file) {
        RecordFlushSoon();
        REXLOG_WARN("[net] the session's inputs are in {}: with the other player's, tools/online/session_report.py "
                    "says where the two games parted", g_rec.path);
      }
      WriteDesyncReport(frame, local, remote, false);
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
  if (g.watching) {
    REXLOG_INFO("[watch] {} of {} frames compared differed from the match", g.watch_diffs, g.watch_compared);
  }
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
  // the settings both games must share; bit 1 (mk9-lauta 2026-11-11): Kratos installed (mk9_kratos.h); bit 2
  // (2026-11-13): two players at this machine (its pads carry the second's: PadBytes)
  p.push_back(uint8_t((virtual_time::Enabled() ? 1 : 0) | (mk9::kratos::Installed() ? 2 : 0) |
                      (g.local_players == 2 ? 4 : 0)));
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
  // (a machine with two players: 30 bytes a frame, so 40 frames keep a packet as short as 64 of 18)
  first = std::max<int64_t>(first, newest - (g.local_players == 2 ? 40 : kMaxPerPacket) + 1);
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
    if (g.local_players == 2) p.insert(p.end(), g.local[size_t(f)].bytes2, g.local[size_t(f)].bytes2 + 12);
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
  // mk9-lauta (2026-11-11, C1): the live frame the last paced release started here and the time since it (10 us
  // units; 0xFFFF: none yet in this epoch), and this side's own measure of the phase (10 us units, signed): the
  // peer's two halves of the measure
  Put32(p, uint32_t(g.release_frame));
  Put16(p, g.release_us ? uint16_t(std::min<uint64_t>((NowUs() - std::min(NowUs(), g.release_us)) / 10, 0xFFFE))
                        : uint16_t(0xFFFF));
  Put16(p, uint16_t(int16_t(std::clamp(g.phase * 100.0, -32767.0, 32767.0))));
  Put64(p, final_frame >= 0 && g.local_hash[size_t(final_frame)] ? g.local_fighters[size_t(final_frame)] : 0);
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
        if (left >= 14 + p[12]) {
          g.peer_players = ((p[13 + p[12]] >> 2) & 1) ? 2 : 1;
          if (g.peer_players == 2 || g.local_players == 2) {
            REXLOG_INFO("[net] players at each machine: {} here, {} there", g.local_players, g.peer_players);
          }
        }
        // the settings both games must share (a test switch changed on one side)
        if (left >= 14 + p[12] && ((p[13 + p[12]] >> 1) & 1) != (mk9::kratos::Installed() ? 1 : 0)) {
          REXLOG_ERROR("[net] the other player's game {} Kratos and this one {}: the games will go apart (the rooms "
                       "keep such a fight from starting)",
                       ((p[13 + p[12]] >> 1) & 1) ? "has" : "does not have", mk9::kratos::Installed() ? "has" : "does not");
        }
        if (left >= 14 + p[12] && (p[13 + p[12]] & 1) != (virtual_time::Enabled() ? 1 : 0)) {
          REXLOG_ERROR("[net] the other player's game {} the game thread's clock made of frames and this one {} "
                       "(mk9_virtual_time): their clocks differ, the games may go apart",
                       (p[13 + p[12]] & 1) ? "has" : "does not have", virtual_time::Enabled() ? "has" : "does not");
        }
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
      const int size = PadBytes(g.peer_players);
      if (left < 13 + count * size + 16) continue;
      g.peer_acked = std::max(g.peer_acked, ack);
      for (int i = 0; i < count; ++i) {
        const int64_t f = first + i;
        Ensure(f);
        if (g.have_remote[size_t(f)]) continue;
        Pad& pad = g.remote[size_t(f)];
        std::memcpy(pad.bytes, p + 9 + i * size, 12);
        pad.busy = p[9 + i * size + 12];
        pad.ready = p[9 + i * size + 13];
        pad.sound = Get32(p + 9 + i * size + 14);
        if (size == kPadBytes2) std::memcpy(pad.bytes2, p + 9 + i * size + 18, 12);
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
      const uint8_t* h = p + 9 + count * size;
      const uint32_t hash_frame = Get32(h);
      if (hash_frame != kNoHash && Get64(h + 4) != 0) {
        Ensure(hash_frame);
        g.remote_hash[hash_frame] = Get64(h + 4);
        g.remote_hash_newest = std::max<int64_t>(g.remote_hash_newest, hash_frame);
        if (left >= 13 + count * size + 32) g.remote_fighters[hash_frame] = Get64(h + 24);
      }
      g.peer_latest = std::max<int64_t>(g.peer_latest, int64_t(Get32(h + 12)));
      if (left >= 13 + count * size + 24) {
        const uint32_t into = uint32_t(h[20]) | uint32_t(h[21]) << 8;
        if (into != 0xFFFF) NotePhase(int64_t(Get32(h + 16)), uint64_t(into) * 10);
        g.peer_phase = double(int16_t(uint16_t(h[22] | h[23] << 8))) / 100.0;
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

// mk9_net_input_replay: the recording's own pads (the first of each 'F' record), by epoch and frame.
std::vector<std::vector<std::array<uint8_t, 12>>> g_replay_pads;

void LoadReplayPads() {
  static bool loaded = false;
  if (loaded) return;
  loaded = true;
  const std::string path = REXCVAR_GET(mk9_net_input_replay);
  if (path.empty()) return;
  FILE* f = std::fopen(path.c_str(), "rb");
  if (!f) {
    REXLOG_ERROR("[net] mk9_net_input_replay: cannot read {}", path);
    return;
  }
  std::vector<uint8_t> data;
  uint8_t chunk[65536];
  for (size_t n; (n = std::fread(chunk, 1, sizeof(chunk), f)) > 0;) data.insert(data.end(), chunk, chunk + n);
  std::fclose(f);
  size_t at = 20;  // the header
  uint64_t frames = 0;
  while (at < data.size()) {
    const uint8_t tag = data[at];
    if (tag == 'E' && at + 14 <= data.size()) {
      g_replay_pads.emplace_back();
      at += 14;
    } else if ((tag == 'F' || tag == 'G') &&
               at + 1 + 4 + 2 * PadBytes(tag == 'G' ? 2 : 1) + 8 <= data.size() && !g_replay_pads.empty()) {
      uint32_t frame;
      std::memcpy(&frame, &data[at + 1], 4);
      auto& pads = g_replay_pads.back();
      if (pads.size() <= frame) pads.resize(size_t(frame) + 1);
      std::memcpy(pads[frame].data(), &data[at + 5], 12);
      ++frames;
      at += 1 + 4 + 2 * PadBytes(tag == 'G' ? 2 : 1) + 8;
    } else if (tag == 'M' && at + 5 <= data.size()) {
      at += 5;
    } else {
      break;
    }
  }
  REXLOG_INFO("[net] mk9_net_input_replay: {} epoch(s), {} frames of this player's buttons from {}",
              g_replay_pads.size(), frames, path);
}

// The recording's buttons of this frame, over the devices' (none past the recording's end).
void ReplayPad(uint32_t epoch, int64_t frame, Pad* pad) {
  LoadReplayPads();
  if (g_replay_pads.empty()) return;
  std::memset(pad->bytes, 0, 12);
  if (epoch < g_replay_pads.size() && frame >= 0 && size_t(frame) < g_replay_pads[epoch].size()) {
    std::memcpy(pad->bytes, g_replay_pads[epoch][size_t(frame)].data(), 12);
  }
}

// This player's pad: every device of this PC together (the four players' seats merged), so the keyboard and any pad
// play, whichever seat the options give them offline (mk9_controls.h). mk9-lauta (2026-11-13): with two players at
// this machine, the first seat's devices (the first pad, the keyboard on player 1) are the first player's and the
// second seat's (the second pad, the keyboard on player 2) the second's.
Pad ReadLocalPad() {
  Pad pad;
  auto* runtime = REX_KERNEL_STATE() ? REX_KERNEL_STATE()->emulator() : nullptr;
  auto* is = runtime ? static_cast<rex::input::InputSystem*>(runtime->input_system()) : nullptr;
  if (!is) return pad;
  if (g.local_players == 2) {
    rex::input::X_INPUT_STATE state{};
    if (is->GetState(0, &state) == 0) std::memcpy(pad.bytes, &state.gamepad, 12);
    state = {};
    if (is->GetState(1, &state) == 0) std::memcpy(pad.bytes2, &state.gamepad, 12);
    return pad;
  }
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
  if (role == "watch") {
    // mk9-lauta (2026-11-10): a spectator: the lobby as the player's name, the room joined with an id of its own, and
    // the host's recording through the relay (feed.h); it plays as player 1's seat, both pads from the recording
    std::string name = REXCVAR_GET(mk9_online_name);
    if (name.empty()) name = "Spectator";
    lobby::Start(name);
    std::random_device rd;
    const std::string id = fmt::format("{:08x}{:08x}", rd(), rd());
    lobby::Join(room, id);
    g.watching = true;
    g.host = false;
    g.room = room;
    room::EnterWatch(room, name, id);
    return true;
  }
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
  std::string title = REXCVAR_GET(mk9_net_room_title);
  if (title.empty()) title = name;
  const bool listed = g.host && REXCVAR_GET(mk9_net_room_listed);
  if (g.host) {
    lobby::Host(room, title, listed, true);
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
  room::EnterSession(room, g.host, name, id, title, listed);  // the room's chat goes on in the session
  return true;
}

bool Configure() {
  g.configured = true;
  // mk9-lauta (2026-11-11, PLAN-0.4 F5): a replay is a spectator whose match is a file, there whole from the start
  if (const std::string replay = REXCVAR_GET(mk9_net_replay); !replay.empty()) {
    g.watching = true;
    g.replay = true;
    g.host = false;
    g.player = 0;
    const std::filesystem::path path = std::filesystem::u8path(replay);
    if (!feed::LoadFile(path)) {
      REXLOG_ERROR("[watch] the replay {} cannot be read", replay);
      g.failed = true;
      return false;
    }
    REXLOG_INFO("[watch] a replay: {} ({} bytes)", replay, feed::Size());
    return true;
  }
  const int host_port = REXCVAR_GET(mk9_net_host);
  const std::string join = REXCVAR_GET(mk9_net_join);
  const std::string room = REXCVAR_GET(mk9_net_room);
  if (host_port <= 0 && join.empty() && room.empty()) return false;
  g.player = 0;
  g.lockstep_delay = std::clamp(REXCVAR_GET(mk9_net_delay), 0, 15);
  g.rollback_delay = std::clamp(REXCVAR_GET(mk9_net_rollback_delay), -1, 7);
  g.rollback_wanted = REXCVAR_GET(mk9_net_rollback);
  g.delay = g.lockstep_delay;
  g.local_players = std::clamp(REXCVAR_GET(mk9_net_local_players), 1, 2);
  if (!room.empty()) {
    if (!ConfigureRoom(room, REXCVAR_GET(mk9_net_role))) return false;
    g.player = g.host || g.watching ? 0 : 1;
    if (g.host && !seed::Configured(&g.seed)) g.seed = std::random_device{}();
    ParseRanges();
    REXLOG_INFO("[net] room {} as its {}; lockstep delay {}, rollback {} (delay {}); the host decides", room,
                g.host ? "host" : g.watching ? "spectator" : "guest", g.lockstep_delay, g.rollback_wanted ? "on" : "off",
                g.rollback_delay);
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
  g.local_fighters.clear();
  g.remote_fighters.clear();
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
  g.fighters_compared = 0;
  g.fighters_at = -1;
  g.fighters_frames = 0;
  g.rollback = rollback;
  g.frames_sound = rollback && REXCVAR_GET(mk9_sound_sync_fights);
  g.speculating = false;
  g.rollback_to = -1;
  g.prev_hidden = true;
  g.mode_change_at = -1;
  g.advantage = 0;
  g.release_us = 0;
  g.starts.fill({-1, 0});
  g.phase = 0;
  g.peer_phase = 0;
  g.phase_samples = 0;
  g.phase_check_at = 0;
  g.phase_ahead_checks = 0;
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
  uint8_t* const base = rex::system::kernel_memory()->TranslateVirtual<uint8_t*>(0);
  // mk9-lauta (2026-11-11): in a fight, each player's health and position too (fight_state.h HashCore): two screens
  // showing another fight are a desync though the game's checks agree (5 % of a fighter's health taken on one machine
  // went 359 frames without them seeing it)
  h = fight_state::HashCore(base, h);
  // mk9-lauta (2026-11-11, A2): in a fight, the fighters' state apart (fight_state.h HashFighters: what both machines
  // always had the same; 0: not a fight's frame), so where a desync starts shows at its frame, not once it reaches the
  // random number generator. Not a spectator's: the recordings have the sync hash only.
  g.local_fighters[size_t(g.frame - 1)] =
      g.watching ? 0 : fight_state::HashFighters(base, 0, uint64_t(g.epoch) << 32 | uint64_t(g.frame - 1));
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
                       "batches collected of {} query objects, {} pool waits); advantage {:.1f}, {} sleeps; phase {:+.1f} ms ahead "
                       "(here {:+.1f}, there {:+.1f}), the grid moved back {:.1f} ms in {} steps; {} frames "
                       "run again and {} live with no snapshot; the delay changed {} times",
                       g.speculating ? "" : " (learning)", g.rollbacks, g.resimulated, g.max_depth, g.predicted,
                       g.mispredicted, g.keeps ? g.ms_keep / double(g.keeps) : 0.0,
                       g.rollbacks ? g.ms_restore / double(g.rollbacks) : 0.0,
                       q.settles ? q.ms_total / double(q.settles) : 0.0,
                       q.settles ? q.ms_flush / double(q.settles) : 0.0,
                       q.settles ? q.ms_drain / double(q.settles) : 0.0,
                       q.settles ? q.ms_park / double(q.settles) : 0.0, q.ms_max, q.unparked, q.collected,
                       q.queries_seen, q.pool_waits, g.advantage, g.syncs_slept, (g.phase - g.peer_phase) / 2.0,
                       g.phase, g.peer_phase, g.phase_shifted_ms, g.phase_shifts, g.light_boundaries, g.light_live,
                       g.delay_changes);
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
                      "in a fight ({} by one machine only, {} of them given here by itself), {} rollbacks for them",
                      callbacks.numbered, callbacks.deferred, callbacks.delivered, callbacks.dropped,
                      callbacks.waiting, callbacks.passed, callbacks.regiven, callbacks.flushed, callbacks.announced,
                      callbacks.alone, callbacks.made, callbacks.rollbacks);
    REXLOG_INFO("[net] epoch {} frame {} (game {}), rtt {:.1f} ms, delay {}, waited {:.1f} ms in total ({} stalls, "
                "longest {:.1f} ms), peer inputs to {}; sent {}, received {}; {} frames' sync checked{}{}",
                g.epoch, g.frame, g.start_game_frame + g.frame, g.rtt_ms, g.delay, g.wait_ms_window, g.stalls_window,
                g.wait_ms_max, g.remote_upto, g.sent, g.received, g.hashes_compared, rb,
                g.desync_at >= 0 ? fmt::format("; {} frame(s) out of sync, the first {}", g.desync_frames, g.desync_at)
                                 : "");
    if (g.fighters_at >= 0) {
      REXLOG_INFO("[net] the fighters' state differed in {} of {} frames compared, the first {}", g.fighters_frames,
                  g.fighters_compared, g.fighters_at);
    }
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
// mk9-lauta (2026-11-12): in Test Your Luck (game modes 12 and 13: mk9_capture.cpp GameMode) the slot machine's
// mission draws its modifiers before the fight's barrier, so everything before it has to run at the same frames on
// every machine. The VS screen waits (up to 28 frames) for the two fighters' body images, which a thread pool decodes
// on each machine's own clock: one game left the wait a frame before the other, the same random numbers went to
// other draws, and each screen had its own modifiers (a desync before the fight, 1.0.0). With the pools' work
// finished at each boundary (what a rollback's boundary does), what a frame started is done by the next one on all:
// the players' lockstep frames and a spectator's or a replay's frames out of a fight.
void SettleForLuck(PPCContext& ctx, uint8_t* base) {
  static const std::string rest = REXCVAR_GET(mk9_net_lockstep_rest);
  if (rest == "off") return;
  if (rest == "pools") {
    quiesce::DrainPools(ctx, base);
    return;
  }
  if (rest == "full") {
    quiesce::Settle(ctx, base);
    quiesce::Release();
    return;
  }
  static const bool settle_luck = REXCVAR_GET(mk9_net_settle_luck);
  if (!settle_luck) return;
  const uint32_t info = Load32(base, 0x833848D4);
  const uint32_t mode = info >= 0x10000 && info < 0xF0000000u ? Load32(base, info + 8) : 0;
  if (mode != 12 && mode != 13) return;
  quiesce::Settle(ctx, base);
  quiesce::Release();
}

void LockstepFrame(PPCContext& ctx, uint8_t* base) {
  SettleForLuck(ctx, base);
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
    ReplayPad(g.epoch, target, &mine);  // mk9_net_input_replay (tests)
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
  // the sounds' callbacks only one machine has said (sound_sync.h Decide: their frame is still ahead in lockstep)
  if (g.frames_sound) sound_sync::Decide(g.local_made_upto, g.remote_upto);
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
  // C1: a paced release since the last boundary started the live frame this one runs
  if (const uint64_t release = sa2::renut::LastReleaseUs(); release != g.release_us) {
    g.release_us = release;
    g.release_frame = g.latest;
    g.starts[size_t(g.latest) % g.starts.size()] = {g.latest, release};
  }
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
      ReplayPad(g.epoch, g.latest + g.delay, &pad);  // mk9_net_input_replay (tests)
      // no pause in a fight with rollback: the pause menu's load would turn the rest of the fight to lockstep, and a
      // player could hold the other one there (START is X_INPUT_GAMEPAD's 0x0010, the buttons' low byte)
      if (!REXCVAR_GET(mk9_net_pause_in_fights)) {
        pad.bytes[1] &= uint8_t(~0x10);
        pad.bytes2[1] &= uint8_t(~0x10);  // the second player at this machine's too
      }
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
  // the sounds' callbacks only one machine has said: given at a frame both decide alike (sound_sync.h Decide); one
  // that frame already ran goes back to it
  if (g.frames_sound) {
    const int64_t due = sound_sync::Decide(g.local_made_upto, g.remote_upto);
    if (due >= 0 && due < g.frame) {
      g.rollback_to = g.rollback_to < 0 ? due : std::min(g.rollback_to, due);
      sound_sync::NoteRollback();
    }
  }
  // mk9-lauta (2026-11-10): a frame a rollback runs again with both players' inputs already known cannot be predicted
  // wrong, so no rollback will ever come back to its start: its boundary takes no snapshot (the next snapshot's undo
  // record covers the frames in between, tagged with the last frame kept: a rollback to that one or earlier still
  // finds every page) and so needs no rest either. It runs on as a lockstep frame does, UE3's render thread alongside,
  // only its sync hash taken. The boundary before the shown frame (the hidden frames' drawing done first) and one that
  // undoes a prediction rest as before. A rollback whose frames' inputs had arrived costs about half.
  // mk9-lauta (2026-11-10, mk9_net_light_live): a live frame whose rival input has come already cannot be predicted
  // wrong either, and needs no rest when the frame before it was shown too (after frames run again, the rest lets
  // their drawing finish hidden first). With an input delay that covers the way between the two (a short round trip),
  // most live frames: the boundary's 4-13 ms (UE3's render thread finishing its frame first) go.
  const bool live_light = !g.prev_hidden && REXCVAR_GET(mk9_net_light_live);
  bool light = REXCVAR_GET(mk9_net_light_resim) && g.speculating && g.frame > 0 &&
               (g.frame < g.latest || live_light) && g.frame <= g.remote_upto && g.rollback_to < 0 &&
               g.mode_change_at < 0 && !loads;
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
    if (g.frame < g.latest) {
      ++g.light_boundaries;
    } else {
      ++g.light_live;
    }
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
    for (int64_t f = to; f <= g.latest && size_t(f) < g.local_hash.size(); ++f) {
      g.local_hash[size_t(f)] = 0;
      g.local_fighters[size_t(f)] = 0;
    }
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
    // mk9-lauta (2026-11-10): the match is decided, the same on both machines: its result for the players' records
    fight_state::Report(base, g.player);
    RecordModeEnd(g.mode_change_at);
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
  if (g.frame == g.latest && g.frame % 600 == 0) fight_state::Diagnose(base);  // mk9_diag_fighters
  sa2::renut::SetFrameHidden(g.frame < g.latest);
  g.prev_hidden = g.frame < g.latest;
  heap_undo::SetFrame(g.frame);
  ++g.frame;
  ++g.frames_window;
  if (g.frame > g.latest) g.latest = g.frame;
  // time sync: the peer's live frame lags by about half the round trip; a side ahead by two frames or more on average
  // sleeps one frame, at most once a second. mk9-lauta (2026-11-11, C1/C2): with the two machines' measures of the
  // phase (mk9_net_phase_align, NotePhase: half their difference, the error they share out), it goes by them instead
  // (half the round trip is not the way there when frames run late: a busy PC's "round trip" had its frames' waits in
  // it, and the side ahead by 100 ms and more did not sleep), and a part of a frame ahead moves this side's grid back
  // a quarter of it, 2 ms at most, once a second, once three checks running found it ahead by 2 ms or more (the 60 Hz
  // grids' phase: the side behind does nothing; on a PC that could not keep 60 fps both sides moved theirs back and
  // forth with half of it each second)
  if (g.speculating && g.frame == g.latest && g.peer_latest >= 0) {
    constexpr double kFrameMs = 1000.0 / 60.0;
    const double lag = g.rtt_ms / 2.0 / kFrameMs;
    const double ahead = double(g.latest) - (double(g.peer_latest) + lag);
    g.advantage = g.advantage * 0.95 + ahead * 0.05;
    const bool measured = REXCVAR_GET(mk9_net_phase_align) && g.phase_samples >= 60;
    const double ahead_ms = measured ? (g.phase - g.peer_phase) / 2.0 : g.advantage * kFrameMs;
    if (ahead_ms >= 2.0 * kFrameMs && uint64_t(g.frame) > g.slept_at_frame + 60) {
      std::this_thread::sleep_for(std::chrono::microseconds(16667));
      g.slept_at_frame = uint64_t(g.frame);
      g.advantage -= 1.0;
      if (measured) g.phase -= kFrameMs;
      ++g.syncs_slept;
    } else if (measured && uint64_t(g.frame) >= g.phase_check_at) {
      g.phase_check_at = uint64_t(g.frame) + 60;
      g.phase_ahead_checks = ahead_ms >= 2.0 ? g.phase_ahead_checks + 1 : 0;
      if (g.phase_ahead_checks >= 3) {
        const double step = std::min(ahead_ms / 4.0, 2.0);
        sa2::renut::ShiftGrid(step);
        g.phase -= step;
        g.phase_shifted_ms += step;
        ++g.phase_shifts;
      }
    }
  }
  LogStats();
}

// --- a spectator (mk9-lauta, 2026-11-10) ---

// The recording's bytes that came, parsed into epochs and frames (feed.h; the format of Recorder). They come from the
// host through the relay: epochs and frames in order or nothing (a number out of it would size the vectors).
void WatchParse() {
  auto read = [](uint64_t at, void* out, size_t n) { return feed::Read(at, out, n); };
  auto broken = [](const char* what, uint64_t value) {
    REXLOG_ERROR("[watch] the recording is broken at byte {}: {} {}", g.watch_read, what, value);
    g.failed = true;
  };
  if (!g.watch_header) {
    uint8_t h[20];
    if (!read(0, h, sizeof(h))) return;
    if (std::memcmp(h, "MK9NET1", 8) != 0) {
      REXLOG_ERROR("[watch] what came is not a session recording");
      g.failed = true;
      return;
    }
    // a match of another release (its protocol: 0 before 10) plays back otherwise and hashes otherwise
    uint32_t version;
    std::memcpy(&version, h + 8, 4);
    if ((version >> 8) != Protocol()) {
      REXLOG_ERROR("[watch] the match is played with another release of mk9-lauta (session protocol {}, this one {})",
                   version >> 8, Protocol());
      g.watch_other_release = true;
      g.failed = true;
      return;
    }
    std::memcpy(&g.seed, h + 13, 4);
    g.rollback_wanted = h[17] != 0;
    g.replay_swap = g.replay && h[12] == 1;  // a guest's recording: its own input first
    g.watch_header = true;
    g.watch_read = sizeof(h);
  }
  for (;;) {
    uint8_t tag = 0;
    if (!read(g.watch_read, &tag, 1)) return;
    if (tag == 'E') {
      uint8_t r[14];
      if (!read(g.watch_read, r, sizeof(r))) return;
      uint32_t epoch;
      std::memcpy(&epoch, r + 1, 4);
      if (epoch != g.watched.size()) return broken("epoch", epoch);
      g.watched.emplace_back();
      std::memcpy(&g.watched[epoch].start, r + 5, 8);
      g.watched[epoch].rollback = (r[13] & 1) != 0;
      g.watched[epoch].frames_sound = (r[13] & 2) != 0;
      g.watched[epoch].players[0] = (r[13] & 4) ? 2 : 1;
      g.watched[epoch].players[1] = (r[13] & 8) ? 2 : 1;
      g.watch_read += sizeof(r);
    } else if (tag == 'F' || tag == 'G') {
      const int size = tag == 'G' ? kPadBytes2 : kPadBytes;
      uint8_t r[1 + 4 + 2 * kPadBytes2 + 8];
      const size_t length = size_t(1 + 4 + 2 * size + 8);
      if (!read(g.watch_read, r, length)) return;
      uint32_t frame;
      std::memcpy(&frame, r + 1, 4);
      if (g.watched.empty() || frame != g.watched.back().frames.size()) return broken("frame", frame);
      {
        auto& frames = g.watched.back().frames;
        frames.emplace_back();
        Pad* pads[2] = {&frames[frame].p1, &frames[frame].p2};
        if (g.replay_swap) std::swap(pads[0], pads[1]);
        for (int p = 0; p < 2; ++p) {
          const uint8_t* at = r + 5 + p * size;
          std::memcpy(pads[p]->bytes, at, 12);
          pads[p]->busy = at[12];
          pads[p]->ready = at[13];
          std::memcpy(&pads[p]->sound, at + 14, 4);
          if (size == kPadBytes2) std::memcpy(pads[p]->bytes2, at + 18, 12);
        }
        std::memcpy(&frames[frame].hash, r + 5 + 2 * size, 8);
      }
      g.watch_read += length;
    } else if (tag == 'M') {
      uint8_t r[5];
      if (!read(g.watch_read, r, sizeof(r))) return;
      uint32_t frame;
      std::memcpy(&frame, r + 1, 4);
      if (!g.watched.empty()) g.watched.back().mode_end = frame;
      g.watch_read += sizeof(r);
    } else {
      REXLOG_ERROR("[watch] the recording has a record it does not know ({}) at byte {}", int(tag), g.watch_read);
      g.failed = true;
      return;
    }
  }
}

// Frames of the match there are beyond the one this game plays next.
int64_t WatchBehind() {
  int64_t behind = 0;
  for (size_t e = g.epoch; e < g.watched.size(); ++e) behind += int64_t(g.watched[e].frames.size());
  return behind - g.frame;
}

// In the recorded epoch's fight with rollback, up to the frame after its mode ended (the players' switch to lockstep
// runs that one with the fight's busy 0 and ready 0xFF still: RollbackFrame): what the players' fights polled.
bool WatchFight() {
  if (!g.watching || g.epoch >= g.watched.size()) return false;
  const auto& e = g.watched[g.epoch];
  return e.rollback && (e.mode_end < 0 || g.frame <= e.mode_end + 1);
}

// Waits on the match (the relay brings it) until done(); false: it ended there, or 40 s without a byte.
template <typename Done>
bool WatchWait(Done done) {
  uint64_t size = feed::Size(), changed = NowUs();
  for (;;) {
    WatchParse();
    if (g.failed) return false;
    if (done()) return true;
    if (feed::Ended()) return false;
    if (feed::Size() != size) {
      size = feed::Size();
      changed = NowUs();
    } else if (NowUs() - changed > 40000000) {
      REXLOG_ERROR("[watch] nothing came of the match for 40 s");
      return false;
    }
    g_frame_started_us = 0;  // the watchdog: waiting on the relay is no hang
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
  }
}

// A spectator's frame that ran (the one before g.frame): its sync hash against the players'.
void WatchCompareLast() {
  FinishFrameHash();
  if (g.frame <= 0 || g.epoch >= g.watched.size()) return;
  const auto& frames = g.watched[g.epoch].frames;
  if (size_t(g.frame - 1) >= frames.size()) return;
  const uint64_t mine = g.local_hash[size_t(g.frame - 1)], theirs = frames[size_t(g.frame - 1)].hash;
  if (mine && theirs) {
    ++g.watch_compared;
    if (mine != theirs) {
      ++g.watch_diffs;
      if (g.watch_diffs <= 10 || g.watch_diffs % 100 == 0) {
        REXLOG_WARN("[watch] frame {} of epoch {} differs from the match (sync hash {:016x}, there {:016x}; {} of {} "
                    "frames so far)",
                    g.frame - 1, g.epoch, mine, theirs, g.watch_diffs, g.watch_compared);
      }
    }
  }
}

// A spectator's frame: the recording's two inputs of this frame, waited for. More than 5 s behind the match it catches
// up (the frames hidden, unpaced and muted) until 1.5 s from it. Where the recording says a scene is ready, it first
// waits for its own loads to end (the players' machines were ready then; this one may be slower).
void SpectatorFrame(PPCContext& ctx, uint8_t* base, uint64_t game_frame) {
  if (!g.started) {
    g_frame_started_us = 0;
    if (!WatchWait([] { return g.watch_header && !g.watched.empty(); })) {
      EndSession(g.watch_other_release ? EndReason::kOtherVersion : EndReason::kFailed, false);
      return;
    }
    g.started = true;
    StartEpoch(game_frame, 0, false);
    g.local_players = g.watched[0].players[0];  // the spectator in the host's seat: the host's players and the guest's
    g.peer_players = g.watched[0].players[1];
    g.seed_pending = true;
    REXLOG_INFO("[watch] the match comes: seed {}, playing it back from game frame {}", g.seed, game_frame);
  } else if (g.at_barrier) {
    g.at_barrier = false;
    WatchCompareLast();
    const uint32_t next = g.epoch + 1;
    // (the epoch's game frame there is not this one's: the players' count the frames their rollbacks ran again)
    if (!WatchWait([&] { return g.watched.size() > next; })) {
      EndSession(EndReason::kPeerLeft, false);
      return;
    }
    StartEpoch(game_frame, next, false);
    g.local_players = g.watched[next].players[0];
    g.peer_players = g.watched[next].players[1];
    REXLOG_INFO("[watch] epoch {} at game frame {}", next, game_frame);
  }
  // the frame's inputs; the match's next epoch already there and this one's frames over: this game missed the barrier
  // the players reached (its playback went another way)
  bool missed = false;
  if (!WatchWait([&] {
        if (g.epoch < g.watched.size() && g.watched[g.epoch].frames.size() > size_t(g.frame)) return true;
        missed = g.watched.size() > g.epoch + 1 && g.watched[g.epoch].frames.size() <= size_t(g.frame);
        return missed;
      }) ||
      missed) {
    if (missed) {
      REXLOG_ERROR("[watch] the match went on to epoch {} at frame {} of epoch {}, and this playback did not: it went "
                   "another way",
                   g.epoch + 1, g.watched[g.epoch].frames.size(), g.epoch);
    }
    EndSession(missed ? EndReason::kDesync : EndReason::kPeerLeft, false);
    return;
  }
  g_frame_started_us = NowUs();
  const auto& epoch = g.watched[g.epoch];
  const bool fight = WatchFight();
  if (!fight) SettleForLuck(ctx, base);  // as the players' lockstep frames (Test Your Luck)
  g.watch_in_fight.store(fight, std::memory_order_relaxed);
  g.watch_fight.store(fight && g.frame >= kLearnFrames, std::memory_order_relaxed);
  // the sounds' callbacks as the players had them: in lockstep at the frames their announcements agreed on (with this
  // machine's own FMOD's callbacks, waited for); in a fight's epoch whose callbacks went by frame numbers, at the
  // frames the players' recorded words decide (sound_sync.h kFrames: one this machine's FMOD has not given is given
  // from its sound's hand-off), its whole length as the players had it; in an older fight, as they come
  const bool frames_sound = epoch.rollback && epoch.frames_sound;
  sound_sync::SetMode(ctx, base,
                      frames_sound ? sound_sync::Mode::kFrames
                      : fight      ? sound_sync::Mode::kOff
                                   : sound_sync::Mode::kLockstep,
                      g.epoch);
  // catching up: hidden (no drawing, no pacing) and muted; the switch with UE3's render thread drained first. Not while
  // a fight loads (GS_LOADING_FIGHT): what the engine streams in (textures, sounds) takes the players' frames of
  // wall-clock time, and fast frames reached the fight's start later than the players' (a missed barrier). In a fight
  // too (mk9_watch_catch_up_fights) since 2026-11-11: it parted from the match ~430 frames in because a stage's script
  // asked a sound's frames left from FMOD's position, on the mixer's clock (behind fast frames); the game thread's
  // FMOD positions are made of frames now (virtual_time.h, SoundStarted). Found with mk9_watch_catch_up_mode=paced:
  // hidden at the players' speed it did not part, so not the drawing and not the muting
  const int64_t behind = WatchBehind();
  g.watch_behind.store(behind, std::memory_order_relaxed);
  const bool loading = g.game_state.load(std::memory_order_relaxed) == fight_state::kStateLoadingFight;
  const bool hide = g.replay ? !fight && !loading  // a replay: the menus fast, the fights at their pace
                             : (!fight || REXCVAR_GET(mk9_watch_catch_up_fights)) && !loading &&
                                   REXCVAR_GET(mk9_watch_catch_up) && (g.watch_hidden ? behind > 90 : behind > 300);
  // (tests, mk9_watch_catch_up_mode: "drawn", the frames drawn and shown but not paced, run with renut_vsync off;
  // "paced", hidden at the players' speed: which of the two a fight's catching up goes apart by)
  static const std::string mode = REXCVAR_GET(mk9_watch_catch_up_mode);
  if (hide != g.watch_hidden) {
    snapshot::FlushRendering(ctx, base);
    if (mode == "drawn") {
      sa2::renut::SetUnpaced(hide);
    } else {
      sa2::renut::SetFrameHidden(hide);
      if (mode == "paced") sa2::renut::SetPaceHidden(hide);
    }
    rex::cvar::SetFlagByName("audio_mute", hide ? "true" : "false");
    g.watch_hidden = hide;
    g.watch_peeking = false;
    g.watch_peek_us = NowUs();
    REXLOG_INFO("[watch] {} at frame {} of epoch {} ({} frames behind the match)",
                hide        ? "catching up"
                : loading   ? "at the match's pace while a fight loads"
                : fight && !REXCVAR_GET(mk9_watch_catch_up_fights) ? "at the match's pace in a fight"
                : behind <= 90 ? "caught up"
                               : "at the match's pace",
                g.frame, g.epoch, behind);
  } else if (hide && mode.empty()) {
    // mk9-lauta (2026-11-11): catching up, a frame drawn and shown every half second, muted: the screen and the
    // badge's progress move. A spectator far behind a long session saw one still picture for 30-45 s at a time (its
    // frames run hidden until a fight loads), and took it for a hang. Drawing changes nothing the game decides (the
    // catching up was found the same drawn or hidden: mk9_watch_catch_up_mode).
    const uint64_t now = NowUs();
    static const int64_t every_us = int64_t(REXCVAR_GET(mk9_watch_peek_ms)) * 1000;
    const bool peek = every_us > 0 && !g.watch_peeking && now - g.watch_peek_us >= uint64_t(every_us);
    if (peek != g.watch_peeking) {
      snapshot::FlushRendering(ctx, base);
      sa2::renut::SetFrameHidden(!peek);
      g.watch_peeking = peek;
      if (peek) g.watch_peek_us = now;
    }
  }
  Ensure(g.frame + 2);
  WatchCompareLast();
  const auto& f = epoch.frames[size_t(g.frame)];
  // a scene the players' machines found ready: this one's loads first (its disk may be slower)
  const uint8_t ready = f.p1.ready & f.p2.ready;
  if (!fight && (ready & ~g.watch_ready_before) != 0) {
    const uint64_t start = NowUs();
    while (Load32(base, kAsyncPackagesNum) != 0 && NowUs() - start < 20000000) {
      g_frame_started_us = 0;
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    g_frame_started_us = NowUs();
  }
  g.watch_ready_before = fight ? 0 : ready;
  SetFramePads(f.p1, f.p2);
  if (frames_sound) {
    // the players' words up to this frame (a callback's frame is never earlier than its later word's: the recording
    // up to here decides every one due here)
    if (g.frame == 0) g.watch_words_upto = -1;
    for (; g.watch_words_upto < g.frame; ++g.watch_words_upto) {
      const auto& w = epoch.frames[size_t(g.watch_words_upto + 1)];
      sound_sync::WatchWords(g.watch_words_upto + 1, w.p1.sound, w.p2.sound);
    }
    sound_sync::Decide(g.watch_words_upto, g.watch_words_upto);
    sound_sync::FrameStart(ctx, base, g.frame, g.frame - 1);
  }
  if (fight) {
    g.frame_busy = 0;
    g.frame_ready = 0xFF;
  } else if (frames_sound) {
    g.frame_busy = f.p1.busy | f.p2.busy;
    g.frame_ready = ready;
  } else {
    g.frame_busy = f.p1.busy | f.p2.busy;
    g.frame_ready = ready;
    // the callbacks the two announced by this frame (one this machine's FMOD has not given goes from its source)
    sound_sync::WatchDeliver(ctx, base, f.p1.sound, f.p2.sound);
  }
  ++g.frames_window;
  ++g.frame;
  g.latest = g.frame;
}

}  // namespace

bool SessionGame() {
  if (!g.configured) Configure();
  return g.watching || g.link != nullptr;
}

// mk9-lauta (2026-11-11): the game's state sessions or not (OnFrame; GameState: the main menu's corner, Discord).
std::atomic<int> g_state_any{-1};

int GameState() { return g_state_any.load(std::memory_order_relaxed); }

int LocalPlayer() {
  if (!g.configured) Configure();
  return g.link != nullptr && !g.watching ? g.player : -1;
}

bool Active() {
  if (!g.configured) Configure();
  // a host waits for its guest; a room's guest waits for the path to the host too; a spectator has no path
  return (g.watching || (g.link && (g.link->HasPeer() || g.host || !g.room.empty()))) && !g.failed && !g.ended;
}

void OnFrame(PPCContext& ctx, uint8_t* base, uint64_t game_frame) {
  if ((game_frame & 3) == 0 || g_state_any.load(std::memory_order_relaxed) < 0) {
    g_state_any.store(fight_state::State(base), std::memory_order_relaxed);
  }
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
  if (const int state = fight_state::State(base); state != g.game_state.load(std::memory_order_relaxed)) {
    REXLOG_INFO("[net] the game's state {} at frame {} of epoch {}", state, g.frame, g.epoch);
    g.game_state.store(state, std::memory_order_relaxed);
  }
  WatchFrames();
  g_frame_started_us = NowUs();
  if (g.watching) {
    SpectatorFrame(ctx, base, game_frame);
    return;
  }
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
    // mk9-lauta (2026-11-10): the epoch's last frame, the barrier's: its hash and its record (each frame's goes out at
    // the start of the one after it, and the one after this is the next epoch's). A spectator plays that frame too,
    // and without its record it never reached the barrier the players had
    FinishFrameHash();
    RecordFinal();
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
    // a spectator follows the players' word alone: its own movie may play on (it started later, or plays slower)
    if (!g.watching) return local;
  }
  return (g.frame_busy & flag) ? (g.busy_value[bit] ? g.busy_value[bit] : 1u) : 0u;
}

uint32_t Ready(ReadyFlag flag, uint32_t local) {
  if (!g.started || g.ended || g.failed || std::this_thread::get_id() != g.game_thread) return local;
  if (g.rollback || WatchFight()) {
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
  const uint32_t answer = (g.frame_ready & flag) ? (g.ready_value[bit] ? g.ready_value[bit] : 1u) : 0u;
  if (static int logged = 0; (g.watching || REXCVAR_GET(mk9_net_log_ready)) && answer && logged < 40) {
    ++logged;
    REXLOG_INFO("[net] readiness poll {} at frame {} of epoch {}: here {:#x}, answered {:#x}", int(flag), g.frame - 1,
                g.epoch, local, answer);
  }
  return answer;
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

bool AboutToLoad(const char* package) {
  if (!g.started || g.ended || g.failed || !g.rollback || !g.speculating ||
      std::this_thread::get_id() != g.game_thread) {
    return true;
  }
  if (!ConfirmFrame("a package loads")) return false;
  // the rest of the epoch in lockstep, from the frame after this one (RollbackFrame, as a mode change)
  const int64_t frame = g.frame - 1;
  if (g.mode_change_at < 0 || g.mode_change_at > frame) {
    g.mode_change_at = frame;
    REXLOG_INFO("[net] a package loads at frame {} ({}): the fight is over, lockstep from here", frame, package);
  }
  return true;
}

void Barrier(uint8_t* base, const char* what) {
  if (!g.started || g.ended || g.failed || g.at_barrier || g.rollback || WatchFight()) return;
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
  if (info.active && !g.watching && g.last_heard_us) {
    const uint64_t now = NowUs();
    info.silent_s = now > g.last_heard_us ? double(now - g.last_heard_us) / 1e6 : 0.0;
    info.timeout_s = double(TimeoutUs()) / 1e6;
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
  info.local_players = g.local_players;
  info.peer_players = g.peer_players;
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
  info.watching = g.watching;
  info.in_fight = g.watching ? g.watch_in_fight.load(std::memory_order_relaxed) : g.rollback;
  info.fight_on = g.watching ? g.watch_fight.load(std::memory_order_relaxed) : g.rollback && g.speculating;
  info.game_state = g.game_state.load(std::memory_order_relaxed);
  if (g.watching && g.started) {
    info.behind_s = double(std::max<int64_t>(0, g.watch_behind.load(std::memory_order_relaxed))) / 60.0;
    info.catching_up = g.watch_hidden;
    info.watch_diffs = g.watch_diffs;
  }
  info.replay = g.replay;
  {
    std::lock_guard lock(g_desync_report_mutex);
    info.desync_report = g_desync_report;
  }
  return info;
}

void Leave() { g_end_request.store(1, std::memory_order_release); }

void PeerLeft() {
  int none = 0;
  g_end_request.compare_exchange_strong(none, 2, std::memory_order_acq_rel);  // this player's leaving goes first
}

void OverrideBlock(uint8_t* base, uint32_t tag, uint32_t data, uint32_t size, uint32_t user, uint32_t r1) {
  if (!g.started || g.ended || g.failed || g.frame <= 0) return;
  // users 0 and 1: the host's and the guest's pads; 2 and 3 (mk9-lauta 2026-11-13): their second players', when that
  // machine has two (mk9_net_local_players)
  const Pad* pad = user == 0 ? &g.frame_pads[0] : user == 1 ? &g.frame_pads[1] : nullptr;
  const bool second = user == 2 || user == 3;
  if (second && PlayersOf(int(user) - 2) == 2) pad = &g.frame_pads[user - 2];
  const uint8_t* gamepad = pad ? (second ? pad->bytes2 : pad->bytes) : nullptr;
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
      put_be32(out, g.packet[second ? user - 2 : user][size_t(g.frame - 1)]);
      std::memcpy(out + 4, gamepad, 12);
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
