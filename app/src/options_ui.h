// mk9-nx (2026-11-08): the game's options for the players, in Spanish (Renut's overlay, F2): a quality preset (Baja,
// Consola, Alta, Ultra) and each of Renut's graphics options; the controls (the keyboard on or off, the player it
// plays, each action's key, bound by pressing it, mk9_controls.h; the pads connected); full screen or a window (also
// F11 and Alt+Enter, any time) and the game's language; the online fights' input delay. A change applies at once (MSAA
// and the language: from the next start) and is kept in mk9.toml beside the executable: only these keys are written
// there, the rest of the file and the options given on the command line stay as they are.

#pragma once

#include <string>
#include <utility>
#include <vector>

namespace mk9::options_ui {

// Once (any thread): the window goes into the overlay.
void Install();

// mk9-lauta (2026-11-09): a quality preset's keys and values ("Muy baja", "Baja", "Consola", "Alta", "Ultra"; nullptr
// for another name), and the keys' lines written to mk9.toml (replaced or added; every other line as it was).
const std::vector<std::pair<const char*, const char*>>* PresetValues(const std::string& name);
bool SaveToConfig(const std::vector<std::pair<std::string, std::string>>& values);

}  // namespace mk9::options_ui
