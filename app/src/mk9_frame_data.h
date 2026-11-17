// mk9-lauta (2026-11-17): frame data in Practice (a tester's ask; MK9's Practice shows none): each attack's startup
// (its frames up to the first that touches, that one included), its advantage on hit or on block (how many frames
// before the other fighter the attacker can act: + or -) and its total frames, measured each frame from the two
// fighters' state (fight_state::ReadFighter). The measure was checked against the community's MK9 tables (Scorpion's
// 1, 2, 3 and d3, on hit and on block: startup, total and advantage, all equal). Shown in a small panel at the right
// (mk9_frame_data, F2 > Controles); nothing of it reaches the game.

#pragma once

#include <cstdint>

namespace mk9::frame_data {

// At each frame's boundary (game thread): the fighters' state read and the attacks measured, in a Practice fight
// only (never in an online session).
void OnFrame(uint8_t* base);

// Once (any thread): the panel goes into the overlay.
void Install();

}  // namespace mk9::frame_data
