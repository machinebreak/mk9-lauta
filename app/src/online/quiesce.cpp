// MK9 online: the rollback boundary at rest (quiesce.h).
//
//   mk9_settle_park_us=<us>   how long the other threads get to come to rest at a rollback boundary (8000)
//   mk9_settle_drain_ms=<ms>  how long the engine's background work gets to finish there (50)
//   mk9_settle_log=<n>        the first n boundaries log every thread's place of rest (diagnostics)

#include "online/quiesce.h"

#include <algorithm>
#include <chrono>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <fmt/format.h>
#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/ppc/context.h>
#include <rex/ppc/func.h>
#include <rex/system/guest_park.h>
#include <rex/system/util/object_table.h>

#include "online/snapshot.h"

REXCVAR_DEFINE_INT32(mk9_settle_park_us, 8000, "MK9 Online",
                     "Rollback boundary: how long the other threads get to come to rest, microseconds");
REXCVAR_DEFINE_INT32(mk9_settle_drain_ms, 50, "MK9 Online",
                     "Rollback boundary: how long the engine's background work gets to finish, milliseconds");
REXCVAR_DEFINE_INT32(mk9_settle_log, 0, "MK9 Online",
                     "The first this many rollback boundaries log every thread's place of rest (diagnostics)");

extern "C" REX_FUNC(__imp__sub_830AACC0);  // NxSceneQuery::finish(bool block) (PhysX, SceneQuery.cpp)

namespace mk9::quiesce {
namespace {

constexpr uint32_t kThreadPool = 0x8341FC5C;        // GThreadPool: UE3's FQueuedThreadPoolWin*
constexpr uint32_t kSceneQueryVtable = 0x82275948;  // PhysX's NxSceneQuery implementation
constexpr size_t kMaxQueries = 256;

using Clock = std::chrono::steady_clock;

uint32_t Load32(const uint8_t* base, uint32_t a) {
  const uint8_t* p = base + a;
  return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3];
}

bool Guest(uint32_t a) { return a >= 0x40000000u && a < 0xA0000000u && (a & 3) == 0; }

double MsSince(Clock::time_point t) { return std::chrono::duration<double, std::milli>(Clock::now() - t).count(); }

// A thread pool: FQueuedThreadPoolWin, +0 QueuedWork (TArray: data, num, max), +12 QueuedThreads (TArray: its idle
// threads), +24 its critical section; an FQueuedThreadWin's +16 is the work it was given (0: none).
struct Pool {
  uint32_t address = 0;
  std::vector<uint32_t> threads;  // its threads seen handing back work
  uint32_t most_idle = 0;         // the most its idle list ever held
};

std::mutex g_mutex;  // the hooks run on the pools' threads
std::vector<Pool> g_pools;
std::vector<uint32_t> g_queries;  // the scene query objects that ran a batch

bool g_parked = false;
bool g_drain_failed = false;  // this boundary's drain ran out of time: no more attempts
Stats g_stats;
int g_logged = 0;
std::string g_settle_log;  // a boundary's diagnostics, logged once it is over (no park on)

Pool* FindPool(uint32_t address) {
  for (Pool& p : g_pools) {
    if (p.address == address) return &p;
  }
  return nullptr;
}

struct PoolState {
  bool valid = false;
  uint32_t queued = 0, idle = 0;
  bool idle_clear = true;  // no thread in the idle list has work given
  bool lock_free = true;   // the pool's critical section has no owner
};

PoolState ReadPool(const uint8_t* base, uint32_t pool) {
  PoolState s;
  if (!Guest(pool)) return s;
  s.queued = Load32(base, pool + 4);
  s.idle = Load32(base, pool + 16);
  const uint32_t threads = Load32(base, pool + 12);
  if (s.queued > 0x10000 || s.idle > 64 || (s.idle && !Guest(threads))) return PoolState{};  // not a pool (yet)
  s.valid = true;
  for (uint32_t i = 0; i < s.idle; ++i) {
    const uint32_t thread = Load32(base, threads + 4 * i);
    if (Guest(thread) && Load32(base, thread + 16) != 0) s.idle_clear = false;
  }
  const uint32_t cs = Load32(base, pool + 24);
  if (Guest(cs)) s.lock_free = Load32(base, cs + 24) == 0;  // X_RTL_CRITICAL_SECTION's owning thread
  return s;
}

// Every pool's queue empty and every one of its threads back (g_mutex held).
bool PoolsIdle(const uint8_t* base, std::string* why = nullptr) {
  bool idle = true;
  for (const Pool& p : g_pools) {
    const PoolState s = ReadPool(base, p.address);
    if (!s.valid) continue;
    const uint32_t threads = std::max<uint32_t>(uint32_t(p.threads.size()), p.most_idle);
    if (s.queued == 0 && s.idle >= threads && s.idle_clear && s.lock_free) continue;
    idle = false;
    if (why) {
      *why += fmt::format(" [pool {:08X}: {} queued, {} of {} threads idle{}{}]", p.address, s.queued, s.idle, threads,
                          s.idle_clear ? "" : ", one given work", s.lock_free ? "" : ", lock held");
    }
  }
  return idle;
}

// The scene query objects with a batch pending (g_mutex held).
std::vector<uint32_t> PendingQueries(const uint8_t* base) {
  std::vector<uint32_t> pending;
  for (const uint32_t query : g_queries) {
    if (Load32(base, query) == kSceneQueryVtable && Load32(base, query + 4) != 0) pending.push_back(query);
  }
  return pending;
}

std::string Describe(const std::vector<rex::system::park::ThreadView>& threads) {
  std::string text;
  for (const auto& t : threads) {
    std::string sites;
    for (int i = 0; i < 8; ++i) {
      if (t.counts[i]) sites += fmt::format(" {:08X}x{}", t.sites[i], t.counts[i]);
    }
    text += fmt::format(" [{} {:X}{}{} sp {:08X} arg {:08X}; rests at{}]", t.name.empty() ? "?" : t.name, t.host_id,
                        t.busy ? " BUSY" : (t.idle ? "" : " MIDWAY"), t.exempt ? " exempt" : "", t.sp, t.arg,
                        sites);
  }
  return text;
}

// The pools' work and the scene query batches finished (the threads still running).
void Drain(PPCContext& ctx, uint8_t* base) {
  const auto start = Clock::now();
  const double allowed = std::max(1, REXCVAR_GET(mk9_settle_drain_ms));
  bool waited = false;
  for (;;) {
    std::string why;
    {
      std::lock_guard<std::mutex> lock(g_mutex);
      if (PoolsIdle(base, &why)) break;
    }
    if (MsSince(start) > allowed) {
      static int warned = 0;
      if (warned++ < 10) {
        REXLOG_WARN("[settle] the thread pools still busy after {:.0f} ms:{}; the boundary goes on", allowed, why);
      }
      g_drain_failed = true;
      break;
    }
    waited = true;
    std::this_thread::sleep_for(std::chrono::microseconds(50));
  }
  if (waited) ++g_stats.pool_waits;
  std::vector<uint32_t> pending;
  {
    std::lock_guard<std::mutex> lock(g_mutex);
    pending = PendingQueries(base);
  }
  for (const uint32_t query : pending) {
    const PPCContext saved = ctx;
    ctx.r3.u64 = query;
    ctx.r4.u64 = 1;  // block until the batch is done
    __imp__sub_830AACC0(ctx, base);
    ctx = saved;
    ++g_stats.collected;
  }
}

}  // namespace

void Observe(const uint8_t* base, uint64_t game_frame) {
  std::lock_guard<std::mutex> lock(g_mutex);
  // GThreadPool counts from the start, before any of its threads has handed work back
  const uint32_t global = Load32(base, kThreadPool);
  if (Guest(global) && !FindPool(global) && ReadPool(base, global).valid) {
    g_pools.push_back({global, {}, 0});
    REXLOG_INFO("[settle] thread pool {:08X} (GThreadPool)", global);
  }
  for (Pool& p : g_pools) {
    const PoolState s = ReadPool(base, p.address);
    if (s.valid) p.most_idle = std::max(p.most_idle, s.idle);
  }
}

bool Settle(PPCContext& ctx, uint8_t* base) {
  namespace park = rex::system::park;
  const auto start = Clock::now();
  snapshot::FlushRendering(ctx, base);
  g_stats.ms_flush += MsSince(start);
  const auto park_for = std::chrono::microseconds(std::max(100, REXCVAR_GET(mk9_settle_park_us)));
  g_parked = false;
  g_drain_failed = false;
  for (int attempt = 0; attempt < 4; ++attempt) {
    const auto drain_start = Clock::now();
    Drain(ctx, base);
    const uint64_t objects = snapshot::RefreshKernelObjectHoles();  // takes the global lock: before the park
    g_stats.ms_drain += MsSince(drain_start);
    std::vector<park::ThreadView> busy;
    const auto park_start = Clock::now();
    const bool parked = park::Begin(park_for, &busy);
    g_stats.ms_park += MsSince(park_start);
    if (!parked) {
      ++g_stats.unparked;
      if (g_stats.unparked <= 10 || g_stats.unparked % 600 == 0) {
        REXLOG_WARN("[settle] {} thread(s) not at rest at their idle place after {} us (boundary {}, unparked {} "
                    "times):{}", busy.size(),
                    park_for.count(), g_stats.settles, g_stats.unparked, Describe(busy));
      }
      break;
    }
    // parked: nobody can start work now; whatever started since the drain is finished first
    bool rest = rex::system::util::ObjectTable::Mutations() == objects;
    if (rest && !g_drain_failed) {
      std::lock_guard<std::mutex> lock(g_mutex);
      rest = PoolsIdle(base) && PendingQueries(base).empty();
    }
    if (rest) {
      g_parked = true;
      break;
    }
    park::End();
  }
  ++g_stats.settles;
  const double ms = MsSince(start);
  g_stats.ms_total += ms;
  g_stats.ms_max = std::max(g_stats.ms_max, ms);
  if (g_logged < REXCVAR_GET(mk9_settle_log)) {
    ++g_logged;
    g_settle_log = fmt::format("[settle] boundary {} ({}): {:.2f} ms;{}", g_stats.settles - 1,
                               g_parked ? "parked" : "NOT parked", ms, Describe(park::Threads()));
  }
  return g_parked;
}

void Release() {
  if (g_parked) rex::system::park::End();
  g_parked = false;
  if (!g_settle_log.empty()) {
    REXLOG_INFO("{}", g_settle_log);
    g_settle_log.clear();
  }
}

Stats Totals() { return g_stats; }

void NotePoolThread(uint32_t pool, uint32_t thread) {
  std::lock_guard<std::mutex> lock(g_mutex);
  Pool* p = FindPool(pool);
  if (!p) {
    if (g_pools.size() >= 16) return;
    g_pools.push_back({pool, {}, 0});
    p = &g_pools.back();
    REXLOG_INFO("[settle] thread pool {:08X}", pool);
  }
  if (std::find(p->threads.begin(), p->threads.end(), thread) == p->threads.end() && p->threads.size() < 64) {
    p->threads.push_back(thread);
  }
}

void NoteSceneQuery(const uint8_t* base, uint32_t query) {
  std::lock_guard<std::mutex> lock(g_mutex);
  if (std::find(g_queries.begin(), g_queries.end(), query) != g_queries.end()) return;
  // The game makes new query objects as fights and rounds go (and frees the old ones): one that is no longer a scene
  // query is forgotten, and a full list gives up its oldest. A query left out would keep its batch going across a
  // boundary, and a rollback to that boundary would leave the render thread waiting for a batch PhysX already ran.
  if (g_queries.size() >= kMaxQueries) {
    g_queries.erase(std::remove_if(g_queries.begin(), g_queries.end(),
                                   [&](uint32_t q) { return Load32(base, q) != kSceneQueryVtable; }),
                    g_queries.end());
  }
  if (g_queries.size() >= kMaxQueries) g_queries.erase(g_queries.begin());
  g_queries.push_back(query);
  ++g_stats.queries_seen;
  if (g_stats.queries_seen <= 8) REXLOG_INFO("[settle] scene query {:08X} runs batches", query);
}

}  // namespace mk9::quiesce
