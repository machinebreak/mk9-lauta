// MK9 online: the rollback boundary at rest (docs/online/netplay-design.md, "Rollback: el reposo").
//
// A rollback keeps the game's memory at a frame's start and may put it back later. Both need the memory to hold a
// state the game can go on from: no other thread halfway through work whose state lives in that memory. A thread
// halfway through a task when its memory is put back finishes it on the memory it had (or never: the task "pending"
// again, with nobody to run it), and the game waits forever for a task done in a future that was undone.
//
// The engine's work that crosses a frame's start, finished there first (Settle):
//   - the render thread's commands: FlushRenderingCommands (snapshot::FlushRendering);
//   - the thread pools' work (UE3's FQueuedThreadPoolWin: GThreadPool at [0x8341FC5C] and two more, found as their
//     threads hand back their work, sub_827798D8): every queue empty and every thread back in its pool. A render
//     command of each frame (sub_82802EC8 -> sub_827F2D58) hands GThreadPool a task (sub_82802C80) that runs a PhysX
//     batch of scene queries (raycasts) in the background;
//   - those batches: PhysX's NxSceneQuery (SceneQuery.cpp, vtable 0x82275948) in asynchronous mode keeps one pending
//     (execute, sub_830AC338) until the next frame's render command collects it with finish(true) (sub_830AACC0),
//     which waits on its event, then deletes it and the event's handle. Here the boundary collects it, so no frame's
//     memory holds a batch in flight or the handle of an event that a later frame closes.
// Then every other thread that runs guest code is parked at rest (rex/system/guest_park.h: in a kernel wait, held
// there until the boundary is over), so nothing writes the memory while it is kept or put back.
//
// The same work runs, in the same order, on both machines; only when it finishes moves (to the boundary), the same on
// both. Lockstep frames keep nothing and need none of this.

#pragma once

#include <cstdint>

struct PPCContext;

namespace mk9::quiesce {

// Every frame boundary (game thread, any mode): what the boundary learns about the engine (the pools' threads).
void Observe(const uint8_t* base, uint64_t game_frame);
// The boundary at rest (game thread; ctx: the hook's, put back as it was): the render thread flushed, the pools' work
// and the scene query batches finished, the other threads parked. False when some thread stayed busy past the time
// allowed (logged): the boundary goes on unparked. Release ends it either way.
bool Settle(PPCContext& ctx, uint8_t* base);
void Release();
// mk9-lauta (2026-11-13): only Settle's drain (the pools' work and the scene query batches finished): no render flush,
// no park. Game thread; ctx as Settle's.
void DrainPools(PPCContext& ctx, uint8_t* base);

struct Stats {
  uint64_t settles = 0, unparked = 0, collected = 0, pool_waits = 0;
  uint64_t queries_seen = 0;  // scene query objects that ran a batch (the game makes new ones as it goes)
  double ms_total = 0, ms_max = 0;
  double ms_flush = 0, ms_drain = 0, ms_park = 0;  // the parts: the render thread, the pools and queries, the park
};
Stats Totals();

// The hooks (mk9_capture.cpp; any thread): a pool's thread handing back its work (sub_827798D8: pool, thread), and a
// scene query batch started (NxSceneQuery::execute, sub_830AC338: the query object).
void NotePoolThread(uint32_t pool, uint32_t thread);
void NoteSceneQuery(const uint8_t* base, uint32_t query);

}  // namespace mk9::quiesce
