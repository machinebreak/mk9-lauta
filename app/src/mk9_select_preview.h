// mk9-lauta (2026-11-23): the character select's fighters in 3D. A player asked to see the alternate costumes in the
// character select, where MK9 shows a 2D art per side: with the option on (F2, mk9_select_preview), each side shows the
// fighter it has highlighted in 3D instead, in the costume being browsed in the costume menu (START), lit, in its
// fighting stance; when it is picked it taunts its rival, turned toward it: one of its Story lines (in the game's
// language), its intro as before a fight or its taunt, at random. Offline only: an online session's select keeps the
// art. Off by default. See mk9_select_preview.cpp.

#pragma once

#include <cstdint>

struct PPCContext;

namespace mk9::select_preview {

// At each frame's start (the game thread, the gamepad read's hook): the previews' loads, actors and animations.
void OnFrame(PPCContext& ctx, uint8_t* base);

}  // namespace mk9::select_preview
