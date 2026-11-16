// rex_mod.h: the code plugins' public ABI (mk9_plugins.h). Copy it into your plugin as it is.
//
// The same ABI as the DBZ Budokai 3 HD Collection port's code plugins (its src/rex_mod.h, MIT), so one plugin can
// serve both ReXGlue ports: an accessibility mod that reads the menus out loud, for example.
//
// The game loads every .dll (Linux: .so) in the "plugins" folder beside mk9.exe, in alphabetical order, only when the
// player turns plugins on (mk9_plugins = true in mk9.toml). Export, with C linkage:
//   void rex_mod_init(const RexModHost* host);   // required: once, with the game's code loaded, before it starts
//   void rex_mod_shutdown(void);                 // optional: when the game closes normally
// host stays valid the whole session. Check api_version and struct_size before using fields added later: new fields
// are only ever appended.
#pragma once

#include <stdint.h>

#define REX_MOD_API_VERSION 1

#ifdef __cplusplus
extern "C" {
#endif

// A recompiled function of the game: void f(PPCContext& ctx, uint8_t* base).
typedef void RexModPPCFunc(void* ctx, uint8_t* base);

typedef struct RexModHost {
  uint32_t struct_size;      // the game's sizeof(RexModHost)
  uint32_t api_version;      // the game's REX_MOD_API_VERSION
  const char* port_version;  // "1.2.2" (mk9-lauta's version)
  const char* region;        // "eu": MK9 by Lauta plays the European disc only
  uint8_t* guest_base;       // the game's memory: guest address A is guest_base + A (big-endian)
  // The recompiled function at a guest address (0x82xxxxxx), or NULL if there is none. To hook every call to it,
  // detour that pointer with your own hook library: recompiled code calls other functions directly.
  RexModPPCFunc* (*get_function)(uint32_t guest_address);
  // Replaces the function table's entry for that address (an existing function only). 1: done. The table serves
  // only indirect calls (function pointers, vtables).
  int (*set_function)(uint32_t guest_address, RexModPPCFunc* func);
  // A line in the game's log (logs/mk9_*.log), with the plugin's name in front.
  void (*log)(const char* plugin, const char* message);
} RexModHost;

#ifdef __cplusplus
}
#endif
