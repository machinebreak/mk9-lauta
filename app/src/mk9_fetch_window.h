// mk9-lauta (2026-11-22): the small window before the game while something it needs downloads: Kratos (mk9_kratos.cpp)
// and the pack of the PS Vita costumes made of the Vita's own data (mk9_vita_costumes.cpp). A heading, a text, the bar
// with "<n> de <m> MB" and what closing it does. Windows: a Win32 window with a message loop on a thread of its own;
// Linux: a small window of the game's (mk9_small_window.h) on this thread, the main one, as SDL asks. The Switch: none.
#pragma once

#include <cstdint>
#include <functional>
#include <string>

namespace mk9::fetch_window {

struct Words {
  std::string title;    // the window's own ("MK9 Online by Lauta - Kratos")
  std::string heading;  // these three translated (i18n::T)
  std::string text;
  std::string closing;  // what its X does
  uint64_t bytes = 0;   // the download's size, for "<n> de <m> MB"
};

// The window while downloading() says the download goes on (progress: 0-1). False: the player closed it (the caller
// ends the game: nothing of it runs yet). True: the download ended or stopped by itself, or no window could open (a
// Linux without a display: the download goes on behind).
bool Run(const Words& words, const std::function<bool(float& progress)>& downloading);

// The game closed from the window's X (nothing of it runs yet): the log flushed and the process ended at once, the
// download's thread with it (its .partial stays: the next start's download goes on from there).
[[noreturn]] void CloseGame();

}  // namespace mk9::fetch_window
