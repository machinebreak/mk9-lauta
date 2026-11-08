// sa2-nx (2026-10-20): the game's frame pacing with Renut's renderer (sa2_renut_pacing.cpp).
#pragma once

#include <cstdint>

namespace sa2::renut {

// Called by the Swap hook before the frame goes to the render thread: waits until the game's presentation interval
// since the previous Swap has passed (the console's vertical blank).
void PaceSwap(uint32_t device_va);

// mk9-nx online (2026-11-07): the frames a rollback runs again (app/src/online/netplay.cpp) are hidden: their Swap is
// neither paced nor presented, so the game catches up within a display frame. Set by the game thread at a frame's
// start, with the guest's render thread idle (the rollback drains it there), so the frame's Swap reads its own value.
void SetFrameHidden(bool hidden);
bool FrameHidden();

}  // namespace sa2::renut
