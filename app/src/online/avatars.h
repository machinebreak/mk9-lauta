// MK9 online: the players' profile pictures (mk9-lauta, 2026-11-11).
//
// A picture is a 128x128 JPEG of a few KB (kMaxBytes at most), named by a hash of its bytes (16 hex digits). The
// player's own is online_avatar.jpg beside the executable: any picture Windows opens, cut to a square from its middle
// (turned as the camera said) and scaled down, or the player's Discord picture, from Discord's own servers. Another
// player's comes once, with the room's messages or a profile (profiles.h), only when this game lacks that hash, and is
// kept in avatars\ beside the executable (the newest 300) for the next time. No server keeps them: the lobby's relay
// passes those few KB from one game to the other, as it does a room's chat.
//
// Windows' own imaging (WIC) reads and writes them, on a thread of their own, never the game's: a picture from another
// player is checked (its hash, a JPEG of exactly 128x128, kMaxBytes at most) before it is kept or shown. Its texture
// (64 KB) is made on the overlay's thread once decoded, 24 at most at once (the least used go). Drawing one is a quad.

#pragma once

#include <cstddef>
#include <string>

#include "imgui.h"

namespace mk9::avatars {

constexpr int kSize = 128;
constexpr size_t kMaxBytes = 20 * 1024;

// --- this player's ---
// Its hash ("" none) and its bytes. Any thread.
std::string MineHash();
std::string MineData();
// A picture from a file (Windows' dialog, then the work on a thread: MineHash changes when done). UI thread.
void ChooseFile();
// The same from a file named here (UTF-8; tests: mk9_avatar_test_file).
void UseFile(const std::string& path);
// The player's Discord picture (the Discord app open: discord.h), downloaded from Discord's servers and kept as above.
void UseDiscord();
void Remove();
// A change in progress, and the last one's outcome for the screen ("" none; in the player's language).
bool Busy();
std::string Note();

// --- the others' ---
// A picture kept here (avatars\<hash>.jpg, or this player's).
bool Have(const std::string& hash);
// Its bytes ("" when not kept).
std::string Read(const std::string& hash);
// A picture another game sent (any thread): checked on the pictures' thread, then kept (else dropped, logged).
void Store(const std::string& hash, const std::string& jpeg);
// A hash this game lacks and has not asked for in the last 20 s (then marked asked): whether to ask for it now.
bool Wanted(const std::string& hash);
// Other players' pictures are shown (mk9_avatars_others, F2 > Online).
bool ShowOthers();

// --- drawing (UI thread, the overlay's ImGui) ---
// The picture at pos, size px square with rounded corners and a border (border_size 0: none). False, nothing drawn,
// when there is none or it is not decoded yet (it is being: a later frame draws it).
bool Draw(ImDrawList* list, const std::string& hash, const ImVec2& pos, float size, float rounding, ImU32 border = 0,
          float border_size = 0.0f);
// As an item at the cursor; with no picture, the name's first letter on a square of a color of its own.
void Item(const std::string& hash, const std::string& name, float size);

// The relay's text for the bytes, and back ("" for a bad text).
std::string Base64(const std::string& bytes);
std::string Unbase64(const std::string& text);
// A picture's name: FNV-1a 64 of its bytes, 16 hex digits.
std::string HashOf(const std::string& bytes);
// A hash as sent: 16 hex digits, else "".
std::string CleanHash(const std::string& hash);

}  // namespace mk9::avatars
