// MK9 online: the sounds' callbacks at the same frame on both machines (sound_sync.h).

#include "online/sound_sync.h"

#include <algorithm>
#include <map>
#include <mutex>
#include <set>
#include <thread>
#include <unordered_map>
#include <vector>

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/ppc.h>
#include <rex/system/guest_park.h>

REXCVAR_DEFINE_BOOL(mk9_sound_sync, true, "MK9 Online",
                    "In lockstep the sounds' callbacks (a sync point, an end) reach the game at the same frame on both "
                    "machines, once both have had them (sound_sync.h); false: as FMOD calls them");
REXCVAR_DEFINE_INT32(mk9_test_sound_late, 0, "MK9 Online",
                     "Tests: this machine's sounds' callbacks come this many frames late (as a slower mixer would): "
                     "with mk9_sound_sync their announcement waits, without it the game gets them late")
    .range(0, 600);

extern "C" REX_FUNC(__imp__sub_82E46F78);  // FMOD::Event::setCallback(event, callback, userdata)
extern "C" REX_FUNC(__imp__sub_82603360);  // the game's event callback (event, type, param1, param2, source)

namespace mk9::sound_sync {
namespace {

constexpr uint32_t kGameCallback = 0x82603360;
constexpr uint32_t kForget = 4096;  // numbers this far behind the newest leave the decision (a sound long gone)

struct Callback {
  uint32_t event = 0, type = 0, param1 = 0, param2 = 0, source = 0;
  uint64_t kept_at = 0;  // the boundary it came after (mk9_test_sound_late)
};

struct Source {
  uint32_t number = 0;  // 0: not numbered (its callbacks go through)
  uint32_t event = 0;   // the FMOD event it was handed with
  uint32_t sync_points = 0;
  bool ended = false;
};

// What the callbacks touch (they come on FMOD's thread or the game's).
std::mutex g_mutex;
bool g_deferring = false;
std::thread::id g_game_thread;
uint32_t g_next_number = 0;
uint64_t g_boundary = 0;
std::unordered_map<uint32_t, Source> g_sources;  // source address -> the sound it plays
std::map<uint32_t, Callback> g_waiting;           // number << 8 | slot (0: the end, 1-255: sync points) -> callback
std::vector<Callback> g_late;                     // mk9_test_sound_late without mk9_sound_sync: given late here
Stats g_stats;

// Decided from the two machines' inputs only (game thread): the same on both.
std::set<uint32_t> g_local_said, g_remote_said, g_delivered;
uint32_t g_last_announced = 0;

void Call(PPCContext& ctx, uint8_t* base, const Callback& cb) {
  rex::CallFrame frame(ctx);
  frame.ctx.r3.u64 = cb.event;
  frame.ctx.r4.u64 = cb.type;
  frame.ctx.r5.u64 = cb.param1;
  frame.ctx.r6.u64 = cb.param2;
  frame.ctx.r7.u64 = cb.source;
  __imp__sub_82603360(frame, base);
}

// The game handed a source to FMOD with its callback (any thread): in lockstep on the game thread, a new number.
void Numbered(uint32_t source, uint32_t event) {
  std::lock_guard lock(g_mutex);
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
    ++g_stats.numbered;
  } else {
    s = Source{};
  }
}

// FMOD's callback (any thread): true if it waits (the game gets it later), false if it goes through now.
bool Keep(uint32_t event, uint32_t type, uint32_t param1, uint32_t param2, uint32_t source) {
  static const bool sync = REXCVAR_GET(mk9_sound_sync);
  static const int32_t late = REXCVAR_GET(mk9_test_sound_late);
  if (type != 0 && type != 3 && type != 4) return false;  // the ones that mark the source
  std::lock_guard lock(g_mutex);
  if (!g_deferring) {
    ++g_stats.passed;
    return false;
  }
  if (!sync) {
    if (late <= 0) return false;
    g_late.push_back(Callback{event, type, param1, param2, source, g_boundary});
    return true;
  }
  auto it = g_sources.find(source);
  if (it == g_sources.end() || it->second.number == 0) {
    ++g_stats.passed;
    return false;
  }
  Source& s = it->second;
  if (s.event != event) {
    ++g_stats.dropped;  // an event the source played before (the game hands each source's sound to FMOD anew)
    return true;
  }
  uint32_t slot = 0;
  if (type == 0) {
    if (s.sync_points >= 255) {
      ++g_stats.passed;
      return false;
    }
    slot = ++s.sync_points;
  } else {
    if (s.ended) return true;  // stolen, then finished: one end
    s.ended = true;
  }
  g_waiting[s.number << 8 | slot] = Callback{event, type, param1, param2, source, g_boundary};
  ++g_stats.deferred;
  return true;
}

}  // namespace

void SetDeferring(PPCContext& ctx, uint8_t* base, bool on) {
  static const bool sync = REXCVAR_GET(mk9_sound_sync);
  static const int32_t late = REXCVAR_GET(mk9_test_sound_late);
  std::vector<Callback> give;
  {
    std::lock_guard lock(g_mutex);
    g_game_thread = std::this_thread::get_id();
    ++g_boundary;
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
    ++g_stats.delivered;
  }
}

Stats GetStats() {
  std::lock_guard lock(g_mutex);
  Stats s = g_stats;
  s.waiting = g_waiting.size();
  return s;
}

}  // namespace mk9::sound_sync

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
