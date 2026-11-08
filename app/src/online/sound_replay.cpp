// MK9 online: the sounds of the frames a rollback runs again (sound_replay.h).

#include "online/sound_replay.h"

#include <algorithm>
#include <map>
#include <thread>
#include <unordered_map>
#include <vector>

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>

#include "online/netplay.h"

REXCVAR_DEFINE_BOOL(mk9_music_confirm, true, "MK9 Online",
                    "In a fight with rollback the music changes tracks once the frame's inputs are confirmed");
REXCVAR_DEFINE_BOOL(mk9_sound_replay, true, "MK9 Online",
                    "Frames a rollback runs again keep the sounds their first run started instead of starting them "
                    "again (sound_replay.h)");

extern "C" REX_FUNC(__imp__sub_82607310);  // a sound's next event instance from its pool (the sound's virtual)
extern "C" REX_FUNC(__imp__sub_82E46DE8);  // FMOD::Event::start()
extern "C" REX_FUNC(__imp__sub_82E46F78);  // FMOD::Event::setCallback(callback, userdata)
extern "C" REX_FUNC(__imp__sub_82E46E28);  // FMOD::Event::stop(immediate)

namespace mk9::sound_replay {
namespace {

constexpr uint64_t kKeep = 32;  // frames of records kept (a rollback goes 8 frames back at most)

struct Pop {
  uint32_t sound, order, event;  // the sound, its pop's order in the frame, the instance it gave
  bool claimed;                  // made by the frame's latest run
};
struct Start {
  uint32_t event;
  bool claimed;
};
struct Record {
  std::vector<Pop> pops;
  std::vector<Start> starts;
};

// The game thread's only.
struct State {
  bool on = false;
  uint64_t key = 0;  // the frame running (epoch << 32 | frame)
  bool hidden = false;
  std::thread::id thread;
  std::map<uint64_t, Record> frames;
  std::unordered_map<uint32_t, uint32_t> pops_now;  // sound -> its pops so far in this run of the frame
  Stats stats;
};
State g;

bool Mine() { return g.on && std::this_thread::get_id() == g.thread; }

void CallGuest(PPCContext& ctx, uint8_t* base, void (*fn)(PPCContext&, uint8_t*), uint32_t r3, uint32_t r4,
               uint32_t r5) {
  rex::CallFrame frame(ctx);
  frame.ctx.r3.u64 = r3;
  frame.ctx.r4.u64 = r4;
  frame.ctx.r5.u64 = r5;
  fn(frame, base);
}

// The run of the frame g.key ended: what an earlier run of it started and this one did not is stopped (the game's
// memory no longer knows the instance: its callback goes first, it would mark whichever source sits at that address).
void Finish(PPCContext& ctx, uint8_t* base) {
  auto it = g.frames.find(g.key);
  if (it == g.frames.end()) return;
  Record& r = it->second;
  for (auto s = r.starts.begin(); s != r.starts.end();) {
    if (s->claimed) {
      ++s;
      continue;
    }
    CallGuest(ctx, base, __imp__sub_82E46F78, s->event, 0, 0);
    CallGuest(ctx, base, __imp__sub_82E46E28, s->event, 0, 0);
    ++g.stats.stopped;
    s = r.starts.erase(s);
  }
  r.pops.erase(std::remove_if(r.pops.begin(), r.pops.end(), [](const Pop& p) { return !p.claimed; }), r.pops.end());
}

}  // namespace

void OnFrame(PPCContext& ctx, uint8_t* base, uint64_t key, bool hidden) {
  static const bool enabled = REXCVAR_GET(mk9_sound_replay);
  if (g.on) Finish(ctx, base);
  if (!enabled || key == 0) {
    g.on = false;
    g.key = 0;
    g.frames.clear();
    return;
  }
  if ((key >> 32) != (g.key >> 32)) g.frames.clear();  // a new epoch: its frames count from 0 again
  g.on = true;
  g.key = key;
  g.hidden = hidden;
  g.thread = std::this_thread::get_id();
  g.pops_now.clear();
  Record& r = g.frames[key];
  for (Pop& p : r.pops) p.claimed = false;
  for (Start& s : r.starts) s.claimed = false;
  while (!g.frames.empty() && g.frames.begin()->first + kKeep < key) g.frames.erase(g.frames.begin());
}

Stats GetStats() { return g.stats; }

}  // namespace mk9::sound_replay

using mk9::sound_replay::g;

// The sound's next event instance (r3 the sound; its pool at +200/+204 in the game's memory): in a frame run again, the
// instance the frame's first run got for the same pop. The pool still gives up its last one, as in any run (the game's
// memory goes through the same steps); that instance, which FMOD has not started, is left unused.
REX_HOOK_RAW(sub_82607310) {
  if (!mk9::sound_replay::Mine()) {
    __imp__sub_82607310(ctx, base);
    return;
  }
  const uint32_t sound = ctx.r3.u32;
  const uint32_t order = g.pops_now[sound]++;
  __imp__sub_82607310(ctx, base);
  auto& pops = g.frames[g.key].pops;
  for (auto& p : pops) {
    if (p.sound != sound || p.order != order) continue;
    p.claimed = true;
    if (g.hidden) {
      ctx.r3.u64 = p.event;
      ++g.stats.pops_replayed;
    } else {
      p.event = ctx.r3.u32;
    }
    return;
  }
  pops.push_back({sound, order, ctx.r3.u32, true});
}

// FMOD::Event::start (r3 the event): in a frame run again, an instance the frame's first run started is playing already.
REX_HOOK_RAW(sub_82E46DE8) {
  if (!mk9::sound_replay::Mine()) {
    __imp__sub_82E46DE8(ctx, base);
    return;
  }
  const uint32_t event = ctx.r3.u32;
  auto& starts = g.frames[g.key].starts;
  if (g.hidden) {
    for (auto& s : starts) {
      if (s.event != event || s.claimed) continue;
      s.claimed = true;
      ++g.stats.starts_skipped;
      ctx.r3.u64 = 0;  // FMOD_OK
      return;
    }
    ++g.stats.starts_new;
  } else {
    ++g.stats.starts_live;
  }
  __imp__sub_82E46DE8(ctx, base);
  starts.push_back({event, true});
}

// The music changing tracks (sub_8245E7F8: the music object and the track's index 0-5; it fades the track playing out
// and has FMOD stop its stream and open the next one, sub_82A352C0 and sub_8260C268). FMOD's streams are outside the
// rollback: a frame run again would change the music once more. In a fight with rollback the frame first waits for the
// other player's inputs up to it (netplay.h, ConfirmFrame), so the change is nearly always in a frame no rollback will
// run again. It goes ahead either way: leaving it out of a frame that will be run again broke the rest of that frame
// (the game read through what it had not set up), and the sounds a corrected run no longer starts are stopped anyway.
extern "C" REX_FUNC(__imp__sub_8245E7F8);
REX_HOOK_RAW(sub_8245E7F8) {
  static const bool confirm = REXCVAR_GET(mk9_music_confirm);
  if (confirm) mk9::netplay::ConfirmFrame("the music changes");
  __imp__sub_8245E7F8(ctx, base);
}
