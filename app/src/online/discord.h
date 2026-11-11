// MK9 online: Discord's Rich Presence (mk9-lauta, 2026-11-10): what the player is doing shows in their Discord
// profile ("Peleando online vs X"), through the Discord app's local pipe (discord-ipc-N). Needs a Discord application's
// id (mk9_discord_app_id: the developer portal's Application ID; its name is what "Jugando a" shows, so the
// application is named "MK9 Online by Lauta"); empty, nothing is sent. Nothing either without the Discord app running.
//
// mk9-lauta (2026-11-11): the picture is the release's logo by its address (mk9_discord_image: Discord takes a URL for
// it, no art uploaded to the portal), with "MK9 Online by Lauta" over it, and two buttons the others see: the download
// (the latest release) and the community's Discord server.

#pragma once

#include <cstdint>
#include <string>

namespace mk9::discord {

// What shows (any thread; sent at most every few seconds, when it changed): details, the first line ("Peleando
// online"); state, the second ("vs Lucho"); since, the activity's start (seconds since 1970; 0: none).
void SetPresence(const std::string& details, const std::string& state, int64_t since);

// mk9-lauta (2026-11-11): the Discord account of the Discord app open on this PC (its READY): the user's id and its
// picture's name ("" for none). Waits up to timeout_ms for the app when the presence's link has not heard it yet. Any
// thread but the UI's; false: no Discord app answered.
bool User(std::string* id, std::string* avatar, int timeout_ms);

// A country's flag as Discord shows it (its two regional letters: "AR" -> the Argentine flag); "" for none.
std::string FlagEmoji(const std::string& country);

}  // namespace mk9::discord
