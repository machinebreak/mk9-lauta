// mk9-lauta: Kratos, the PS3 version's guest fighter, from the community's Xbox 360 port
// (github.com/eLGatoRDV/mk9-kratos-xbox360, release v1.0). He comes with the game by himself: the first start without
// him downloads the port's release zip from that repository in the background (StartFetch; checked by its SHA-256),
// and the next start installs it into the player's game files before the game opens any file (AtStart, ~1.5 s). This
// project hosts none of his files. Then this build does what the port's patched default.xex does (kratos.toml) and
// shows the game the port's DLC package.
//
// Installed, the game files hold: Kratos' 12 packages and his 3 sound banks in Asset/, the Coalesced.ini that registers
// him (and an Xbox360TOC.txt that lists his packages), the DLC package's files in
// dlc/0000000000000000/575207FD/00000002/KRATOS/ (as on the console's drive; the SDK's content manager reads DLC from
// the cvar dlc_root, set here to that dlc folder), and the disc's Coalesced.ini and TOC in kratos_backup/.

#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>

namespace mk9::kratos {

// At the start, before the game (and before the file check): the downloaded zip installed if he is not in these game
// files yet (not in an online session's game: its rival checked what this one had), then whether he is. If he is,
// the content manager gets his DLC folder and the two code changes are on; a partial install is logged and left off.
void AtStart(const std::filesystem::path& game_root);

// Kratos is in this game (fixed at the start). The online games compare it: both must have him or neither.
bool Installed();

// A game file's size as the port leaves it (Config/Coalesced.ini, Xbox360TOC.txt), with his packages in the game
// files: the file check takes it as well as the disc's.
bool PatchedSize(std::string_view path, uint64_t size);

// Once the game runs (the window exists): without him and without the zip, its download starts on a thread of its own
// (not in an online session's game nor in a scripted test; mk9_kratos_auto).
void StartFetch();

// The download, for the main menu's corner: downloading (progress 0-1), or done (he comes at the next start).
struct Fetch {
  bool downloading = false;
  bool ready = false;
  float progress = 0;
};
Fetch FetchStatus();

}  // namespace mk9::kratos
