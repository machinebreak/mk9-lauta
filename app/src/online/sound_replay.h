// MK9 online: the sounds of the frames a rollback runs again (netplay.cpp).
//
// The sound lives outside the rollback (sound_memory.h): an FMOD event a frame started keeps playing when the game's
// memory goes back to before that frame. Run again, the frame would start it once more (the same hit heard twice, a
// frame or more apart). UE3's FMOD sound source plays a sound (sub_82607A48) with an event instance it pops from its
// sound's pool (sub_82607310, the sound's virtual at vtable +180: the pool at +200/+204 in the game's memory, refilled
// beforehand by sub_825CA9B8, which takes a new instance for each one FMOD says is busy) and then starts it
// (FMOD::Event::start, sub_82E46DE8).
//
// While a fight runs with rollback, each frame's pops and starts are kept (the frame, the sound and its pop's order in
// the frame, the instance). In a frame run again, a pop the first run made gives the instance it gave then, and a
// start of an instance that is already playing does nothing: the game holds the same instance as before and the sound
// goes on as it was. What the corrected run does differently goes to FMOD as usual (a sound heard late by the
// rollback's frames), and a sound the first run started that the corrected run no longer starts is stopped (with its
// fade; its callback taken off first, as the game's callback, sub_82603360, would mark the source that held it).

#pragma once

#include <cstdint>

struct PPCContext;

namespace mk9::sound_replay {

// Before each frame the session runs (game thread): key = the epoch and frame (nonzero) in a fight with rollback,
// hidden = a frame run again; key 0 = no rollback (lockstep, no session): sounds go to FMOD as usual. The run before
// ends here: the sounds it no longer started are stopped.
void OnFrame(PPCContext& ctx, uint8_t* base, uint64_t key, bool hidden);

struct Stats {
  uint64_t starts_live = 0;      // starts in frames shown (fights with rollback)
  uint64_t pops_replayed = 0;    // pops in frames run again that gave the instance of the first run
  uint64_t starts_skipped = 0;   // starts of instances already playing
  uint64_t starts_new = 0;       // starts in frames run again that went to FMOD (what the corrected run changed)
  uint64_t stopped = 0;          // sounds the corrected run no longer started, stopped
};
Stats GetStats();

}  // namespace mk9::sound_replay
