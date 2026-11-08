// MK9 online: the sound's own memory, out of rollback's reach (sound_memory.h).
//
// The arena's bookkeeping is the host's (free blocks by address and by size, best fit, neighbours merged on free):
// nothing of it lives in guest memory, and FMOD's MemPool serialises its calls anyway (one more lock here for any
// other caller).

#include "online/sound_memory.h"

#include <algorithm>
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

REXCVAR_DEFINE_BOOL(mk9_sound_apart, false, "MK9 Online",
                    "FMOD in memory of its own, out of rollback's reach (sound_memory.h; not finished: off)");

namespace mk9::sound {
namespace {

constexpr uint32_t kArenaStart = 0x7B000000;  // the top of the stack range (the SDK's stacks start at 0x70000000)
constexpr uint32_t kArenaSize = 0x04000000;   // 64 MB: FMOD caps itself at 27 MB (its MemPool's limit, 0x1B00000)
constexpr uint32_t kPhysical = 0x00100000;    // FMOD_MEMORY_XBOX360_PHYSICAL
constexpr uint32_t kGranule = 16;

class Arena {
 public:
  uint32_t Alloc(uint32_t size) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!Ready()) return 0;
    const uint32_t align = size >= 1024 ? 128 : kGranule;
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

  bool Contains(uint32_t address) const { return address >= kArenaStart && address < kArenaStart + kArenaSize; }

  HeapStats Stats() {
    std::lock_guard<std::mutex> lock(mutex_);
    return stats_;
  }

 private:
  bool Ready() {
    if (ready_ || tried_) return ready_;
    tried_ = true;
    auto* memory = rex::system::kernel_memory();
    auto* heap = memory ? memory->LookupHeap(kArenaStart) : nullptr;
    if (!heap || !heap->AllocFixed(kArenaStart, kArenaSize, 0x10000,
                                   rex::memory::kMemoryAllocationReserve | rex::memory::kMemoryAllocationCommit,
                                   rex::memory::kMemoryProtectRead | rex::memory::kMemoryProtectWrite)) {
      REXLOG_ERROR("[sound] FMOD's arena {:08X}-{:08X} could not be committed: FMOD stays in the game's heap",
                   kArenaStart, kArenaStart + kArenaSize);
      return false;
    }
    Insert(kArenaStart, kArenaSize);
    stats_.arena_start = kArenaStart;
    stats_.arena_size = kArenaSize;
    ready_ = true;
    REXLOG_INFO("[sound] FMOD's memory: an arena of its own at {:08X}-{:08X}, out of the rollback's ranges",
                kArenaStart, kArenaStart + kArenaSize);
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
  bool ready_ = false, tried_ = false;
  std::map<uint32_t, uint32_t> free_;            // start -> size
  std::multimap<uint32_t, uint32_t> by_size_;    // size -> start
  std::unordered_map<uint32_t, uint32_t> used_;  // start -> size
  HeapStats stats_;
};

Arena g_arena;

}  // namespace

bool Apart() {
  static const bool apart = REXCVAR_GET(mk9_sound_apart);
  return apart;
}

std::vector<std::pair<uint32_t, uint32_t>> UntrackedRanges() {
  if (!Apart()) return {};
  // FMOD's .data (from its gGlobal pointer on) and .bss (from its MemPool on), each up to the first address other
  // code refers to (tools/online/module_data.py --strings "..\src\fmod" --strings "FMOD ")
  return {{0x833A7D28, 0x833AE0BC}, {0x83462198, 0x834B67A0}};
}

HeapStats Stats() { return g_arena.Stats(); }

}  // namespace mk9::sound

// FMOD's memory callbacks (sound_memory.h): the arena for its virtual memory, GMalloc's physical memory as before.
extern "C" REX_FUNC(__imp__sub_8258DBC8);
REX_HOOK_RAW(sub_8258DBC8) {
  const uint32_t size = ctx.r3.u32, type = ctx.r4.u32;
  if (!mk9::sound::Apart() || (type & mk9::sound::kPhysical)) {
    __imp__sub_8258DBC8(ctx, base);
    return;
  }
  const uint32_t block = mk9::sound::g_arena.Alloc(size);
  if (!block) {
    __imp__sub_8258DBC8(ctx, base);  // the arena is full or missing: the game's heap, as before
    return;
  }
  ctx.r3.u64 = block;
}

extern "C" REX_FUNC(__imp__sub_8258DC08);
REX_HOOK_RAW(sub_8258DC08) {
  const uint32_t block = ctx.r3.u32, size = ctx.r4.u32, type = ctx.r5.u32;
  auto& arena = mk9::sound::g_arena;
  if (!mk9::sound::Apart() || (type & mk9::sound::kPhysical) || (block && !arena.Contains(block))) {
    __imp__sub_8258DC08(ctx, base);
    return;
  }
  const uint32_t old_size = block ? arena.SizeOf(block) : 0;
  const uint32_t fresh = size ? arena.Alloc(size) : 0;
  if (size && !fresh) {
    if (!block) {
      __imp__sub_8258DC08(ctx, base);  // a new block with no arena room: the game's heap, as before
    } else {
      ctx.r3.u64 = 0;  // as a failed realloc: the old block stays
    }
    return;
  }
  if (fresh && old_size) std::memcpy(base + fresh, base + block, std::min(old_size, size));
  if (block) arena.Free(block);
  ctx.r3.u64 = fresh;
}

extern "C" REX_FUNC(__imp__sub_8258DCA0);
REX_HOOK_RAW(sub_8258DCA0) {
  const uint32_t block = ctx.r3.u32, type = ctx.r4.u32;
  if (!(type & mk9::sound::kPhysical) && mk9::sound::g_arena.Contains(block)) {
    mk9::sound::g_arena.Free(block);
    return;
  }
  __imp__sub_8258DCA0(ctx, base);
}

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
