// MK9 online: the sounds' callbacks at the same frame on both machines.
//
// FMOD calls the game's event callback (sub_82603360) when an event reaches a sync point (type 0: the source's +268
// gets 0x40000000) or ends (3, stolen, and 4, finished: +88 gets 0x10000000 and +256 -1), and the game's audio update
// reads those marks on the source. FMOD gets there on each machine's own clock (its mixer), so now and then the game
// sees a sound end a frame apart on the two machines, and whatever waits on it (the menus' announcer, a stage's
// ambient script, a round's announcer) goes on a frame apart: the game's random draws then come in another order.
//
// In lockstep (the menus, the loads) the callbacks of the sources the game started there wait instead: each machine
// announces the ones it got with its inputs (Announcement: one a frame, again and again until delivered), and a
// callback goes to the game at the frame boundary where both machines' inputs have announced it, in the callbacks'
// order: the same frame on both, since both decide from the same two input streams (Deliver). A source started in
// lockstep is numbered when the game hands it to FMOD (the game thread's count, the same on both machines); its end is
// one callback, its sync points the next ones in order. A source the game starts again is a new number: what its old
// number still had waiting is dropped on both machines at the same frame.
//
// A fight's epoch (Mode::kFrames: its rollback, and the lockstep after the fight until the next barrier) does the same
// by frame numbers, which a frame run again keeps (the sounds handed to FMOD before it go through as they come: the two
// machines need not have run the same frames before a barrier): each callback is announced once, with the input of
// frame L(k) on this machine and R(k) on the other, and goes to the game at the start of frame max(L, R) on both
// machines, in every run of that frame (FrameStart). A frame run on a prediction does not know R yet: when the peer's input says it and
// that frame has run, the rollback goes back to it (RemoteAnnounced). A sound is numbered once the frame that handed it
// to FMOD is final (both players' real inputs: no rollback runs it again), by that frame and its place among the
// frame's hand-offs, and its callbacks are announced from then on. (A sound one machine's FMOD could not start, its
// event's instances busy on that mixer's clock, is not started there: its number is that machine's alone, and nothing
// else moves.) A callback goes only to a source that still plays its sound then (the
// source's event, in the game's memory: a source's address and an event's handle are not the same on both machines,
// what a source plays is).

#pragma once

#include <cstdint>

struct PPCContext;

namespace mk9::sound_sync {

enum class Mode {
  kOff,       // no session: the callbacks go through as they come (what waits goes to the game)
  kLockstep,  // a session's lockstep epoch (the menus)
  kFrames,    // a session's fight epoch
};
// The frame boundary (game thread), before the frame: the mode of the epoch running.
void SetMode(PPCContext& ctx, uint8_t* base, Mode mode, uint32_t epoch);

// kLockstep: this machine's announcement for the input it is making: a callback it got that the game has not had
// yet, round robin (0: none).
uint32_t Announcement();
// kLockstep: a frame's two inputs' announcements (this machine's and the peer's), before the frame runs: the callbacks
// both machines' inputs have announced go to the game.
void Deliver(PPCContext& ctx, uint8_t* base, uint32_t local, uint32_t remote);

// mk9-lauta (2026-11-10), a spectator (feed.h): whether every callback the two players' recorded announcements agree
// on, with this frame's, has come from this machine's own FMOD already (its game then gets them at the same frame as
// theirs did; else the frame waits for them).
bool AgreedReady(uint32_t local, uint32_t remote);

// kFrames: this machine's announcement for its input of frame input_frame (0: none): each callback once.
uint32_t AnnounceFor(int64_t input_frame);
// kFrames: the peer's real input of frame `frame` announces `key`: the frame the callback goes to the game at, once
// both machines' inputs have said it (-1: not yet, or said before). A frame that already ran without it must run again.
int64_t RemoteAnnounced(uint32_t key, int64_t frame);
// kFrames: before frame `frame` runs (its first run or one a rollback makes; game thread), with the frames up to
// final_upto final: the sounds those frames handed to FMOD are numbered, and the callbacks due at this frame go to the
// game.
void FrameStart(PPCContext& ctx, uint8_t* base, int64_t frame, int64_t final_upto);
// kFrames: the peer's word made a frame run again (statistics).
void NoteRollback();

// kOff in a fight with rollback (mk9_sound_sync_fights off: the callbacks go to the game as they come): a frame's run
// keeps the callbacks that came during it, and when a rollback runs that frame again (the game's memory went back to
// before the marks they made, and FMOD calls each one once), they are given again at the start of the frame after it,
// where the game first saw them. Before each frame of a fight with rollback (game thread); again: a frame a rollback
// runs again; frames up to final_upto are final.
void PassedFrameStart(PPCContext& ctx, uint8_t* base, int64_t frame, bool again, int64_t final_upto);
// The end of a fight with rollback: nothing kept any more.
void PassedEnd();

struct Stats {
  uint64_t numbered = 0;    // sources the game handed to FMOD in a session (numbered)
  uint64_t deferred = 0;    // callbacks kept for both machines' word
  uint64_t delivered = 0;   // ... and given to the game (in a fight's epoch, again in each frame run again)
  uint64_t dropped = 0;     // ... whose source the game started again first
  uint64_t passed = 0;      // callbacks given at once (no session, a source not numbered)
  uint64_t flushed = 0;     // kept ones given when the session ended
  uint64_t waiting = 0;     // kept now
  uint64_t announced = 0;   // fight epochs: callbacks this machine's inputs said
  uint64_t rollbacks = 0;   // fight epochs: the peer's word on frames that had run (RemoteAnnounced)
  uint64_t regiven = 0;     // fights with rollback: callbacks given again in frames run again (PassedFrameStart)
};
Stats GetStats();

}  // namespace mk9::sound_sync
