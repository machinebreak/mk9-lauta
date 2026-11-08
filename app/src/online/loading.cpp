// MK9 online: loads that end at the same frame on every machine (loading.h).

#include "online/loading.h"

#include <chrono>

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/ppc/context.h>
#include <rex/ppc/func.h>
#include <rex/hook.h>

#include "online/netplay.h"

REXCVAR_DEFINE_BOOL(mk9_flush_loads, false, "MK9 Online",
                    "Finish the game's pending package loads at the start of each frame, so they end at the same frame "
                    "on every machine (on by itself in an online session)");

extern "C" REX_FUNC(__imp__sub_8272EC30);  // UObject::FlushAsyncLoading

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
  if (!mk9::netplay::AboutToLoad()) return;
  __imp__sub_82730F70(ctx, base);
}
