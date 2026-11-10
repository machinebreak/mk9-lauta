// MK9 online: Discord's Rich Presence (mk9-lauta, 2026-11-10): what the player is doing shows in their Discord
// profile ("Peleando online vs X"), through the Discord app's local pipe (discord-ipc-N). Needs a Discord application's
// id (mk9_discord_app_id: the developer portal's Application ID; its name is what "Jugando a" shows, its art asset
// "logo" the picture); empty, nothing is sent. Nothing either without the Discord app running.

#pragma once

#include <cstdint>
#include <string>

namespace mk9::discord {

// What shows (any thread; sent at most every few seconds, when it changed): details, the first line ("Peleando
// online"); state, the second ("vs Lucho"); since, the activity's start (seconds since 1970; 0: none).
void SetPresence(const std::string& details, const std::string& state, int64_t since);

}  // namespace mk9::discord
