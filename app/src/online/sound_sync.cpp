// MK9 online: the sounds' callbacks at the same frame on both machines (sound_sync.h).

#include "online/sound_sync.h"

#include <algorithm>
#include <climits>
#include <cstdio>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/ppc.h>
#include <rex/system/guest_park.h>

#include "online/host_stack.h"
#include "online/netplay.h"

REXCVAR_DEFINE_BOOL(mk9_sound_sync, true, "MK9 Online",
                    "In a session the sounds' callbacks (a sync point, an end) reach the game at the same frame on both "
                    "machines, once both have had them (sound_sync.h); false: as FMOD calls them");
REXCVAR_DEFINE_INT32(mk9_test_sound_late, 0, "MK9 Online",
                     "Tests: this machine's sounds' callbacks come this many frames late (as a slower mixer would): "
                     "with mk9_sound_sync their announcement waits, without it the game gets them late")
    .range(0, 600);
REXCVAR_DEFINE_STRING(mk9_sound_log, "", "MK9 Online",
                      "Tests: write the sounds' callbacks to this file as they reach the game: \"<epoch> <frame> "
                      "<again> <type> <source> <event> <how> <key>\" (frame: the session's frame running; how: p = as "
                      "they come, d = given at both machines' word, with its key)");

REXCVAR_DEFINE_BOOL(mk9_sound_blocking, true, "MK9 Online",
                    "In a session a sound's new FMOD event instance waits for its event's data (FMOD's blocking mode) "
                    "instead of FMOD saying \"not ready\" while it loads, on each machine's own disk clock");

extern "C" REX_FUNC(__imp__sub_82E46F78);  // FMOD::Event::setCallback(event, callback, userdata)
extern "C" REX_FUNC(__imp__sub_825CA9B8);  // a sound's pool takes one more FMOD event instance
extern "C" REX_FUNC(__imp__sub_82603360);  // the game's event callback (event, type, param1, param2, source)

namespace mk9::sound_sync {
namespace {

constexpr uint32_t kGameCallback = 0x82603360;
constexpr uint32_t kForget = 4096;  // numbers this far behind the newest leave the decision (a sound long gone)
// mk9-lauta (2026-11-10), a fight's epoch: a callback goes kSlack frames after the later of the two words (the later
// word, carried by an input made ahead of its frame, almost always comes before then: no rollback for it); one only one
// machine said goes kAlone frames after that word (the two words of one callback came 2 frames apart in the median of
// a test fight, 9-17 in nine of ten, up to ~180 for a sound one mixer cut short).
constexpr int64_t kSlack = 8;
constexpr int64_t kAlone = 45;
extern "C" REX_FUNC(__imp__sub_82E46E28);  // FMOD::Event::stop(event, immediate)
// The game's sound source keeps the FMOD event it plays at +0x108 (found 2026-11-09: every hand-off's event there).
// A source's address and an event's handle differ between the two machines (the game's heap is not laid out the same
// after a barrier; FMOD's instance pools fill on each mixer's clock), but what a source plays is the game's state.
constexpr uint32_t kSourceEvent = 0x108;

uint32_t Load32(const uint8_t* base, uint32_t address) {
  const uint8_t* p = base + address;
  return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3];
}

struct Callback {
  uint32_t event = 0, type = 0, param1 = 0, param2 = 0, source = 0;
  uint64_t kept_at = 0;  // the boundary it came after (mk9_test_sound_late)
};

// A sound handed to FMOD in lockstep, by its source.
struct Source {
  uint32_t number = 0;  // 0: not numbered (its callbacks go through)
  uint32_t event = 0;   // the FMOD event it was handed with
  uint32_t sync_points = 0;
  bool ended = false;
};

// A fight's epoch: one hand-off of an FMOD event instance with the game's callback, made by a run of a frame.
struct Handoff {
  uint32_t source = 0, event = 0;
  int64_t frame = -1;          // -1: outside the frames (an epoch's start)
  uint32_t number = 0;         // 0 until its frame is final
  uint32_t sync_points = 0;
  bool ended = false;
  bool dead = false;           // the latest run of its frame did not make it
  std::vector<std::pair<uint32_t, Callback>> early;  // slot and callback, come before it was numbered
  std::string stack;           // mk9_sound_log: the host stack of the hand-off (the game's call chain)
};

// A fight's epoch: a callback of a numbered sound, by key (number << 8 | slot).
struct Kept {
  Callback cb;
  bool announced = false;  // this machine's inputs said it (once an epoch)
  int64_t due = -1;        // max(L, R): the frame it goes to the game at
};

// What the callbacks touch (they come on FMOD's thread or the game's).
std::mutex g_mutex;
Mode g_mode = Mode::kOff;
uint32_t g_epoch = 0;
bool g_deferring = false;  // lockstep (or the tests' late callbacks)
std::thread::id g_game_thread;
uint32_t g_next_number = 0;
uint64_t g_boundary = 0;
std::unordered_map<uint32_t, Source> g_sources;  // source address -> the sound it plays (lockstep)
std::unordered_map<uint32_t, uint32_t> g_numbered;  // lockstep: a sound's number -> its source (a spectator's own)
std::map<uint32_t, Callback> g_waiting;           // lockstep: number << 8 | slot (0: the end, 1-255: sync points)
std::vector<Callback> g_late;                     // mk9_test_sound_late without mk9_sound_sync: given late here
Stats g_stats;

// Lockstep: decided from the two machines' inputs only (game thread): the same on both.
std::set<uint32_t> g_local_said, g_remote_said, g_delivered;
uint32_t g_last_announced = 0;

// A fight's epoch.
std::deque<std::unique_ptr<Handoff>> g_handoffs;            // every hand-off of the epoch (and the live ones before)
std::unordered_map<uint32_t, Handoff*> g_by_event;          // an event instance's newest hand-off
std::map<int64_t, std::vector<Handoff*>> g_frame_runs;      // a frame not numbered yet: the hand-offs of its latest run
std::vector<Handoff*> g_prev_run;                           // the running frame's run before (a rollback's frame)
int64_t g_running = -1;                                     // the frame running (-1: none)
size_t g_run_order = 0;                                     // its hand-offs so far
int64_t g_numbered_upto = -1;
std::map<uint32_t, Kept> g_kept;

// A fight with rollback, the callbacks as they come (PassedFrameStart): by the frame whose run got them.
struct Passed {
  Callback cb;
  bool given = false;  // the frame's latest run has it (given as it came, or again)
};
bool g_passed_on = false;
int64_t g_passed_frame = -1;                         // the frame running
std::map<int64_t, std::vector<Passed>> g_passed;
std::unordered_map<uint32_t, int64_t> g_first_local, g_first_remote;  // key -> the frame of its first word
std::map<int64_t, std::set<uint32_t>> g_due;                           // frame -> the keys given at its start
std::set<uint32_t> g_decided;                    // the epoch's keys whose frame is set (a late word is not said)
std::unordered_map<uint32_t, Handoff*> g_by_number;  // a numbered hand-off (a callback this machine gives itself)

void Call(PPCContext& ctx, uint8_t* base, const Callback& cb) {
  rex::CallFrame frame(ctx);
  frame.ctx.r3.u64 = cb.event;
  frame.ctx.r4.u64 = cb.type;
  frame.ctx.r5.u64 = cb.param1;
  frame.ctx.r6.u64 = cb.param2;
  frame.ctx.r7.u64 = cb.source;
  __imp__sub_82603360(frame, base);
}

// mk9_sound_log: a callback that reaches the game (how: 'p' as it comes, 'd' at both machines' word).
void LogCallback(uint32_t event, uint32_t type, uint32_t source, char how, uint32_t key = 0) {
  static const std::string path = REXCVAR_GET(mk9_sound_log);
  if (path.empty()) return;
  static FILE* file = std::fopen(path.c_str(), "w");
  if (!file) return;
  uint32_t epoch = 0;
  bool again = false;
  const int64_t frame = mk9::netplay::FrameRunning(&epoch, &again);
  std::fprintf(file, "%u %lld %d %u %08X %08X %c %08X\n", epoch, static_cast<long long>(frame), again ? 1 : 0, type,
               source, event, how, key);
  std::fflush(file);
}

// mk9_sound_log: a line of the fight epoch's bookkeeping (h: a hand-off numbered; x: a callback not given, its
// source playing another event; a: this machine said a key; r: the peer said one).
void LogLine(const char* what, int64_t frame, uint32_t a, uint32_t b, uint32_t c, uint32_t d,
             const char* tail = "") {
  static const std::string path = REXCVAR_GET(mk9_sound_log);
  if (path.empty()) return;
  static FILE* file = std::fopen((path + ".frames").c_str(), "w");
  if (!file) return;
  std::fprintf(file, "%s %lld %08X %08X %08X %08X%s\n", what, static_cast<long long>(frame), a, b, c, d, tail);
  std::fflush(file);
}

// --- a fight's epoch (g_mutex held) ---

Handoff* NewHandoff(uint32_t source, uint32_t event, int64_t frame) {
  g_handoffs.push_back(std::make_unique<Handoff>());
  Handoff* h = g_handoffs.back().get();
  h->source = source;
  h->event = event;
  h->frame = frame;
  return h;
}

// A hand-off's frame is final: its number, its frame's and its place among the frame's hand-offs (so a sound one
// machine's FMOD could not start, its instances busy on that mixer's clock, changes no other sound's number), and the
// callbacks it already had.
void NumberHandoff(Handoff* h, uint32_t index) {
  h->number = (uint32_t(h->frame < 0 ? 0xFFFF : (h->frame & 0xFFFF)) << 8) | (index & 0xFF);
  g_by_number[h->number] = h;
  ++g_stats.numbered;
  LogLine("h", h->frame, h->number, h->source, h->event, uint32_t(h->early.size()), h->stack.c_str());
  for (const auto& [slot, cb] : h->early) g_kept[h->number << 8 | slot] = Kept{cb};
  h->early.clear();
}

// The running frame's run is over: what its run before made and this one did not is gone (sound_replay stops it).
void EndRun() {
  for (Handoff* h : g_prev_run) {
    if (!h) continue;
    h->dead = true;
    h->early.clear();
    if (auto it = g_by_event.find(h->event); it != g_by_event.end() && it->second == h) g_by_event.erase(it);
  }
  g_prev_run.clear();
}

// The game handed an event instance to FMOD with its callback (game thread, the frame g_running's run).
void HandOff(uint32_t source, uint32_t event) {
  if (g_running < 0) {
    // between frames (an epoch's start): the same on both machines, numbered now
    static uint32_t between = 0;
    Handoff* h = NewHandoff(source, event, -1);
    NumberHandoff(h, between++);
    g_by_event[event] = h;
    return;
  }
  ++g_run_order;
  Handoff* h = nullptr;
  for (Handoff*& p : g_prev_run) {
    if (p && p->event == event && p->source == source) {
      h = p;  // the same hand-off as in the frame's run before (sound_replay gives the run the same instance)
      p = nullptr;
      break;
    }
  }
  if (!h) {
    h = NewHandoff(source, event, g_running);
    static const bool logging = !REXCVAR_GET(mk9_sound_log).empty();
    if (logging) {
      uint64_t stack[16];
      const int depth = host_stack::Capture(stack, 16, 3);
      char buf[24];
      for (int i = 0; i < depth; ++i) {
        std::snprintf(buf, sizeof(buf), " %llx", static_cast<unsigned long long>(stack[i]));
        h->stack += buf;
      }
    }
  }
  g_frame_runs[g_running].push_back(h);
  g_by_event[event] = h;
}

void SetDue(uint32_t key, int64_t frame) {
  if (!g_decided.insert(key).second) return;
  g_due[frame].insert(key);
  if (auto it = g_kept.find(key); it != g_kept.end()) it->second.due = frame;
}

// Forgets what a final frame decided: the callbacks given (or no longer to be given) and the word about them.
void Collect(int64_t final_upto) {
  for (auto it = g_due.begin(); it != g_due.end() && it->first <= final_upto;) {
    for (uint32_t key : it->second) {
      g_kept.erase(key);
      g_first_local.erase(key);
      g_first_remote.erase(key);
    }
    it = g_due.erase(it);
  }
}

// Leaves a fight's epoch (the session ends, or a lockstep epoch follows): every callback kept goes to the game now, as
// lockstep's do when the session ends.
void LeaveFrames(std::vector<Callback>& give) {
  EndRun();
  for (auto& [key, kept] : g_kept) give.push_back(kept.cb);
  for (const auto& h : g_handoffs) {
    if (h->dead) continue;
    for (const auto& [slot, cb] : h->early) give.push_back(cb);
  }
  g_stats.flushed += give.size();
  g_kept.clear();
  g_handoffs.clear();
  g_by_event.clear();
  g_frame_runs.clear();
  g_first_local.clear();
  g_first_remote.clear();
  g_due.clear();
  g_decided.clear();
  g_by_number.clear();
  g_running = -1;
  g_numbered_upto = -1;
}

// Starts a fight's epoch. Both machines start its frames at the same game moment, but not having run the same frames
// before it (a barrier: the one that got there first waited while the other played on to it), so the sounds handed to
// FMOD before it may differ: their callbacks go to the game as they come, and what was kept goes now (as when a fight
// started before); the epoch's sounds are numbered from its own base, the same on both machines.
void EnterFrames(uint32_t epoch, std::vector<Callback>& give) {
  LeaveFrames(give);
  for (const auto& [key, cb] : g_waiting) give.push_back(cb);
  g_stats.flushed += g_waiting.size();
  g_waiting.clear();
  g_local_said.clear();
  g_remote_said.clear();
  g_delivered.clear();
  g_sources.clear();
  g_numbered.clear();
  g_next_number = (epoch & 0xFF) << 16;
}

// --- lockstep ---

// The game handed a source to FMOD with its callback (any thread): in a session on the game thread, a new number.
void Numbered(uint32_t source, uint32_t event) {
  std::lock_guard lock(g_mutex);
  if (g_mode == Mode::kFrames && std::this_thread::get_id() == g_game_thread) {
    HandOff(source, event);
    return;
  }
  Source& s = g_sources[source];
  if (s.number) {
    // the source's last sound: what it still had waiting goes (both machines start the source again at this frame)
    const uint32_t first = s.number << 8;
    for (auto it = g_waiting.lower_bound(first); it != g_waiting.end() && it->first <= (first | 0xFF);) {
      it = g_waiting.erase(it);
      ++g_stats.dropped;
    }
  }
  if (g_deferring && std::this_thread::get_id() == g_game_thread) {
    s = Source{++g_next_number, event, 0, false};
    g_numbered[s.number] = source;
    if (s.number > kForget) g_numbered.erase(s.number - kForget);
    ++g_stats.numbered;
  } else {
    s = Source{};
  }
}

// The slot of a callback of a sound (0: its end, then its sync points in order); false: not one to keep.
bool Slot(uint32_t type, uint32_t& sync_points, bool& ended, uint32_t& slot, bool& drop) {
  drop = false;
  if (type == 0) {
    if (sync_points >= 255) return false;
    slot = ++sync_points;
    return true;
  }
  if (ended) {
    drop = true;  // stolen, then finished: one end
    return true;
  }
  ended = true;
  slot = 0;
  return true;
}

// FMOD's callback (any thread): true if it waits (the game gets it later), false if it goes through now.
bool Keep(uint32_t event, uint32_t type, uint32_t param1, uint32_t param2, uint32_t source) {
  static const bool sync = REXCVAR_GET(mk9_sound_sync);
  static const int32_t late = REXCVAR_GET(mk9_test_sound_late);
  if (type != 0 && type != 3 && type != 4) return false;  // the ones that mark the source
  std::lock_guard lock(g_mutex);
  const Callback cb{event, type, param1, param2, source, g_boundary};
  if (sync && g_mode == Mode::kFrames) {
    uint32_t slot = 0;
    bool drop = false;
    auto h = g_by_event.find(event);
    if (h != g_by_event.end() && !h->second->dead && h->second->source == source) {
      Handoff& ho = *h->second;
      if (!Slot(type, ho.sync_points, ho.ended, slot, drop)) {
        ++g_stats.passed;
        LogCallback(event, type, source, 'p');
        return false;
      }
      if (drop) return true;
      if (ho.number) {
        const uint32_t key = ho.number << 8 | slot;
        if (g_decided.count(key) && !g_kept.count(key)) {
          // decided without this machine's word: given already from the hand-off, or about to be
          ++g_stats.dropped;
          return true;
        }
        g_kept[key] = Kept{cb};
      } else {
        ho.early.emplace_back(slot, cb);
      }
      ++g_stats.deferred;
      return true;
    }
    // a sound numbered in lockstep, before the fight's epoch
    auto s = g_sources.find(source);
    if (s != g_sources.end() && s->second.number && s->second.event == event) {
      if (!Slot(type, s->second.sync_points, s->second.ended, slot, drop)) {
        ++g_stats.passed;
        LogCallback(event, type, source, 'p');
        return false;
      }
      if (drop) return true;
      g_kept[s->second.number << 8 | slot] = Kept{cb};
      ++g_stats.deferred;
      return true;
    }
    ++g_stats.passed;
    LogCallback(event, type, source, 'p');
    return false;
  }
  if (!g_deferring) {
    ++g_stats.passed;
    LogCallback(event, type, source, 'p');
    // a fight with rollback: kept with the frame that got it, for a rollback's run of that frame
    if (g_passed_on && g_passed_frame >= 0) g_passed[g_passed_frame].push_back(Passed{cb, true});
    return false;
  }
  if (!sync) {
    if (late <= 0) return false;
    g_late.push_back(cb);
    return true;
  }
  auto it = g_sources.find(source);
  if (it == g_sources.end() || it->second.number == 0) {
    ++g_stats.passed;
    LogCallback(event, type, source, 'p');
    return false;
  }
  Source& s = it->second;
  if (s.event != event) {
    ++g_stats.dropped;  // an event the source played before (the game hands each source's sound to FMOD anew)
    return true;
  }
  uint32_t slot = 0;
  bool drop = false;
  if (!Slot(type, s.sync_points, s.ended, slot, drop)) {
    ++g_stats.passed;
    return false;
  }
  if (drop) return true;
  g_waiting[s.number << 8 | slot] = cb;
  ++g_stats.deferred;
  return true;
}

// Lockstep's switch (and mk9_test_sound_late's): on in a lockstep epoch.
void SetDeferringLocked(bool on, std::vector<Callback>& give) {
  static const bool sync = REXCVAR_GET(mk9_sound_sync);
  static const int32_t late = REXCVAR_GET(mk9_test_sound_late);
  if (!sync) {
    // the test's late callbacks, given by this machine alone
    for (auto it = g_late.begin(); it != g_late.end();) {
      if (!on || it->kept_at + uint64_t(late) <= g_boundary) {
        give.push_back(*it);
        it = g_late.erase(it);
      } else {
        ++it;
      }
    }
  } else if (!on && g_deferring) {
    for (const auto& [key, cb] : g_waiting) give.push_back(cb);
    g_stats.flushed += g_waiting.size();
    g_waiting.clear();
    g_local_said.clear();
    g_remote_said.clear();
    g_delivered.clear();
  }
  g_deferring = on;
}

}  // namespace

void SetMode(PPCContext& ctx, uint8_t* base, Mode mode, uint32_t epoch) {
  static const bool sync = REXCVAR_GET(mk9_sound_sync);
  if (!sync && mode == Mode::kFrames) mode = Mode::kOff;  // as FMOD calls them
  std::vector<Callback> give;
  {
    std::lock_guard lock(g_mutex);
    g_game_thread = std::this_thread::get_id();
    ++g_boundary;
    if (mode != g_mode || (mode == Mode::kFrames && epoch != g_epoch)) {
      if (mode == Mode::kFrames) {
        EnterFrames(epoch, give);
        g_deferring = false;
      } else if (g_mode == Mode::kFrames) {
        LeaveFrames(give);
      }
      g_mode = mode;
      g_epoch = epoch;
    }
    if (mode != Mode::kFrames) SetDeferringLocked(mode == Mode::kLockstep, give);
  }
  for (const Callback& cb : give) Call(ctx, base, cb);
}

uint32_t Announcement() {
  static const int32_t late = REXCVAR_GET(mk9_test_sound_late);
  std::lock_guard lock(g_mutex);
  if (!g_deferring || g_waiting.empty()) return 0;
  // round robin: the one after the last announced (a lost or unread announcement comes round again)
  auto it = g_waiting.upper_bound(g_last_announced);
  for (size_t tried = 0; tried < g_waiting.size(); ++tried) {
    if (it == g_waiting.end()) it = g_waiting.begin();
    if (it->second.kept_at + uint64_t(std::max(late, 0)) <= g_boundary) {
      g_last_announced = it->first;
      return it->first;
    }
    ++it;
  }
  return 0;
}

void Deliver(PPCContext& ctx, uint8_t* base, uint32_t local, uint32_t remote) {
  if (local) g_local_said.insert(local);
  if (remote) g_remote_said.insert(remote);
  // a sound long gone leaves the decision (both machines' numbers are the same, so both forget the same keys)
  uint32_t newest = 0;
  {
    std::lock_guard lock(g_mutex);
    newest = g_next_number;
  }
  if (newest > kForget) {
    const uint32_t below = (newest - kForget) << 8;
    for (auto* set : {&g_local_said, &g_remote_said, &g_delivered}) set->erase(set->begin(), set->lower_bound(below));
  }
  std::vector<uint32_t> agreed;
  for (uint32_t key : g_local_said) {
    if (g_remote_said.count(key) && !g_delivered.count(key)) agreed.push_back(key);
  }
  for (uint32_t key : agreed) {
    g_delivered.insert(key);
    g_local_said.erase(key);
    g_remote_said.erase(key);
    Callback cb;
    bool have = false;
    {
      std::lock_guard lock(g_mutex);
      if (auto it = g_waiting.find(key); it != g_waiting.end()) {
        cb = it->second;
        g_waiting.erase(it);
        have = true;
      }
    }
    if (!have) continue;  // its source started another sound since (dropped on both)
    Call(ctx, base, cb);
    LogCallback(cb.event, cb.type, cb.source, 'd', key);
    ++g_stats.delivered;
  }
}

void WatchDeliver(PPCContext& ctx, uint8_t* base, uint32_t p1, uint32_t p2) {
  if (p1) g_local_said.insert(p1);
  if (p2) g_remote_said.insert(p2);
  std::vector<uint32_t> agreed;
  for (uint32_t key : g_local_said) {
    if (g_remote_said.count(key) && !g_delivered.count(key)) agreed.push_back(key);
  }
  for (uint32_t key : agreed) {
    g_delivered.insert(key);
    g_local_said.erase(key);
    g_remote_said.erase(key);
    Callback cb;
    bool have = false, make = false;
    {
      std::lock_guard lock(g_mutex);
      if (auto it = g_waiting.find(key); it != g_waiting.end()) {
        cb = it->second;
        g_waiting.erase(it);
        have = true;
      } else if (auto n = g_numbered.find(key >> 8); n != g_numbered.end()) {
        // not given by this machine's FMOD (yet): from the source that plays that sound still
        auto src = g_sources.find(n->second);
        if (src != g_sources.end() && src->second.number == (key >> 8)) {
          Source& so = src->second;
          const bool end = (key & 0xFF) == 0;
          if (end) {
            so.ended = true;  // FMOD's own end, if it comes, is not the game's any more
          } else {
            so.sync_points = std::max(so.sync_points, key & 0xFF);
          }
          cb = Callback{so.event, end ? 4u : 0u, 0, 0, n->second, 0};
          have = make = true;
        }
      }
    }
    if (!have) continue;
    if (make && cb.type == 4) {
      // the end: the sound stopped first, its callback taken off (FMOD's own would come for whatever plays it next)
      rex::CallFrame off(ctx);
      off.ctx.r3.u64 = cb.event;
      off.ctx.r4.u64 = 0;
      off.ctx.r5.u64 = 0;
      __imp__sub_82E46F78(off, base);
      rex::CallFrame halt(ctx);
      halt.ctx.r3.u64 = cb.event;
      halt.ctx.r4.u64 = 1;  // immediate
      __imp__sub_82E46E28(halt, base);
    }
    Call(ctx, base, cb);
    LogCallback(cb.event, cb.type, cb.source, make ? 'm' : 'd', key);
    std::lock_guard lock(g_mutex);
    ++g_stats.delivered;
    if (make) ++g_stats.made;
  }
}

uint32_t AnnounceFor(int64_t input_frame) {
  std::lock_guard lock(g_mutex);
  if (g_mode != Mode::kFrames) return 0;
  for (auto& [key, kept] : g_kept) {
    if (kept.announced) continue;
    kept.announced = true;
    if (g_decided.count(key)) continue;  // decided without this word (only the other machine had said it)
    ++g_stats.announced;
    LogLine("a", input_frame, key, kept.cb.source, kept.cb.event, 0);
    g_first_local.emplace(key, input_frame);
    if (auto r = g_first_remote.find(key); r != g_first_remote.end()) {
      SetDue(key, std::min(std::max(input_frame, r->second), std::min(input_frame, r->second) + kAlone) + kSlack);
    }
    return key;
  }
  return 0;
}

int64_t RemoteAnnounced(uint32_t key, int64_t frame) {
  std::lock_guard lock(g_mutex);
  if (g_mode != Mode::kFrames || !key) return -1;
  if (g_decided.count(key) || !g_first_remote.emplace(key, frame).second) return -1;
  LogLine("r", frame, key, 0, 0, 0);
  auto l = g_first_local.find(key);
  if (l == g_first_local.end()) return -1;
  const int64_t due = std::min(std::max(l->second, frame), std::min(l->second, frame) + kAlone) + kSlack;
  SetDue(key, due);
  return due;
}

void WatchWords(int64_t frame, uint32_t p1, uint32_t p2) {
  std::lock_guard lock(g_mutex);
  if (g_mode != Mode::kFrames) return;
  auto due = [](int64_t a, int64_t b) { return std::min(std::max(a, b), std::min(a, b) + kAlone) + kSlack; };
  if (p1 && !g_decided.count(p1) && g_first_local.emplace(p1, frame).second) {
    if (auto r = g_first_remote.find(p1); r != g_first_remote.end()) SetDue(p1, due(frame, r->second));
  }
  if (p2 && !g_decided.count(p2) && g_first_remote.emplace(p2, frame).second) {
    if (auto l = g_first_local.find(p2); l != g_first_local.end()) SetDue(p2, due(l->second, frame));
  }
}

int64_t Decide(int64_t local_made_upto, int64_t remote_upto) {
  std::lock_guard lock(g_mutex);
  if (g_mode != Mode::kFrames) return -1;
  int64_t earliest = -1;
  auto decide = [&](uint32_t key, int64_t first) {
    const int64_t due = first + kAlone + kSlack;
    SetDue(key, due);
    ++g_stats.alone;
    LogLine("1", first, key, uint32_t(due), 0, 0);
    if (earliest < 0 || due < earliest) earliest = due;
  };
  // this machine's word alone: the peer's inputs up to kAlone frames after it did not say it
  for (const auto& [key, first] : g_first_local) {
    if (!g_first_remote.count(key) && !g_decided.count(key) && remote_upto >= first + kAlone) decide(key, first);
  }
  // the peer's word alone: this machine's inputs up to kAlone frames after it did not say it
  for (const auto& [key, first] : g_first_remote) {
    if (!g_first_local.count(key) && !g_decided.count(key) && local_made_upto >= first + kAlone) decide(key, first);
  }
  return earliest;
}

void FrameStart(PPCContext& ctx, uint8_t* base, int64_t frame, int64_t final_upto) {
  std::vector<Callback> give, made;
  std::vector<uint32_t> keys, made_keys, stop;
  {
    std::lock_guard lock(g_mutex);
    if (g_mode != Mode::kFrames) return;
    EndRun();
    // the frames no rollback runs again: their hand-offs numbered in order, the same count on both machines
    for (auto it = g_frame_runs.begin(); it != g_frame_runs.end() && it->first <= final_upto;) {
      uint32_t index = 0;
      for (Handoff* h : it->second) NumberHandoff(h, index++);
      it = g_frame_runs.erase(it);
    }
    g_numbered_upto = std::max(g_numbered_upto, final_upto);
    // this frame's run: its run before (a rollback's frame) is matched hand-off by hand-off
    g_running = frame;
    g_run_order = 0;
    if (auto it = g_frame_runs.find(frame); it != g_frame_runs.end()) {
      g_prev_run = std::move(it->second);
      g_frame_runs.erase(it);
    }
    // the callbacks due here, in their keys' order, to the sources that still play their sounds (one the game has
    // since given another sound, or let go, does not get them: the game's state, the same on both machines)
    if (auto due = g_due.find(frame); due != g_due.end()) {
      for (uint32_t key : due->second) {
        auto kept = g_kept.find(key);
        if (kept == g_kept.end()) {
          // only the other machine's FMOD gave it: this one gives it from the hand-off, an end (slot 0) after
          // stopping the sound (its callback taken off first: FMOD's own, later, would mark whatever the instance
          // plays then), a sync point as it is
          auto h = g_by_number.find(key >> 8);
          if (h == g_by_number.end() || h->second->dead) continue;
          const Handoff& ho = *h->second;
          if (Load32(base, ho.source + kSourceEvent) != ho.event) {
            LogLine("x", frame, key, ho.source, ho.event, Load32(base, ho.source + kSourceEvent));
            ++g_stats.dropped;
            continue;
          }
          const bool end = (key & 0xFF) == 0;
          if (end) stop.push_back(ho.event);
          made.push_back(Callback{ho.event, end ? 4u : 0u, 0, 0, ho.source, 0});
          made_keys.push_back(key);
          continue;
        }
        const Callback& cb = kept->second.cb;
        if (Load32(base, cb.source + kSourceEvent) != cb.event) {
          LogLine("x", frame, key, cb.source, cb.event, Load32(base, cb.source + kSourceEvent));
          ++g_stats.dropped;
          continue;
        }
        give.push_back(cb);
        keys.push_back(key);
      }
    }
    Collect(final_upto);
  }
  for (size_t i = 0; i < give.size(); ++i) {
    Call(ctx, base, give[i]);
    LogCallback(give[i].event, give[i].type, give[i].source, 'd', keys[i]);
  }
  for (uint32_t event : stop) {
    rex::CallFrame off(ctx);
    off.ctx.r3.u64 = event;
    off.ctx.r4.u64 = 0;
    off.ctx.r5.u64 = 0;
    __imp__sub_82E46F78(off, base);
    rex::CallFrame halt(ctx);
    halt.ctx.r3.u64 = event;
    halt.ctx.r4.u64 = 1;  // immediate
    __imp__sub_82E46E28(halt, base);
  }
  for (size_t i = 0; i < made.size(); ++i) {
    Call(ctx, base, made[i]);
    LogCallback(made[i].event, made[i].type, made[i].source, 'm', made_keys[i]);
  }
  std::lock_guard lock(g_mutex);
  g_stats.delivered += give.size() + made.size();
  g_stats.made += made.size();
}

void PassedFrameStart(PPCContext& ctx, uint8_t* base, int64_t frame, bool again, int64_t final_upto) {
  std::vector<Callback> give;
  {
    std::lock_guard lock(g_mutex);
    g_passed_on = true;
    // the frame before this one: what its runs before its latest one got, its latest run did not have (the memory had
    // gone back), so it goes now, where the game first saw it (after that frame); a source the game has since given
    // another sound (a corrected run) does not get it
    if (auto it = g_passed.find(frame - 1); it != g_passed.end()) {
      for (Passed& p : it->second) {
        if (p.given) continue;
        p.given = true;
        if (Load32(base, p.cb.source + kSourceEvent) != p.cb.event) continue;
        give.push_back(p.cb);
      }
    }
    // this frame runs again: what its runs before got is gone from the memory
    if (again) {
      if (auto it = g_passed.find(frame); it != g_passed.end()) {
        for (Passed& p : it->second) p.given = false;
      }
    }
    g_passed_frame = frame;
    // final frames are not run again
    while (!g_passed.empty() && g_passed.begin()->first < final_upto - 1) g_passed.erase(g_passed.begin());
    g_stats.regiven += give.size();
  }
  for (const Callback& cb : give) {
    Call(ctx, base, cb);
    LogCallback(cb.event, cb.type, cb.source, 'r');
  }
}

void PassedEnd() {
  std::lock_guard lock(g_mutex);
  g_passed_on = false;
  g_passed_frame = -1;
  g_passed.clear();
}

Stats GetStats() {
  std::lock_guard lock(g_mutex);
  Stats s = g_stats;
  s.waiting = g_waiting.size() + g_kept.size();
  return s;
}

void NoteRollback() {
  std::lock_guard lock(g_mutex);
  ++g_stats.rollbacks;
}

}  // namespace mk9::sound_sync

// mk9-lauta (2026-11-10): a sound's pool takes one more FMOD event instance when it has none to give (r3 the audio
// device, r4 the sound, r5 FMOD's mode, 1 = FMOD_EVENT_NONBLOCKING; r3 nonzero: one more). Non-blocking, FMOD says no
// while the event's data still loads (on each machine's own disk clock) or its instances are all playing (on each
// mixer's clock): one machine's game then plays the sound and the other's not, and its memory goes another way.
// mk9_sound_blocking makes a session's wait for the data; the log says the ones FMOD refused.
REX_HOOK_RAW(sub_825CA9B8) {
  static const bool blocking = REXCVAR_GET(mk9_sound_blocking);
  uint32_t epoch = 0;
  const int64_t frame = mk9::netplay::FrameRunning(&epoch, nullptr);
  if (frame >= 0 && blocking) ctx.r5.u64 = 0;
  const uint32_t sound = ctx.r4.u32, mode = ctx.r5.u32;
  __imp__sub_825CA9B8(ctx, base);
  if (frame < 0) return;
  static int refused = 0;
  if (ctx.r3.u32 == 0 && ++refused <= 40) {
    REXLOG_INFO("[sound] no new FMOD instance for the sound at {:08X} (mode {}) at frame {} of epoch {}", sound, mode,
                frame, epoch);
  }
}

// The game hands a source's event to FMOD with its callback (sub_82607xxx: r5 the source).
REX_HOOK_RAW(sub_82E46F78) {
  const uint32_t event = ctx.r3.u32, callback = ctx.r4.u32, source = ctx.r5.u32;
  __imp__sub_82E46F78(ctx, base);
  if (callback == mk9::sound_sync::kGameCallback && source != 0) {
    mk9::sound_sync::Numbered(source, event);
  }
}

// FMOD calls it on its own threads: one in it is the game's for the time being, as with the game's other callbacks
// (sound_memory.cpp, rex/system/guest_park.h GuardScope).
REX_HOOK_RAW(sub_82603360) {
  rex::system::park::GuardScope guard;
  if (mk9::sound_sync::Keep(ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, ctx.r6.u32, ctx.r7.u32)) {
    ctx.r3.u64 = 0;  // FMOD_OK
    return;
  }
  __imp__sub_82603360(ctx, base);
}
