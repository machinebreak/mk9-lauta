// MK9 online: the online screens in the game (Renut's overlay; F1 shows and hides them): the player's name (needed to
// play online, kept in online_name.txt), the lobby (players online, the open rooms, quick match, a room to create,
// public or private, or to join by its code), the global chat, and the room (who is in it, ready, its chat). In a
// room's session the same window has the room's chat, the rival's ping and the way back to the lobby.

#pragma once

namespace mk9::online_ui {

// Once (any thread): the window goes into the overlay; with a name kept, the lobby connects.
void Install();

}  // namespace mk9::online_ui
