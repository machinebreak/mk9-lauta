// mk9-nx (2026-11-08): the PC's controls. Which player each device plays: pads in connection order (the first pad
// player 1, the second player 2), the keyboard the player mk9_keyboard_player says (player 2 lets one PC hold a local
// Versus with the keyboard and one pad), the test script player 1. The list the options window shows, and the
// keyboard's actions with their keys (the SDK's keybind_* cvars) and the keys' names as the player's keyboard prints them.
//
// Online, all of a PC's devices play its player (netplay.cpp ReadLocalPad merges them), whatever this says.

#pragma once

#include <memory>
#include <string>
#include <vector>

namespace rex::input {
class DeviceAssignment;
}
namespace rex::ui {
class Window;
}

namespace mk9::controls {

// The input system's assignment (mk9_script_input.cpp CreateInputSystem).
std::unique_ptr<rex::input::DeviceAssignment> CreateAssignment();
// SDL's controller mappings (hid_mappings_file, gamecontrollerdb.txt) beside the executable when they are not in the
// working folder (the game started from a shortcut or by the online session's restart). Before the input system.
void FindMappingsFile();

// Once the window exists (its UI thread): the key capture listens to its key presses, ahead of the game's keyboard.
void AttachWindow(rex::ui::Window* window);
// A key to bind (the options window). After CaptureNextKey(true) the next key pressed goes to the capture only, not to
// the game, and TakeCapturedKey returns its virtual-key code once (-1 until then); CaptureNextKey(false) stops waiting.
// The code is the SDK's: the key's position on an English keyboard (the key right of L is Semicolon on any layout), as
// the game's keyboard reads it. F1, F2, F11 and the modifier keys are left out: they keep their own jobs.
void CaptureNextKey(bool on);
int TakeCapturedKey();

struct Device {
  std::string name;
  int player = 0;  // 1-based; 0: none (a fifth pad)
  bool keyboard = false;
};
// The devices now connected, as last enumerated (any thread).
std::vector<Device> Devices();

// One keyboard action: its key cvar (the SDK's keybind_*), its name, the gamepad button it presses.
struct Action {
  const char* cvar;
  const char* label;
  const char* button;
};
// In the options window's order: the four directions, the attacks, the rest.
const std::vector<Action>& Actions();

// A bind's keys as the player's keyboard prints them ("U", "Ñ", "Enter", "Flecha izquierda"), comma-separated ("" for
// none). The SDK names keys by their place on an English keyboard (Semicolon: right of L), shown as this layout's key
// there (Ñ on a Spanish keyboard, Ç on a Brazilian one).
std::string KeysLabel(const std::string& bind);
// The SDK's name for one of its virtual-key codes ("" when it has none: such a key cannot be bound).
std::string KeyName(int virtual_key);

}  // namespace mk9::controls
