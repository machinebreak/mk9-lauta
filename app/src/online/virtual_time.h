// MK9 online: the game thread's clock in a session (mk9-lauta, 2026-11-10; docs/online/netplay-design.md).
//
// The game reads the time through the PowerPC timebase (mftb: the SDK's Clock::QueryGuestTickCount, REX_QUERY_TIMEBASE);
// on the game thread, through UE3's appCycles (sub_82366BF8). Each machine's clock goes its own way, so whatever the
// game does with the time (a script's timer, a menu's wait, a time stored with a load) may happen a frame apart on the
// two machines. In a session's game (a player's or a spectator's) the game thread reads a clock made of its frames:
//
//   kBase (1000 s) + frame * (a 60th of a second)
//
// frame: GuardedMain's frame counter ([0x833848D4]+108: the game's memory, the same on both machines, put back by a
// rollback), so a frame run again reads the time its first run read. Every read of a frame gets the same time: how
// many times a frame reads the clock is not the same on both machines (mk9_clock_audit, 2026-11-10, ~12k frames of a
// session: UE3's waits for its render thread read it only when they wait, up to 20k times a frame; the incremental
// purge of garbage as long as its time budget lasts; PhysX's zone when its thread is late), so Tekken 6 Native
// Online's 10 us a read (t6_determinism.cpp) would give the two machines different times. The waits for the render
// thread (WaitZone: the render command fence's sub_82741630 and sub_82360480, the incremental purge's pass that asks an
// object again until the render thread lets go of it, sub_82568540) read 10 us more each from the frame's time (their
// time budgets, the async loading they do meanwhile in 10 ms slices, end as on the host's clock), and the frame's time
// again after them. A valve: past 20000 reads outside them in a frame (the audit's most, 126: a loop waiting on the
// time alone), the frame's time goes on by 10 us a read, logged. From the
// guest's main thread's first read (main, before the engine's init): one clock all along, never the host's for a while
// and then this one. Other threads (UE3's render thread, FMOD, XAudio2, streaming) keep the host's. The scripts' clock
// (native 824b75f8, mk9_capture.cpp's ScriptClock) is UE3's appSeconds of it, this same clock.
//
//   mk9_virtual_time          on/off (a session's game only)
//   mk9_clock_audit=<file>    test: at each frame's start, the game thread's reads of the frame before (its logic's,
//                             the boundary's and the waits' apart) and who made them (host stacks);
//                             tools/online/dev/clock_audit.py compares two machines'

#pragma once

#include <cstdint>

namespace mk9::virtual_time {

// main's start (the guest's main thread, the game thread from here on): the filter goes in for a session's game.
void Install();
// The calling thread is the game thread of a session's game with its clock made of frames.
bool On();
// This game's game thread reads the clock made of frames (any thread may ask; a session's game only).
bool Enabled();
// A wait of the game thread for another thread, for its scope (UE3's render command fence: sub_82741630, sub_82360480;
// the incremental purge asking an object again until the render thread lets go of it: sub_82568540): how many times it
// reads the clock depends on the other thread, so its reads go on by 10 us each from the frame's time (its time budget,
// its async loading's 10 ms slices end) and do not count as the frame's; the frame's time again after it.
struct WaitZone {
  WaitZone();
  ~WaitZone();
  WaitZone(const WaitZone&) = delete;
  WaitZone& operator=(const WaitZone&) = delete;
};
// FMOD's event positions, the other clock the game thread reads (2026-11-11): FMOD::Event::getInfo's positionms
// (sub_82E46D40) is where the event plays on the mixer's own clock, and a stage's script asks a sound's frames left
// from it (native 824a5610: lengthms minus positionms, through the audio component's virtuals +336 sub_82604DD8 and
// +300 sub_82589898). A frame a machine's mixer runs late (or a spectator catching up, its frames faster than the
// mixer) gets another answer, and the script goes another way: a spectator catching up in a fight went apart at its
// ~430th frame. With the clock made of frames, the game thread's getInfo answers the time since the game started the
// event, in frames (its start's frame: SoundStarted, from FMOD::Event::start's hook in sound_replay.cpp), at most the
// event's length. Events started before the clock or by other threads keep FMOD's.
void SoundStarted(uint32_t event);
// The start of a frame's boundary (game thread; the pad read's hook): the frame before's logic is over.
void BoundaryStart(const uint8_t* base);
// The end of a frame's boundary (game thread), right before the game's pad read: the frame's reads count from here.
void FrameStart(const uint8_t* base);

}  // namespace mk9::virtual_time
