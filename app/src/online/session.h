// MK9 online: a room's fight starts the game again already joined (room.h): the same executable with its options and
// the room's (mk9_net_room, mk9_net_role, mk9_online_name), and a profile of its own made anew (online_session beside
// the executable): both machines start from the same save, so the session runs in lockstep from the first frame and
// the game's own menus lead the two players to the Versus selection. When the session is over, Back starts the game
// again as it was.

#pragma once

#include <string>
#include <vector>

namespace mk9::session {

// Starts the room's session and ends this game.
[[noreturn]] void Launch(const std::string& room, bool host, const std::string& name);
// Starts the game again without a session and ends this one.
[[noreturn]] void Back();

// The player's name in the lobby (online_name.txt beside the executable; mk9_online_name before it): "" none yet.
std::string LoadName();
void SaveName(const std::string& name);

// mk9-lauta (2026-11-10): the players this one silenced (online_blocked.txt beside the executable, a name a line; any
// thread): their lines in the chats, their rooms in the list and their hellos to this one's room are left out, on
// this machine only. Names compare without regard to case.
bool IsBlocked(const std::string& name);
void SetBlocked(const std::string& name, bool blocked);
std::vector<std::string> Blocked();

}  // namespace mk9::session
