// MK9 online (2026-11-10): the game's heaps in the rollback (docs/online/netplay-design.md).
//
// The timeline (snapshot.h) puts the game's memory back to a frame's start, but not the heaps' bookkeeping: which
// blocks are allocated is the kernel's (the SDK's page tables), outside the game's memory. A fight's frames allocate
// and free blocks through the kernel's and XAM's exports (physical memory for an X-Ray's meshes and textures, XAM's
// and the pool's small blocks): a rollback over them left the game with pointers to blocks freed in the frames it
// undid, or freeing again what was freed already ("PhysicalHeap::Release failed", then the render thread's tree
// broken in sub_82A4F710: the crashes of 150-220 ms of round trip).
//
// So, while a fight runs with rollback, the title's allocations are kept with the frame they ran in, and its frees are
// held (the block stays allocated) until no rollback can reach their frame: a rollback undoes the allocations of the
// frames it runs again (their blocks freed) and forgets the frees held for them (the blocks the game had then are
// still there, with their bytes). The frames run again allocate and free the same way, now kept for them.
//
// The SDK tells (rex::memory::NotifyGuestHeap, sdk/src/system/xmemory.cpp): physical allocs and frees
// (MmAllocatePhysicalMemoryEx / MmFreePhysicalMemory), XAM's and the pool's (XamAlloc / XamFree, ExAllocatePool* /
// ExFreePool) and the virtual ones (NtAllocateVirtualMemory / NtFreeVirtualMemory: counted and logged; the timeline
// already restores their pages' bytes).

#pragma once

#include <cstdint>

namespace mk9::heap_undo {

// The SDK's observer, set once (any thread).
void Install();
// A fight's rollback starts (game thread, at a frame boundary): from here allocations are kept and frees held.
void Begin();
// The frame the game runs from this boundary on (the timeline's numbering: a rollback to frame F undoes what frames
// F and later did).
void SetFrame(int64_t frame);
// Before the timeline goes back to frame 'to' (game thread, the boundary at rest): the allocations of frames >= to
// are freed, the frees held for them forgotten. The renderer's queue is drained first when a block goes.
void RollBack(int64_t to);
// Frames up to 'final_upto' can no longer be undone: their held frees are done now, their allocations stay.
void Confirm(int64_t final_upto);
// The fight's rollback is over (lockstep again, or the session's end): every held free is done.
void End();

struct Stats {
  uint64_t allocations = 0, frees_held = 0, undone = 0, forgotten = 0, freed_late = 0, virtual_ops = 0;
};
Stats Totals();

}  // namespace mk9::heap_undo
