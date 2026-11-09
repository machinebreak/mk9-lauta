// MK9 online: the game's heaps in the rollback (heap_undo.h).

#include "online/heap_undo.h"

#include <atomic>
#include <deque>
#include <mutex>

#include <rex/logging.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xmemory.h>

#include "renut_engine/gpu/backend/render_thread.h"

namespace rex::memory {
// sdk/src/system/xmemory.cpp
using GuestHeapObserver = bool (*)(uint32_t op, uint32_t address, uint32_t size, uint32_t extra);
void SetGuestHeapObserver(GuestHeapObserver observer) noexcept;
}  // namespace rex::memory

namespace mk9::heap_undo {
namespace {

enum Op : uint32_t {
  kPhysicalAlloc = 1,
  kPhysicalFree = 2,
  kSystemAlloc = 3,
  kSystemFree = 4,
  kVirtualAlloc = 5,
  kVirtualFree = 6,
};

struct Record {
  int64_t frame = 0;
  uint32_t op = 0;
  uint32_t address = 0;
  uint32_t size = 0;
};

std::mutex g_mutex;
std::deque<Record> g_records;  // oldest first
std::atomic<bool> g_active{false};
std::atomic<int64_t> g_frame{0};
thread_local bool g_doing = false;  // this thread frees for real (a held free done, an allocation undone)
Stats g_stats;

// The free the title asked for, done now (the export's own work: MmFreePhysicalMemory, SystemHeapFree).
void FreeNow(const Record& r) {
  auto* memory = REX_KERNEL_MEMORY();
  if (!memory) return;
  g_doing = true;
  if (r.op == kPhysicalAlloc || r.op == kPhysicalFree) {
    if (auto* heap = memory->LookupHeap(r.address)) heap->Release(r.address);
  } else {
    memory->SystemHeapFree(r.address);
  }
  g_doing = false;
}

bool Observe(uint32_t op, uint32_t address, uint32_t size, uint32_t /*extra*/) {
  if (g_doing || !g_active.load(std::memory_order_acquire) || !address) return false;
  std::lock_guard<std::mutex> lock(g_mutex);
  if (!g_active.load(std::memory_order_relaxed)) return false;
  const int64_t frame = g_frame.load(std::memory_order_relaxed);
  switch (op) {
    case kPhysicalAlloc:
    case kSystemAlloc:
      g_records.push_back({frame, op, address, size});
      ++g_stats.allocations;
      return false;
    case kPhysicalFree:
    case kSystemFree:
      g_records.push_back({frame, op, address, 0});
      ++g_stats.frees_held;
      return true;  // held: done once no rollback can reach this frame
    case kVirtualAlloc:
    case kVirtualFree: {
      const uint64_t n = ++g_stats.virtual_ops;
      if (n <= 10) {
        REXLOG_WARN("[heap] a fight's frame {} {} virtual memory at {:08X} ({} bytes): the timeline keeps the pages' "
                    "bytes, not the heap's allocation",
                    frame, op == kVirtualAlloc ? "allocated" : "freed", address, size);
      }
      return false;
    }
    default:
      return false;
  }
}

}  // namespace

void Install() {
  static std::once_flag once;
  std::call_once(once, [] { rex::memory::SetGuestHeapObserver(&Observe); });
}

void Begin() {
  Install();
  std::lock_guard<std::mutex> lock(g_mutex);
  g_records.clear();
  g_frame.store(0, std::memory_order_relaxed);
  g_active.store(true, std::memory_order_release);
}

void SetFrame(int64_t frame) { g_frame.store(frame, std::memory_order_relaxed); }

void RollBack(int64_t to) {
  std::deque<Record> undone;
  {
    std::lock_guard<std::mutex> lock(g_mutex);
    while (!g_records.empty() && g_records.back().frame >= to) {
      undone.push_back(g_records.back());  // newest first
      g_records.pop_back();
    }
  }
  if (undone.empty()) return;
  bool drained = false;
  for (const Record& r : undone) {
    if (r.op == kPhysicalFree || r.op == kSystemFree) {
      ++g_stats.forgotten;  // the block stays the game's, as it was at frame 'to'
      continue;
    }
    // an allocation the game at frame 'to' never made: the renderer may still have work queued that reads the block
    if (!drained) {
      renut::gpu::render_thread::Drain();
      drained = true;
    }
    FreeNow(r);
    ++g_stats.undone;
  }
  static int logged = 0;
  if (logged++ < 20) {
    REXLOG_INFO("[heap] rollback to frame {}: {} of the game's heap operations undone (allocations freed, held frees "
                "forgotten)",
                to, undone.size());
  }
}

void Confirm(int64_t final_upto) {
  std::deque<Record> due;
  {
    std::lock_guard<std::mutex> lock(g_mutex);
    while (!g_records.empty() && g_records.front().frame <= final_upto) {
      if (g_records.front().op == kPhysicalFree || g_records.front().op == kSystemFree) due.push_back(g_records.front());
      g_records.pop_front();
    }
  }
  for (const Record& r : due) {
    FreeNow(r);
    ++g_stats.freed_late;
  }
}

void End() {
  std::deque<Record> due;
  {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_active.store(false, std::memory_order_release);
    for (const Record& r : g_records) {
      if (r.op == kPhysicalFree || r.op == kSystemFree) due.push_back(r);
    }
    g_records.clear();
  }
  for (const Record& r : due) {
    FreeNow(r);
    ++g_stats.freed_late;
  }
}

Stats Totals() {
  std::lock_guard<std::mutex> lock(g_mutex);
  return g_stats;
}

}  // namespace mk9::heap_undo
