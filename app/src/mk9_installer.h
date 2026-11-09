// mk9-lauta (2026-11-09): the first start's installer, as other recompiled ports have one. With no game files found
// (no game_root beside the executable, none configured), a window of its own asks for the player's copy of the game
// before anything else starts: an .iso of the Xbox 360 disc, or a folder where it is already extracted. The disc's
// default.xex must be the one this build was made from (its SHA-256); from an .iso, the 1,263 files the game uses
// (app/mk9_game_files.txt: $SystemUpdate and the like stay out) are extracted to game_root beside the executable, each
// checked against its SHA-256 as it is written; a folder is checked (default.xex's hash, every file's size) and used
// where it is, its path kept in mk9.toml (game_data_root).
//
// Tests: MK9_TEST_INSTALL=<.iso or folder> picks it without the window's buttons (the window still shows the work).

#pragma once

#include <filesystem>

namespace mk9::installer {

// The game's folder to use, or empty when the player closed the window (the game then ends). Blocks until done.
std::filesystem::path Run(const std::filesystem::path& exe_folder);

// The folder an earlier start chose (mk9.toml's game_data_root), read here: the SDK reads mk9.toml only after it has
// set the paths up, so its game_data_root reaches the paths only from the command line. Empty: none.
std::filesystem::path ConfiguredRoot(const std::filesystem::path& config_path);

}  // namespace mk9::installer
