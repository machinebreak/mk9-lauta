// mk9-nx: the game's profile questions answered (mk9_profile_prompt.cpp). mk9-lauta (2026-11-18, issue #14): player
// one's profile read by the game once it reaches the main menu.

#pragma once

#include <cstdint>

namespace mk9::profile {

// Every frame's boundary (game thread): once, at the game's first main menu outside an online session, the sign-in
// notification that makes the game read player one's profile save.
void OnFrame(uint8_t* base);

}  // namespace mk9::profile
