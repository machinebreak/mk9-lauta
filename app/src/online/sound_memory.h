// MK9 online: the sound's own memory, out of rollback's reach (docs/online/netplay-design.md, "Rollback: el sonido").
//
// A rollback puts the game's memory back some frames. The sound must not go back with it: what was heard would play
// again (the music and every sound jump back up to the frames undone), and FMOD's threads, halfway through a mix
// when their state changes under them, spin or wait on a lock forever. So FMOD (FMOD Ex 4.x and its Event system,
// the game's sound library) lives outside the memory the timeline keeps:
//   - its heap: FMOD allocates through the three callbacks the game hands FMOD::Memory_Initialize (UFmodAudioDevice's
//     init, sub_82606880 -> sub_82DCD640): sub_8258DBC8 alloc(size, type), sub_8258DC08 realloc(block, size, type),
//     sub_8258DCA0 free(block, type). They used GMalloc, so FMOD's blocks sat among the game's; here they come from an
//     arena of its own at the top of the stack range (outside the timeline's ranges, snapshot.h). FMOD's physical
//     memory (its XMA buffers: the type's bit 0x00100000) stays GMalloc's physical memory, outside the ranges too.
//   - its static data: FMOD's .data and .bss (tools/online/module_data.py: the image data only FMOD's code refers to,
//     up to what other code refers to), left as they are by a rollback (UntrackedRanges).
//   - its threads (FMOD::Thread's loop, sub_82DEB0B8: the mixer, stream, file, non-blocking and geometry threads) run
//     on through a boundary's park (rex/system/guest_park.h, SetExempt): they touch nothing of the game's. Its banks
//     are read from the game's packages in memory, unchanged by a rollback.
// What the game asks of FMOD during a frame (an event started, a parameter set) is not undone by a rollback either:
// a frame run again asks again, and FMOD's own memory below it (XAudio2's voices, ~0x832A0000) is still the game's.
// Until those calls are recorded and played back to a frame run again, and XAudio2 is apart too, this stays off
// (mk9_sound_apart): the sound is rolled back with the rest, its threads parked at their idle places.
//
//   mk9_sound_apart=<bool>   FMOD in memory of its own (read once, at FMOD's first allocation)

#pragma once

#include <cstdint>
#include <utility>
#include <vector>

namespace mk9::sound {

// FMOD is apart (mk9_sound_apart).
bool Apart();
// FMOD's static data, when apart: byte ranges of the image's data a rollback leaves as they are.
std::vector<std::pair<uint32_t, uint32_t>> UntrackedRanges();

struct HeapStats {
  uint32_t arena_start = 0, arena_size = 0;
  uint64_t in_use = 0, peak = 0, blocks = 0, allocs = 0, frees = 0, failed = 0;
};
HeapStats Stats();

}  // namespace mk9::sound
