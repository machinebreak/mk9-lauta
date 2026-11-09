/**
 * @file    gpu/guest/guest_memory.h
 * @brief   Guest virtual address translation for the renderer.
 */
#pragma once

#include <atomic>

#include <rex/system/kernel_state.h>
#include <rex/types.h>

// mk9-nx (2026-11-09): the SDK's fault handler leaves an access violation inside a marked read to the reader
// (sdk/src/system/xmemory.cpp): not logged, not the guest's crash.
namespace rex::memory {
void BeginQuietGuestFaults() noexcept;
void EndQuietGuestFaults() noexcept;
}  // namespace rex::memory

namespace renut::gpu {

// mk9-nx (2026-11-09): the renderer reads guest memory well after the game named it (vertex, index and texture data,
// on the render thread), and a draw can name memory the game no longer has: a stale buffer header pointing where
// nothing is mapped closed the game in online tests. Such reads run marked (this scope) and under SEH (Windows: the
// ...Guarded helpers of draw.cpp and textures.cpp); one that faults gives zeros (or skips the draw) and is logged by
// NoteGoneGuestRead (draw.cpp).
struct QuietGuestFaultScope {
  QuietGuestFaultScope() { rex::memory::BeginQuietGuestFaults(); }
  ~QuietGuestFaultScope() { rex::memory::EndQuietGuestFaults(); }
  QuietGuestFaultScope(const QuietGuestFaultScope &) = delete;
  QuietGuestFaultScope &operator=(const QuietGuestFaultScope &) = delete;
};
void NoteGoneGuestRead(const char *what, const void *guest, u32 bytes, uint64_t shader_hash);

namespace detail {
// banjo-nx (2026-10-19, render-thread proposal 11): the guest memory bases, read once (they never change after
// Memory::Initialize): one load at a translation instead of the kernel state -> memory -> base chain (603 sites).
inline std::atomic<u8 *> g_guest_vbase{nullptr};
inline std::atomic<u8 *> g_guest_pbase{nullptr};
[[gnu::noinline, gnu::cold]] inline void LoadGuestBases() {
  rex::memory::Memory *memory = REX_KERNEL_MEMORY();
  g_guest_vbase.store(memory->virtual_membase(), std::memory_order_relaxed);
  g_guest_pbase.store(memory->physical_membase(), std::memory_order_relaxed);
}
inline u8 *GuestPhysicalBase() {
  u8 *base = g_guest_pbase.load(std::memory_order_relaxed);
  if (!base) [[unlikely]] {
    LoadGuestBases();
    base = g_guest_pbase.load(std::memory_order_relaxed);
  }
  return base;
}
inline u8 *GuestVirtualBase() {
  u8 *base = g_guest_vbase.load(std::memory_order_relaxed);
  if (!base) [[unlikely]] {
    LoadGuestBases();
    base = g_guest_vbase.load(std::memory_order_relaxed);
  }
  return base;
}
} // namespace detail

// Host pointer for a guest VA, or nullptr for VA 0.
template <typename T>
inline T *GuestPtr(u32 guest_address) {
  if (!guest_address)
    return nullptr;
#if REX_PLATFORM_WIN32 || (REX_PLATFORM_MAC && defined(__aarch64__))
  return REX_KERNEL_MEMORY()->TranslateVirtual<T *>(guest_address);  // heaps with host offsets there
#else
  return reinterpret_cast<T *>(detail::GuestVirtualBase() + guest_address);  // TranslateVirtual's sum (no offsets)
#endif
}

// Host pointer for a GPU physical address (fetch constants, index buffer
// addresses): the low 29 bits index physical memory.
template <typename T>
inline T *GuestPhysicalPtr(u32 physical_address) {
  return reinterpret_cast<T *>(detail::GuestPhysicalBase() + (physical_address & 0x1FFFFFFF));  // TranslatePhysical
}

} // namespace renut::gpu
