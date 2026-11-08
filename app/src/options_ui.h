// mk9-nx (2026-11-08): the game's options for the players, in Spanish (Renut's overlay, F2): a quality preset (Baja,
// Consola, Alta, Ultra) and each of Renut's graphics options, and the online fights' input delay. A change applies at
// once (MSAA: from the next start) and is kept in mk9.toml beside the executable: only these keys are written there,
// the rest of the file and the options given on the command line stay as they are.

#pragma once

namespace mk9::options_ui {

// Once (any thread): the window goes into the overlay.
void Install();

}  // namespace mk9::options_ui
