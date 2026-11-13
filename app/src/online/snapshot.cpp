// MK9 online, F4: the simulation's memory over time (snapshot.h).

#include "online/snapshot.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <vector>
#include <mutex>
#include <functional>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
#include <unordered_map>

#include <rex/cvar.h>
#include <fmt/format.h>
#include <rex/logging.h>
#include <rex/ppc/context.h>
#include <rex/ppc/func.h>
#include <rex/system/kernel_state.h>
#include <rex/system/guest_park.h>
#include <rex/system/util/object_table.h>
#include <rex/system/xmemory.h>
#include <rex/system/xobject.h>

#define XXH_INLINE_ALL
#include <xxhash.h>

#include "online/fight_state.h"
#include "online/guest_memory.h"
#include "online/heap_undo.h"
#include "online/quiesce.h"
#include "online/sound_memory.h"
#include "profiler/sa2_cpu_profile_pc.h"
#include "renut_engine/gpu/backend/render_thread.h"
#include "renut_engine/gpu/sa2_renut_pacing.h"
#include "online/write_trace.h"

REXCVAR_DEFINE_STRING(mk9_rollback_test, "", "MK9 Online",
                      "F,K: snapshot the simulation at frame F's start and restore it at frame F+K's start, so the next "
                      "K frames run frames F..F+K-1 again (their checks must repeat)");
REXCVAR_DEFINE_BOOL(mk9_timeline_foreign, true, "MK9 Online",
                    "The timeline leaves out the pages other threads write while the game thread waits (test switch)");
REXCVAR_DEFINE_INT32(mk9_timeline_verify, 0, "MK9 Online",
                     "Every this many frames the timeline compares its whole shadow with memory and logs the pages "
                     "written with no card marked (0 = never; test)");
REXCVAR_DEFINE_BOOL(mk9_write_trace, false, "MK9 Online",
                    "mk9_synctest: once the timeline has learnt the pages written with no card, log who writes them "
                    "(write_trace.h; use with no rollback, P larger than the run)");
REXCVAR_DEFINE_STRING(mk9_synctest, "", "MK9 Online",
                      "F,P,K[,D]: from frame F on (0: from the next fight's start, D frames after it), keep the "
                      "simulation's memory frame by "
                      "frame and every P frames roll it back K frames; the frames that run again must give the game's "
                      "same checks (rollback's test). A package load (the fight decided) ends it. With "
                      "mk9_input_script_frames a frame run again gets its first run's scripted input");
REXCVAR_DEFINE_INT32(mk9_timeline_dirty_report, 0, "MK9 Online",
                     "Diagnostic: every this many frames, log where the pages the timeline keeps each frame are (by MB "
                     "of the guest's memory, pages a frame); 0 = off");
REXCVAR_DEFINE_BOOL(mk9_synctest_hidden, true, "MK9 Online",
                    "mk9_synctest: the frames run again are not shown and not paced, as in a session's rollback");

extern "C" REX_FUNC(__imp__sub_82747FE0);  // FlushRenderingCommands
extern std::atomic<uint64_t> g_mk9_game_frames;  // src/mk9_frame_counter.cpp

namespace mk9::snapshot {
namespace {

constexpr uint32_t kPage = 4096;
constexpr uint32_t kPages = 0x100000;  // 4 GB of 4 KB pages: also the card table's size
// The simulation's memory, in pages: the virtual heap below 0x70000000 and the image's data and heap
// 0x80000000-0x9FFFFFFF. (The low heap, 0x00000000-0x3FFFFFFF, holds the kernel's and XAM's allocations: its pages
// come and go and the kernel's clock is there.)
constexpr std::pair<uint32_t, uint32_t> kRanges[] = {{0x40000, 0x70000}, {0x80000, 0xA0000}};

bool InRanges(uint32_t page) {
  for (const auto& [lo, hi] : kRanges) {
    if (page >= lo && page < hi) return true;
  }
  return false;
}

// The kernel's objects that live in the simulation's memory: the events, semaphores, mutants and timers the game set
// up in its own structures (KeInitializeEvent and friends). The SDK keeps their state on the host side too and writes
// their headers from there (the signal state, its 'REX' stash of the handle), so a rollback never touches their bytes:
// [start, end) ranges.
std::vector<std::pair<uint32_t, uint32_t>> KernelObjectHoles() {
  std::vector<std::pair<uint32_t, uint32_t>> holes;
  auto* kernel = REX_KERNEL_STATE();
  if (!kernel) return holes;
  using Type = rex::system::XObject::Type;
  for (const auto& object : kernel->object_table()->GetAllObjects()) {
    const uint32_t guest = object->guest_object();
    if (!guest || !InRanges(guest >> 12)) continue;
    uint32_t size = 0;
    switch (object->type()) {
      case Type::Event: size = 0x10; break;      // X_KEVENT
      case Type::Semaphore: size = 0x14; break;  // X_KSEMAPHORE
      case Type::Mutant: size = 0x20; break;     // the header, the mutant list entry, the owner, the flags
      case Type::Timer: size = 0x28; break;      // X_KTIMER
      default: break;                            // threads, files, ... live in the kernel's own memory
    }
    if (size) holes.emplace_back(guest, guest + size);
  }
  return holes;
}

std::vector<std::pair<uint32_t, uint32_t>> g_holes;
uint64_t g_holes_at = ~0ull;  // the object table's mutation count g_holes are current for

// mk9-lauta (2026-11-10): a frame's page work split over a few host threads. At a boundary the game's threads are
// parked and their cores idle, and the timeline's compares and copies (~1500 pages a frame of a fight, ~2.3 ms on one
// thread; a rollback's restore ~1.5 ms) are independent page by page. The caller takes part; with few pages, or no
// spare cores, it does them alone.
class PageWorkers {
 public:
  static PageWorkers& Get() {
    static PageWorkers workers;
    return workers;
  }

  // Runs work(i) for i in [0, n), split among the workers and the caller; returns when every one is done.
  template <typename Work>
  void Run(size_t n, Work&& work) {
    if (n < kMinParallel || threads_.empty()) {
      for (size_t i = 0; i < n; ++i) work(i);
      return;
    }
    auto runner = [&work](size_t i) { work(i); };
    {
      std::lock_guard<std::mutex> lock(mutex_);
      job_ = runner;
      count_ = n;
      next_.store(0, std::memory_order_relaxed);
      pending_ = threads_.size();
      ++generation_;
    }
    wake_.notify_all();
    Drain();
    std::unique_lock<std::mutex> lock(mutex_);
    done_.wait(lock, [this] { return pending_ == 0; });
    job_ = nullptr;
  }

 private:
  static constexpr size_t kMinParallel = 96;
  static constexpr size_t kChunk = 16;

  PageWorkers() {
    const unsigned hw = std::thread::hardware_concurrency();
    const unsigned extra = hw >= 6 ? std::min(3u, hw / 2 - 1) : 0u;
    for (unsigned i = 0; i < extra; ++i) threads_.emplace_back([this] { Loop(); });
  }

  void Drain() {
    for (;;) {
      const size_t from = next_.fetch_add(kChunk, std::memory_order_relaxed);
      if (from >= count_) return;
      const size_t to = std::min(count_, from + kChunk);
      for (size_t i = from; i < to; ++i) job_(i);
    }
  }

  void Loop() {
    uint64_t seen = 0;
    for (;;) {
      {
        std::unique_lock<std::mutex> lock(mutex_);
        wake_.wait(lock, [&] { return generation_ != seen; });
        seen = generation_;
      }
      Drain();
      {
        std::lock_guard<std::mutex> lock(mutex_);
        --pending_;
      }
      done_.notify_one();
    }
  }

  std::vector<std::thread> threads_;
  std::mutex mutex_;
  std::condition_variable wake_, done_;
  std::function<void(size_t)> job_;
  size_t count_ = 0, pending_ = 0;
  uint64_t generation_ = 0;
  std::atomic<size_t> next_{0};
};

double MsSince(std::chrono::steady_clock::time_point start) {
  return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
}

// The full snapshot's part of a region: the simulation's memory only.
guest::Region Owned(guest::Region r) {
  if (r.heap >= 0xA0000000u) return {r.heap, r.start, r.start, r.protect};
  if (r.heap == 0x40000000u) {
    if (r.start >= 0x70000000u) return {r.heap, r.start, r.start, r.protect};
    r.end = std::min(r.end, 0x70000000u);
  }
  return r;
}

}  // namespace

namespace {

// A flush that takes long (the render thread stuck behind something) gets every thread's stack dumped
// (logs/pilas_<pid>_N.txt, tools/online/stacks_report.py), three times at most.
std::atomic<int64_t> g_flush_started_us{0};
std::atomic<int> g_flush_dumps{0};

int64_t NowUs() {
  return std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

void WatchFlushes() {
  static const bool started = [] {
    std::thread([] {
      int64_t dumped_for = 0;
      for (;;) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        const int64_t since = g_flush_started_us.load();
        if (since && since != dumped_for && NowUs() - since > 700000 && g_flush_dumps.load() < 3) {
          dumped_for = since;
          ++g_flush_dumps;
          REXLOG_WARN("[timeline] FlushRenderingCommands has waited {} ms: every thread's stack goes to logs/pilas_<pid>_N.txt",
                      (NowUs() - since) / 1000);
          nfsmw::perfil_pc::VolcarPilas("timeline flush stalled");
        }
      }
    }).detach();
    return true;
  }();
  (void)started;
}

}  // namespace

uint64_t RefreshKernelObjectHoles() {
  const uint64_t now = rex::system::util::ObjectTable::Mutations();
  if (now != g_holes_at) {
    g_holes = KernelObjectHoles();
    g_holes_at = now;
  }
  return now;
}

bool KernelObjectHolesCurrent() { return g_holes_at == rex::system::util::ObjectTable::Mutations(); }

void FlushRendering(PPCContext& ctx, uint8_t* base) {
  WatchFlushes();
  g_flush_started_us = NowUs();
  // Renut's gate holds the frame's GPU work until the game thread's Swap, which would wait for this flush: open it
  renut::gpu::render_thread::HoldGateOpen(true);
  const PPCContext saved = ctx;
  __imp__sub_82747FE0(ctx, base);
  ctx = saved;
  renut::gpu::render_thread::HoldGateOpen(false);
  g_flush_started_us = 0;
}

// --- Snapshot -----------------------------------------------------------------------------------------------------

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
  for (const Block& block : blocks_) std::memcpy(base + block.start, block.bytes.data(), block.bytes.size());
}

// --- Timeline -----------------------------------------------------------------------------------------------------

uint8_t* Timeline::Take() {
  if (free_.empty()) {
    constexpr size_t kChunk = 256;  // pages per allocation
    chunks_.emplace_back(new uint8_t[kChunk * kPage]);
    for (size_t i = 0; i < kChunk; ++i) free_.push_back(chunks_.back().get() + i * kPage);
  }
  uint8_t* page = free_.back();
  free_.pop_back();
  return page;
}

void Timeline::Give(uint8_t* page) {
  if (page) free_.push_back(page);
}

void Timeline::RefreshCommitted() {
  const uint64_t mutations = rex::memory::HeapMutations();
  if (mutations == mutations_ && !committed_.empty()) return;  // no heap committed or decommitted a page since
  mutations_ = mutations;
  const auto start = std::chrono::steady_clock::now();
  committed_.assign(kPages, 0);
  for (const guest::Region& region : guest::CommittedRegions(true)) {
    for (uint64_t a = region.start & ~uint64_t(kPage - 1); a < region.end; a += kPage) {
      const uint32_t page = uint32_t(a >> 12);
      if (InRanges(page)) committed_[page] = 1;
    }
  }
  for (uint32_t page = 0; page < kPages; ++page) {
    if (!committed_[page] && shadow_[page]) {
      Give(shadow_[page]);
      shadow_[page] = nullptr;
      --shadow_pages_;
    }
  }
  ++refreshes_;
  ms_refresh_ += MsSince(start);
}

// The pages written since the cards were last consumed (outside the foreign ones), their cards set back to 1.
std::vector<uint32_t> Timeline::ConsumeCards(uint8_t* base) {
  std::vector<uint32_t> dirty;
  uint8_t* cards = base;  // the card of page p: the byte at guest address p
  for (const auto& [lo, hi] : kRanges) {
    for (uint32_t p = lo; p < hi; p += 8) {
      uint64_t v;
      std::memcpy(&v, cards + p, 8);
      if (((v - 0x0101010101010101ull) & ~v & 0x8080808080808080ull) == 0) continue;  // no card zero
      for (uint32_t i = 0; i < 8; ++i) {
        if (cards[p + i] != 0) continue;
        cards[p + i] = 1;
        if (!foreign_.empty() && foreign_[p + i]) continue;
        dirty.push_back(p + i);
      }
    }
  }
  return dirty;
}

void Timeline::Start(uint8_t* base, uint64_t frame, int keep, bool calibrate) {
  const auto start = std::chrono::steady_clock::now();
  keep_ = std::max(1, keep);
  for (Undo& u : undo_) {
    for (uint8_t* page : u.bufs) Give(page);
  }
  undo_.clear();
  for (uint8_t*& page : shadow_) {
    Give(page);
    page = nullptr;
  }
  shadow_.assign(kPages, nullptr);
  foreign_.assign(kPages, 0);
  suspect_.assign(kPages, 0);
  marked_.assign(kPages, 0);
  suspects_.clear();
  recent_.clear();
  committed_.clear();
  shadow_pages_ = foreign_count_ = 0;
  ConsumeCards(base);
  RefreshCommitted();
  for (uint32_t page = 0; page < kPages; ++page) {
    if (!committed_[page]) continue;
    shadow_[page] = Take();
    ++shadow_pages_;
    std::memcpy(shadow_[page], base + (uint64_t(page) << 12), kPage);
  }
  // The pages other threads write while this one waits: the audio library's, the loaders'. Three looks, 15 ms apart.
  std::vector<uint32_t> found;
  for (int round = 0; calibrate && round < 3; ++round) {
    std::this_thread::sleep_for(std::chrono::milliseconds(15));
    for (uint32_t page : ConsumeCards(base)) {
      if (!foreign_[page]) {
        foreign_[page] = 1;
        ++foreign_count_;
        found.push_back(page);
      }
    }
  }
  std::sort(found.begin(), found.end());
  std::string ranges;
  for (size_t i = 0; i < found.size();) {
    size_t j = i;
    while (j + 1 < found.size() && found[j + 1] == found[j] + 1) ++j;
    char text[40];
    std::snprintf(text, sizeof(text), " %08X-%08X", found[i] << 12, (found[j] + 1) << 12);
    ranges += text;
    i = j + 1;
  }
  if (calibrate) REXLOG_INFO("[timeline] pages other threads write:{}", ranges);
  if (!calibrate || !REXCVAR_GET(mk9_timeline_foreign)) {
    foreign_.assign(kPages, 0);
    foreign_count_ = 0;
    if (calibrate) REXLOG_INFO("[timeline] mk9_timeline_foreign off: they stay in");
  }
  // what is not the simulation (sound_memory.h: FMOD's static data; sa2_renut_pacing.h: the D3D device's words that
  // count the GPU's progress): its whole pages are never kept, and a rollback leaves its bytes in the pages it shares
  // as they are
  untracked_ = sound::UntrackedRanges();
  for (const auto& range : sa2::renut::GpuProgressRanges()) untracked_.push_back(range);  // the GPU's progress
  untracked_pages_ = 0;
  for (const auto& [lo, hi] : untracked_) {
    for (uint32_t page = (lo + kPage - 1) >> 12; (uint64_t(page) + 1) << 12 <= hi; ++page) {
      if (!foreign_[page]) {
        foreign_[page] = 1;
        ++untracked_pages_;
      }
    }
  }
  frame_ = frame;
  started_ = true;
  REXLOG_INFO("[timeline] {} whole pages of what is not the simulation left out (the sound's static data)",
              untracked_pages_);
  REXLOG_INFO("[timeline] started at frame {}: {} pages ({:.1f} MB) shadowed, {} written by other threads left out, "
              "{:.1f} ms", frame, shadow_pages_, shadow_pages_ * 4.0 / 1024.0, foreign_count_, MsSince(start));
}

void Timeline::Advance(uint8_t* base, uint64_t frame) {
  RefreshCommitted();
  // the candidates: the pages whose cards were written; the pages written the frame before (a store in flight then
  // has landed by now: its card was consumed before); the suspects (written with no card at all)
  std::vector<uint32_t> candidates;
  std::vector<uint32_t> carded = ConsumeCards(base);
  for (const std::vector<uint32_t>* list : {&carded, &recent_, &suspects_}) {
    for (uint32_t page : *list) {
      if (marked_[page] || !committed_[page]) continue;
      marked_[page] = 1;
      candidates.push_back(page);
    }
  }
  Undo u;
  u.frame = frame_;
  // the compares and copies, page by page on the workers (PageWorkers); then the records, in order
  std::vector<uint8_t*> fresh(candidates.size());
  for (uint8_t*& f : fresh) f = Take();
  std::vector<uint8_t> changed(candidates.size(), 0);
  PageWorkers::Get().Run(candidates.size(), [&](size_t i) {
    const uint32_t page = candidates[i];
    const uint8_t* shadow = shadow_[page];
    const uint8_t* now = base + (uint64_t(page) << 12);
    if (shadow && std::memcmp(shadow, now, kPage) == 0) return;  // written with the same bytes
    std::memcpy(fresh[i], now, kPage);
    changed[i] = 1;
  });
  for (size_t i = 0; i < candidates.size(); ++i) {
    const uint32_t page = candidates[i];
    marked_[page] = 0;
    if (!changed[i]) {
      Give(fresh[i]);
      continue;
    }
    uint8_t* shadow = shadow_[page];
    if (!shadow) {  // a page committed since: its content before is zeros
      shadow = Take();
      std::memset(shadow, 0, kPage);
      ++shadow_pages_;
    }
    u.pages.push_back(page);
    u.bufs.push_back(shadow);  // the shadow's bytes (the page at the frame's start) become the undo record's
    shadow_[page] = fresh[i];
  }
  last_dirty_ = u.pages.size();
  // diagnostic (mk9_timeline_dirty_report): where the pages each frame changes are, by MB, over that many frames
  if (static const int every = REXCVAR_GET(mk9_timeline_dirty_report); every > 0) {
    static std::unordered_map<uint32_t, uint64_t> by_mb;
    static uint64_t frames = 0, pages = 0, compared = 0;
    for (uint32_t page : u.pages) ++by_mb[page >> 8];
    pages += u.pages.size();
    compared += candidates.size();
    if (++frames == uint64_t(every)) {
      std::vector<std::pair<uint32_t, uint64_t>> top(by_mb.begin(), by_mb.end());
      std::sort(top.begin(), top.end(), [](const auto& a, const auto& b) { return a.second > b.second; });
      std::string line;
      for (size_t i = 0; i < top.size() && i < 16; ++i)
        line += fmt::format(" {:08X}:{:.0f}", top[i].first << 20, double(top[i].second) / double(frames));
      REXLOG_INFO("[timeline] {} frames: {:.0f} pages changed and {:.0f} compared a frame; by MB (pages a frame):{}",
                  frames, double(pages) / double(frames), double(compared) / double(frames), line);
      by_mb.clear();
      frames = pages = compared = 0;
    }
  }
  recent_ = carded;
  undo_.push_back(std::move(u));
  while (int(undo_.size()) > keep_) {
    for (uint8_t* page : undo_.front().bufs) Give(page);
    undo_.erase(undo_.begin());
  }
  frame_ = frame;
}

bool Timeline::RollBack(uint8_t* base, uint64_t frame) {
  if (frame > frame_ || undo_.empty() || undo_.front().frame > frame) return false;
  // the kernel's objects keep their bytes of now: saved here, put back after (the list from before the park: looking
  // the objects up takes the kernel's global lock, which a parked thread may hold)
  if (!KernelObjectHolesCurrent()) {
    if (!rex::system::park::Parked()) {
      RefreshKernelObjectHoles();
    } else {
      static int warned = 0;
      if (warned++ < 10) REXLOG_WARN("[timeline] the kernel's objects changed since the park: their list is older");
    }
  }
  std::vector<std::pair<uint32_t, uint32_t>> holes = g_holes;
  holes.insert(holes.end(), untracked_.begin(), untracked_.end());
  std::vector<std::vector<uint8_t>> kept;
  kept.reserve(holes.size());
  for (const auto& [lo, hi] : holes) kept.emplace_back(base + lo, base + hi);
  // the records undone, oldest first: a page takes the bytes of the oldest record it is in (its bytes at frame's
  // start), once
  size_t first = undo_.size();
  while (first > 0 && undo_[first - 1].frame >= frame) --first;
  std::vector<uint32_t> restored;
  std::vector<const uint8_t*> sources;
  for (size_t r = first; r < undo_.size(); ++r) {
    Undo& u = undo_[r];
    for (size_t i = 0; i < u.pages.size(); ++i) {
      const uint32_t page = u.pages[i];
      uint8_t* old = u.bufs[i];
      if (marked_[page] || !committed_[page]) {  // a newer copy of a page restored already, or one decommitted since
        Give(old);
        continue;
      }
      marked_[page] = 1;
      restored.push_back(page);
      sources.push_back(old);
      Give(shadow_[page]);
      shadow_[page] = old;
    }
    u.bufs.clear();
  }
  // the bytes back, page by page on the workers (the buffers stay as the shadow: nothing else writes them now)
  PageWorkers::Get().Run(restored.size(), [&](size_t i) {
    std::memcpy(base + (uint64_t(restored[i]) << 12), sources[i], kPage);
  });
  for (uint32_t page : restored) marked_[page] = 0;
  undo_.erase(undo_.begin() + std::ptrdiff_t(first), undo_.end());
  for (size_t i = 0; i < holes.size(); ++i) {
    const auto& [lo, hi] = holes[i];
    std::memcpy(base + lo, kept[i].data(), hi - lo);
    for (uint32_t page = lo >> 12; page <= (hi - 1) >> 12; ++page) {
      if (shadow_[page]) {
        const uint32_t from = std::max(lo, page << 12), to = std::min(hi, (page + 1) << 12);
        std::memcpy(shadow_[page] + (from & (kPage - 1)), base + from, to - from);
      }
    }
  }
  holes_ = holes.size();
  last_restored_ = restored.size();
  frame_ = frame;
  recent_.clear();
  return true;
}

size_t Timeline::Verify(uint8_t* base) {
  RefreshCommitted();
  std::vector<uint32_t> changed;
  for (uint32_t page = 0; page < kPages; ++page) {
    uint8_t* shadow = shadow_[page];
    if (!shadow || !committed_[page] || (!foreign_.empty() && foreign_[page])) continue;
    const uint8_t* now = base + (uint64_t(page) << 12);
    if (std::memcmp(shadow, now, kPage) != 0) {
      // the first words that changed (big-endian), for the first pages: what the writer writes tells who it is
      static int detailed = 0;
      if (detailed < 60 && !suspect_[page]) {
        ++detailed;
        std::string words;
        int shown = 0;
        for (uint32_t o = 0; o < kPage && shown < 6; o += 4) {
          if (std::memcmp(shadow + o, now + o, 4) == 0) continue;
          char text[48];
          std::snprintf(text, sizeof(text), " +%03X %02X%02X%02X%02X->%02X%02X%02X%02X", o, shadow[o], shadow[o + 1],
                        shadow[o + 2], shadow[o + 3], now[o], now[o + 1], now[o + 2], now[o + 3]);
          words += text;
          ++shown;
        }
        REXLOG_WARN("[timeline] page {:08X} changed with a clean card:{}", page << 12, words);
      }
      changed.push_back(page);
      std::memcpy(shadow, now, kPage);
      if (!suspect_[page]) {
        suspect_[page] = 1;
        suspects_.push_back(page);
      }
    }
  }
  if (!changed.empty()) {
    std::string ranges;
    int shown = 0;
    for (size_t i = 0; i < changed.size() && shown < 24; ++shown) {
      size_t j = i;
      while (j + 1 < changed.size() && changed[j + 1] == changed[j] + 1) ++j;
      char text[40];
      std::snprintf(text, sizeof(text), " %08X-%08X", changed[i] << 12, (changed[j] + 1) << 12);
      ranges += text;
      i = j + 1;
    }
    REXLOG_WARN("[timeline] {} pages changed with clean cards (a writer that marks none):{}", changed.size(), ranges);
  }
  return changed.size();
}

// --- the tests ----------------------------------------------------------------------------------------------------

namespace {

struct SyncTest {
  bool configured = false;
  bool at_fight = false;          // F = 0: from the next fight's start
  bool stopped = false;           // a package load: no rollback any more
  uint64_t from = 0, period = 0, back = 0;
  uint64_t after_start = 1;       // F = 0: this many frames after the fight's start
  Timeline timeline;
  uint64_t sim = 0;               // the simulation's frame number (goes back with a rollback)
  uint64_t latest = 0;            // the newest simulation frame reached: the frames before it run again
  uint64_t next_rollback = 0;     // the simulation frame of the next rollback
  // the game's checks by its frame number (tag 9): the first run's hash
  std::unordered_map<uint32_t, uint64_t> first;
  uint32_t key = 0;
  uint64_t hash = 0;
  uint64_t compared = 0, differed = 0, rollbacks = 0;
  double ms_advance = 0, ms_rollback = 0;
  size_t pages = 0;
};
SyncTest g_sync;
std::atomic<int64_t> g_sync_script_frame{-1};  // SyncTestScriptFrame
uint64_t g_sync_script_base = 0;

uint32_t g_sync_last_key = 0;  // the game frame whose checks FinishCheckFrame took last (SyncTestFighters)

void FinishCheckFrame() {
  g_sync_last_key = g_sync.key;
  if (!g_sync.key) return;
  auto [it, inserted] = g_sync.first.emplace(g_sync.key, g_sync.hash);
  if (!inserted) {
    ++g_sync.compared;
    if (it->second != g_sync.hash) {
      if (++g_sync.differed <= 20) {
        REXLOG_ERROR("[synctest] DIFFERS: game frame {} ran again with other checks ({:016x}, first {:016x})",
                     g_sync.key, g_sync.hash, it->second);
      }
      it->second = g_sync.hash;
    }
  }
  g_sync.key = 0;
  g_sync.hash = 0;
}

}  // namespace

void OnCheck(uint32_t tag, uint32_t value) {
  if (!g_sync.timeline.started()) return;
  if ((tag & 0xFFFF) == 9) g_sync.key = value;
  const uint32_t pair[2] = {tag, value};
  g_sync.hash = XXH3_64bits_withSeed(pair, sizeof(pair), g_sync.hash);
}

void OnFrame(PPCContext& ctx, uint8_t* base, uint64_t frame) {
  // mk9_rollback_test
  static const std::string test = REXCVAR_GET(mk9_rollback_test);
  if (!test.empty()) {
    static uint64_t at = 0, again = 0;
    static const bool parsed = [] {
      unsigned long long f = 0, k = 0;
      if (std::sscanf(test.c_str(), "%llu,%llu", &f, &k) == 2 && f > 0 && k > 0) {
        at = f;
        again = f + k;
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
    } else if (frame == again && !snapshot.empty()) {
      const auto start = std::chrono::steady_clock::now();
      FlushRendering(ctx, base);
      snapshot.Restore(base);
      std::memset(base, 0, 0xA0000);  // every card of the simulation's memory written: Renut sees no difference
      REXLOG_INFO("[snapshot] frame {}: back to frame {} ({:.1f} MB restored in {:.1f} ms)", frame, at,
                  snapshot.bytes() / 1048576.0, MsSince(start));
    }
  }

  // mk9_synctest
  static const std::string sync = REXCVAR_GET(mk9_synctest);
  if (sync.empty()) return;
  if (!g_sync.configured) {
    g_sync.configured = true;
    unsigned long long f = 0, p = 0, k = 0, after = 1;
    if (std::sscanf(sync.c_str(), "%llu,%llu,%llu,%llu", &f, &p, &k, &after) >= 3 && p > k && k > 0) {
      g_sync.at_fight = f == 0;
      g_sync.from = f ? f : ~0ull;  // F = 0: set by SyncTestFightStart, a 4th number of frames after the start
      g_sync.after_start = after ? after : 1;
      g_sync.period = p;
      g_sync.back = k;
    } else {
      REXLOG_ERROR("[synctest] mk9_synctest wants F,P,K with P > K > 0");
    }
  }
  if (!g_sync.from) return;
  FinishCheckFrame();
  if (frame < g_sync.from) return;
  if (g_sync.stopped) {
    sa2::renut::SetFrameHidden(false);
    g_sync_script_frame.store(-1, std::memory_order_relaxed);
    return;
  }
  // the boundary at rest (quiesce.h) until this function returns
  quiesce::Settle(ctx, base);
  struct Released {
    ~Released() { quiesce::Release(); }
  } released;
  // the fighters after the frame that just ran, against that frame's first run (fight_state.h)
  if (g_sync.timeline.started() && g_sync.sim > 120) fight_state::SyncTestFighters(base, g_sync_last_key);
  if (!g_sync.timeline.started()) {
    g_sync.timeline.Start(base, 0, int(g_sync.back) + 2, false);  // as a session keeps it: every page in
    // and as a session's fight, the game's allocations and frees kept with their frame (heap_undo.h): without it a
    // rollback over them left the render thread a broken tree (sub_82A4F710) now and then, here only
    heap_undo::Begin();
    g_sync.sim = 0;
    g_sync_script_base = g_mk9_game_frames.load(std::memory_order_relaxed);
    g_sync_script_frame.store(int64_t(g_sync_script_base), std::memory_order_relaxed);
    g_sync.next_rollback = 120 + g_sync.period;  // after the 2 s that learn the pages written with no card
    return;
  }
  if (REXCVAR_GET(mk9_write_trace)) write_trace::Rearm();
  auto start = std::chrono::steady_clock::now();
  ++g_sync.sim;
  g_sync_script_frame.store(int64_t(g_sync_script_base + g_sync.sim), std::memory_order_relaxed);
  g_sync.timeline.Advance(base, g_sync.sim);
  heap_undo::SetFrame(int64_t(g_sync.sim));
  // a rollback goes back 'back' frames from here at most: the frames before that are final
  if (g_sync.sim > g_sync.back) heap_undo::Confirm(int64_t(g_sync.sim - g_sync.back) - 1);
  g_sync.ms_advance += MsSince(start);
  // the first 2 s learn the pages written with no card (each frame compared whole); mk9_timeline_verify keeps it on
  const int every = REXCVAR_GET(mk9_timeline_verify);
  if (g_sync.sim <= 120 || (every > 0 && g_sync.sim % uint64_t(every) == 0)) {
    g_sync.timeline.Verify(base);
    if (g_sync.sim == 120) {
      REXLOG_INFO("[timeline] {} pages written with no card marked: compared at every frame from now on",
                  g_sync.timeline.suspect_pages());
      if (REXCVAR_GET(mk9_write_trace)) write_trace::Watch(base, g_sync.timeline.suspects());
    }
  }
  if (g_sync.sim <= 120) return;  // no rollback while learning
  g_sync.pages += g_sync.timeline.last_dirty();
  g_sync.latest = std::max(g_sync.latest, g_sync.sim);
  if (g_sync.sim == g_sync.next_rollback) {
    start = std::chrono::steady_clock::now();
    const uint64_t to = g_sync.sim - g_sync.back;
    heap_undo::RollBack(int64_t(to));  // the allocations of the frames undone go, the frees held for them are forgotten
    if (g_sync.timeline.RollBack(base, to)) {
      g_sync.sim = to;
      heap_undo::SetFrame(int64_t(to));
      g_sync_script_frame.store(int64_t(g_sync_script_base + to), std::memory_order_relaxed);
      ++g_sync.rollbacks;
      g_sync.ms_rollback += MsSince(start);
      static uint64_t advances = 0;
      advances += g_sync.period;
      if (g_sync.rollbacks % 25 == 0) {
        REXLOG_INFO("[synctest] {} rollbacks of {} frames: {} frames compared, {} differed; a frame writes {:.0f} "
                    "pages ({:.2f} ms to keep them; the committed map rebuilt {} times, {:.1f} ms in all), a rollback "
                    "restores {} pages in {:.2f} ms ({} kernel objects kept as they were)",
                    g_sync.rollbacks, g_sync.back, g_sync.compared, g_sync.differed,
                    double(g_sync.pages) / double(advances), g_sync.ms_advance / double(advances),
                    g_sync.timeline.refreshes(), g_sync.timeline.ms_refresh(), g_sync.timeline.last_restored(),
                    g_sync.ms_rollback / double(g_sync.rollbacks), g_sync.timeline.kernel_holes());
        REXLOG_INFO("[synctest] {}", fight_state::SyncTestFighterSummary());
      }
    }
    g_sync.next_rollback += g_sync.period;  // the frames run again, then the next rollback
  }
  sa2::renut::SetFrameHidden(REXCVAR_GET(mk9_synctest_hidden) && g_sync.sim < g_sync.latest);
}

int64_t SyncTestScriptFrame() { return g_sync_script_frame.load(std::memory_order_relaxed); }

void SyncTestFightStart(uint64_t frame) {
  if (!g_sync.at_fight || g_sync.from != ~0ull) return;
  g_sync.from = frame + g_sync.after_start;
  REXLOG_INFO("[synctest] the fight starts at frame {}: the test runs from frame {}", frame, g_sync.from);
}

void SyncTestStop(const char* why) {
  if (!g_sync.timeline.started() || g_sync.stopped) return;
  g_sync.stopped = true;
  heap_undo::End();  // no rollback from here: every free held is done
  // the scripted input goes on from the frame this run is at (no frame runs again from here)
  g_mk9_game_frames.store(g_sync_script_base + g_sync.sim, std::memory_order_relaxed);
  REXLOG_INFO("[synctest] {} at simulation frame {}: the test stops ({} rollbacks of {} frames: {} frames compared, "
              "{} differed)",
              why, g_sync.sim, g_sync.rollbacks, g_sync.back, g_sync.compared, g_sync.differed);
  REXLOG_INFO("[synctest] {}", fight_state::SyncTestFighterSummary());
}

}  // namespace mk9::snapshot
