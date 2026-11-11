// mk9-lauta (2026-11-10): the guest's tick count, filtered. An online session's game thread reads a clock made of its
// frames (the same on every machine, and gone back with a rollback) instead of the host's. A header of its own, apart
// from clock.h: the generated code's precompiled header includes clock.h (REX_QUERY_TIMEBASE), and a change there would
// rebuild every generated file.

#pragma once

#include <cstdint>

namespace rex::chrono {

// Given the guest tick count Clock::QueryGuestTickCount would return (on the calling thread, any thread), the one it
// returns instead.
using GuestTickFilter = uint64_t (*)(uint64_t real_ticks);

// Sets the filter, or clears it (nullptr): any thread, at any time.
void SetGuestTickFilter(GuestTickFilter filter);

}  // namespace rex::chrono
