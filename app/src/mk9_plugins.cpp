// mk9-lauta (2026-11-16, issue #8): code plugins (mk9_plugins.h).

#include "mk9_plugins.h"

#if !defined(_WIN32) && !(defined(__linux__) && !defined(__ANDROID__))
// The consoles' builds: no plugins (no dynamic libraries there).
namespace mk9::plugins {
void Load() {}
void Shutdown() {}
std::vector<std::string> Loaded() { return {}; }
}  // namespace mk9::plugins
#else

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <mutex>
#include <string>
#include <vector>

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/runtime.h>
#include <rex/system/function_dispatcher.h>

#include "mk9_platform.h"
#include "rex_mod.h"

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <dlfcn.h>
#endif

#ifndef MK9_LAUTA_VERSION
#define MK9_LAUTA_VERSION ""
#endif

REXCVAR_DEFINE_BOOL(mk9_plugins, false, "MK9",
                    "Code plugins (mk9_plugins.h, rex_mod.h): every .dll (Linux: .so) in the plugins folder beside the "
                    "executable is loaded and its rex_mod_init called before the game starts. Third-party code with "
                    "the game's own access: only plugins you trust. Read at the start")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

namespace mk9::plugins {
namespace {

using InitFn = void (*)(const RexModHost*);
using ShutdownFn = void (*)();

struct Plugin {
  std::string name;
  void* handle = nullptr;
  ShutdownFn shutdown = nullptr;
};

std::mutex g_mutex;
std::vector<Plugin> g_plugins;
RexModHost g_host{};
bool g_shut_down = false;

#if defined(_WIN32)
constexpr const char* kExtension = ".dll";

void* Open(const std::filesystem::path& path, std::string* error) {
  // its own folder first for the DLLs it needs (LOAD_WITH_ALTERED_SEARCH_PATH: the path is absolute)
  HMODULE module = LoadLibraryExW(path.wstring().c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
  if (!module) *error = "Windows error " + std::to_string(GetLastError());
  return module;
}
void* Symbol(void* handle, const char* name) {
  return reinterpret_cast<void*>(GetProcAddress(static_cast<HMODULE>(handle), name));
}
void Close(void* handle) { FreeLibrary(static_cast<HMODULE>(handle)); }
#else
constexpr const char* kExtension = ".so";

void* Open(const std::filesystem::path& path, std::string* error) {
  void* handle = dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
  if (!handle) {
    const char* why = dlerror();
    *error = why ? why : "dlopen failed";
  }
  return handle;
}
void* Symbol(void* handle, const char* name) { return dlsym(handle, name); }
void Close(void* handle) { dlclose(handle); }
#endif

rex::runtime::FunctionDispatcher* Dispatcher() {
  rex::Runtime* runtime = rex::Runtime::instance();
  return runtime ? runtime->function_dispatcher() : nullptr;
}

RexModPPCFunc* GetFunction(uint32_t guest_address) {
  rex::runtime::FunctionDispatcher* dispatcher = Dispatcher();
  return dispatcher ? reinterpret_cast<RexModPPCFunc*>(dispatcher->GetFunction(guest_address)) : nullptr;
}

int SetFunction(uint32_t guest_address, RexModPPCFunc* func) {
  rex::runtime::FunctionDispatcher* dispatcher = Dispatcher();
  // an existing function only: no new entries
  if (!dispatcher || !func || !dispatcher->GetFunction(guest_address)) return 0;
  const bool done = dispatcher->SetFunction(guest_address, reinterpret_cast<::PPCFunc*>(func));
  REXLOG_INFO("[plugins] the function at {:08X} replaced by a plugin: {}", guest_address, done ? "done" : "refused");
  return done ? 1 : 0;
}

void Log(const char* plugin, const char* message) {
  REXLOG_INFO("[plugin {}] {}", plugin ? plugin : "?", message ? message : "");
}

bool IsPlugin(const std::filesystem::directory_entry& entry) {
  std::error_code ec;
  if (!entry.is_regular_file(ec)) return false;
  std::string extension = entry.path().extension().string();
  std::transform(extension.begin(), extension.end(), extension.begin(),
                 [](unsigned char c) { return char(std::tolower(c)); });
  return extension == kExtension;
}

}  // namespace

void Load() {
  static std::once_flag once;
  std::call_once(once, [] {
    const std::filesystem::path folder = mk9::platform::ExeDir() / "plugins";
    std::vector<std::filesystem::path> files;
    std::error_code ec;
    if (std::filesystem::is_directory(folder, ec)) {
      for (const auto& entry : std::filesystem::directory_iterator(folder, ec)) {
        if (IsPlugin(entry)) files.push_back(entry.path());
      }
    }
    std::sort(files.begin(), files.end(),
              [](const auto& a, const auto& b) { return a.filename().string() < b.filename().string(); });
    if (!REXCVAR_GET(mk9_plugins)) {
      if (!files.empty()) {
        REXLOG_INFO("[plugins] {} in {} not loaded: plugins are off (mk9_plugins = false in mk9.toml)", files.size(),
                    folder.string());
      }
      return;
    }
    rex::Runtime* runtime = rex::Runtime::instance();
    g_host.struct_size = sizeof(RexModHost);
    g_host.api_version = REX_MOD_API_VERSION;
    g_host.port_version = MK9_LAUTA_VERSION;
    g_host.region = "eu";
    g_host.guest_base = runtime ? runtime->virtual_membase() : nullptr;
    g_host.get_function = &GetFunction;
    g_host.set_function = &SetFunction;
    g_host.log = &Log;
    REXLOG_INFO("[plugins] on (mk9_plugins): {} in {}", files.size(), folder.string());
    for (const std::filesystem::path& path : files) {
      const std::string name = path.filename().string();
      std::string error;
      void* handle = Open(path, &error);
      if (!handle) {
        REXLOG_WARN("[plugins] {} could not be loaded: {}", name, error);
        continue;
      }
      const auto init = reinterpret_cast<InitFn>(Symbol(handle, "rex_mod_init"));
      if (!init) {
        REXLOG_WARN("[plugins] {} does not export rex_mod_init: left out", name);
        Close(handle);
        continue;
      }
      const auto shutdown = reinterpret_cast<ShutdownFn>(Symbol(handle, "rex_mod_shutdown"));
      const uintmax_t size = std::filesystem::file_size(path, ec);
      REXLOG_INFO("[plugins] {} loaded ({} bytes){}: rex_mod_init", name, ec ? 0 : size,
                  shutdown ? ", with rex_mod_shutdown" : "");
      rex::FlushLogging();  // a plugin that crashes in its init: its name is already in the log
      {
        std::lock_guard lock(g_mutex);
        g_plugins.push_back({name, handle, shutdown});
      }
      init(&g_host);
    }
  });
}

void Shutdown() {
  std::vector<Plugin> plugins;
  {
    std::lock_guard lock(g_mutex);
    if (g_shut_down) return;
    g_shut_down = true;
    plugins = g_plugins;
  }
  for (const Plugin& plugin : plugins) {
    if (!plugin.shutdown) continue;
    REXLOG_INFO("[plugins] {}: rex_mod_shutdown", plugin.name);
    plugin.shutdown();
  }
}

std::vector<std::string> Loaded() {
  std::lock_guard lock(g_mutex);
  std::vector<std::string> names;
  for (const Plugin& plugin : g_plugins) names.push_back(plugin.name);
  return names;
}

}  // namespace mk9::plugins

#endif
