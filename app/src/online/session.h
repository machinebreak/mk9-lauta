// MK9 online: a room's fight starts the game again already joined (room.h): the same executable with its options and
// the room's (mk9_net_room, mk9_net_role, mk9_online_name), and a profile of its own made anew (online_session beside
// the executable): both machines start from the same save, so the session runs in lockstep from the first frame and
// the game's own menus lead the two players to the Versus selection. When the session is over, Back starts the game
// again as it was.

#pragma once

#include <string>

namespace mk9::session {

// Starts the room's session and ends this game.
[[noreturn]] void Launch(const std::string& room, bool host, const std::string& name);
// Starts the game again without a session and ends this one.
[[noreturn]] void Back();

// The player's name in the lobby (online_name.txt beside the executable; mk9_online_name before it): "" none yet.
std::string LoadName();
void SaveName(const std::string& name);

}  // namespace mk9::session
