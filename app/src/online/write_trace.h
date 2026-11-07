// MK9 online: who writes a guest page (a diagnostic of the timeline's store cards, snapshot.h). The pages given are
// made read-only in the host; the first write to one faults, a vectored exception handler logs the writing host
// thread and its call stack (offsets in the executable: tools/online/stacks_report.py's symbolizer names them, and
// write_trace lines are what tools/online/write_report.py reads), and the page is writable again until Rearm.
// Windows only.

#pragma once

#include <cstdint>
#include <vector>

namespace mk9::write_trace {

// Watches these guest pages (page numbers: address >> 12). base: the guest window.
void Watch(uint8_t* base, const std::vector<uint32_t>& pages);
// Makes the watched pages read-only again (game thread, a frame's start).
void Rearm();
// Stops watching (everything writable again).
void Stop();

}  // namespace mk9::write_trace
