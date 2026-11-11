// mk9-lauta (2026-11-11, PLAN-0.4 B2): the frames a rollback runs again, without their scene drawn (experiment, off
// unless mk9_hidden_frames_draw = false).
//
// A hidden frame (sa2_renut_pacing.h FrameHidden: a rollback's frame run again, a spectator or a replay catching up)
// still drew its scene: UE3's render thread walked it whole and Renut threw the draws away at the end (online the
// render thread worked 5-6.5 ms a frame against 2.5 offline, baseline-0.3.0.md). FViewport::Draw is sub_828BBEC0: it
// enqueues BeginDrawingCommand (vtable 0x82133B90, its DescribeCommand sub_828A9170 says "BeginDrawingCommand"), calls
// the viewport client's Draw (vtable +12: the scene and the HUD), and enqueues EndDrawingCommand (vtable 0x82133BCC
// here, or 0x82133B9C in sub_828BB750, which it calls: the Present, the Swap hook that counts g_mk9_game_frames).
// UGameEngine's redraws call it (sub_82869CA0, sub_82869E20; sub_82875358 jumps to it). Here a hidden frame does not
// call it: no scene, no HUD, no Present (the frame counters of the Swap do not count it; they skipped hidden frames
// already: CountForDisplay, AfterSwap).
//
// What must stay is what the game's logic does: the primitives' updates are enqueued by their components' ticks, not by
// the draw. What could depend on the draw (a HUD that draws a random number, a skeleton updated only when rendered
// recently) shows in the SyncTest, which compares a frame's first run (drawn) with its runs again (hidden): the
// game's checks, the sync hash and the fighters' whole blocks (fight_state.h SyncTestFighters).
//
// Measured (2026-11-11, SyncTest 4700,12,6, 325 rollbacks of 6 frames each way, [frame]'s hidden share, B1): a frame
// run again took 1.85 ms of game work and 1.39 ms of boundary after it (p99 3.56: the render thread's walk of its
// scene, flushed), 3.24 ms; undrawn 1.76 + 0.44 (p99 0.80), 2.20 ms. The SyncTest: 0 frames differed either way; of
// the fighters' blocks only the actor's +0xac (the time it was last drawn: the first run drew, the run again did not)
// and render feedback the draw already left apart (+0xc8, +0x1b4); drawn, the runs again also had actor words at
// +0x2c0-0x2dc and +0x580-0x59c otherwise (the render thread writing them at its own time), undrawn none. Only for a
// rollback's frames: see Rerun.

#include <atomic>
#include <cstdint>

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>

#include "online/netplay.h"
#include "renut_engine/gpu/sa2_renut_pacing.h"

REXCVAR_DEFINE_BOOL(mk9_hidden_frames_draw, true, "MK9 Online",
                    "A rollback's frames run again (a player's, the SyncTest's) still draw their scene (UE3's render "
                    "thread walks it; Renut throws it away). false: FViewport::Draw is skipped in them (PLAN-0.4 B2: "
                    "3.2 -> 2.2 ms a frame run again, the SyncTest the same); a spectator's frames always draw");

namespace {

std::atomic<uint64_t> g_skipped{0};

// A rollback's frames run again: a player's (netplay) or the SyncTest's. A spectator or a replay catching up runs
// hidden for many seconds at a time, and what the game does with "last drawn long ago" (a skeleton updated only while
// rendered...) is not what the players' machines did: it keeps drawing.
bool Rerun() {
  if (!sa2::renut::FrameHidden()) return false;
  return !mk9::netplay::SessionGame() || mk9::netplay::LocalPlayer() >= 0;
}

}  // namespace

extern "C" REX_FUNC(__imp__sub_828BBEC0);
REX_HOOK_RAW(sub_828BBEC0) {
  if (!REXCVAR_GET(mk9_hidden_frames_draw) && Rerun()) {
    const uint64_t n = g_skipped.fetch_add(1, std::memory_order_relaxed) + 1;
    if (n == 1 || n % 3600 == 0) REXLOG_INFO("[draw] {} hidden frames left undrawn (mk9_hidden_frames_draw)", n);
    return;
  }
  __imp__sub_828BBEC0(ctx, base);
}
