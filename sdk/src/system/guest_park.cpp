// mk9-nx (2026-11-07): parking the threads that run guest code (guest_park.h).

#include <rex/system/guest_park.h>

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <mutex>
#include <thread>

#include <rex/system/thread_state.h>
#include <rex/system/xthread.h>
#include <rex/thread.h>

namespace rex::system::park {
namespace {

// One per host thread that ever rested or called guest code; never freed (a few dozen).
struct Record {
  std::atomic<int32_t> busy{0};  // > 0: running guest code
  std::atomic<bool> dead{false};
  std::atomic<bool> exempt{false};  // runs on through parks
  std::atomic<uint32_t> rests{0}, sp{0}, arg{0};
  std::atomic<uint8_t> rest_kind{0};  // 0: in no kernel wait, 1: in one at rest, 2: in one in the middle of a task
  std::atomic<int32_t> work{0};       // WorkScopes it is inside
  // where it rests (stack pointers) and how often; written by the thread itself, read by others for diagnostics
  std::atomic<uint32_t> site_sp[8] = {}, site_count[8] = {};
  uint32_t host_id = 0;
  std::string name;
};

std::mutex g_records_mutex;
std::vector<Record*> g_records;

std::atomic<bool> g_on{false};
std::atomic<Record*> g_parker{nullptr};
std::mutex g_mutex;
std::condition_variable g_cv;

struct Holder {
  Record* record = nullptr;
  ~Holder() {
    if (record) {
      record->busy.store(0);
      record->dead.store(true);
    }
  }
};
thread_local Holder t_holder;

Record* Self() {
  Record* r = t_holder.record;
  if (!r) [[unlikely]] {
    r = new Record();
    r->host_id = rex::thread::current_thread_system_id();
    {
      std::lock_guard<std::mutex> lock(g_records_mutex);
      g_records.push_back(r);
    }
    t_holder.record = r;
  }
  return r;
}

// The thread's name as its XThread has it now (the game names its threads after they start): at its first rests and
// every 4096th after, by the thread itself.
void RefreshName(Record* r, uint32_t rests) {
  if (rests > 4 && (rests & 4095) != 0) return;
  if (!XThread::IsInThread()) return;
  std::string name = XThread::GetCurrentThread()->thread_name();
  std::lock_guard<std::mutex> lock(g_records_mutex);
  r->name = std::move(name);
}

// Busy one level more; from at rest to busy, not while a park is on when parkable (the parker's own never waits; a
// thread leaving a rest that is not at its idle place runs on to it).
void TurnBusy(Record* r, bool parkable) {
  for (;;) {
    if (r->busy.fetch_add(1) + 1 != 1 || !parkable || !g_on.load() || g_parker.load() == r || r->exempt.load()) {
      return;
    }
    r->busy.fetch_sub(1);
    std::unique_lock<std::mutex> lock(g_mutex);
    g_cv.wait(lock, [] { return !g_on.load(); });
  }
}

// One more rest at sp (a place not counted yet takes the least used of the eight); whether the thread rests there at
// least a quarter as often as at its most usual place.
bool CountSite(Record* r, uint32_t sp) {
  int at = -1, least = 0;
  uint32_t most = 0;
  for (int i = 0; i < 8; ++i) {
    const uint32_t count = r->site_count[i].load(std::memory_order_relaxed);
    if (count && r->site_sp[i].load(std::memory_order_relaxed) == sp) at = i;
    most = std::max(most, count);
    if (count < r->site_count[least].load(std::memory_order_relaxed)) least = i;
  }
  if (at < 0) {
    at = least;
    r->site_sp[at].store(sp, std::memory_order_relaxed);
    r->site_count[at].store(0, std::memory_order_relaxed);
  }
  uint32_t count = r->site_count[at].load(std::memory_order_relaxed) + 1;
  if (count == 0xFFFFFFFF) {  // halve them all now and then, so the counts stay comparable
    for (int i = 0; i < 8; ++i) r->site_count[i].store(r->site_count[i].load() / 2, std::memory_order_relaxed);
    count /= 2;
  }
  r->site_count[at].store(count, std::memory_order_relaxed);
  return uint64_t(count) * 4 >= std::max(most, count);
}

// Not running guest code, and not in a wait halfway through something.
bool Idle(const Record& r) { return r.busy.load() <= 0 && r.rest_kind.load() != 2; }

void TurnRest(Record* r) { r->busy.fetch_sub(1); }

ThreadView View(const Record& r) {
  ThreadView v;
  v.host_id = r.host_id;
  v.name = r.name;
  v.busy = r.busy.load() > 0;
  v.exempt = r.exempt.load();
  v.rests = r.rests.load(std::memory_order_relaxed);
  v.sp = r.sp.load(std::memory_order_relaxed);
  v.arg = r.arg.load(std::memory_order_relaxed);
  v.idle = Idle(r);
  for (int i = 0; i < 8; ++i) {
    v.sites[i] = r.site_sp[i].load(std::memory_order_relaxed);
    v.counts[i] = r.site_count[i].load(std::memory_order_relaxed);
  }
  return v;
}

}  // namespace

RestScope::RestScope() {
  Record* r = Self();
  uint8_t kind = 1;  // with no guest context: host code waiting, nothing of the guest's halfway
  if (auto* state = runtime::ThreadState::Get()) {
    const PPCContext* ctx = state->context();
    const uint32_t sp = ctx->r1.u32;
    r->sp.store(sp, std::memory_order_relaxed);
    r->arg.store(ctx->r3.u32, std::memory_order_relaxed);
    CountSite(r, sp);
  }
  if (r->work.load(std::memory_order_relaxed) > 0) kind = 2;
  previous_ = r->rest_kind.load();
  r->rest_kind.store(kind);
  RefreshName(r, r->rests.fetch_add(1, std::memory_order_relaxed));
  TurnRest(r);
}

RestScope::~RestScope() {
  Record* r = Self();
  TurnBusy(r, r->rest_kind.load() == 1);
  r->rest_kind.store(previous_);
}

BusyScope::BusyScope() { TurnBusy(Self(), true); }

WorkScope::WorkScope() { Self()->work.fetch_add(1); }

WorkScope::~WorkScope() { Self()->work.fetch_sub(1); }

BusyScope::~BusyScope() { TurnRest(Self()); }

void SetExempt(bool exempt) { Self()->exempt.store(exempt); }

GuardScope::GuardScope() {
  Record* r = Self();
  was_exempt_ = r->exempt.load();
  if (!was_exempt_) return;
  r->exempt.store(false);
  // a park already on may be looking at this thread: it waits for the park's end at rest
  while (g_on.load() && g_parker.load() != r) {
    r->busy.fetch_sub(1);
    {
      std::unique_lock<std::mutex> lock(g_mutex);
      g_cv.wait(lock, [] { return !g_on.load(); });
    }
    r->busy.fetch_add(1);
  }
}

GuardScope::~GuardScope() {
  if (was_exempt_) Self()->exempt.store(true);
}

// mk9-lauta (2026-11-10): a stretch of a guarded (or plain) thread's work that touches none of the memory a park keeps
// (FMOD's streaming copy out of a mapped package into its own buffer, which can wait on a slow disk for tens of ms):
// the thread is exempt for it, so a boundary does not wait for it; taken back, a park on meanwhile is waited out at
// rest first, as GuardScope's start does. Declared where they are used (no header of their own).
bool Exempt() { return Self()->exempt.load(); }

void LendExempt() { Self()->exempt.store(true); }

void TakeBackExempt() {
  Record* r = Self();
  r->exempt.store(false);
  while (g_on.load() && g_parker.load() != r) {
    r->busy.fetch_sub(1);
    {
      std::unique_lock<std::mutex> lock(g_mutex);
      g_cv.wait(lock, [] { return !g_on.load(); });
    }
    r->busy.fetch_add(1);
  }
}

void ThreadExiting() {
  Record* r = Self();
  r->busy.store(0);
  r->dead.store(true);
}

bool Begin(std::chrono::microseconds timeout, std::vector<ThreadView>* busy) {
  Record* self = Self();
  {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_parker.store(self);
    g_on.store(true);
  }
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  for (int round = 0;; ++round) {
    bool all = true;
    {
      std::lock_guard<std::mutex> lock(g_records_mutex);
      for (const Record* r : g_records) {
        if (r != self && !r->dead.load() && !r->exempt.load() && !Idle(*r)) {
          all = false;
          break;
        }
      }
    }
    if (all) return true;
    if (std::chrono::steady_clock::now() >= deadline) {
      if (busy) {
        busy->clear();
        std::lock_guard<std::mutex> lock(g_records_mutex);
        for (const Record* r : g_records) {
          if (r != self && !r->dead.load() && !r->exempt.load() && !Idle(*r)) busy->push_back(View(*r));
        }
      }
      End();
      return false;
    }
    if (round < 64) {
      std::this_thread::yield();
    } else {
      std::this_thread::sleep_for(std::chrono::microseconds(50));
    }
  }
}

void End() {
  {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_on.store(false);
    g_parker.store(nullptr);
  }
  g_cv.notify_all();
}

bool Parked() { return g_on.load(); }

std::vector<ThreadView> Threads() {
  std::vector<ThreadView> views;
  std::lock_guard<std::mutex> lock(g_records_mutex);
  for (const Record* r : g_records) {
    if (!r->dead.load()) views.push_back(View(*r));
  }
  return views;
}

}  // namespace rex::system::park
