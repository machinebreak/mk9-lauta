// MK9 online: the host call stack of the calling thread, for diagnostics. The game's recompiled functions are C++
// functions (sub_XXXXXXXX), so the host stack is the guest call chain too; tools/online/rand_report.py names the
// addresses with llvm-symbolizer.

#pragma once

#include <cstdint>

namespace mk9::host_stack {

// Up to max return addresses of the caller's callers (skip: frames left out above the caller), as offsets from the
// executable's load address (~0 for addresses outside it). Returns how many.
int Capture(uint64_t* offsets, int max, int skip);

}  // namespace mk9::host_stack
