// mk9-lauta (2026-11-11, PLAN-0.4 F3): the first start as a few steps, in the overlay: welcome, the game's language,
// the online name, the quality (the one the first start chose for this PC, mk9_hardware.h, with the others to pick)
// and the controller (what is connected, and a test of its buttons), then what F1 and F2 do. Each choice is written to
// mk9.toml at once, as the options window (F2) does; "Saltar" leaves the rest as it is. The first start writes
// mk9_first_run_done = false (mk9_hardware.cpp), so the steps show until they are finished or skipped, also after a
// start that closed half way; the players of earlier releases (no such line) never see them. While they show the game
// gets no input (mk9_overlay_input.h) and the online window does not open by itself.

#pragma once

namespace mk9::first_run_ui {

// Once (any thread): the window goes into the overlay.
void Install();

// The steps are showing (UI thread).
bool Active();

}  // namespace mk9::first_run_ui
