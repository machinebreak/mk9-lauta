// MK9 online: the player's attention while the game's window is not in front (notify.h).

#include "online/notify.h"

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <mmsystem.h>
#else
// mk9-lauta (2026-11-13): Linux: the game's SDL window flashes (its taskbar entry, as the desktop shows urgency) and
// the desktop's "new message" sound plays when there is a player for it (canberra-gtk-play, or paplay of the
// freedesktop sound theme)
#include <SDL3/SDL.h>

#include <cstdlib>
#include <filesystem>
#include <thread>
#endif

#include <rex/cvar.h>

REXCVAR_DEFINE_BOOL(mk9_online_notify, true, "MK9 Online",
                    "With the game's window not in front: its taskbar button flashes and a sound plays when a rival "
                    "comes into your room, a quick match finds one, or the rival is ready or writes");

namespace mk9::notify {
#if defined(_WIN32)
namespace {

// This process's main window: visible, top level, with a title.
HWND GameWindow() {
  struct Find {
    DWORD pid;
    HWND found;
  } find{GetCurrentProcessId(), nullptr};
  EnumWindows(
      [](HWND hwnd, LPARAM param) -> BOOL {
        auto* f = reinterpret_cast<Find*>(param);
        DWORD pid = 0;
        GetWindowThreadProcessId(hwnd, &pid);
        if (pid != f->pid || !IsWindowVisible(hwnd) || GetWindow(hwnd, GW_OWNER) != nullptr ||
            GetWindowTextLengthW(hwnd) == 0) {
          return TRUE;
        }
        f->found = hwnd;
        return FALSE;
      },
      reinterpret_cast<LPARAM>(&find));
  return find.found;
}

}  // namespace

void Attention(bool sound) {
  if (!REXCVAR_GET(mk9_online_notify)) return;
  static HWND window = nullptr;
  if (!window || !IsWindow(window)) window = GameWindow();
  if (!window) return;
  if (GetForegroundWindow() == window && !IsIconic(window)) return;
  FLASHWINFO flash{};
  flash.cbSize = sizeof(flash);
  flash.hwnd = window;
  flash.dwFlags = FLASHW_ALL | FLASHW_TIMERNOFG;  // until the window comes to the front
  FlashWindowEx(&flash);
  if (sound) PlaySoundW(L"Notification.Default", nullptr, SND_ALIAS | SND_ASYNC | SND_NODEFAULT);
}
#else
namespace {

// The game's window: SDL's first (the game has one).
SDL_Window* GameWindow() {
  int count = 0;
  SDL_Window** windows = SDL_GetWindows(&count);
  SDL_Window* window = windows && count > 0 ? windows[0] : nullptr;
  SDL_free(windows);
  return window;
}

void PlayMessageSound() {
  // off the game's threads: the players for the sound may take a moment to start
  std::thread([] {
    static const char* const kSounds[] = {"/usr/share/sounds/freedesktop/stereo/message-new-instant.oga",
                                          "/usr/share/sounds/freedesktop/stereo/message.oga"};
    if (std::system("command -v canberra-gtk-play >/dev/null 2>&1") == 0) {
      std::system("canberra-gtk-play -i message-new-instant >/dev/null 2>&1");
      return;
    }
    std::error_code ec;
    for (const char* sound : kSounds) {
      if (!std::filesystem::exists(sound, ec)) continue;
      const std::string command = std::string("paplay '") + sound + "' >/dev/null 2>&1";
      std::system(command.c_str());
      return;
    }
  }).detach();
}

}  // namespace

void Attention(bool sound) {
  if (!REXCVAR_GET(mk9_online_notify)) return;
  SDL_Window* window = GameWindow();
  if (!window) return;
  const SDL_WindowFlags flags = SDL_GetWindowFlags(window);
  if ((flags & SDL_WINDOW_INPUT_FOCUS) && !(flags & SDL_WINDOW_MINIMIZED)) return;
  SDL_FlashWindow(window, SDL_FLASH_UNTIL_FOCUSED);  // until the window comes to the front
  if (sound) PlayMessageSound();
}
#endif

}  // namespace mk9::notify
