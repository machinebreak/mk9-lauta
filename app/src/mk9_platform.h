// mk9-lauta (2026-11-13): what the game asks its operating system for, the same on Windows and on Linux: where the
// game is (everything it writes lives beside it, as in the Windows release's folder), files by path, this process's
// own arguments, and starting another process (a session's game, the update, the way back).
#pragma once

#include <cstdio>
#include <ctime>
#include <filesystem>
#include <string>
#include <vector>

// The PC builds, Windows' and Linux's (not the Switch's): what the two share (the installer, this PC's defaults, the
// overlay's controller and close, the updater...).
#if defined(_WIN32) || (defined(__linux__) && !defined(__SWITCH__))
#define MK9_PC 1
#else
#define MK9_PC 0
#endif

namespace mk9::platform {

// The running executable, and its folder (the game's: cache, logs, saves, its settings).
std::filesystem::path ExePath();
std::filesystem::path ExeDir();

// fopen by path (UTF-16 on Windows, as the paths are; bytes on Linux).
std::FILE* OpenFile(const std::filesystem::path& path, const char* mode);

// Replaces `to` with `from` in one step (on the same volume): a download or a file written aside, put in place.
bool ReplaceFile(const std::filesystem::path& from, const std::filesystem::path& to);

// This process's arguments after the executable's own, as UTF-8.
std::vector<std::string> OwnArguments();

// Starts `exe` with `args` (UTF-8; quoted as each system needs) in `dir`, on its own: it outlives this process.
bool StartDetached(const std::filesystem::path& exe, const std::vector<std::string>& args,
                   const std::filesystem::path& dir);

// The local time of `t` (localtime_s on Windows, localtime_r on Linux).
inline std::tm LocalTime(std::time_t t) {
  std::tm out{};
#if defined(_WIN32)
  localtime_s(&out, &t);
#else
  localtime_r(&t, &out);
#endif
  return out;
}

// The program a player starts (Linux: the AppImage's file when the game runs from one, not its mounted inside).
std::filesystem::path LaunchPath();

}  // namespace mk9::platform
