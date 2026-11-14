// Fatal host exceptions/signals that the SDK does not report, logged with the module and offset so the crash can be
// symbolized afterwards. Windows: sa2-nx's vectored handler (app/src/sa2_crash_log.cpp there), and (mk9-nx online,
// 2026-11-09) a last-chance filter: an exception nothing else handled writes a minidump into logs (every thread's
// stack, ~1 MB: the players send it with their log; open it with the build's mk9.pdb). Linux: a signal handler.

#if defined(_WIN32)

#include <windows.h>

#include <dbghelp.h>

#include "mk9_crash_log.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

#include <rex/logging.h>
#include <rex/logging/api.h>

namespace {

LONG CALLBACK LogFatalException(EXCEPTION_POINTERS* info) {
  const DWORD code = info->ExceptionRecord->ExceptionCode;
  switch (code) {
    case EXCEPTION_ILLEGAL_INSTRUCTION:
    case EXCEPTION_PRIV_INSTRUCTION:
    case EXCEPTION_INT_DIVIDE_BY_ZERO:
    case EXCEPTION_STACK_OVERFLOW:
    case EXCEPTION_ARRAY_BOUNDS_EXCEEDED:
    case STATUS_STACK_BUFFER_OVERRUN:
      break;
    default:
      return EXCEPTION_CONTINUE_SEARCH;
  }
  const auto pc = reinterpret_cast<uintptr_t>(info->ExceptionRecord->ExceptionAddress);
  HMODULE module = nullptr;
  char path[MAX_PATH] = "?";
  if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                         reinterpret_cast<LPCSTR>(pc), &module) && module) {
    GetModuleFileNameA(module, path, MAX_PATH);
  }
  const char* name = std::strrchr(path, '\\');
  REXLOG_CRITICAL("[crash] host exception {:08X} at {:016X} ({}+0x{:X}) on thread {}", uint32_t(code), pc,
                  name ? name + 1 : path, pc - reinterpret_cast<uintptr_t>(module), GetCurrentThreadId());
  rex::FlushLogging();
  return EXCEPTION_CONTINUE_SEARCH;
}

// Installed before main, first in the chain; it only logs and never handles.
const PVOID g_handler = AddVectoredExceptionHandler(1, LogFatalException);

// The last chance: an exception no vectored handler or __except took (the SDK's guest memory faults never get here).
// dbghelp is loaded only then, so a game that never crashes never loads it.
LONG WINAPI WriteMinidump(EXCEPTION_POINTERS* info) {
  static volatile LONG once = 0;
  if (InterlockedExchange(&once, 1) != 0) return EXCEPTION_CONTINUE_SEARCH;
  wchar_t exe[MAX_PATH] = {};
  GetModuleFileNameW(nullptr, exe, MAX_PATH);
  if (wchar_t* slash = wcsrchr(exe, L'\\')) slash[1] = 0;
  SYSTEMTIME t;
  GetLocalTime(&t);
  wchar_t path[MAX_PATH] = {};
  swprintf_s(path, L"%slogs\\crash_%04u%02u%02u_%02u%02u%02u.dmp", exe, t.wYear, t.wMonth, t.wDay, t.wHour,
             t.wMinute, t.wSecond);
  bool written = false;
  if (HMODULE dbghelp = LoadLibraryW(L"dbghelp.dll")) {
    using Write = BOOL(WINAPI*)(HANDLE, DWORD, HANDLE, MINIDUMP_TYPE, PMINIDUMP_EXCEPTION_INFORMATION,
                                PMINIDUMP_USER_STREAM_INFORMATION, PMINIDUMP_CALLBACK_INFORMATION);
    const auto write = reinterpret_cast<Write>(GetProcAddress(dbghelp, "MiniDumpWriteDump"));
    const HANDLE file = CreateFileW(path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (write && file != INVALID_HANDLE_VALUE) {
      MINIDUMP_EXCEPTION_INFORMATION exception{GetCurrentThreadId(), info, FALSE};
      written = write(GetCurrentProcess(), GetCurrentProcessId(), file,
                      MINIDUMP_TYPE(MiniDumpWithThreadInfo | MiniDumpWithIndirectlyReferencedMemory |
                                    MiniDumpScanMemory),
                      &exception, nullptr, nullptr) != FALSE;
    }
    if (file != INVALID_HANDLE_VALUE) CloseHandle(file);
  }
  const auto pc = reinterpret_cast<uintptr_t>(info->ExceptionRecord->ExceptionAddress);
  HMODULE module = nullptr;
  GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                     reinterpret_cast<LPCSTR>(pc), &module);
  char name[MAX_PATH] = "?";
  if (module) GetModuleFileNameA(module, name, MAX_PATH);
  const char* short_name = std::strrchr(name, '\\');
  char narrow[MAX_PATH * 3] = {};
  WideCharToMultiByte(CP_UTF8, 0, path, -1, narrow, sizeof(narrow), nullptr, nullptr);
  REXLOG_CRITICAL("[crash] unhandled exception {:08X} at {:016X} ({}+0x{:X}) on thread {}; minidump {}",
                  uint32_t(info->ExceptionRecord->ExceptionCode), pc, short_name ? short_name + 1 : name,
                  module ? pc - reinterpret_cast<uintptr_t>(module) : pc, GetCurrentThreadId(),
                  written ? narrow : "not written");
  rex::FlushLogging();
  return EXCEPTION_CONTINUE_SEARCH;  // then Windows ends the process as before
}
const LPTOP_LEVEL_EXCEPTION_FILTER g_last_chance = SetUnhandledExceptionFilter(WriteMinidump);

// Tests only: MK9_TEST_CRASH=<seconds> crashes the game on purpose that long after start, on a thread of its own (the
// minidump and its log line can be checked).
DWORD WINAPI CrashLater(LPVOID seconds) {
  Sleep(DWORD(reinterpret_cast<uintptr_t>(seconds)) * 1000);
  *static_cast<volatile int*>(nullptr) = 1;
  return 0;
}
const bool g_test_crash = [] {
  char value[16] = {};
  if (GetEnvironmentVariableA("MK9_TEST_CRASH", value, sizeof(value)) == 0) return false;
  const uintptr_t seconds = uintptr_t(std::strtoul(value, nullptr, 10));
  if (HANDLE thread = CreateThread(nullptr, 0, CrashLater, reinterpret_cast<LPVOID>(seconds), 0, nullptr)) {
    CloseHandle(thread);
  }
  return true;
}();

}  // namespace

#endif

// Linux: logs fatal host signals that the SDK does not report (illegal instruction from a __builtin_trap in the
// generated code, divide by zero, ...) with the executable offset, so the crash can be symbolized afterwards:
// llvm-symbolizer --obj=mk9 0x<offset> (or addr2line -e mk9 -f -C 0x<offset>). SIGSEGV/SIGBUS are left alone: the
// SDK handles those itself (guest memory, MMIO) and reports the unhandled ones. The Linux counterpart of sa2-nx's
// Windows vectored handler (app/src/sa2_crash_log.cpp there).

#if defined(__linux__)

#include <dlfcn.h>
#include <execinfo.h>
#include <fcntl.h>
#include <signal.h>
#include <ucontext.h>
#include <unistd.h>

#include <pthread.h>

#include <algorithm>
#include <atomic>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <mutex>

#include <rex/exception_handler.h>
#include <rex/logging.h>
#include <rex/logging/api.h>

#include "mk9_crash_log.h"
#include "mk9_platform.h"
#include "renut_engine/gpu/guest/guarded_read.h"

namespace {

struct sigaction g_previous[32];

void LogFatalSignal(int sig, siginfo_t* info, void* context) {
  uintptr_t pc = 0;
#if defined(__x86_64__)
  pc = static_cast<uintptr_t>(static_cast<ucontext_t*>(context)->uc_mcontext.gregs[REG_RIP]);
#elif defined(__aarch64__)
  pc = static_cast<uintptr_t>(static_cast<ucontext_t*>(context)->uc_mcontext.pc);
#endif
  Dl_info dl{};
  const char* name = "?";
  uintptr_t base = 0;
  if (dladdr(reinterpret_cast<void*>(pc), &dl) && dl.dli_fname) {
    name = std::strrchr(dl.dli_fname, '/') ? std::strrchr(dl.dli_fname, '/') + 1 : dl.dli_fname;
    base = reinterpret_cast<uintptr_t>(dl.dli_fbase);
  }
  REXLOG_CRITICAL("[crash] host signal {} ({}) at {:016X} ({}+0x{:X}), fault address {:016X}, thread {}", sig,
                  strsignal(sig), pc, name, pc - base, reinterpret_cast<uintptr_t>(info->si_addr), gettid());
  rex::FlushLogging();
  // Hand over to whatever was installed before (or the default action: core dump / exit).
  sigaction(sig, &g_previous[sig], nullptr);
  raise(sig);
}

bool Install() {
  for (int sig : {SIGILL, SIGFPE, SIGTRAP, SIGABRT}) {
    struct sigaction sa {};
    sa.sa_sigaction = LogFatalSignal;
    sa.sa_flags = SA_SIGINFO | SA_ONSTACK;
    sigemptyset(&sa.sa_mask);
    sigaction(sig, &sa, &g_previous[sig]);
  }
  return true;
}

// Installed before main; it only logs, then re-raises with the previous action.
const bool g_installed = Install();

// mk9-lauta (2026-11-13): the last chance in the SDK's fault chain. On Linux the SDK's handler of SIGSEGV and SIGILL
// (rex::arch::ExceptionHandler, which takes them over from the handlers above when the runtime is set up) asks its
// handlers in turn and, when none takes the fault, returns to the instruction, which faults again: the game hangs at
// 100 % of a core with nothing in its log. Last in that chain, this one writes the fault and the thread's stack (module
// and offset of each frame) to logs/crash_<date>.txt (Windows' minidump's place) and the log, then gives the signal its
// default action, so the fault, met again at the return, ends the process. A guarded read's fault (renut's
// guarded_read.h) is left to its own handler, before this one in the chain.
char g_logs[1024] = "";  // <the game's folder>/logs, ready before any fault (AfterRuntime)

// write() of a formatted line from a stack buffer (no allocation, no stdio stream).
void Line(int fd, const char* format, ...) {
  char text[512];
  va_list args;
  va_start(args, format);
  const int n = std::vsnprintf(text, sizeof(text), format, args);
  va_end(args);
  if (n > 0) {
    ssize_t ignored = write(fd, text, size_t(std::min(n, int(sizeof(text)) - 1)));
    (void)ignored;
  }
}

bool LastChance(rex::arch::Exception* ex, void*) {
  using Code = rex::arch::Exception::Code;
  if (ex->code() != Code::kAccessViolation && ex->code() != Code::kIllegalInstruction) return false;
  if (renut::gpu::detail::t_guarded_read) return false;
  const int sig = ex->code() == Code::kAccessViolation ? SIGSEGV : SIGILL;
  static thread_local bool t_reporting = false;
  static std::atomic<int> state{0};  // 0: no fault yet, 1: a thread reports one, 2: reported
  if (t_reporting) {  // a fault while this thread reported one: the process ends at once
    signal(sig, SIG_DFL);
    return true;
  }
  t_reporting = true;
  int none = 0;
  if (!state.compare_exchange_strong(none, 1)) {
    // another thread's fault is being reported: this one waits for it (2 s at most) before it ends the process
    for (int i = 0; i < 200 && state.load() == 1; ++i) {
      timespec pause{0, 10 * 1000 * 1000};
      nanosleep(&pause, nullptr);
    }
  } else {
    const uintptr_t pc = uintptr_t(ex->pc());
    Dl_info dl{};
    const char* name = "?";
    uintptr_t base = 0;
    if (dladdr(reinterpret_cast<void*>(pc), &dl) && dl.dli_fname) {
      name = std::strrchr(dl.dli_fname, '/') ? std::strrchr(dl.dli_fname, '/') + 1 : dl.dli_fname;
      base = reinterpret_cast<uintptr_t>(dl.dli_fbase);
    }
    char thread[32] = "?";
    pthread_getname_np(pthread_self(), thread, sizeof(thread));
    char path[1200] = "";
    const std::time_t now = std::time(nullptr);
    std::tm t{};
    localtime_r(&now, &t);
    std::snprintf(path, sizeof(path), "%s/crash_%04d%02d%02d_%02d%02d%02d.txt", g_logs, t.tm_year + 1900,
                  t.tm_mon + 1, t.tm_mday, t.tm_hour, t.tm_min, t.tm_sec);
    const int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (fd >= 0) {
      Line(fd, "mk9-lauta %s: %s at %016lx (%s+0x%lx), fault address %016lx, thread %d (%s)\n", MK9_LAUTA_VERSION,
           sig == SIGSEGV ? "access violation" : "illegal instruction", (unsigned long)pc, name,
           (unsigned long)(pc - base), (unsigned long)ex->fault_address(), int(gettid()), thread);
      void* frames[64];
      const int count = backtrace(frames, 64);
      for (int i = 0; i < count; ++i) {
        Dl_info f{};
        if (dladdr(frames[i], &f) && f.dli_fname) {
          const char* module = std::strrchr(f.dli_fname, '/') ? std::strrchr(f.dli_fname, '/') + 1 : f.dli_fname;
          Line(fd, "#%02d %s+0x%lx %s\n", i, module, (unsigned long)(uintptr_t(frames[i]) - uintptr_t(f.dli_fbase)),
               f.dli_sname ? f.dli_sname : "");
        } else {
          Line(fd, "#%02d %p\n", i, frames[i]);
        }
      }
      close(fd);
    }
    // the log last: a lock the fault left held stops it, not the report above
    REXLOG_CRITICAL("[crash] unhandled {} at {:016X} ({}+0x{:X}), fault address {:016X}, thread {} ({}); stack in {}",
                    sig == SIGSEGV ? "access violation" : "illegal instruction", pc, name, pc - base,
                    ex->fault_address(), gettid(), thread, fd >= 0 ? path : "not written");
    rex::FlushLogging();
    state.store(2);
  }
  signal(sig, SIG_DFL);
  return true;  // the instruction runs again: its fault now ends the process (the default action, a core dump)
}

}  // namespace

namespace mk9::crash_log {

void AfterRuntime() {
  static std::once_flag once;
  std::call_once(once, [] {
    void* warm[1];
    backtrace(warm, 1);  // libgcc's unwinder loaded now, not inside a fault
    std::snprintf(g_logs, sizeof(g_logs), "%s", (mk9::platform::ExeDir() / "logs").c_str());
    renut::gpu::detail::InstallGuardedReadHandler();
    rex::arch::ExceptionHandler::Install(&LastChance, nullptr);
  });
}

}  // namespace mk9::crash_log

#endif

#if !defined(__linux__)  // Windows (its handlers are in place before main), the Switch
#include "mk9_crash_log.h"
namespace mk9::crash_log {
void AfterRuntime() {}
}  // namespace mk9::crash_log
#endif

