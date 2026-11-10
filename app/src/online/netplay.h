// MK9 online: the two-player session at the game's frame boundary (docs/online/netplay-design.md, path A).
//
// The game reads its gamepads once per frame in sub_829D5230; at its entry (mk9_capture.cpp) OnFrame exchanges this
// frame's inputs with the peer and, in lockstep, waits for theirs. The capture interface's blocks of users 0 and 1
// (sub_82A1C550, also hooked there) then get the session's inputs: player 1 is the host's pad, player 2 the guest's,
// on both machines alike; users 2 and 3 read as disconnected.
//
// Some things the game polls end on each machine's own clock (a full-screen movie playing, a scene getting ready);
// they travel with the inputs as flags, and the game sees them end at the same frame on both machines (Busy, Ready).

#pragma once

#include <cstdint>
#include <string>

struct PPCContext;

namespace mk9::netplay {

// Things the game polls that end on each machine's own clock: one bit each in the inputs.
enum BusyFlag : uint8_t {
  kBusyMovie = 1 << 0,  // a full-screen movie is playing (sub_829CE600)
};

// Readiness polls (0 = not yet) that end on each machine's own clock: one bit each in the inputs.
enum ReadyFlag : uint8_t {
  kReadyScene = 1 << 0,      // the scripts' last wait before a fight starts (native 824a5268, sub_82A30588)
  kReadyStreaming = 1 << 1,  // the engine's streaming done, waited on just before (native 8247bf38, sub_82A2EAB8)
  kReadyModeGate = 1 << 2,   // the Versus selection's gate to the fight mode (native 824a56d0, sub_82A311D8)
  kReadyTask = 1 << 3,       // mk9-lauta (2026-11-10): the engine's task at 0x8341EF1C done (native 8247b658)
};

// A session is configured (mk9_net_host / mk9_net_join).
bool Active();
// mk9-lauta (2026-11-10): this game is a session's (a player's or a spectator's), from its first frame to its last,
// whether the session has started, runs or ended.
bool SessionGame();
// Test logs: the epoch and the epoch's frame running, and whether it runs again (a rollback's hidden frames); -1 when no
// session runs.
int64_t FrameRunning(uint32_t* epoch = nullptr, bool* again = nullptr);
// The frame boundary (game thread; ctx: the hook's, for the guest calls a rollback makes). game_frame: the game's
// frame number (1-based, mk9_capture.cpp).
void OnFrame(PPCContext& ctx, uint8_t* base, uint64_t game_frame);
// What the game gets from a busy poll this frame (game thread): local, what the poll gave on this machine. While the
// session runs, it stays busy until neither machine was busy at the frame whose inputs this frame plays (and this
// machine is not now): the same on both.
uint32_t Busy(BusyFlag flag, uint32_t local);
// What the game gets from a readiness poll this frame (game thread): local, what the poll gave on this machine. While
// the session runs, it reads ready only once both machines' polls said ready in the frame before this frame's inputs
// left (s - delay - 1), and not ready otherwise: the same on both.
uint32_t Ready(ReadyFlag flag, uint32_t local);
// The game is about to do something a rollback cannot undo, outside the simulation's memory (game thread, during a
// frame; what: for the log), such as the music changing tracks. In a fight with rollback the frame first waits for the
// other player's inputs up to it. True: every frame so far ran on the right inputs, so no rollback will run this one
// again (go ahead); false: one of them ran on a wrong prediction and this frame will be run again (leave the action to
// the corrected run). Anywhere else: true.
bool ConfirmFrame(const char* what);
// The game asks for a package to load (UObject::LoadPackageAsync; game thread, during a frame). In a fight with
// rollback that only happens once the fight is decided (FINISH HIM's FatalityArt, the victory, the menus after it),
// and what follows (loads, saves, the sound's teardown, the music) cannot be undone: once the frame is confirmed the
// rest of the epoch goes on in lockstep, as after the fight's mode ends. True: go ahead; false: the frame ran on a
// wrong prediction and will be run again, so the load is left to the corrected run (nothing in the frame waits for an
// asynchronous load).
bool AboutToLoad();
// A barrier both machines reach (game thread, during a frame; what: for the log): the generators are reseeded on the
// spot with the next epoch's seed, and the next frame boundary waits for the other machine (netplay.cpp).
void Barrier(uint8_t* base, const char* what);
// A check of the game's own (sub_82A1C5C0: tag, value; game thread): this frame's go into the sync hash both players
// compare.
void OnCheck(uint32_t tag, uint32_t value);
// Once, at the session's first frame: the seed both players start the game's random number generators from (seed.h).
bool TakeSessionSeed(uint32_t* seed);
// Why a session ended.
enum class EndReason : uint8_t {
  kNone,
  kLeft,      // this player left (Leave)
  kPeerLeft,  // the other player left (its goodbye, here or through the room)
  kPeerLost,  // no word from the other player for mk9_net_timeout_s
  kDesync,    // the two games' sync hashes differed: each game went its own way
  kFailed,    // the session could not start (no path to the other player, a different build)
  kOtherVersion,  // the other player's game is another release of mk9-lauta (SessionInfo::peer_release)
};
// The session as the online screens show it.
struct SessionInfo {
  bool active = false;
  bool ended = false;       // it ran, or tried to, and is over (end_reason)
  EndReason end_reason = EndReason::kNone;
  int player = 0;           // 0: player 1 (the host), 1: player 2
  double rtt_ms = 0;
  int delay = 0;            // this epoch's input delay, frames
  bool rollback = false;    // a fight with rollback
  bool speculating = false; // ... past its intro
  uint64_t rollbacks = 0, resimulated = 0;
  bool desynced = false;
  bool waiting = false;     // the game is waiting for the other player's inputs (for longer than a moment)
  double waiting_s = 0;     // ... this long so far
  std::string peer_release; // the other player's release ("0.2.1"; empty: not said, 0.2.0 and older say nothing)
  // mk9-lauta (2026-11-10): the connection's indicator: how much the round trip moves (the last pongs' standard
  // deviation), the share of pings that got no answer (the last ~12 s), and a path through a relay (TURN)
  double jitter_ms = 0;
  double loss_pct = 0;
  bool relayed = false;
  // mk9-lauta (2026-11-10): a spectator's game (feed.h): how far behind the match it plays, whether it is catching up
  // (frames run hidden and fast), and the frames whose sync hash differed from the players'
  bool watching = false;
  double behind_s = 0;
  // in a fight's epoch (the players': with rollback; a spectator's: the one it plays back), past its intro, and the
  // game's state (fight_state.h State) at the last frame
  bool in_fight = false;
  bool fight_on = false;
  int game_state = -1;
  bool catching_up = false;
  uint64_t watch_diffs = 0;
};
SessionInfo Info();
// This player leaves the session (any thread): at its next frame boundary, or while it waits for the other player,
// the game thread says goodbye to the peer and the session ends (EndReason::kLeft).
void Leave();
// The other player said goodbye through the lobby's room (room.h; any thread): the session ends the same way as with
// its goodbye over the session's own path (EndReason::kPeerLeft).
void PeerLeft();
// A capture block (sub_82A1C550's arguments; r1: the caller's stack pointer). Replaces the players' raw gamepad
// blocks while the session runs.
void OverrideBlock(uint8_t* base, uint32_t tag, uint32_t data, uint32_t size, uint32_t user, uint32_t r1);

}  // namespace mk9::netplay
