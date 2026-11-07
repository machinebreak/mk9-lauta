// MK9 online: loads that end at the same frame on every machine (docs/online/engine-map.md, "Cargas").
//
// UE3 loads packages asynchronously: UObject::LoadPackageAsync (sub_82730F70) queues them in GObjAsyncPackages
// (0x834162A4, a TArray of 96-byte FAsyncPackage), UObject::ProcessAsyncLoading (sub_82728848) works on them a few
// milliseconds per frame, and the async I/O thread reads and decompresses. So a load ends at a frame that depends on
// the machine's disk and CPU, and the game's scripts poll for it (IsAsyncLoading, sub_8270BF88, the native 8246ec38;
// MK9's own loaders, the natives 824730b8 and 824a5268, sit on the same packages): two machines in lockstep then go
// on at different frames. Here every pending load is finished at the start of the next frame, on the game thread
// (UObject::FlushAsyncLoading, sub_8272EC30), so what a frame asks for is ready one frame later on every machine.
//
//   mk9_flush_loads   finish pending loads at each frame's start (on by itself in an online session)

#pragma once

#include <cstdint>

struct PPCContext;

namespace mk9::loading {

// mk9_flush_loads.
bool Enabled();
// At a frame's start (the gamepad read's hook, game thread): finishes every pending load. ctx: the hook's context
// (put back as it was).
void FlushPending(PPCContext& ctx, uint8_t* base);

}  // namespace mk9::loading
