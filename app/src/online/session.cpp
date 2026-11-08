// MK9 online: a room's session started anew (session.h).

#include "online/session.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
// after windows.h
#include <shellapi.h>

#include <filesystem>
#include <fstream>
#include <sstream>
#include <vector>

#include <rex/cvar.h>
#include <rex/logging.h>

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

// This game's own options, without a session's.
std::vector<std::wstring> OwnOptions() {
  int argc = 0;
  wchar_t** argv = CommandLineToArgvW(GetCommandLineW(), &argc);
  std::vector<std::wstring> out;
  for (int i = 1; argv && i < argc; ++i) {
    const std::wstring a = argv[i];
    if (a.rfind(L"--mk9_net_room", 0) == 0 || a.rfind(L"--mk9_net_role", 0) == 0 ||
        a.rfind(L"--mk9_online_name", 0) == 0 || a.find(L"online_session") != std::wstring::npos) {
      continue;
    }
    out.push_back(a);
  }
  if (argv) LocalFree(argv);
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

void Launch(const std::string& room, bool host, const std::string& name) {
  // a profile made anew: both games start from the same save (the role's own, so two games on one PC do not share it)
  const std::filesystem::path profile = ExeDir() / (host ? "online_session_host" : "online_session_guest");
  std::error_code ec;
  std::filesystem::remove_all(profile, ec);
  std::filesystem::create_directories(profile, ec);
  std::vector<std::wstring> options = OwnOptions();
  options.push_back(L"--mk9_net_room=" + Wide(room));
  options.push_back(std::wstring(L"--mk9_net_role=") + (host ? L"host" : L"guest"));
  options.push_back(L"--mk9_online_name=" + Wide(name));
  options.push_back(L"--user_data_root=" + profile.wstring());
  Start(options, "the room's fight");
}

void Back() { Start(OwnOptions(), "back from the room"); }

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

}  // namespace mk9::session
