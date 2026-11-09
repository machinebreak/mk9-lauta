// MK9 online: the sounds' callbacks at the same frame on both machines.
//
// FMOD calls the game's event callback (sub_82603360) when an event reaches a sync point (type 0: the source's +268
// gets 0x40000000) or ends (3, stolen, and 4, finished: +88 gets 0x10000000 and +256 -1), and the game's audio update
// reads those marks on the source. FMOD gets there on each machine's own clock (its mixer), so now and then the game
// sees a sound end a frame apart on the two machines, and whatever waits on it (the menus' announcer) goes on a frame
// apart: the one desync of the menus (netplay-design.md).
//
// In lockstep (the menus, the loads, after a fight) the callbacks of the sources the game started there wait instead:
// each machine announces the ones it got with its inputs (Announcement: one a frame, again and again until delivered),
// and a callback goes to the game at the frame boundary where both machines' inputs have announced it, in the
// callbacks' order: the same frame on both, since both decide from the same two input streams (Deliver). A source
// started in lockstep is numbered when the game hands it to FMOD (the game thread's count, the same on both machines);
// its end is one callback, its sync points the next ones in order. A source the game starts again is a new number: what
// its old number still had waiting is dropped on both machines at the same frame.
//
// In a fight with rollback the callbacks go through as they come (a frame run again cannot wait for the peer, and a
// fight's logic does not wait on a sound); what is still waiting when one starts goes to the game then.

#pragma once

#include <cstdint>

struct PPCContext;

namespace mk9::sound_sync {

// The frame boundary (game thread): on in a session's lockstep frames, off otherwise (what waits goes to the game).
void SetDeferring(PPCContext& ctx, uint8_t* base, bool on);
// This machine's announcement for the input it is making: a callback it got that the game has not had yet, round
// robin (0: none).
uint32_t Announcement();
// A lockstep frame's two inputs' announcements (this machine's and the peer's), before the frame runs: the callbacks
// both machines' inputs have announced go to the game.
void Deliver(PPCContext& ctx, uint8_t* base, uint32_t local, uint32_t remote);

struct Stats {
  uint64_t numbered = 0;   // sources the game handed to FMOD in lockstep
  uint64_t deferred = 0;   // callbacks kept for both machines' word
  uint64_t delivered = 0;  // ... and given to the game
  uint64_t dropped = 0;    // ... whose source the game started again first
  uint64_t passed = 0;     // callbacks given at once (outside lockstep, a source not numbered)
  uint64_t flushed = 0;    // kept ones given when lockstep ended
  uint64_t waiting = 0;    // kept now
};
Stats GetStats();

}  // namespace mk9::sound_sync
