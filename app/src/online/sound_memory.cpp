// MK9 online: the sound's own memory, out of rollback's reach (sound_memory.h).
//
// The arena's bookkeeping is the host's (free blocks by address and by size, best fit, neighbours merged on free):
// nothing of it lives in guest memory, and FMOD's MemPool serialises its calls anyway (one more lock here for any
// other caller).

#include "online/sound_memory.h"

#include <algorithm>
#include <atomic>
#include <cstring>
#include <map>
#include <mutex>
#include <unordered_map>

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/ppc/context.h>
#include <rex/ppc/func.h>
#include <rex/system/guest_park.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xmemory.h>

REXCVAR_DEFINE_BOOL(mk9_sound_apart, true, "MK9 Online",
                    "FMOD and XAudio2 in memory of their own, out of rollback's reach (sound_memory.h)");
REXCVAR_DEFINE_BOOL(mk9_sound_xaudio2_data, false, "MK9 Online",
                    "Test: XAudio2's static data left as it is by a rollback too (the ranges only its code refers to)");

namespace mk9::sound {
namespace {

constexpr uint32_t kArenaStart = 0x7B000000;  // the top of the stack range (the SDK's stacks start at 0x70000000)
constexpr uint32_t kArenaSize = 0x04000000;   // 64 MB: FMOD caps itself at 27 MB (its MemPool's limit, 0x1B00000)
constexpr uint32_t kPhysicalArenaSize = 0x03000000;  // 48 MB of physical memory: FMOD's XMA buffers and XAudio2's
constexpr uint32_t kPhysical = 0x00100000;    // FMOD_MEMORY_XBOX360_PHYSICAL
constexpr uint32_t kGranule = 16;
constexpr uint32_t kPhysicalAlign = 4096;     // XMA's buffers: a page

class Arena {
 public:
  // physical: the range comes from the SDK's physical heap (4 KB pages) when first needed; else the fixed virtual
  // range kArenaStart.
  explicit Arena(bool physical) : physical_(physical) {}

  uint32_t Alloc(uint32_t size, uint32_t align = 0) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!Ready()) return 0;
    if (!align) align = size >= 1024 ? 128 : kGranule;
    align = std::max(align, kGranule);
    size = std::max<uint32_t>(kGranule, (size + kGranule - 1) & ~(kGranule - 1));
    // the smallest free block that holds the size once aligned
    for (auto it = by_size_.lower_bound(size); it != by_size_.end(); ++it) {
      const uint32_t block = it->second, block_size = it->first;
      const uint32_t start = (block + align - 1) & ~(align - 1);
      if (start + size > block + block_size) continue;
      by_size_.erase(it);
      free_.erase(block);
      if (start > block) Insert(block, start - block);
      if (block + block_size > start + size) Insert(start + size, block + block_size - (start + size));
      used_[start] = size;
      stats_.in_use += size;
      stats_.peak = std::max(stats_.peak, stats_.in_use);
      ++stats_.blocks;
      ++stats_.allocs;
      return start;
    }
    ++stats_.failed;
    if (stats_.failed <= 10) {
      REXLOG_ERROR("[sound] FMOD's arena has no room for {} bytes ({} MB in use, {} blocks)", size,
                   stats_.in_use >> 20, stats_.blocks);
    }
    return 0;
  }

  // The block's size, 0 when it is not one of the arena's.
  uint32_t SizeOf(uint32_t block) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = used_.find(block);
    return it == used_.end() ? 0 : it->second;
  }

  bool Free(uint32_t block) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = used_.find(block);
    if (it == used_.end()) return false;
    uint32_t start = block, size = it->second;
    used_.erase(it);
    stats_.in_use -= size;
    --stats_.blocks;
    ++stats_.frees;
    // merged with the free neighbours
    auto next = free_.lower_bound(start);
    if (next != free_.end() && next->first == start + size) {
      size += next->second;
      Remove(next->first, next->second);
      next = free_.lower_bound(start);
    }
    if (next != free_.begin()) {
      auto prev = std::prev(next);
      if (prev->first + prev->second == start) {
        start = prev->first;
        size += prev->second;
        Remove(prev->first, prev->second);
      }
    }
    Insert(start, size);
    return true;
  }

  bool Contains(uint32_t address) const { return ready_ && address >= start_ && address - start_ < size_; }

  HeapStats Stats() {
    std::lock_guard<std::mutex> lock(mutex_);
    return stats_;
  }

 private:
  bool Ready() {
    if (ready_ || tried_) return ready_;
    tried_ = true;
    auto* memory = rex::system::kernel_memory();
    const uint32_t type = rex::memory::kMemoryAllocationReserve | rex::memory::kMemoryAllocationCommit;
    const uint32_t protect = rex::memory::kMemoryProtectRead | rex::memory::kMemoryProtectWrite;
    uint32_t start = 0, size = 0;
    if (physical_) {
      // the physical heap's top, as the kernel's physical allocations do (not the game's heap's: GMalloc's physical
      // pool keeps its books in the simulation's memory)
      size = kPhysicalArenaSize;
      auto* heap = memory ? memory->LookupHeapByType(true, 4096) : nullptr;
      if (heap && !heap->AllocRange(heap->heap_base(), heap->heap_base() + heap->heap_size() - 1, size, 0x10000, type,
                                    protect, true, &start)) {
        start = 0;
      }
    } else {
      start = kArenaStart;
      size = kArenaSize;
      auto* heap = memory ? memory->LookupHeap(kArenaStart) : nullptr;
      if (!heap || !heap->AllocFixed(kArenaStart, kArenaSize, 0x10000, type, protect)) start = 0;
    }
    if (!start) {
      REXLOG_ERROR("[sound] the sound's {} arena ({} MB) could not be committed: it stays in the game's heap",
                   physical_ ? "physical" : "virtual", size >> 20);
      return false;
    }
    start_ = start;
    size_ = size;
    Insert(start, size);
    stats_.arena_start = start;
    stats_.arena_size = size;
    ready_ = true;
    REXLOG_INFO("[sound] the sound's {} memory: an arena of its own at {:08X}-{:08X}, out of the rollback's reach",
                physical_ ? "physical" : "virtual", start, start + size);
    return true;
  }

  void Insert(uint32_t start, uint32_t size) {
    free_[start] = size;
    by_size_.emplace(size, start);
  }

  void Remove(uint32_t start, uint32_t size) {
    free_.erase(start);
    auto [lo, hi] = by_size_.equal_range(size);
    for (auto it = lo; it != hi; ++it) {
      if (it->second == start) {
        by_size_.erase(it);
        break;
      }
    }
  }

  std::mutex mutex_;
  const bool physical_;
  std::atomic<bool> ready_{false};
  bool tried_ = false;
  uint32_t start_ = 0, size_ = 0;
  std::map<uint32_t, uint32_t> free_;            // start -> size
  std::multimap<uint32_t, uint32_t> by_size_;    // size -> start
  std::unordered_map<uint32_t, uint32_t> used_;  // start -> size
  HeapStats stats_;
};

Arena g_arena(false);
Arena g_physical(true);  // FMOD's and XAudio2's physical memory (it went to GMalloc's physical pool, whose lock and
                         // lists are the simulation's: a sound thread taking it while a boundary parks the game, or a
                         // rollback restoring it under them, left the allocator's spin lock held for good)

}  // namespace

bool Apart() {
  static const bool apart = REXCVAR_GET(mk9_sound_apart);
  return apart;
}

std::vector<std::pair<uint32_t, uint32_t>> UntrackedRanges() {
  if (!Apart()) return {};
  // FMOD's .data (from its gGlobal pointer on) and .bss (from its MemPool on), each up to the first address other
  // code refers to (tools/online/module_data.py --strings "..\src\fmod" --strings "FMOD ")
  static const bool xaudio2_data = REXCVAR_GET(mk9_sound_xaudio2_data);
  std::vector<std::pair<uint32_t, uint32_t>> ranges = {{0x833A7D28, 0x833AE0BC}, {0x83462198, 0x834B67A0}};
  if (xaudio2_data) {
    // XAudio2's (test): what only its code refers to (module_data.py --code 832A0000-832F0000)
    ranges.insert(ranges.end(), {{0x833A01D8, 0x833A01DC}, {0x833C3760, 0x833C3B40}, {0x833C58B0, 0x833C5B28},
                                 {0x834C199C, 0x834C1A38}, {0x834C1F48, 0x834C2424}, {0x834CB0A8, 0x834CB0AC}});
  }
  return ranges;
}

HeapStats Stats() {
  HeapStats stats = g_arena.Stats();
  const HeapStats physical = g_physical.Stats();
  stats.physical_in_use = physical.in_use;
  stats.physical_peak = physical.peak;
  stats.physical_failed = physical.failed;
  return stats;
}

}  // namespace mk9::sound

// FMOD's memory callbacks (sound_memory.h): its virtual memory from the arena, its physical memory (XMA buffers,
// allocated by its streams' thread too) from the physical arena.
extern "C" REX_FUNC(__imp__sub_8258DBC8);
REX_HOOK_RAW(sub_8258DBC8) {
  const uint32_t size = ctx.r3.u32, type = ctx.r4.u32;
  if (!mk9::sound::Apart()) {
    __imp__sub_8258DBC8(ctx, base);
    return;
  }
  const bool physical = (type & mk9::sound::kPhysical) != 0;
  const uint32_t block = physical ? mk9::sound::g_physical.Alloc(size, mk9::sound::kPhysicalAlign)
                                  : mk9::sound::g_arena.Alloc(size);
  if (!block) {
    __imp__sub_8258DBC8(ctx, base);  // the arena is full or missing: the game's heap, as before
    return;
  }
  ctx.r3.u64 = block;
}

extern "C" REX_FUNC(__imp__sub_8258DC08);
REX_HOOK_RAW(sub_8258DC08) {
  const uint32_t block = ctx.r3.u32, size = ctx.r4.u32, type = ctx.r5.u32;
  const bool physical = (type & mk9::sound::kPhysical) != 0;
  auto& arena = physical ? mk9::sound::g_physical : mk9::sound::g_arena;
  if (!mk9::sound::Apart() || (block && !arena.Contains(block))) {
    __imp__sub_8258DC08(ctx, base);
    return;
  }
  const uint32_t old_size = block ? arena.SizeOf(block) : 0;
  const uint32_t fresh = size ? arena.Alloc(size, physical ? mk9::sound::kPhysicalAlign : 0) : 0;
  if (size && !fresh) {
    if (!block) {
      __imp__sub_8258DC08(ctx, base);  // a new block with no arena room: the game's heap, as before
    } else {
      ctx.r3.u64 = 0;  // as a failed realloc: the old block stays
    }
    return;
  }
  if (fresh && old_size) {
    std::memcpy(rex::memory::GuestPtr(base, fresh), rex::memory::GuestPtr(base, block), std::min(old_size, size));
  }
  if (block) arena.Free(block);
  ctx.r3.u64 = fresh;
}

extern "C" REX_FUNC(__imp__sub_8258DCA0);
REX_HOOK_RAW(sub_8258DCA0) {
  const uint32_t block = ctx.r3.u32, type = ctx.r4.u32;
  auto& arena = (type & mk9::sound::kPhysical) ? mk9::sound::g_physical : mk9::sound::g_arena;
  if (arena.Contains(block)) {
    arena.Free(block);
    return;
  }
  __imp__sub_8258DCA0(ctx, base);
}

// XMemAlloc (r3 size, r4 the XALLOC attributes) and XMemFree (r3 block, r4 attributes): XAudio2's virtual blocks
// (allocator id 0x97) from the arena; everything else as before.
namespace mk9::sound {
namespace {
constexpr uint32_t kXAudio2 = 0x97;
bool XAudio2Block(uint32_t attributes) { return ((attributes >> 16) & 0xFF) == kXAudio2; }
bool XAudio2Physical(uint32_t attributes) { return (attributes & 0x80000000u) != 0; }
}  // namespace
}  // namespace mk9::sound

extern "C" REX_FUNC(__imp__sub_823667D8);
REX_HOOK_RAW(sub_823667D8) {
  const uint32_t size = ctx.r3.u32, attributes = ctx.r4.u32;
  if (!mk9::sound::Apart() || !mk9::sound::XAudio2Block(attributes)) {
    __imp__sub_823667D8(ctx, base);
    return;
  }
  const uint32_t code = (attributes >> 24) & 0xF;
  const uint32_t align = code > 4 ? 1u << code : 16u;
  const bool physical = mk9::sound::XAudio2Physical(attributes);
  const uint32_t block = physical ? mk9::sound::g_physical.Alloc(size, std::max(align, mk9::sound::kPhysicalAlign))
                                  : mk9::sound::g_arena.Alloc(size, align);
  if (!block) {
    __imp__sub_823667D8(ctx, base);
    return;
  }
  if (attributes & 0x40000000u) std::memset(rex::memory::GuestPtr(base, block), 0, size);  // XALLOC_ZEROINITIALIZE
  ctx.r3.u64 = block;
}

extern "C" REX_FUNC(__imp__sub_823667E0);
REX_HOOK_RAW(sub_823667E0) {
  if (mk9::sound::Apart() && mk9::sound::g_arena.Contains(ctx.r3.u32)) {
    mk9::sound::g_arena.Free(ctx.r3.u32);
    return;
  }
  if (mk9::sound::Apart() && mk9::sound::g_physical.Contains(ctx.r3.u32)) {
    mk9::sound::g_physical.Free(ctx.r3.u32);
    return;
  }
  __imp__sub_823667E0(ctx, base);
}

// XAudio2's worker thread and the render callback the SDK's Audio Worker calls: they run on through a rollback
// boundary's park once the sound is apart (the exemption stays with the thread).
extern "C" REX_FUNC(__imp__sub_832C92D0);
REX_HOOK_RAW(sub_832C92D0) {
  if (mk9::sound::Apart()) rex::system::park::SetExempt(true);
  __imp__sub_832C92D0(ctx, base);
}
extern "C" REX_FUNC(__imp__sub_832CCC30);
REX_HOOK_RAW(sub_832CCC30) {
  if (mk9::sound::Apart()) rex::system::park::SetExempt(true);
  __imp__sub_832CCC30(ctx, base);
}

// The game's callbacks FMOD calls, on its own threads too: the file system UFmodAudioDevice hands FMOD (sub_82603488:
// open, close, read, seek, and one more; they go through UE3's file and async IO code, in the game's memory). An
// exempt sound thread in one of them is the game's for the time being (rex/system/guest_park.h, GuardScope).
#define MK9_GAME_CALLBACK(name)                   extern "C" REX_FUNC(__imp__##name);             REX_HOOK_RAW(name) {                              rex::system::park::GuardScope guard;            __imp__##name(ctx, base);                     }
MK9_GAME_CALLBACK(sub_82602B30)
MK9_GAME_CALLBACK(sub_82602CA8)
MK9_GAME_CALLBACK(sub_82602D00)
MK9_GAME_CALLBACK(sub_82602D88)
MK9_GAME_CALLBACK(sub_82602E00)
// (and the events' callback, sub_82603360, with the same guard in sound_sync.cpp: it waits there in lockstep)
#undef MK9_GAME_CALLBACK

// FMOD::Thread's loop (its mixer, stream, file, non-blocking and geometry threads): they run on through a rollback
// boundary's park.
extern "C" REX_FUNC(__imp__sub_82DEB0B8);
REX_HOOK_RAW(sub_82DEB0B8) {
  {
    // (diagnostics) the thread's work: its callback (+288, with +272 its argument) or its vtable's first function
    const uint32_t self = ctx.r3.u32;
    auto load = [&](uint32_t a) {
      const uint8_t* p = base + a;
      return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3];
    };
    const uint32_t vtable = load(self);
    REXLOG_INFO("[sound] FMOD thread {:08X}: callback {:08X} (arg {:08X}), vtable {:08X} -> {:08X}, semaphore {:08X}, "
                "sleep {} ms", self, load(self + 288), load(self + 272), vtable, vtable ? load(vtable) : 0,
                load(self + 280), load(self + 292));
  }
  const bool apart = mk9::sound::Apart();
  if (apart) rex::system::park::SetExempt(true);
  __imp__sub_82DEB0B8(ctx, base);
  if (apart) rex::system::park::SetExempt(false);
}
