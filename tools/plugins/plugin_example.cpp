// A minimal code plugin for MK9 by Lauta (docs/mods/plugins.md): it says hello in the game's log with what the host
// gives it, reads the game's memory and looks a function up; it says goodbye when the game closes.
//
//   Windows: clang++ -shared -O1 -I app/src -o plugins/plugin_example.dll tools/plugins/plugin_example.cpp
//   Linux:   clang++ -shared -fPIC -O1 -I app/src -o plugins/plugin_example.so tools/plugins/plugin_example.cpp
//
// Then put it in the "plugins" folder beside mk9.exe and set mk9_plugins = true in mk9.toml.

#include <cstdio>

#include "rex_mod.h"

#if defined(_WIN32)
#define PLUGIN_EXPORT extern "C" __declspec(dllexport)
#else
#define PLUGIN_EXPORT extern "C" __attribute__((visibility("default")))
#endif

namespace {
const RexModHost* g_host = nullptr;
constexpr const char* kName = "plugin_example";
}  // namespace

PLUGIN_EXPORT void rex_mod_init(const RexModHost* host) {
  g_host = host;
  char line[256];
  std::snprintf(line, sizeof(line), "hello: api %u, host struct %u bytes, mk9-lauta %s, region %s",
                unsigned(host->api_version), unsigned(host->struct_size), host->port_version, host->region);
  host->log(kName, line);
  // the game's image is loaded at 0x82000000: its first bytes are the PE header's "MZ"
  const uint8_t* image = host->guest_base + 0x82000000u;
  std::snprintf(line, sizeof(line), "the image starts with %c%c", image[0], image[1]);
  host->log(kName, line);
  // a recompiled function of the game (the DLC reader's entry, for example)
  std::snprintf(line, sizeof(line), "the function at 826B2710: %s",
                host->get_function(0x826B2710u) ? "found" : "not found");
  host->log(kName, line);
}

PLUGIN_EXPORT void rex_mod_shutdown(void) {
  if (g_host) g_host->log(kName, "goodbye");
}
