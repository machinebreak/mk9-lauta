// mk9-nx online (2026-11-09): the game's own texts where the PC version is not the Xbox 360's.
//
// The main menu's Xbox LIVE entry (MK9Game.<lang> [MainMenu]: XboxLIVE, XboxLIVE2Player and their descriptions) and
// the message it gives on a PC (MKOnline.<lang> [General] MultiplayerPrivilegeError_Message: "your profile has no
// permission for Xbox LIVE") point to this version's online instead, F1, in each of the game's languages. At start the
// Coalesced.<lang> files of the game's Localization folder are written again with those texts into cache\localization
// beside the executable (from the player's own files; only when something changed) and that folder is mounted over the
// game's Localization (the SDK's file system takes the most specific mount).

#pragma once

#include <filesystem>

namespace rex::filesystem {
class VirtualFileSystem;
}

namespace mk9::localization {

// Once the game's file system is set up and before the game starts (Mk9App::OnPostLoadXexImage).
void Install(rex::filesystem::VirtualFileSystem* vfs, const std::filesystem::path& game_root);

}  // namespace mk9::localization
