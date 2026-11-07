// MK9 online: the guest memory's committed regions, for the memory dumps of the determinism tools and the
// simulation's snapshots (snapshot.h).

#pragma once

#include <cstdint>
#include <vector>

namespace mk9::guest {

struct Region {
  uint32_t heap, start, end, protect;
};

// Every committed region of the game's heaps (0x00000000 from 0x00100000, 0x40000000, 0x80000000, 0x90000000,
// 0xA0000000, 0xC0000000, 0xE0000000), adjacent ones with the same protection merged; writable_only: only the ones
// the game can write.
std::vector<Region> CommittedRegions(bool writable_only);

}  // namespace mk9::guest
