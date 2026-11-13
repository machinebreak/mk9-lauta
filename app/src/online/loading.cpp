// MK9 online: loads that end at the same frame on every machine (loading.h).

#include "online/loading.h"

#include <chrono>
#include <string>

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/ppc/context.h>
#include <rex/ppc/func.h>
#include <rex/hook.h>

#include "online/netplay.h"
#include "online/snapshot.h"

REXCVAR_DEFINE_BOOL(mk9_flush_loads, false, "MK9 Online",
                    "Finish the game's pending package loads at the start of each frame, so they end at the same frame "
                    "on every machine (on by itself in an online session)");
REXCVAR_DEFINE_BOOL(mk9_log_loads, false, "MK9 Online",
                    "Log every package the game asks to load, with its frame (test)");

extern "C" REX_FUNC(__imp__sub_8272EC30);  // UObject::FlushAsyncLoading

namespace mk9::online {
uint64_t CurrentFrame();  // mk9_capture.cpp
}

namespace mk9::loading {
namespace {

constexpr uint32_t kAsyncPackagesNum = 0x834162A4 + 4;  // GObjAsyncPackages.Num

uint64_t g_flushes = 0, g_packages = 0;
double g_ms = 0, g_longest_ms = 0;

uint32_t Load32(const uint8_t* base, uint32_t addr) {
  const uint8_t* p = base + addr;
  return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3];
}

}  // namespace

bool Enabled() { return REXCVAR_GET(mk9_flush_loads); }

// A package's name as LoadPackageAsync gets it: an FString (TCHAR* data, num with the terminator, max), one byte a
// character in MK9's build ("ui_s_legal").
std::string PackageName(const uint8_t* base, uint32_t fstring) {
  std::string out;
  if (fstring < 0x10000 || fstring >= 0xF0000000u) return out;
  const uint32_t data = Load32(base, fstring);
  const uint32_t num = Load32(base, fstring + 4);
  if (data < 0x10000 || data >= 0xF0000000u || num > 260) return out;
  for (uint32_t i = 0; i + 1 < num; ++i) {
    const uint8_t c = base[data + i];
    if (c == 0) break;
    out += (c >= 32 && c < 127) ? char(c) : '?';
  }
  return out;
}

void FlushPending(PPCContext& ctx, uint8_t* base) {
  const uint32_t pending = Load32(base, kAsyncPackagesNum);
  if (pending == 0 || pending > 100000) return;
  const auto start = std::chrono::steady_clock::now();
  const PPCContext saved = ctx;
  __imp__sub_8272EC30(ctx, base);
  ctx = saved;
  const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
  ++g_flushes;
  g_packages += pending;
  g_ms += ms;
  if (ms > g_longest_ms) g_longest_ms = ms;
  REXLOG_INFO("[loading] {} package(s) finished at the frame's start in {:.1f} ms ({} flushes, {} packages, "
              "{:.0f} ms so far; longest {:.1f} ms)", pending, ms, g_flushes, g_packages, g_ms, g_longest_ms);
}

}  // namespace mk9::loading

// UObject::LoadPackageAsync (r3 the package's name, an FString; r4 the completion callback, r5 its data, r6 a GUID
// or null): in a fight with rollback a load means the fight is decided, and the rest of the epoch goes on in lockstep
// (netplay.h, AboutToLoad); a frame that will be run again leaves the load to its corrected run.
extern "C" REX_FUNC(__imp__sub_82730F70);
REX_HOOK_RAW(sub_82730F70) {
  const std::string name = mk9::loading::PackageName(base, ctx.r3.u32);
  if (REXCVAR_GET(mk9_log_loads)) {
    REXLOG_INFO("[loading] package '{}' asked for at frame {}", name, mk9::online::CurrentFrame());
  }
  if (!mk9::netplay::AboutToLoad(name.c_str())) return;
  mk9::snapshot::SyncTestStop(("a package load (" + name + ")").c_str());
  __imp__sub_82730F70(ctx, base);
}
