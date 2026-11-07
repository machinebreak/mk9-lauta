// MK9 online: the guest memory's committed regions (guest_memory.h).

#include "online/guest_memory.h"

#include <algorithm>

#include <rex/system/kernel_state.h>
#include <rex/system/xmemory.h>

namespace mk9::guest {

std::vector<Region> CommittedRegions(bool writable_only) {
  std::vector<Region> out;
  auto* memory = rex::system::kernel_memory();
  if (!memory) return out;
  for (uint32_t heap_base : {0x00000000u, 0x40000000u, 0x80000000u, 0x90000000u, 0xA0000000u, 0xC0000000u,
                             0xE0000000u}) {
    auto* heap = memory->LookupHeap(heap_base == 0 ? 0x00100000u : heap_base);
    if (!heap) continue;
    const uint64_t end = uint64_t(heap->heap_base()) + heap->heap_size();
    uint64_t address = std::max<uint64_t>(heap->heap_base(), heap_base == 0 ? 0x00100000u : 0);
    while (address < end) {
      rex::memory::HeapAllocationInfo info{};
      if (!heap->QueryRegionInfo(uint32_t(address), &info) || info.region_size == 0) break;
      const uint64_t next = uint64_t(info.base_address) + info.region_size;
      const bool committed = (info.state & rex::memory::kMemoryAllocationCommit) != 0;
      const bool writable = (info.protect & rex::memory::kMemoryProtectWrite) != 0;
      if (committed && (writable || !writable_only)) {
        if (!out.empty() && out.back().heap == heap_base && out.back().end == info.base_address &&
            out.back().protect == info.protect) {
          out.back().end = uint32_t(next);
        } else {
          out.push_back({heap_base, info.base_address, uint32_t(next), info.protect});
        }
      }
      if (next <= address) break;
      address = next;
    }
  }
  return out;
}

}  // namespace mk9::guest
