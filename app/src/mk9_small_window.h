// mk9-lauta (2026-11-13): Linux: a small window of the game's own before the game's window exists (the first start's
// installer, the Kratos download at the start): SDL's (the X11 driver, as the game's), drawn with Dear ImGui (Renut's
// copy, as the overlay's) in the online screens' look and the Barlow fonts. Windows' are Win32 windows (mk9_installer.cpp,
// mk9_kratos.cpp). On the main thread, as SDL asks, before the SDK's message loop runs.
#pragma once

#if defined(__linux__) && !defined(__SWITCH__)

#include <filesystem>
#include <functional>

struct ImFont;
struct SDL_Window;

namespace mk9::small_window {

struct Look {
  float scale = 1.0f;       // the display's: sizes in points times this
  ImFont* title = nullptr;  // Barlow Condensed Bold (the default font is Barlow)
};

// Opens the window (SDL's video for its time; width and height in points) and, once a frame, calls frame() inside a
// full-window ImGui window until it returns false. close(): its X was pressed (true: it closes now). dropped(): a file
// or folder dropped on it. False when no window could open (no display).
bool Run(const char* title, int width, int height, const std::function<bool(const Look&)>& frame,
         const std::function<bool()>& close, const std::function<void(const std::filesystem::path&)>& dropped = {});

// The open window (a file dialog's parent), or null.
SDL_Window* Current();

}  // namespace mk9::small_window

#endif
