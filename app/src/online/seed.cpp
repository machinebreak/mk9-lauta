// MK9 online: one seed for the game's random number generators (seed.h).

#include "online/seed.h"

#include <cstdlib>
#include <string>

#include <rex/cvar.h>
#include <rex/logging.h>

REXCVAR_DEFINE_STRING(mk9_seed, "", "MK9 Online",
                      "Seed the game's random number generators with this number at the first frame (empty: from "
                      "the clock, as the game does); a session's host sends it to the other player");

namespace mk9::seed {
namespace {

constexpr uint32_t kSRandSeed = 0x833ED6FC;      // GSRandSeed
constexpr uint32_t kGameSeed = 0x834161D8;       // the game's generator
constexpr uint32_t kGRandom = 0x833AFA84;        // Scaleform's global GRandom: u32 Q[8], u32 carry, u32 index
constexpr uint32_t kGRandomCarry = 362436;       // GRandom::SeedRandom's (sub_82F89198)
constexpr uint32_t kGRandomIndex = 7;
constexpr uint32_t kTagSeed = 0x00FF0004;

bool g_started = false;
uint32_t g_seed = 0;
uint32_t g_modes = 0;  // mode seeds given so far

void Store32(uint8_t* base, uint32_t addr, uint32_t v) {
  uint8_t* p = base + addr;
  p[0] = uint8_t(v >> 24);
  p[1] = uint8_t(v >> 16);
  p[2] = uint8_t(v >> 8);
  p[3] = uint8_t(v);
}

// An integer hash (Chris Wellons' lowbias32): the n-th seed of the sequence from g_seed + n * golden ratio.
uint32_t Mix(uint32_t x) {
  x ^= x >> 16;
  x *= 0x7FEB352Du;
  x ^= x >> 15;
  x *= 0x846CA68Bu;
  x ^= x >> 16;
  return x;
}

}  // namespace

bool Configured(uint32_t* seed) {
  const std::string text = REXCVAR_GET(mk9_seed);
  if (text.empty()) return false;
  char* end = nullptr;
  const unsigned long long v = std::strtoull(text.c_str(), &end, 0);
  if (end == text.c_str()) return false;
  *seed = uint32_t(v);
  return true;
}

bool FixedSeedFor(uint64_t game_frame, uint32_t* seed) { return game_frame == 1 && Configured(seed); }

void Start(uint8_t* base, uint32_t seed) {
  g_started = true;
  g_seed = seed;
  g_modes = 0;
  Store32(base, kSRandSeed, seed);
  Store32(base, kGameSeed, seed);
  // GRandom::SeedRandom: the eight words from an xorshift of the seed
  uint32_t x = seed;
  for (int i = 0; i < 8; ++i) {
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    Store32(base, kGRandom + 4 * i, x);
  }
  Store32(base, kGRandom + 32, kGRandomCarry);
  Store32(base, kGRandom + 36, kGRandomIndex);
  REXLOG_INFO("[seed] the random number generators start from {}", seed);
}

void OverrideModeSeed(uint8_t* base, uint32_t tag, uint32_t data, uint32_t size) {
  if (!g_started || tag != kTagSeed || size != 4 || data == 0) return;
  const uint32_t seed = Mix(g_seed + 0x9E3779B9u * ++g_modes);
  Store32(base, data, seed);
  REXLOG_INFO("[seed] mode seed {}: {}", g_modes, seed);
}

}  // namespace mk9::seed
