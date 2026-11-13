/**
 * @file    gpu/guest/guarded_read.h
 *
 * mk9-lauta (2026-11-13): Linux: a read of guest memory that may be gone by the time the renderer reads it (the game
 * freed it after the draw named it: guest_memory.h), as Windows' __try/__except around those reads. The SDK's fault
 * handler (rex::arch::ExceptionHandler: SIGSEGV's there) asks its handlers in turn; this one, while a guarded read runs
 * on its thread, jumps back to it (siglongjmp: the signal mask restored), and the read says false. Only for plain reads
 * (no objects to unwind between the guard and the fault), as on Windows. Callers keep QuietGuestFaultScope around it,
 * so the SDK's own handler stays quiet and passes the fault on.
 */
#pragma once

#if defined(__linux__) && !defined(__SWITCH__)

#include <csetjmp>
#include <mutex>

#include <rex/exception_handler.h>

namespace renut::gpu::detail {

inline thread_local sigjmp_buf *t_guarded_read = nullptr;

inline bool GuardedReadFault(rex::arch::Exception *ex, void *) {
  sigjmp_buf *env = t_guarded_read;
  if (!env || ex->code() != rex::arch::Exception::Code::kAccessViolation)
    return false;
  t_guarded_read = nullptr;
  siglongjmp(*env, 1);
}

// In the SDK's chain once (before mk9's last chance, mk9_crash_log.cpp, which installs it first).
inline void InstallGuardedReadHandler() {
  static std::once_flag installed;
  std::call_once(installed, [] { rex::arch::ExceptionHandler::Install(&GuardedReadFault, nullptr); });
}

// True when read() ran to its end; false when it touched memory that is not there.
template <typename Read> bool GuardedRead(Read &&read) {
  InstallGuardedReadHandler();
  sigjmp_buf env;
  if (sigsetjmp(env, 1) != 0) {
    t_guarded_read = nullptr;
    return false;
  }
  t_guarded_read = &env;
  read();
  t_guarded_read = nullptr;
  return true;
}

} // namespace renut::gpu::detail

#endif
