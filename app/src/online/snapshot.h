// MK9 online, F4: snapshots of the game's simulation in guest memory, the base of rollback
// (docs/online/netplay-design.md).
//
// A snapshot is a copy of the guest memory the simulation writes: every committed writable region of the game's heaps
// but the threads' stacks and the kernel's objects (the top 256 MB of the virtual heap, 0x70000000-0x7FFFFFFF, where
// the SDK puts stacks, TLS and its system allocations) and the system physical heap (0xE0000000 up: the kernel's audio
// and GPU buffers). Restoring one writes it all back at a frame's start and marks the pages written for Renut (their
// store cards), so its cached vertex and index copies are proven again: the frames after it run the game again from
// that moment. Both happen with the render thread idle (UE3's FlushRenderingCommands, sub_82747FE0, first), so the
// memory it writes is at rest too.
//
//   mk9_rollback_test=F,K   at frame F's start take a snapshot; at frame F+K's start restore it: the next K frames
//                           run frames F..F+K-1 again, and their checks (mk9_check_log) must repeat

#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

struct PPCContext;

namespace mk9::snapshot {

// UE3's FlushRenderingCommands (sub_82747FE0): waits until the render thread has run every command enqueued (game
// thread; ctx: the hook's context, put back as it was).
void FlushRendering(PPCContext& ctx, uint8_t* base);

class Snapshot {
 public:
  // Copies the simulation's memory now (game thread, at a frame's start).
  void Take(const uint8_t* base);
  // Writes it back and marks its pages written (game thread, at a frame's start).
  void Restore(uint8_t* base) const;
  size_t bytes() const { return bytes_; }
  bool empty() const { return blocks_.empty(); }

 private:
  struct Block {
    uint32_t start = 0;
    std::vector<uint8_t> bytes;
  };
  std::vector<Block> blocks_;
  size_t bytes_ = 0;
};

// A frame's start (the gamepad read's hook, game thread): mk9_rollback_test.
void OnFrame(PPCContext& ctx, uint8_t* base, uint64_t frame);

}  // namespace mk9::snapshot
