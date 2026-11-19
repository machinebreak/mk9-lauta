// mk9-lauta (2026-11-18): the opponent recorded in Practice (Irrit8's ask, with a donation). MK9's own Practice
// GRABACIÓN/REPRODUCCIÓN records the player's own fighter; this records the dummy and plays it back on it, as the
// later fighting games' training modes do:
//   F5 (BACK + LB)  recording: the player's controller (or the keyboard) drives the dummy while the player's own fighter
//                   stands still; F5 again (or START: the pause stays the player's) ends it. 60 s at most.
//   F6 (BACK + RB)  the dummy plays the recording back (F6 again, or START, stops it)
//   F7 (BACK + X)   the playback in a loop, or once
// The dummy follows player 2's controller only with Pausa > Opciones de IA > Tipo de enemigo: HUMANA (the panel says
// so). While a recording is made or played, player 2's controller looks connected (the capture interface's blocks, as
// an online session's guest's: netplay::OverrideBlock) and gets the inputs. A direction is kept as forward or back: a
// recording made with the dummy on one side plays on the other side mirrored. Never in an online session. The
// recording stays while the game is open. F2 > Controles turns it off (mk9_practice_record).

#pragma once

#include <cstdint>

namespace mk9::practice_record {

// At each frame's boundary (game thread): Practice or not, the requests from the keys, the dummy's side.
void OnFrame(uint8_t* base);

// The capture interface's block (game thread, the gamepad read; after an online session's own): player 1's pad taken
// for the dummy while recording, player 2's pad given the recording's while recording or playing.
void OnPadBlock(uint8_t* base, uint32_t tag, uint32_t data, uint32_t size, uint32_t user, uint32_t r1);

// Once (any thread): the panel goes into the overlay.
void Install();

}  // namespace mk9::practice_record
