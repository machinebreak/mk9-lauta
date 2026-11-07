// MK9 online: who writes a guest page (write_trace.h).

#include "online/write_trace.h"

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#endif

#include <atomic>
#include <cstdio>
#include <mutex>
#include <string>
#include <unordered_set>

#include <rex/logging.h>

#include "online/host_stack.h"

namespace mk9::write_trace {
namespace {

std::mutex g_mutex;
uint8_t* g_base = nullptr;
std::vector<uint32_t> g_pages;
std::vector<uint8_t> g_watched;    // by page: 1 = watched
std::vector<uint8_t> g_disarmed;   // by page: 1 = made writable by a fault, until Rearm
std::unordered_set<uint64_t> g_seen;  // (page, writer) pairs logged
void* g_handler = nullptr;
std::atomic<int> g_logged{0};

#if defined(_WIN32)
LONG CALLBACK OnFault(EXCEPTION_POINTERS* info) {
  const EXCEPTION_RECORD* rec = info->ExceptionRecord;
  if (rec->ExceptionCode != EXCEPTION_ACCESS_VIOLATION || rec->NumberParameters < 2 || rec->ExceptionInformation[0] != 1)
    return EXCEPTION_CONTINUE_SEARCH;  // only writes
  const uint8_t* address = reinterpret_cast<const uint8_t*>(rec->ExceptionInformation[1]);
  if (!g_base || address < g_base || address >= g_base + 0x100000000ull) return EXCEPTION_CONTINUE_SEARCH;
  const uint32_t page = uint32_t((address - g_base) >> 12);
  if (page >= g_watched.size() || !g_watched[page]) return EXCEPTION_CONTINUE_SEARCH;
  std::lock_guard lock(g_mutex);
  DWORD old = 0;
  VirtualProtect(g_base + (uint64_t(page) << 12), 4096, PAGE_READWRITE, &old);
  g_disarmed[page] = 1;
  // the writer: the faulting instruction and the stack above it
  uint64_t stack[16];
  const int depth = host_stack::Capture(stack, 16, 0);
  static const auto* module = reinterpret_cast<const uint8_t*>(GetModuleHandleW(nullptr));
  const uint64_t pc = uint64_t(reinterpret_cast<const uint8_t*>(rec->ExceptionAddress) - module);
  if (g_seen.size() < 4096 && g_seen.insert((uint64_t(page) << 32) ^ pc).second && g_logged.fetch_add(1) < 400) {
    std::string text;
    char buf[24];
    for (int i = 0; i < depth; ++i) {
      std::snprintf(buf, sizeof(buf), " %llx", static_cast<unsigned long long>(stack[i]));
      text += buf;
    }
    REXLOG_WARN("[write_trace] page {:08X} written by host thread {} at {:x}; stack{}", page << 12,
                GetCurrentThreadId(), pc, text);
  }
  return EXCEPTION_CONTINUE_EXECUTION;
}
#endif

}  // namespace

void Watch(uint8_t* base, const std::vector<uint32_t>& pages) {
#if defined(_WIN32)
  std::lock_guard lock(g_mutex);
  g_base = base;
  g_pages = pages;
  g_watched.assign(0x100000, 0);
  g_disarmed.assign(0x100000, 0);
  for (uint32_t page : pages) g_watched[page] = 1;
  if (!g_handler) g_handler = AddVectoredExceptionHandler(1, OnFault);
  for (uint32_t page : pages) {
    DWORD old = 0;
    VirtualProtect(base + (uint64_t(page) << 12), 4096, PAGE_READONLY, &old);
  }
  REXLOG_INFO("[write_trace] watching {} pages", pages.size());
#else
  (void)base;
  (void)pages;
#endif
}

void Rearm() {
#if defined(_WIN32)
  if (!g_base) return;
  for (uint32_t page : g_pages) {
    if (!g_disarmed[page]) continue;
    g_disarmed[page] = 0;
    DWORD old = 0;
    VirtualProtect(g_base + (uint64_t(page) << 12), 4096, PAGE_READONLY, &old);
  }
#endif
}

void Stop() {
#if defined(_WIN32)
  if (!g_base) return;
  for (uint32_t page : g_pages) {
    DWORD old = 0;
    VirtualProtect(g_base + (uint64_t(page) << 12), 4096, PAGE_READWRITE, &old);
  }
  g_pages.clear();
  g_watched.assign(g_watched.size(), 0);
#endif
}

}  // namespace mk9::write_trace
