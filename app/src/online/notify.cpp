// MK9 online: the player's attention while the game's window is not in front (notify.h).

#include "online/notify.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <mmsystem.h>

#include <rex/cvar.h>

REXCVAR_DEFINE_BOOL(mk9_online_notify, true, "MK9 Online",
                    "With the game's window not in front: its taskbar button flashes and a sound plays when a rival "
                    "comes into your room, a quick match finds one, or the rival is ready or writes");

namespace mk9::notify {
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

}  // namespace mk9::notify
