// mk9-lauta: the overlay windows (F1, F2) with a controller, and what the game gets while one is open.
//
// The controllers' state is read once a game frame, at the frame's start (the gamepad read's hook, before the game
// reads its pads), past the input system's active callback, and handed to the overlay's ImGui as gamepad keys:
// A chooses, B goes back (and closes the window when nothing is active), the d-pad or the left stick move, LB / RB
// change tabs. Back + Start opens or closes the options (F2), Back + Y the online screens (F1). While an overlay window
// is open the game gets no input (the input system's active callback says no: neutral pads, also in an online
// session), so navigating a window never moves the fighter; the frames of those combos are kept from the game too.
// The window's close (its X, Alt+F4) asks first.

#pragma once

#include <cstdint>

namespace rex::input {
class InputSystem;
}

namespace mk9::overlay_input {

// Once, after the SDK's setup (Mk9App::OnPostSetup): the active callback, the overlay's gamepad and its close.
void Install(rex::input::InputSystem* input);

// The frame's start (game thread, before the game reads its pads): the controllers' state for the overlay.
void OnFrame();

// The overlay's windows (UI thread, each overlay frame): open or not.
enum Window { kOptions = 0, kOnline = 1, kConfirm = 2, kWizard = 3 };  // kWizard: first_run_ui.h
void SetOpen(Window window, bool open);

// The combos, as the overlay's ImGui sees the controller (UI thread, inside a dialog's Draw).
bool ToggleOptionsPressed();  // Back + Start
bool ToggleOnlinePressed();   // Back + Y
// LB / RB in a window: -1, +1 or 0.
int TabStep();
// B with nothing active in a focused window: close it.
bool ClosePressed();

}  // namespace mk9::overlay_input
