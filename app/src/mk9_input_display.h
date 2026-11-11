// mk9-lauta (2026-11-11, PLAN-0.4 H2): the inputs on screen in a fight (mk9_input_display, F2 > Controles): a player's
// directions and buttons as the game got them, the newest on top, with how many frames each one was held: to practise
// combos (MK9's Practice has no such display). Read from the gamepad blocks the game reads each frame (the capture
// interface, online/mk9_capture.cpp: each user's XINPUT_STATE, once final), drawn by the overlay. Nothing
// of it reaches the game.

#pragma once

#include <cstdint>

namespace mk9::input_display {

// Once (any thread): the display goes into the overlay.
void Install();

// The capture interface's block of a frame, once final (game thread): tag 0x00FF0002 is a user's XINPUT_STATE.
// Player 1's (user 0) offline, this machine's player's in an online session; a fight's live frames only.
void NoteBlock(uint8_t* base, uint32_t tag, uint32_t data, uint32_t size, uint32_t user);

}  // namespace mk9::input_display
