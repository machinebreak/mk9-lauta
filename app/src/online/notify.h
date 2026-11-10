// MK9 online: drawing the player's attention to the game while its window is not in front (mk9-lauta, 2026-11-10):
// a rival came into the room, a quick match found one, the rival is ready or wrote.

#pragma once

namespace mk9::notify {

// The game's window, when it is not the one in front (minimized, behind another program): its taskbar button
// flashes until it is brought up and, with sound, Windows' notification sound plays. Nothing when it is in front.
void Attention(bool sound);

}  // namespace mk9::notify
