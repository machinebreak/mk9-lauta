// mk9-lauta (2026-11-13): mk9_platform.h on Windows and on Linux.

#include "mk9_platform.h"

#include <cstdlib>
#include <system_error>

#if defined(_WIN32)
#include <windows.h>
#include <shellapi.h>
#else
#include <cerrno>
#include <fcntl.h>
#include <signal.h>
#include <spawn.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include <fstream>
#include <iterator>

extern char** environ;
#endif

namespace mk9::platform {

#if defined(_WIN32)

std::filesystem::path ExePath() {
  wchar_t exe[MAX_PATH] = {};
  GetModuleFileNameW(nullptr, exe, MAX_PATH);
  return std::filesystem::path(exe);
}

std::FILE* OpenFile(const std::filesystem::path& path, const char* mode) {
  const std::wstring wide_mode(mode, mode + std::char_traits<char>::length(mode));
  return _wfopen(path.c_str(), wide_mode.c_str());
}

bool ReplaceFile(const std::filesystem::path& from, const std::filesystem::path& to) {
  return MoveFileExW(from.c_str(), to.c_str(), MOVEFILE_REPLACE_EXISTING) != 0;
}

std::vector<std::string> OwnArguments() {
  std::vector<std::string> out;
  int argc = 0;
  LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
  for (int i = 1; argv && i < argc; ++i) {
    const std::wstring w = argv[i];
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), int(w.size()), nullptr, 0, nullptr, nullptr);
    std::string s(size_t(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), int(w.size()), s.data(), n, nullptr, nullptr);
    out.push_back(std::move(s));
  }
  if (argv) LocalFree(argv);
  return out;
}

namespace {
std::wstring Wide(const std::string& s) {
  if (s.empty()) return {};
  const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), nullptr, 0);
  std::wstring w(size_t(n), L'\0');
  MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), w.data(), n);
  return w;
}

// One argument as CommandLineToArgvW reads it back (quotes, and the backslashes before them).
std::wstring Quote(const std::wstring& arg) {
  if (!arg.empty() && arg.find_first_of(L" \t\"") == std::wstring::npos) return arg;
  std::wstring out = L"\"";
  size_t slashes = 0;
  for (wchar_t c : arg) {
    if (c == L'\\') {
      ++slashes;
      continue;
    }
    out.append(c == L'"' ? slashes * 2 + 1 : slashes, L'\\');
    slashes = 0;
    out += c;
  }
  out.append(slashes * 2, L'\\');
  return out + L"\"";
}
}  // namespace

bool StartDetached(const std::filesystem::path& exe, const std::vector<std::string>& args,
                   const std::filesystem::path& dir) {
  std::wstring line = Quote(exe.wstring());
  for (const std::string& a : args) line += L" " + Quote(Wide(a));
  STARTUPINFOW si{};
  si.cb = sizeof(si);
  PROCESS_INFORMATION pi{};
  if (!CreateProcessW(exe.c_str(), line.data(), nullptr, nullptr, FALSE, 0, nullptr, dir.c_str(), &si, &pi)) {
    return false;
  }
  CloseHandle(pi.hThread);
  CloseHandle(pi.hProcess);
  return true;
}

std::filesystem::path LaunchPath() { return ExePath(); }

#else  // Linux

// Read once (at the latest before main, below): /proc/self/exe follows the running file, which the updater renames to
// mk9.old before the game starts again; the new program would not be found by it then.
std::filesystem::path ExePath() {
  static const std::filesystem::path path = [] {
    std::error_code ec;
    std::filesystem::path p = std::filesystem::read_symlink("/proc/self/exe", ec);
    return ec ? std::filesystem::path() : p;
  }();
  return path;
}
const bool kExePathRead = !ExePath().empty();

std::FILE* OpenFile(const std::filesystem::path& path, const char* mode) { return std::fopen(path.c_str(), mode); }

bool ReplaceFile(const std::filesystem::path& from, const std::filesystem::path& to) {
  return std::rename(from.c_str(), to.c_str()) == 0;
}

std::vector<std::string> OwnArguments() {
  // /proc/self/cmdline: the arguments as the process got them, each ended by a NUL
  std::ifstream in("/proc/self/cmdline", std::ios::binary);
  const std::string all((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  std::vector<std::string> out;
  size_t start = 0;
  bool first = true;
  while (start < all.size()) {
    size_t end = all.find('\0', start);
    if (end == std::string::npos) end = all.size();
    if (!first) out.push_back(all.substr(start, end - start));
    first = false;
    start = end + 1;
  }
  return out;
}

bool StartDetached(const std::filesystem::path& exe, const std::vector<std::string>& args,
                   const std::filesystem::path& dir) {
  // everything the child needs, made before fork (after it, only async-signal-safe calls)
  std::vector<std::string> owned;
  owned.push_back(exe.string());
  owned.insert(owned.end(), args.begin(), args.end());
  std::vector<char*> argv;
  for (std::string& a : owned) argv.push_back(a.data());
  argv.push_back(nullptr);
  const std::string where = dir.string();
  // a pipe that closes when the new program starts (O_CLOEXEC): EOF says it started, an errno says it did not
  int report[2];
  if (pipe2(report, O_CLOEXEC) != 0) return false;
  const long max_fd = sysconf(_SC_OPEN_MAX) > 0 ? sysconf(_SC_OPEN_MAX) : 1024;
  // a grandchild: the child forks it and exits at once, so the new game is nobody's to wait for (init adopts it)
  // and it keeps running when this one ends
  const pid_t child = fork();
  if (child < 0) {
    close(report[0]);
    close(report[1]);
    return false;
  }
  if (child == 0) {
    close(report[0]);
    setsid();
    const pid_t grandchild = fork();
    if (grandchild != 0) _exit(grandchild < 0 ? 1 : 0);
    int error = 0;
    if (!where.empty() && chdir(where.c_str()) != 0) {
      error = errno;
    } else {
      // none of this game's descriptors (its sockets above all: a UDP port kept would stay taken) go to the new
      // one: every one from 3 on closes when it starts
      for (long fd = 3; fd < max_fd; ++fd) {
        if (fd != report[1]) fcntl(int(fd), F_SETFD, FD_CLOEXEC);
      }
      signal(SIGPIPE, SIG_DFL);
      execve(argv[0], argv.data(), environ);
      error = errno;
    }
    (void)!write(report[1], &error, sizeof(error));
    _exit(127);
  }
  close(report[1]);
  int status = 0;
  waitpid(child, &status, 0);
  int error = 0;
  ssize_t n;
  do {
    n = read(report[0], &error, sizeof(error));
  } while (n < 0 && errno == EINTR);
  close(report[0]);
  return WIFEXITED(status) && WEXITSTATUS(status) == 0 && n == 0;
}

std::filesystem::path LaunchPath() {
  // an AppImage runs from a mount that goes away with it: a new game must start from the AppImage's own file
  if (const char* appimage = std::getenv("APPIMAGE"); appimage && *appimage) return appimage;
  return ExePath();
}

#endif

std::filesystem::path ExeDir() { return ExePath().parent_path(); }

}  // namespace mk9::platform
