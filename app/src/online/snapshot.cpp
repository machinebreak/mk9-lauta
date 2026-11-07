// MK9 online, F4: snapshots of the game's simulation in guest memory (snapshot.h).

#include "online/snapshot.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/ppc/context.h>
#include <rex/ppc/func.h>

#include "online/guest_memory.h"

REXCVAR_DEFINE_STRING(mk9_rollback_test, "", "MK9 Online",
                      "F,K: snapshot the simulation at frame F's start and restore it at frame F+K's start, so the next "
                      "K frames run frames F..F+K-1 again (their checks must repeat)");

extern "C" REX_FUNC(__imp__sub_82747FE0);  // FlushRenderingCommands

namespace mk9::snapshot {
namespace {

constexpr uint32_t kSystemRangeBegin = 0x70000000;  // up to the heap's end: stacks, TLS, the kernel's objects

// The part of a region the simulation owns: [start, end), or start == end for none.
guest::Region Owned(guest::Region r) {
  if (r.heap == 0xE0000000u) return {r.heap, r.start, r.start, r.protect};
  if (r.heap == 0x40000000u) {
    if (r.start >= kSystemRangeBegin) return {r.heap, r.start, r.start, r.protect};
    r.end = std::min(r.end, kSystemRangeBegin);
  }
  return r;
}

double MsSince(std::chrono::steady_clock::time_point start) {
  return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
}

}  // namespace

void FlushRendering(PPCContext& ctx, uint8_t* base) {
  const PPCContext saved = ctx;
  __imp__sub_82747FE0(ctx, base);
  ctx = saved;
}

void Snapshot::Take(const uint8_t* base) {
  blocks_.clear();
  bytes_ = 0;
  for (const guest::Region& region : guest::CommittedRegions(true)) {
    const guest::Region r = Owned(region);
    if (r.end <= r.start) continue;
    Block block;
    block.start = r.start;
    block.bytes.assign(base + r.start, base + r.end);
    bytes_ += block.bytes.size();
    blocks_.push_back(std::move(block));
  }
}

void Snapshot::Restore(uint8_t* base) const {
  for (const Block& block : blocks_) {
    std::memcpy(base + block.start, block.bytes.data(), block.bytes.size());
    // Renut's store cards: the byte at ea >> 12 in the first MB of the guest window; 0 = written
    const uint32_t first = block.start >> 12, last = uint32_t((uint64_t(block.start) + block.bytes.size() - 1) >> 12);
    std::memset(base + first, 0, size_t(last - first + 1));
  }
}

void OnFrame(PPCContext& ctx, uint8_t* base, uint64_t frame) {
  static const std::string spec = REXCVAR_GET(mk9_rollback_test);
  if (spec.empty()) return;
  static uint64_t at = 0, back = 0;
  static bool parsed = [] {
    unsigned long long f = 0, k = 0;
    if (std::sscanf(spec.c_str(), "%llu,%llu", &f, &k) == 2 && f > 0 && k > 0) {
      at = f;
      back = f + k;
    }
    return true;
  }();
  (void)parsed;
  static Snapshot snapshot;
  if (frame == at) {
    const auto start = std::chrono::steady_clock::now();
    FlushRendering(ctx, base);
    snapshot.Take(base);
    REXLOG_INFO("[snapshot] frame {}: {:.1f} MB taken in {:.1f} ms", frame, snapshot.bytes() / 1048576.0,
                MsSince(start));
  } else if (frame == back && !snapshot.empty()) {
    const auto start = std::chrono::steady_clock::now();
    FlushRendering(ctx, base);
    snapshot.Restore(base);
    REXLOG_INFO("[snapshot] frame {}: back to frame {} ({:.1f} MB restored in {:.1f} ms); the next {} frames run "
                "frames {}-{} again", frame, at, snapshot.bytes() / 1048576.0, MsSince(start), back - at, at,
                back - 1);
  }
}

}  // namespace mk9::snapshot
