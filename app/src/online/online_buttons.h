// mk9-lauta (2026-11-16): the player's buttons in the online fights (F2 > Controles > Botones en el online).
//
// A session's game starts with a profile made anew (session.h), so both machines play the same: its OPCIONES > MANDO
// is the game's own (X front punch, Y back punch, A front kick, B back kick, RB throw, LB tag, RT block, LT flip
// stance), whatever the player chose in their own game. Here the player chooses which of their buttons does each of the
// eight, and in a session's fights (the rollback's frames and a lockstep fight's) this machine's pads are turned into
// the game's own buttons before they are sent: both machines see the same buttons, so nothing goes apart. Out of a
// fight (the menus, the character select) the buttons go as they are.

#pragma once

#include <array>
#include <cstdint>
#include <string>

namespace mk9::online_buttons {

// The pad's eight buttons that do a fighting move. The triggers count as pressed past XInput's threshold (30).
enum Button : uint8_t { kX, kY, kA, kB, kRB, kLB, kRT, kLT };
inline constexpr int kButtonCount = 8;

// The eight moves, in OPCIONES > MANDO's order: front punch, back punch, front kick, back kick, throw, tag, block, flip
// stance. A layout: the button of each.
inline constexpr int kActionCount = 8;
using Layout = std::array<Button, kActionCount>;

// The game's own (a session's game plays with it).
inline constexpr Layout kGameLayout = {kX, kY, kA, kB, kRB, kLB, kRT, kLT};

// "Golpe frontal (Front Punch)" and so on, for F2.
const char* ActionLabel(int action);
// "X", "Y", "A", "B", "RB", "LB", "RT", "LT" (mk9_net_buttons' words).
const char* ButtonName(Button button);
// For F2: the Xbox name and PlayStation's ("X (Cuadrado)", "RB (R1)").
const char* ButtonLabel(Button button);

// The layout mk9_net_buttons keeps ("X,Y,A,B,RB,LB,RT,LT", the moves' order; read once, then what SetCurrent says);
// the game's own when it is empty or not eight different buttons. Any thread.
Layout Current();
// F2's choice (the UI thread, after it went to mk9.toml): the fights from now on use it.
void SetCurrent(const Layout& layout);
// mk9_net_buttons' text for a layout ("" for the game's own).
std::string Encode(const Layout& layout);

// This machine's pad (X_INPUT_GAMEPAD as the guest stores it: 12 bytes, big-endian) with Current()'s buttons turned
// into the game's own: the others (the d-pad, START, BACK, the sticks) as they are. Nothing when Current() is the
// game's own.
void ToGameButtons(uint8_t* gamepad);

}  // namespace mk9::online_buttons
