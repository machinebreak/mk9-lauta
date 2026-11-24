// mk9-lauta (2026-11-23): calls of the game's own functions from the host, on the game thread (the frame's start, a
// hook): a copy of the caller's context on a frame of its own below the caller's stack, with scratch memory between
// the two for the arguments the callee reads through pointers (strings, structs). Used by the select screen's 3D
// preview (mk9_select_preview.cpp).
#pragma once

#include <cstdint>
#include <cstring>
#include <initializer_list>
#include <string_view>
#include <utility>

#include <rex/ppc/context.h>
#include <rex/ppc/func.h>

namespace mk9::guest_call {

PPCFunc* Resolve(uint32_t address);  // the recompiled function at a guest address (nullptr: none)

class Caller {
 public:
  // kFrameGap below the caller's stack pointer the callee's frame starts; the scratch is above it.
  static constexpr uint32_t kFrameGap = 0x4000;

  Caller(const PPCContext& caller, uint8_t* base)
      : caller_(caller), base_(base), top_((caller.r1.u32 - 0x100u) & ~0xFu), next_(top_),
        frame_((caller.r1.u32 - kFrameGap) & ~0xFu) {}

  uint8_t* base() const { return base_; }

  // Scratch guest memory (zeroed, 16-byte aligned), valid until the Caller is gone; 0 when it is full.
  uint32_t Alloc(uint32_t size) {
    const uint32_t n = (size + 15u) & ~15u;
    if (next_ - n < frame_ + 0x400u) return 0;
    next_ -= n;
    std::memset(base_ + next_, 0, n);
    return next_;
  }
  uint32_t Ascii(std::string_view s) {  // a NUL-terminated byte string (UE3's TCHAR in this game: FStrings are bytes)
    const uint32_t a = Alloc(uint32_t(s.size()) + 1);
    if (a) std::memcpy(base_ + a, s.data(), s.size());
    return a;
  }
  uint32_t Wide(std::string_view s) {  // a NUL-terminated big-endian UTF-16 string (UE3's TCHAR on the 360)
    const uint32_t a = Alloc(uint32_t(s.size()) * 2 + 2);
    if (a)
      for (size_t i = 0; i < s.size(); ++i) {
        base_[a + 2 * i] = 0;
        base_[a + 2 * i + 1] = uint8_t(s[i]);
      }
    return a;
  }
  void Store32(uint32_t a, uint32_t v) {
    uint8_t* p = base_ + a;
    p[0] = uint8_t(v >> 24), p[1] = uint8_t(v >> 16), p[2] = uint8_t(v >> 8), p[3] = uint8_t(v);
  }
  void StoreFloat(uint32_t a, float f) {
    uint32_t v;
    std::memcpy(&v, &f, 4);
    Store32(a, v);
  }

  // Calls the function at `address` with r3.. from `regs`, f1.. from `fregs` and `stack`'s (offset, word) pairs at the
  // callee's r1 + offset (the caller's parameter area: the ninth argument's word at r1 + 84, then 92, 100, ...).
  // Returns r3 (and f1 in *f1).
  uint32_t Call(uint32_t address, std::initializer_list<uint64_t> regs, std::initializer_list<double> fregs = {},
                std::initializer_list<std::pair<uint32_t, uint32_t>> stack = {}, double* f1 = nullptr) {
    PPCFunc* fn = Resolve(address);
    if (!fn) return 0;
    PPCContext c = caller_;
    c.r1.u64 = frame_;
    for (const auto& [offset, w] : stack) Store32(frame_ + offset, w);
    uint32_t k = 0;
    PPCRegister* const r[8] = {&c.r3, &c.r4, &c.r5, &c.r6, &c.r7, &c.r8, &c.r9, &c.r10};
    k = 0;
    for (uint64_t a : regs)
      if (k < 8) r[k++]->u64 = a;
    PPCRegister* const f[8] = {&c.f1, &c.f2, &c.f3, &c.f4, &c.f5, &c.f6, &c.f7, &c.f8};
    k = 0;
    for (double d : fregs)
      if (k < 8) f[k++]->f64 = d;
    fn(c, base_);
    if (f1) *f1 = c.f1.f64;
    return c.r3.u32;
  }
  // A virtual call: the object's vtable slot `slot` (its word at vtable + 4 * slot).
  uint32_t Virtual(uint32_t object, uint32_t slot, std::initializer_list<uint64_t> more_regs,
                   std::initializer_list<double> fregs = {}) {
    const uint8_t* o = base_ + object;
    const uint32_t vt = uint32_t(o[0]) << 24 | uint32_t(o[1]) << 16 | uint32_t(o[2]) << 8 | o[3];
    const uint8_t* s = base_ + vt + 4 * slot;
    const uint32_t fn = uint32_t(s[0]) << 24 | uint32_t(s[1]) << 16 | uint32_t(s[2]) << 8 | s[3];
    uint64_t regs[8] = {object};
    uint32_t n = 1;
    for (uint64_t a : more_regs)
      if (n < 8) regs[n++] = a;
    return CallArray(fn, regs, n, fregs);
  }

 private:
  uint32_t CallArray(uint32_t address, const uint64_t* regs, uint32_t n, std::initializer_list<double> fregs) {
    switch (n) {
      case 1: return Call(address, {regs[0]}, fregs);
      case 2: return Call(address, {regs[0], regs[1]}, fregs);
      case 3: return Call(address, {regs[0], regs[1], regs[2]}, fregs);
      case 4: return Call(address, {regs[0], regs[1], regs[2], regs[3]}, fregs);
      case 5: return Call(address, {regs[0], regs[1], regs[2], regs[3], regs[4]}, fregs);
      case 6: return Call(address, {regs[0], regs[1], regs[2], regs[3], regs[4], regs[5]}, fregs);
      case 7: return Call(address, {regs[0], regs[1], regs[2], regs[3], regs[4], regs[5], regs[6]}, fregs);
      default: return Call(address, {regs[0], regs[1], regs[2], regs[3], regs[4], regs[5], regs[6], regs[7]}, fregs);
    }
  }

  const PPCContext& caller_;
  uint8_t* base_;
  uint32_t top_, next_, frame_;
};

}  // namespace mk9::guest_call
