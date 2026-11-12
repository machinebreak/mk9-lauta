// mk9-lauta: the in-game updater. At start (not in an online session's game) it asks GitHub for the newest release of
// mk9-lauta (mk9_update_repo); a newer one is announced in a banner over the game, and "Actualizar ahora" downloads it
// in the background, checks its signature (an ECDSA P-256 signature of the zip, made with the release key, whose
// public half is in the program: tools/release/sign_release.py) and unpacks it beside the program, in update\. On
// "Reiniciar ahora" the program's files are replaced (the old ones are kept as .old until the next start, and put back
// if anything fails) and the new version starts. The player's settings (mk9.toml), saves, game files, logs and cache
// are never touched.

#pragma once

#include <cstdint>
#include <filesystem>
#include <string>

namespace mk9::updater {

// Once (any thread, after the cvars are read): the old files of an update installed before go, the banner goes into
// the overlay, and the check starts (mk9_update_check).
void Install();

enum class Phase { kIdle, kChecking, kUpToDate, kAvailable, kDownloading, kReady, kFailed };
struct Status {
  Phase phase = Phase::kIdle;
  std::string current;   // this program's version ("0.1.0")
  std::string latest;    // the newest release's ("0.2.0"), once checked
  std::string page;      // the newest release's page
  std::string notes;     // ... and its notes
  double progress = 0;   // kDownloading: 0-1
  std::string message;   // what happened (kFailed: why)
};
Status GetStatus();

// The options' "Acerca de" tab: check again now; start the download; install and start the new version.
void CheckNow();
void Download();
void InstallAndRestart();
// mk9-lauta (2026-11-11, F3): the game started again as this one was (its arguments), and this one closed (a setting
// read only at the start: the game's language). Returns only if the new one could not start.
void Restart();
// Opens a web page or a folder (ShellExecute).
void Open(const std::string& what);
// The folder of this program's logs.
std::string LogsFolder();
// mk9-lauta (2026-11-11): for the Kratos installer (mk9_kratos.h): a zip unpacked into a folder made anew (Windows'
// tar, else PowerShell), and a file's SHA-256.
bool UnpackZip(const std::filesystem::path& zip, const std::filesystem::path& to);
bool FileSha256(const std::filesystem::path& file, uint8_t out[32]);

}  // namespace mk9::updater
