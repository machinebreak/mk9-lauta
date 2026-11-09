// MK9 online, F4: the simulation's memory over time, the base of rollback (docs/online/netplay-design.md).
//
// The simulation's memory: the guest memory the game writes but the threads' stacks and the kernel's objects (the top
// 256 MB of the virtual heap, 0x70000000-0x7FFFFFFF, where the SDK puts stacks, TLS and its system allocations) and
// the physical heaps (0xA0000000 up: textures, vertex and index data, audio and GPU buffers, Renut's business). That is
// the low heap from 0x00100000, the virtual heap below 0x70000000 and the image's data and heap (0x80000000-0x9FFFFFFF).
//
// Which of its 4 KB pages each frame writes comes from the store cards (tools/store_cards.py): every guest store
// outside the stack, and the SDK's CRT memory functions, zero the byte at guest address ea >> 12 of the first MB of the
// guest window. Renut reads only the physical heaps' cards, so the timeline consumes these: at each frame's start (with
// the render thread idle, UE3's FlushRenderingCommands, sub_82747FE0) the zeroed cards are the pages written since the
// last frame's start; each such page's content then (kept in a shadow copy) goes to that frame's undo record and the
// shadow takes the page's content now. Rolling back to frame F's start writes the undo records back, newest first.
// A store marks its card first and writes after (the vector stores, dcbz, the SDK's CRT functions): when another
// thread (the audio library's mixer) is between the two as the cards are consumed, the page is copied before its new
// bytes land. So each frame also compares the pages the frame before found written; and the pages written with no card
// at all (file reads into the audio library's buffers) are learnt in the first two seconds and compared every frame.
//
//   mk9_rollback_test=F,K   at frame F's start a full snapshot; at frame F+K's start it is restored: the next K frames
//                           run frames F..F+K-1 again, and their checks (mk9_check_log) must repeat
//   mk9_synctest=F,P,K      from frame F on, the timeline runs, and every P frames it rolls back K frames: the game
//                           runs those again, and every frame's checks must equal the first run's (a mismatch is logged
//                           as "[synctest] DIFFERS"): rollback's own test, on one machine

#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

struct PPCContext;

namespace mk9::snapshot {

// UE3's FlushRenderingCommands (sub_82747FE0): waits until the render thread has run every command enqueued (game
// thread; ctx: the hook's context, put back as it was).
void FlushRendering(PPCContext& ctx, uint8_t* base);

// The kernel's objects that live in the simulation's memory (a rollback leaves their bytes as they are: the SDK keeps
// their state), found again when the kernel's object table changed. It takes the kernel's global lock: called with no
// park on (quiesce.h), before a boundary parks; Timeline::RollBack uses what it found. Returns the object table's
// mutation count it is current for.
uint64_t RefreshKernelObjectHoles();
// The holes are current for the object table as it is now.
bool KernelObjectHolesCurrent();

// A full copy of the simulation's memory (mk9_rollback_test).
class Snapshot {
 public:
  void Take(const uint8_t* base);
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

// The simulation's memory frame by frame, from the store cards.
class Timeline {
 public:
  // Starts at frame's start: the shadow copy of every committed page, the cards consumed and, with calibrate, the
  // pages other threads write found (the game thread waits 45 ms for them; mk9_timeline_foreign says whether they are
  // left out). keep: frames of undo records kept.
  void Start(uint8_t* base, uint64_t frame, int keep, bool calibrate = true);
  bool started() const { return started_; }
  // At frame's start (frame = the last one + 1): the pages the frame before wrote go to its undo record.
  void Advance(uint8_t* base, uint64_t frame);
  // Back to frame's start (one of the last kept): returns false if it is too old. The next frame to Advance to is
  // frame + 1 again.
  bool RollBack(uint8_t* base, uint64_t frame);
  // Right after Advance: every shadowed page compared with memory; the ones that changed with a clean card (a writer
  // that marks none: the kernel's clock, file reads into the audio library's buffers) are logged, taken into the
  // shadow and become suspects: from then on every Advance compares them too. Returns how many.
  size_t Verify(uint8_t* base);
  size_t suspect_pages() const { return suspects_.size(); }
  const std::vector<uint32_t>& suspects() const { return suspects_; }
  uint64_t frame() const { return frame_; }
  // statistics
  size_t shadow_pages() const { return shadow_pages_; }
  size_t foreign_pages() const { return foreign_count_; }
  size_t last_dirty() const { return last_dirty_; }
  double ms_refresh() const { return ms_refresh_; }
  uint64_t refreshes() const { return refreshes_; }
  size_t kernel_holes() const { return holes_; }
  size_t last_restored() const { return last_restored_; }

 private:
  struct Undo {
    uint64_t frame = 0;  // the frame whose start these pages' contents were
    std::vector<uint32_t> pages;
    std::vector<uint8_t*> bufs;  // 4 KB each, from the pool
  };
  std::vector<uint32_t> ConsumeCards(uint8_t* base);
  // 4 KB page buffers: the shadow's and the undo records' (an Advance hands the shadow's buffer of a written page to
  // the record and takes a fresh one: one copy per page).
  uint8_t* Take();
  void Give(uint8_t* page);
  // The committed pages of the simulation's memory now (the heaps' allocators commit and decommit at will): a page
  // not committed is neither read nor written, and its shadow goes.
  void RefreshCommitted();

  bool started_ = false;
  uint64_t frame_ = 0;  // the frame whose start the shadow holds
  int keep_ = 8;
  std::vector<uint8_t*> shadow_;                    // by page: 4 KB from the pool, or null outside the memory
  std::vector<std::unique_ptr<uint8_t[]>> chunks_;  // the pool's allocations
  std::vector<uint8_t*> free_;                      // the pool's free pages
  std::vector<uint8_t> foreign_;                    // by page: 1 = another thread writes it, or not the simulation
  std::vector<std::pair<uint32_t, uint32_t>> untracked_;  // what is not the simulation (sound_memory.h)
  size_t untracked_pages_ = 0;
  std::vector<uint32_t> suspects_;                  // pages written with no card marked: compared at every Advance
  std::vector<uint8_t> suspect_;                    // by page: 1 = in suspects_
  std::vector<uint8_t> committed_;                  // by page: 1 = committed at the last RefreshCommitted
  std::vector<uint32_t> recent_;                    // the pages the last Advance found written: compared again
  std::vector<uint8_t> marked_;                     // by page: in this Advance's list (cleared after)
  double ms_refresh_ = 0;
  uint64_t refreshes_ = 0, mutations_ = 0;
  size_t holes_ = 0, last_restored_ = 0;
  std::vector<Undo> undo_;                          // oldest first
  size_t shadow_pages_ = 0, foreign_count_ = 0, last_dirty_ = 0;
};

// A frame's start (the gamepad read's hook, game thread): mk9_rollback_test and mk9_synctest.
void OnFrame(PPCContext& ctx, uint8_t* base, uint64_t frame);
// mk9_synctest under way: the frame the scripted gamepad goes by (mk9_input_script_frames' count when the test's
// timeline started, plus the simulation's frame, which goes back with a rollback: a frame run again is given its first
// run's scripted input, as a session's rollback gives it the same pads); -1 when no test runs.
int64_t SyncTestScriptFrame();
// mk9_synctest with F = 0 starts at the next fight's start (its barrier's hook, game thread) ...
void SyncTestFightStart(uint64_t frame);
// ... and any test stops for good at a package load (UObject::LoadPackageAsync: the fight decided), which a rollback
// cannot undo (the async loader goes on with what the memory forgot).
void SyncTestStop(const char* why);
// A check of the game's (the capture interface's sub_82A1C5C0; game thread): mk9_synctest compares a frame's checks
// when it runs again (tag 9 is the game's frame number since boot, which rolls back with the rest).
void OnCheck(uint32_t tag, uint32_t value);

}  // namespace mk9::snapshot
