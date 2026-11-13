// MK9 online: the host call stack of the calling thread (host_stack.h).

#include "online/host_stack.h"

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#elif defined(__linux__)
#include <dlfcn.h>
#include <execinfo.h>

#include <algorithm>
#endif

namespace mk9::host_stack {

int Capture(uint64_t* offsets, int max, int skip) {
#if defined(_WIN32)
  void* frames[64];
  if (max > 64) max = 64;
  const int n = RtlCaptureStackBackTrace(DWORD(skip + 1), DWORD(max), frames, nullptr);
  static const auto* module = reinterpret_cast<const uint8_t*>(GetModuleHandleW(nullptr));
  static const uint64_t size = [] {
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(module);
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(module + dos->e_lfanew);
    return uint64_t(nt->OptionalHeader.SizeOfImage);
  }();
  for (int i = 0; i < n; ++i) {
    const uint64_t off = uint64_t(reinterpret_cast<const uint8_t*>(frames[i]) - module);
    offsets[i] = off < size ? off : ~0ull;
  }
  return n;
#elif defined(__linux__)
  // mk9-lauta (2026-11-13): Linux: the same offsets, from the executable's start (its frames; ~0 for the SDK's library's)
  static const uintptr_t exe = [] {
    Dl_info self{};
    return dladdr(reinterpret_cast<void*>(&Capture), &self) ? reinterpret_cast<uintptr_t>(self.dli_fbase) : 0;
  }();
  void* frames[66];
  if (max > 64) max = 64;
  const int got = backtrace(frames, std::min(max + skip + 1, 66));
  int n = 0;
  for (int i = skip + 1; i < got && n < max; ++i, ++n) {
    Dl_info where{};
    const bool ours = exe && dladdr(frames[i], &where) && reinterpret_cast<uintptr_t>(where.dli_fbase) == exe;
    offsets[n] = ours ? uint64_t(reinterpret_cast<uintptr_t>(frames[i]) - exe) : ~0ull;
  }
  return n;
#else
  (void)offsets;
  (void)max;
  (void)skip;
  return 0;
#endif
}

}  // namespace mk9::host_stack
