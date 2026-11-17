// MK9 online: the online screens in the game (Renut's overlay; F1 shows and hides them): the player's name (needed to
// play online, kept in online_name.txt), the lobby (players online, the open rooms, quick match, a room to create,
// public or private, or to join by its code), the global chat, and the room (who is in it, ready, its chat). In a
// room's session the same window has the room's chat, the rival's ping and the way back to the lobby.

#pragma once

#include <string>

namespace mk9::online_ui {

// Once (any thread): the window goes into the overlay; with a name kept, the lobby connects.
void Install();

// mk9-lauta (2026-11-11, F3): the online name the first start's steps chose (UI thread): saved, the lobby connects
// with it, and the window does not need to open by itself for it.
void UseName(const std::string& name);

// mk9-lauta (2026-11-17, issue #10): the game is about to close (its close confirmed; any thread): in a room or a
// session, the goodbye goes to the others (through the lobby too: it gets there while the game thread waits for the
// others' inputs) and waits a moment for it to go out. Nothing otherwise.
void LeaveBeforeClose();

}  // namespace mk9::online_ui
