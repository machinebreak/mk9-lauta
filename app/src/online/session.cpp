// MK9 online: a room's session started anew (session.h).

#include "online/session.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
// after windows.h
#include <shellapi.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <iterator>
#include <mutex>
#include <sstream>
#include <vector>

#include <rex/cvar.h>
#include <rex/logging.h>

#include "online/lobby.h"

REXCVAR_DECLARE(std::string, mk9_online_name);

namespace mk9::session {
namespace {

std::filesystem::path ExeDir() {
  wchar_t path[MAX_PATH] = {};
  GetModuleFileNameW(nullptr, path, MAX_PATH);
  return std::filesystem::path(path).parent_path();
}

std::wstring Wide(const std::string& s) {
  if (s.empty()) return {};
  const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), nullptr, 0);
  std::wstring w(size_t(n), L'\0');
  MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), w.data(), n);
  return w;
}

// An argument as CommandLineToArgvW reads it back.
std::wstring Quote(const std::wstring& arg) {
  if (!arg.empty() && arg.find_first_of(L" \t\"") == std::wstring::npos) return arg;
  std::wstring out = L"\"";
  size_t slashes = 0;
  for (wchar_t c : arg) {
    if (c == L'\\') {
      ++slashes;
      continue;
    }
    if (c == L'"') out.append(slashes * 2 + 1, L'\\');
    else out.append(slashes, L'\\');
    slashes = 0;
    out.push_back(c);
  }
  out.append(slashes * 2, L'\\');
  out.push_back(L'"');
  return out;
}

// This game's own options, without a session's (nor a test's way into one: mk9_online_test, nor the files a test's
// run writes: the game started again would write over them), nor those in dropping, which the caller gives again: the
// command line's parser (CLI11) refuses an option given twice, and then takes none of them (a spectator started this
// way with two user_data_root came up offline, on the default lobby).
std::vector<std::wstring> OwnOptions(std::initializer_list<const wchar_t*> dropping = {}) {
  static const wchar_t* const kTestOutputs[] = {L"--mk9_check_log",    L"--mk9_rand_log",     L"--mk9_native_log",
                                                L"--mk9_script_log",   L"--mk9_state_hash",   L"--mk9_input_record",
                                                L"--mk9_snapshot_s"};
  int argc = 0;
  wchar_t** argv = CommandLineToArgvW(GetCommandLineW(), &argc);
  std::vector<std::wstring> out;
  for (int i = 1; argv && i < argc; ++i) {
    const std::wstring a = argv[i];
    if (a.rfind(L"--mk9_net_room", 0) == 0 || a.rfind(L"--mk9_net_role", 0) == 0 ||
        a.rfind(L"--mk9_online_name", 0) == 0 || a.rfind(L"--mk9_online_show", 0) == 0 ||
        a.rfind(L"--mk9_online_test=", 0) == 0 || a.find(L"online_session") != std::wstring::npos ||
        a.rfind(L"--mk9_net_replay", 0) == 0 || a.rfind(L"--mk9_lobby_by_address", 0) == 0 ||
        a.rfind(L"--mk9_net_local_players", 0) == 0 ||
        std::any_of(std::begin(kTestOutputs), std::end(kTestOutputs),
                    [&](const wchar_t* o) { return a.rfind(o, 0) == 0; }) ||
        std::any_of(dropping.begin(), dropping.end(), [&](const wchar_t* o) { return a.rfind(o, 0) == 0; })) {
      continue;
    }
    out.push_back(a);
  }
  if (argv) LocalFree(argv);
  // mk9-lauta (2026-11-13): this connection's DNS does not know the lobby's name: the next game goes to its address
  // from the start (lobby.h ByAddress), its /ice included (a session that asked by name ran with no TURN)
  if (lobby::ByAddress()) out.push_back(L"--mk9_lobby_by_address=true");
  return out;
}

[[noreturn]] void Start(const std::vector<std::wstring>& options, const char* what) {
  const std::filesystem::path dir = ExeDir();
  wchar_t exe[MAX_PATH] = {};
  GetModuleFileNameW(nullptr, exe, MAX_PATH);
  std::wstring line = Quote(exe);
  for (const std::wstring& o : options) line += L" " + Quote(o);
  STARTUPINFOW startup{};
  startup.cb = sizeof(startup);
  PROCESS_INFORMATION process{};
  std::vector<wchar_t> buffer(line.begin(), line.end());
  buffer.push_back(L'\0');
  if (CreateProcessW(exe, buffer.data(), nullptr, nullptr, FALSE, 0, nullptr, dir.c_str(), &startup, &process)) {
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    REXLOG_INFO("[session] {}: the game starts again", what);
  } else {
    REXLOG_ERROR("[session] {}: the game could not start again (error {})", what, GetLastError());
  }
  rex::FlushLogging();
  TerminateProcess(GetCurrentProcess(), 0);
  for (;;) Sleep(1000);
}

}  // namespace

void Launch(const std::string& room, bool host, const std::string& name, const std::string& title, bool listed,
            int players) {
  // a profile made anew: both games start from the same save (the role's own, so two games on one PC do not share it)
  const std::filesystem::path profile = ExeDir() / (host ? "online_session_host" : "online_session_guest");
  std::error_code ec;
  std::filesystem::remove_all(profile, ec);
  std::filesystem::create_directories(profile, ec);
  std::vector<std::wstring> options = OwnOptions({L"--user_data_root"});
  options.push_back(L"--mk9_net_room=" + Wide(room));
  options.push_back(std::wstring(L"--mk9_net_role=") + (host ? L"host" : L"guest"));
  options.push_back(L"--mk9_online_name=" + Wide(name));
  if (players == 2) options.push_back(L"--mk9_net_local_players=2");  // two at this PC (netplay.h)
  if (host && listed) {
    options.push_back(L"--mk9_net_room_listed=true");
    if (!title.empty()) options.push_back(L"--mk9_net_room_title=" + Wide(title));
  }
  options.push_back(L"--user_data_root=" + profile.wstring());
  Start(options, "the room's fight");
}

void Watch(const std::string& room, const std::string& name) {
  const std::filesystem::path profile = ExeDir() / "online_session_watch";
  std::error_code ec;
  std::filesystem::remove_all(profile, ec);
  std::filesystem::create_directories(profile, ec);
  std::vector<std::wstring> options = OwnOptions({L"--user_data_root"});
  options.push_back(L"--mk9_net_room=" + Wide(room));
  options.push_back(L"--mk9_net_role=watch");
  options.push_back(L"--mk9_online_name=" + Wide(name));
  options.push_back(L"--user_data_root=" + profile.wstring());
  Start(options, "watching a room's session");
}

void Replay(const std::string& path) {
  const std::filesystem::path profile = ExeDir() / "online_session_watch";
  std::error_code ec;
  std::filesystem::remove_all(profile, ec);
  std::filesystem::create_directories(profile, ec);
  std::vector<std::wstring> options = OwnOptions({L"--user_data_root"});
  options.push_back(L"--mk9_net_replay=" + Wide(path));
  options.push_back(L"--user_data_root=" + profile.wstring());
  Start(options, "a replay");
}

void Back() {
  std::vector<std::wstring> options = OwnOptions();
  options.push_back(L"--mk9_online_show=true");  // the lobby's screens open again
  const std::string name = REXCVAR_GET(mk9_online_name);
  if (!name.empty()) options.push_back(L"--mk9_online_name=" + Wide(name));  // the same name (two tests on one PC)
  Start(options, "back from the room");
}

std::string LoadName() {
  const std::string given = REXCVAR_GET(mk9_online_name);
  if (!given.empty()) return given;
  std::ifstream in(ExeDir() / "online_name.txt", std::ios::binary);
  std::string name;
  std::getline(in, name);
  while (!name.empty() && (name.back() == '\r' || name.back() == ' ')) name.pop_back();
  return name;
}

void SaveName(const std::string& name) {
  std::ofstream out(ExeDir() / "online_name.txt", std::ios::binary | std::ios::trunc);
  out << name << "\n";
}

namespace {

std::mutex g_blocked_mutex;
std::vector<std::string> g_blocked;
bool g_blocked_loaded = false;

std::string Folded(std::string s) {
  for (char& c : s) {
    if (c >= 'A' && c <= 'Z') c = char(c - 'A' + 'a');
  }
  return s;
}

// The mutex held.
void LoadBlockedLocked() {
  if (g_blocked_loaded) return;
  g_blocked_loaded = true;
  std::ifstream in(ExeDir() / "online_blocked.txt", std::ios::binary);
  std::string line;
  while (std::getline(in, line)) {
    while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
    if (!line.empty()) g_blocked.push_back(line);
  }
}

}  // namespace

bool IsBlocked(const std::string& name) {
  if (name.empty()) return false;
  std::lock_guard<std::mutex> lock(g_blocked_mutex);
  LoadBlockedLocked();
  const std::string folded = Folded(name);
  return std::any_of(g_blocked.begin(), g_blocked.end(), [&](const std::string& b) { return Folded(b) == folded; });
}

void SetBlocked(const std::string& name, bool blocked) {
  if (name.empty()) return;
  std::lock_guard<std::mutex> lock(g_blocked_mutex);
  LoadBlockedLocked();
  const std::string folded = Folded(name);
  g_blocked.erase(std::remove_if(g_blocked.begin(), g_blocked.end(),
                                 [&](const std::string& b) { return Folded(b) == folded; }),
                  g_blocked.end());
  if (blocked) g_blocked.push_back(name);
  std::ofstream out(ExeDir() / "online_blocked.txt", std::ios::binary | std::ios::trunc);
  for (const std::string& b : g_blocked) out << b << "\n";
  REXLOG_INFO("[session] {} {}", name, blocked ? "silenced" : "no longer silenced");
}

std::vector<std::string> Blocked() {
  std::lock_guard<std::mutex> lock(g_blocked_mutex);
  LoadBlockedLocked();
  return g_blocked;
}

}  // namespace mk9::session
