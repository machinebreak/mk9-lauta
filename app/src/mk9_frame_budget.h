// mk9-lauta (2026-11-10): where the game thread's frame goes (mk9_frame_budget.cpp, "[frame]" every 10 s).

#pragma once

namespace mk9::frame_budget {

// The frame boundary's time (the pad read's hook, before the game's own read: an online session's work, the loads
// finished at the frame's start), game thread.
void NoteBoundary(double ms);

}  // namespace mk9::frame_budget
