// MK9 online, F4: the simulation's memory over time (snapshot.h).

#include "online/snapshot.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
#include <unordered_map>

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/ppc/context.h>
#include <rex/ppc/func.h>

#define XXH_INLINE_ALL
#include <xxhash.h>

#include "online/guest_memory.h"
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
                      "F,P,K: from frame F on, keep the simulation's memory frame by frame and every P frames roll it "
                      "back K frames; the frames that run again must give the game's same checks (rollback's test)");

extern "C" REX_FUNC(__imp__sub_82747FE0);  // FlushRenderingCommands

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

void FlushRendering(PPCContext& ctx, uint8_t* base) {
  const PPCContext saved = ctx;
  __imp__sub_82747FE0(ctx, base);
  ctx = saved;
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

void Timeline::RefreshCommitted() {
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
      shadow_[page].reset();
      --shadow_pages_;
    }
  }
  ms_refresh_ = MsSince(start);
}

uint8_t* Timeline::Shadow(uint32_t page) {
  auto& slot = shadow_[page];
  if (!slot) {
    slot.reset(new uint8_t[kPage]());
    ++shadow_pages_;
  }
  return slot.get();
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

void Timeline::Start(uint8_t* base, uint64_t frame, int keep) {
  const auto start = std::chrono::steady_clock::now();
  keep_ = std::max(1, keep);
  shadow_.clear();
  shadow_.resize(kPages);
  foreign_.assign(kPages, 0);
  suspect_.assign(kPages, 0);
  suspects_.clear();
  undo_.clear();
  shadow_pages_ = foreign_count_ = 0;
  ConsumeCards(base);
  RefreshCommitted();
  for (uint32_t page = 0; page < kPages; ++page) {
    if (committed_[page]) std::memcpy(Shadow(page), base + (uint64_t(page) << 12), kPage);
  }
  // The pages other threads write while this one waits: the audio library's, the loaders'. Three looks, 15 ms apart.
  std::vector<uint32_t> found;
  for (int round = 0; round < 3; ++round) {
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
  REXLOG_INFO("[timeline] pages other threads write:{}", ranges);
  if (!REXCVAR_GET(mk9_timeline_foreign)) {
    foreign_.assign(kPages, 0);
    REXLOG_INFO("[timeline] mk9_timeline_foreign off: they stay in");
  }
  frame_ = frame;
  started_ = true;
  REXLOG_INFO("[timeline] started at frame {}: {} pages ({:.1f} MB) shadowed, {} written by other threads left out, "
              "{:.1f} ms", frame, shadow_pages_, shadow_pages_ * 4.0 / 1024.0, foreign_count_, MsSince(start));
}

void Timeline::Advance(uint8_t* base, uint64_t frame) {
  RefreshCommitted();
  if (marked_.size() != kPages) marked_.assign(kPages, 0);
  std::vector<uint32_t> dirty;
  for (uint32_t page : ConsumeCards(base)) {
    if (committed_[page] && !marked_[page]) {
      marked_[page] = 1;
      dirty.push_back(page);
    }
  }
  // the pages written the frame before (a store in flight then has landed by now: its card was consumed before) and
  // the suspects (written with no card at all), when their bytes changed
  for (const std::vector<uint32_t>* list : {&recent_, &suspects_}) {
    for (uint32_t page : *list) {
      const uint8_t* shadow = shadow_[page].get();
      if (marked_[page] || !shadow || !committed_[page]) continue;
      if (std::memcmp(shadow, base + (uint64_t(page) << 12), kPage) != 0) {
        marked_[page] = 1;
        dirty.push_back(page);
      }
    }
  }
  for (uint32_t page : dirty) marked_[page] = 0;
  Undo u;
  u.frame = frame_;
  u.pages = dirty;
  u.bytes.resize(size_t(dirty.size()) * kPage);
  for (size_t i = 0; i < dirty.size(); ++i) {
    const uint32_t page = dirty[i];
    uint8_t* shadow = Shadow(page);  // a page committed since: its content before is zeros
    std::memcpy(u.bytes.data() + i * kPage, shadow, kPage);
    std::memcpy(shadow, base + (uint64_t(page) << 12), kPage);
  }
  last_dirty_ = dirty.size();
  recent_ = dirty;
  undo_.push_back(std::move(u));
  while (int(undo_.size()) > keep_) undo_.erase(undo_.begin());
  frame_ = frame;
}

bool Timeline::RollBack(uint8_t* base, uint64_t frame) {
  if (frame > frame_ || undo_.empty() || undo_.front().frame > frame) return false;
  while (!undo_.empty() && undo_.back().frame >= frame) {
    const Undo& u = undo_.back();
    for (size_t i = 0; i < u.pages.size(); ++i) {
      const uint32_t page = u.pages[i];
      if (!committed_[page]) continue;  // decommitted since: nothing to write back into
      const uint8_t* old = u.bytes.data() + i * kPage;
      std::memcpy(base + (uint64_t(page) << 12), old, kPage);
      std::memcpy(Shadow(page), old, kPage);
    }
    undo_.pop_back();
  }
  frame_ = frame;
  recent_.clear();
  return true;
}

size_t Timeline::Verify(uint8_t* base) {
  std::vector<uint32_t> changed;
  for (uint32_t page = 0; page < kPages; ++page) {
    uint8_t* shadow = shadow_[page].get();
    if (!shadow || !committed_[page] || (!foreign_.empty() && foreign_[page])) continue;
    const uint8_t* now = base + (uint64_t(page) << 12);
    if (std::memcmp(shadow, now, kPage) != 0) {
      // the first words that changed (big-endian), for the first pages: what the writer writes tells who it is
      static int detailed = 0;
      if (detailed < 60 && suspect_[page]) {
      } else if (detailed < 60) {
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
  uint64_t from = 0, period = 0, back = 0;
  Timeline timeline;
  uint64_t sim = 0;               // the simulation's frame number (goes back with a rollback)
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

void FinishCheckFrame() {
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
    unsigned long long f = 0, p = 0, k = 0;
    if (std::sscanf(sync.c_str(), "%llu,%llu,%llu", &f, &p, &k) == 3 && f > 0 && p > k && k > 0) {
      g_sync.from = f;
      g_sync.period = p;
      g_sync.back = k;
    } else {
      REXLOG_ERROR("[synctest] mk9_synctest wants F,P,K with P > K > 0");
    }
  }
  if (!g_sync.from) return;
  FinishCheckFrame();
  if (frame < g_sync.from) return;
  FlushRendering(ctx, base);
  if (!g_sync.timeline.started()) {
    g_sync.timeline.Start(base, 0, int(g_sync.back) + 2);
    g_sync.sim = 0;
    g_sync.next_rollback = 120 + g_sync.period;  // after the 2 s that learn the pages written with no card
    return;
  }
  if (REXCVAR_GET(mk9_write_trace)) write_trace::Rearm();
  auto start = std::chrono::steady_clock::now();
  ++g_sync.sim;
  g_sync.timeline.Advance(base, g_sync.sim);
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
  if (g_sync.sim == g_sync.next_rollback) {
    start = std::chrono::steady_clock::now();
    const uint64_t to = g_sync.sim - g_sync.back;
    if (g_sync.timeline.RollBack(base, to)) {
      g_sync.sim = to;
      ++g_sync.rollbacks;
      g_sync.ms_rollback += MsSince(start);
      static uint64_t advances = 0;
      advances += g_sync.period;
      if (g_sync.rollbacks % 25 == 0) {
        REXLOG_INFO("[synctest] {} rollbacks of {} frames: {} frames compared, {} differed; a frame writes {:.0f} "
                    "pages ({:.2f} ms to keep them, {:.2f} of them for the committed map), a rollback takes {:.2f} ms",
                    g_sync.rollbacks, g_sync.back, g_sync.compared, g_sync.differed,
                    double(g_sync.pages) / double(advances), g_sync.ms_advance / double(advances),
                    g_sync.timeline.ms_refresh(), g_sync.ms_rollback / double(g_sync.rollbacks));
      }
    }
    g_sync.next_rollback += g_sync.period;  // the frames run again, then the next rollback
  }
}

}  // namespace mk9::snapshot
