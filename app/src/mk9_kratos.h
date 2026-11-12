// mk9-lauta: Kratos, the PS3 version's guest fighter, from the community's Xbox 360 port
// (github.com/eLGatoRDV/mk9-kratos-xbox360, release v1.0). Nothing of his is distributed with this project: the player
// installs the port's package into their own game files (tools/kratos/kratos_install.py), and this build then does
// what the port's patched default.xex does (kratos.toml) and shows the game the port's DLC package.
//
// Installed, the game files hold: Kratos' 12 packages and his 3 sound banks in Asset/, the Coalesced.ini that registers
// him (and an Xbox360TOC.txt that lists his packages), and the DLC package's files in
// dlc/0000000000000000/575207FD/00000002/KRATOS/ (as on the console's drive; the SDK's content manager reads DLC from
// the cvar dlc_root, set here to that dlc folder).

#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>

namespace mk9::kratos {

// At the start, before the game (and before the file check): whether Kratos is in these game files. If he is, the
// content manager gets his DLC folder and the two code changes are on; a partial install is logged and left off.
void AtStart(const std::filesystem::path& game_root);

// Kratos is in this game (fixed at the start). The online games compare it: both must have him or neither.
bool Installed();

// A game file's size as the port leaves it (Config/Coalesced.ini, Xbox360TOC.txt), with his packages in the game
// files: the file check takes it as well as the disc's.
bool PatchedSize(std::string_view path, uint64_t size);

// Some of his files are in the game files but not all he needs (the start said what is missing in the log).
bool Partial();

// F2 > Kratos (mk9_kratos_install.cpp, PC). Install: the player picks the port's release zip
// (Kratos-MK9-360-Share.zip) or its unpacked folder, and on a thread of its own the same steps as
// tools/kratos/kratos_install.py are made in the game files, each checked; the disc's Coalesced.ini and Xbox360TOC.txt
// are kept in game_root/kratos_backup. Remove: at the next start, before the game opens any file (AtStart), his files
// go and the two kept ones come back. Both take effect at a restart.
struct InstallStatus {
  bool busy = false;
  bool done = false;   // installed: restart to play with him
  float progress = 0;  // 0-1 while busy
  std::string step;    // what it is doing, or what failed
  bool failed = false;
};
void ChooseAndInstall();
void InstallFrom(const std::filesystem::path& zip_or_folder);  // tests: mk9_kratos_install
InstallStatus Status();
void RemoveAtNextStart();
bool RemovePending();
// Called by AtStart: a removal asked for at the last run.
void RemoveIfAsked(const std::filesystem::path& game_root);

}  // namespace mk9::kratos
