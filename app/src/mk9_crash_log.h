// mk9-lauta (2026-11-13): the crash log's part that waits for the runtime (mk9_crash_log.cpp).
#pragma once

namespace mk9::crash_log {

// Once the runtime is set up (Mk9App::OnPostSetup). Linux: the last handler in the SDK's fault chain (after its guest
// memory's, which it installs while the runtime is set up): a fault nobody takes is logged with the thread's stack
// (logs/crash_<date>.txt) and ends the process, as Windows' minidump filter does, instead of faulting again forever.
// Windows: nothing (its handlers are in place before main).
void AfterRuntime();

}  // namespace mk9::crash_log
