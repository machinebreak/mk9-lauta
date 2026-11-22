// mk9-lauta (2026-11-21): the PS Vita version's extra costumes on the Xbox 360 game: the retro MK2 costumes of
// Scorpion, Sub-Zero, Reptile, Ermac and Mileena and the MK2 and MK3 ones of Skarlet, each with its Babality.
// 2026-11-22: and the Vita's others, made of meshes and textures only the Vita has (the retro MK3 costumes of Scorpion,
// Sub-Zero, Reptile, Ermac, Smoke, Noob and Rain, Kenshi's and Skarlet's alternate ones): those come in a pack hosted
// apart (machinebreak/mk9-lauta-vita-costumes, about 15 MB), downloaded at the start (AtStart), as Kratos is.
//
// This project ships no game data (the pack is the Vita's, converted, and hosted apart): each costume package is made
// at the first start from the player's own game files (the costume packages of another letter they are built from, and
// the ones that hold the new look) and a patch built into the program (app/costumes/vita_costumes.bin: copies from
// those files' decompressed images plus the bytes that are new: names, tables, references; the pack's patches are the
// same, their new bytes the Vita's meshes and textures; tools/costumes/README.md). They go to cache/costumes/Asset,
// with a Coalesced.ini and an Xbox360TOC.txt made from the game's own (a few lines that register the costumes), and
// that folder is mounted over the game's as an overlay (rex::filesystem::Device::SetOverlay): it serves only those
// files. The game files are not touched (the file check, Kratos' install and his removal see them as they are).
#pragma once

#include <cstdint>
#include <filesystem>
#include <string>

namespace rex::filesystem {
class VirtualFileSystem;
}

namespace mk9::vita_costumes {

// Before the game (OnPreSetup, after the file check): the pack downloaded when it is published and not here, with a
// window (mk9_fetch_window.h) whose X closes the game; its costumes are in from this start. Not in an online session's
// game (its content was what the room compared) nor in a test; with no connection, the game starts without them.
void AtStart();

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

// The pack's costumes are in this game (fixed at the start): the rooms say so (room.cpp's Mods), to tell a player who
// lacks them what to do.
bool PackIn();

// For F2: the pack was wanted at this start but is not here (its download failed: the next start tries again).
bool PackMissing();

}  // namespace mk9::vita_costumes
