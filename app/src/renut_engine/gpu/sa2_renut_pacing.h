// sa2-nx (2026-10-20): the game's frame pacing with Renut's renderer (sa2_renut_pacing.cpp).
#pragma once

#include <cstdint>
#include <utility>
#include <vector>

namespace sa2::renut {

// Called by the Swap hook before the frame goes to the render thread: waits until the game's presentation interval
// since the previous Swap has passed (the console's vertical blank).
void PaceSwap(uint32_t device_va);

// mk9-nx online (2026-11-07): the frames a rollback runs again (app/src/online/netplay.cpp) are hidden: their Swap is
// neither paced nor presented, so the game catches up within a display frame. Set by the game thread at a frame's
// start, with the guest's render thread idle (the rollback drains it there), so the frame's Swap reads its own value.
void SetFrameHidden(bool hidden);
bool FrameHidden();
// mk9-lauta (2026-11-11, tests): frames drawn and presented but not paced (a spectator catching up while drawing,
// mk9_watch_catch_up_draw); the grid starts again once it is off.
void SetUnpaced(bool unpaced);
// mk9-lauta (2026-11-11, tests): hidden frames paced as shown ones (a spectator catching up hidden at the players'
// speed, mk9_watch_catch_up_mode=paced).
void SetPaceHidden(bool paced);
// mk9-lauta (2026-11-11, PLAN-0.4 C1): the online's phase alignment. ShiftGrid moves the 60 Hz grid later by ms (the
// machine whose frames run ahead of the other's in time moves its own back), at the next paced Swap, 4 ms at most a
// call; LastReleaseUs is when the last paced Swap was let go (steady_clock, microseconds; 0: none yet): the start of
// the frame the game runs live now, on the grid.
void ShiftGrid(double ms);
uint64_t LastReleaseUs();

// mk9-lauta (2026-11-09): the performance mode (mk9_draw_mode): frames that are not drawn but still take their 1/60 s,
// so the game keeps its speed while the GPU draws every other frame. Read by the thread that makes the Direct3D calls
// (the draws and the Swap), which also decides it at each Swap (AfterSwap).
bool FrameSkipped();
// The Swap hook, after the frame went to the render thread: whether the next frame is drawn (rollback_hidden: this
// frame was one a rollback ran again, which the alternation does not count).
void AfterSwap(bool rollback_hidden);
// Whether the last Swap presented an image (not a frame run again, not a frame left undrawn).
bool LastSwapShown();
// mk9-lauta (2026-11-10): a presented frame's GPU time, from its timestamps (renut_engine/perf/frame_stats.cpp, the
// render thread): what the dynamic resolution goes by.
void NoteGpuFrame(double ms);

// mk9-nx online (2026-11-08): the guest words of the title D3D device that count the GPU's progress (the current fence
// tag, the fence completion word, the Swap's counters and tags, the kick flag, the ring stub's pointers: [start, end)
// byte ranges), which Renut's host state follows. A rollback leaves them as they are: put back, the device would wait
// for a fence Renut has long completed, or for one it will never queue (hooks/device.cpp). Empty before the device
// exists.
std::vector<std::pair<uint32_t, uint32_t>> GpuProgressRanges();

}  // namespace sa2::renut
