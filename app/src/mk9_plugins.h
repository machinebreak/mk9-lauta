// mk9-lauta (2026-11-16, issue #8): code plugins, off by default. With mk9_plugins = true in mk9.toml, every .dll
// (Linux: .so) in the "plugins" folder beside the executable is loaded in alphabetical order and its rex_mod_init is
// given the host (rex_mod.h: the game's memory, its functions, the log) before the game's first instruction; its
// rex_mod_shutdown, if it has one, when the game closes normally. Every plugin loaded and every function one replaces
// goes to the log. For mods such as an accessibility one (a screen reader for the menus) without wrapping the game's
// DLLs (a fake version.dll, which antivirus programs delete).
//
// A plugin is third-party code with the game's own access: only the player's choice turns it on. One that changes the
// fight in an online session makes the two games go apart, and the session ends (netplay.h).

#pragma once

#include <string>
#include <vector>

namespace mk9::plugins {

// Once, after the game's image is loaded and before its first instruction (the app's OnPostLaunchModule).
void Load();

// Each loaded plugin's rex_mod_shutdown, once (a normal close; any thread).
void Shutdown();

// The plugins loaded ("name.dll"), for F2 > Acerca de.
std::vector<std::string> Loaded();

}  // namespace mk9::plugins
