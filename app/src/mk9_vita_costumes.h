// mk9-lauta (2026-11-21): the PS Vita version's extra costumes on the Xbox 360 game: the retro MK2 costumes of
// Scorpion, Sub-Zero, Reptile, Ermac and Mileena and the MK2 and MK3 ones of Skarlet, each with its Babality (the
// Vita's others need meshes and textures only the Vita has: tools/costumes/README.md).
//
// This project ships no game data: each costume package is made at the first start from the player's own game files
// (the costume packages of another letter they are built from, and the ones that hold the new look) and a patch built
// into the program (app/costumes/vita_costumes.bin: copies from those files' decompressed images plus the bytes that
// are new: names, tables, references; tools/costumes/README.md). They go to cache/costumes/Asset, with a Coalesced.ini
// and an Xbox360TOC.txt made from the game's own (a few lines that register the costumes), and that folder is mounted
// over the game's as an overlay (rex::filesystem::Device::SetOverlay): it serves only those files. The game files are
// not touched (the file check, Kratos' install and his removal see them as they are).
#pragma once

#include <cstdint>
#include <filesystem>
#include <string>

namespace rex::filesystem {
class VirtualFileSystem;
}

namespace mk9::vita_costumes {

// With the game's image loaded and before its first file: the costumes made (the first time, a few seconds; again
// only when a release changes one) and registered, their folder mounted over the game's. Nothing when off
// (mk9_vita_costumes = false, F2 > Controles), or for a costume whose source files are not the ones the patches were
// made for (it is left out; the log says which). Online, both games must have the same set (room.cpp's Mods).
void Install(rex::filesystem::VirtualFileSystem* vfs, const std::filesystem::path& game_root);

// The costumes are in this game (fixed at the start).
bool Installed();

// Which costumes (0: none): the online games compare it (both must have the same set or none).
uint32_t Signature();

// For F2: how many costumes are in ("" none).
std::string Summary();

// For F2: the download of the pack of the costumes made of the Vita's own data (-1 none, 0-100 going, 101 done: in
// from the next start).
int PackDownload();

}  // namespace mk9::vita_costumes
