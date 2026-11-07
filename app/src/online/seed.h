// MK9 online: one seed for the game's random number generators, so that two machines (or two runs) draw the same
// numbers (docs/online/engine-map.md, "Aleatorios").
//
// Left alone, every run draws different ones, because they are seeded from the clock:
//   the game's own generator (Seed = Seed * 196314165 + 907633515; sub_826D91C0 an integer, sub_826D91E8 a float):
//     its seed at 0x834161D8 is set by sub_826D3088 when a mode starts (sub_82395B48: from the cycle counter), and
//     that seed goes through the capture interface first (sub_82A1C550, tag 0x00FF0004, 4 bytes): NRS's own way to
//     give both players of their online (or a Practice recording) the same one;
//   UE3's appSRand (the same generator, ~70 inlined copies): GSRandSeed at 0x833ED6FC, seeded with the cycle counter by
//     the engine's pre-init (sub_82365A40);
//   Scaleform's GRandom (Marsaglia's multiply-with-carry over eight words, drawn by sub_82F891E0): the global one at
//     0x833AFA84 (ActionScript's random), seeded with the tick count at static init (sub_83367A08).
// The C library's rand() is only called by the libraries (audio and others), not by the game.
//
//   mk9_seed=<n>   start them all from n at the first frame (one instance's runs: the record/replay tests); a
//                  session's seed takes its place (the host's: this one if set, else a random one; netplay.cpp)

#pragma once

#include <cstdint>

namespace mk9::seed {

// mk9_seed, if set.
bool Configured(uint32_t* seed);
// mk9_seed, if set and game_frame is the first frame.
bool FixedSeedFor(uint64_t game_frame, uint32_t* seed);
// Starts the generators above from seed, at a frame boundary on the game thread: reseeds them and makes every later
// mode seed (OverrideModeSeed) a function of it.
void Start(uint8_t* base, uint32_t seed);
// A capture block (sub_82A1C550's arguments). Once started, the mode seed block (tag 0x00FF0004) gets the next seed of
// the sequence: the n-th mode started gets the same one on both machines.
void OverrideModeSeed(uint8_t* base, uint32_t tag, uint32_t data, uint32_t size);

}  // namespace mk9::seed
