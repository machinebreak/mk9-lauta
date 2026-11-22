// Scripted gamepad for automated tests: a synthetic player-1 device that presses buttons at given times after start
// (cvars mk9_input_script / mk9_input_script_file). The SDK puts synthetic devices on player 1 and merges them with
// any real pad, so a script and a controller can be used together.

#pragma once

#include <cstdint>
#include <memory>
#include <string>

#include <rex/system/interfaces/input.h>

namespace mk9::script_input {

// The SDK's default input system (SDL pads, keyboard, the NOP stand-in) plus the scripted pad when a script is set.
std::unique_ptr<rex::system::IInputSystem> CreateInputSystem(bool tool_mode);

// mk9-lauta (2026-11-21, tests): the script's anchors ("@fight", "@<package>": mk9_script_input.cpp). The game asks for
// a package (online/loading.cpp; any thread); each game frame (the game thread: the fight in play).
void NotePackage(const std::string& name);
void OnFrame(uint8_t* base);

}  // namespace mk9::script_input
