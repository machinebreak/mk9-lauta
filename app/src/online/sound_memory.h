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
// Below FMOD, XAudio2 (the XDK's, ~0x832A0000-0x832EFFFF) goes apart the same way: its XMemAlloc blocks (allocator
// id 0x97; sub_823667D8 XMemAlloc, sub_823667E0 XMemFree) come from the same arena, its static data is left as it is
// (module_data.py --code 832A0000-832F0000), and its worker thread (sub_832C92D0) and the render callback the SDK's
// Audio Worker calls (sub_832CCC30, registered by sub_832CCE40) run on through parks. XMA's contexts and buffers are
// physical memory, outside the timeline already.
// What the game asks of FMOD during a frame (an event started, a parameter set) is not undone by a rollback either: a
// frame run again asks again (a sound started in the frames run again plays twice) until those calls are recorded and
// played back to a frame run again.
//
//   mk9_sound_apart=<bool>   the sound in memory of its own (true; read once, at FMOD's first allocation). Off, the
//                            sound is rolled back with the rest: FMOD hangs once its state and XMA's part ways

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
