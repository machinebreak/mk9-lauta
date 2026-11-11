// MK9 online: the game thread's clock in a session (virtual_time.h).

#include "online/virtual_time.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <map>
#include <string>
#include <unordered_map>
#include <vector>

#include <fmt/format.h>
#include <rex/chrono/clock.h>
#include <rex/chrono/guest_tick_filter.h>
#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>

#include "online/host_stack.h"

REXCVAR_DEFINE_BOOL(mk9_virtual_time, true, "MK9 Online",
                    "In a session's game the game thread reads a clock made of its frames (virtual_time.h): the same "
                    "on both machines, and put back by a rollback (false: each machine's own clock; both players need "
                    "the same)");
REXCVAR_DEFINE_STRING(mk9_clock_audit, "", "MK9 Online",
                      "Tests: at each frame's start, the game thread's clock reads of the frame before and who made them "
                      "(host stacks), to this file: \"F <frame> <logic reads> <the boundary's> <in waits> "
                      "<key>:<count>...\" and \"K <key> <offsets>\"");

REXCVAR_DECLARE(std::string, mk9_net_room);
REXCVAR_DECLARE(int32_t, mk9_net_host);
REXCVAR_DECLARE(std::string, mk9_net_join);
REXCVAR_DECLARE(std::string, mk9_synctest);
REXCVAR_DECLARE(std::string, mk9_net_replay);

namespace mk9::virtual_time {
namespace {

constexpr uint32_t kGameObject = 0x833848D4;  // [+108]: GuardedMain's frame counter
// A frame's reads outside the waits beyond which its clock starts going (a loop waiting on the time alone would never
// end on a clock that stands still): the 2026-11-10 audit's most was 1385 (the incremental purge's), the valve is
// logged. Deterministic still (the reads' count is), unless a wait on another thread's time reads it.
constexpr uint64_t kValve = 20000;

// The game thread's only (the filter answers other threads with the real clock).
thread_local bool t_game = false;
bool g_virtual = false;
const uint8_t* g_base = nullptr;  // the guest memory, from the first frame's boundary
uint64_t g_reads = 0;             // the game thread's reads outside the waits since the frame's start
uint64_t g_logic_reads = 0;       // ... of them, the frame logic's (before its next boundary)
uint64_t g_wait_reads = 0;        // reads inside the waits since the frame's start (the audit's)
uint32_t g_boundary_frame = 0;    // the frame counter at the boundary's start (before a rollback puts it back)
bool g_in_boundary = false;
int g_wait_depth = 0;             // in a wait for another thread (WaitZone)
uint64_t g_wait_count = 0;        // the wait's reads so far
uint64_t g_frame_ticks = 0, g_read_ticks = 0, g_base_ticks = 0;
uint64_t g_valve_frames = 0;      // frames whose reads opened the valve

// FMOD's event positions (SoundStarted): an event -> the frame its start was called at (the game thread's only).
std::unordered_map<uint32_t, uint32_t> g_sound_start;
uint64_t g_positions = 0;  // getInfo answers made of frames

// A4 (2026-11-11): the game's callers of KeQuerySystemTime (the host's wall clock, not made of frames), counted when
// the game thread calls them in a session's game: the [time] line says which, the first time each one shows.
std::map<std::string, uint64_t> g_system_time;

// mk9_clock_audit.
FILE* g_audit = nullptr;
std::map<std::array<uint64_t, 4>, uint32_t> g_keys;
std::map<uint32_t, uint32_t> g_frame_keys;  // key -> the frame logic's reads

uint32_t Load32(const uint8_t* base, uint32_t addr) {
  const uint8_t* p = base + addr;
  return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3];
}

uint32_t FrameCounter() {
  if (!g_base) return 0;
  const uint32_t game = Load32(g_base, kGameObject);
  if (game < 0x40000000u || game >= 0xA0000000u) return 0;
  return Load32(g_base, game + 108);
}

void AuditRead() {
  if (g_in_boundary || g_wait_depth) return;  // counted, not by caller
  uint64_t stack[8];
  const int n = host_stack::Capture(stack, 8, 2);  // past the filter and the SDK's QueryGuestTickCount
  std::array<uint64_t, 4> key{};
  int k = 0;
  for (int i = 0; i < n && k < 4; ++i) {
    if (stack[i] == ~0ull) continue;  // outside the executable (the SDK's DLL)
    key[size_t(k++)] = stack[i];
  }
  auto it = g_keys.find(key);
  if (it == g_keys.end()) {
    it = g_keys.emplace(key, uint32_t(g_keys.size())).first;
    std::fprintf(g_audit, "K %u %llx %llx %llx %llx\n", it->second, static_cast<unsigned long long>(key[0]),
                 static_cast<unsigned long long>(key[1]), static_cast<unsigned long long>(key[2]),
                 static_cast<unsigned long long>(key[3]));
  }
  ++g_frame_keys[it->second];
}

// The frame's time: the same for every read of the frame (how many reads a frame makes is not the same on both
// machines: a wait for the render thread's fence reads the clock only when it waits, the incremental purge as long as
// its time lasts), past kValve reads going on by a read's time.
uint64_t FrameTime() {
  const uint64_t time = g_base_ticks + uint64_t(FrameCounter()) * g_frame_ticks;
  return g_reads > kValve ? time + (g_reads - kValve) * g_read_ticks : time;
}

uint64_t Filter(uint64_t real_ticks) {
  if (!t_game) return real_ticks;
  if (g_wait_depth) {
    ++g_wait_reads;
    if (!g_virtual) return real_ticks;
    // a wait for another thread: its own reads go on by a read's time (its async loading's time slices end), from the
    // frame's time; the frame's time again after it
    return FrameTime() + ++g_wait_count * g_read_ticks;
  }
  ++g_reads;
  if (g_audit) AuditRead();
  if (!g_virtual) return real_ticks;
  if (g_reads == kValve + 1) ++g_valve_frames;
  return FrameTime();
}

// A session's game (a player's or a spectator's), or the rollback SyncTest (it stands for one: its frames run again
// must read what an online game's would).
bool SessionGame() {
  return !REXCVAR_GET(mk9_net_room).empty() || REXCVAR_GET(mk9_net_host) > 0 || !REXCVAR_GET(mk9_net_join).empty() ||
         !REXCVAR_GET(mk9_synctest).empty() || !REXCVAR_GET(mk9_net_replay).empty();
}

}  // namespace

bool On() { return g_virtual && t_game; }

bool Enabled() { return g_virtual; }

void NoteSystemTime(const char* caller) {
  if (!t_game) return;
  if (++g_system_time[caller] == 1) {
    uint64_t stack[8];
    const int n = host_stack::Capture(stack, 8, 1);
    std::string where;
    for (int i = 0; i < n; ++i) {
      if (stack[i] != ~0ull) where += fmt::format(" {:x}", stack[i]);
    }
    REXLOG_WARN("[time] the game thread read the host's wall clock (KeQuerySystemTime) through {} at frame {}; host "
                "stack:{}",
                caller, FrameCounter(), where);
  }
}

void SoundStarted(uint32_t event) {
  if (!On() || !event) return;
  g_sound_start[event] = FrameCounter();
}

// FMOD::Event::getInfo's answer (info: FMOD_EVENT_INFO in guest memory, big-endian: +4 positionms, +8 lengthms) for an
// event the game thread started: its position made of the frames since.
void SoundPosition(uint8_t* base, uint32_t event, uint32_t info) {
  auto it = g_sound_start.find(event);
  if (it == g_sound_start.end()) return;
  const int64_t frames = std::max<int64_t>(0, int64_t(FrameCounter()) - int64_t(it->second));
  int64_t ms = frames * 1000 / 60;
  uint8_t* p = base + info;
  const int32_t length = int32_t(uint32_t(p[8]) << 24 | uint32_t(p[9]) << 16 | uint32_t(p[10]) << 8 | p[11]);
  if (length >= 0 && ms > length) ms = length;
  const uint32_t v = uint32_t(ms);
  p[4] = uint8_t(v >> 24);
  p[5] = uint8_t(v >> 16);
  p[6] = uint8_t(v >> 8);
  p[7] = uint8_t(v);
  ++g_positions;
}

WaitZone::WaitZone() {
  if (t_game && g_wait_depth++ == 0) g_wait_count = 0;
}

WaitZone::~WaitZone() {
  if (t_game) --g_wait_depth;
}

void Install() {
  if (!SessionGame()) return;
  const std::string audit = REXCVAR_GET(mk9_clock_audit);
  g_virtual = REXCVAR_GET(mk9_virtual_time);
  if (!audit.empty()) {
    g_audit = std::fopen(audit.c_str(), "w");
    if (!g_audit) REXLOG_ERROR("[time] mk9_clock_audit: cannot write {}", audit);
  }
  if (!g_virtual && !g_audit) return;
  const uint64_t frequency = rex::chrono::Clock::guest_tick_frequency();
  g_frame_ticks = frequency / 60;
  g_read_ticks = std::max<uint64_t>(1, frequency / 100000);  // 10 us
  g_base_ticks = frequency * 1000;                           // 1000 s: the same "uptime" on every machine
  t_game = true;
  rex::chrono::SetGuestTickFilter(&Filter);
  REXLOG_INFO("[time] the game thread's clock: {} ({} ticks a second, {} a frame){}",
              g_virtual ? "its frames (mk9_virtual_time)" : "the host's", frequency, g_frame_ticks,
              g_audit ? "; its reads audited (mk9_clock_audit)" : "");
}

void BoundaryStart(const uint8_t* base) {
  if (!t_game) return;
  g_base = base;
  g_boundary_frame = FrameCounter();
  g_logic_reads = g_reads;
  g_in_boundary = true;
}

void FrameStart(const uint8_t* base) {
  if (!t_game) return;
  g_base = base;
  g_in_boundary = false;
  if (g_audit) {
    // "F <the frame starting (before a rollback)> <the logic's reads before it> <the boundary's> <in waits> <key>:<count>..."
    std::fprintf(g_audit, "F %u %llu %llu %llu", g_boundary_frame, static_cast<unsigned long long>(g_logic_reads),
                 static_cast<unsigned long long>(g_reads - g_logic_reads),
                 static_cast<unsigned long long>(g_wait_reads));
    for (const auto& [key, count] : g_frame_keys) std::fprintf(g_audit, " %u:%u", key, count);
    std::fputc('\n', g_audit);
    g_frame_keys.clear();
    static uint32_t frames = 0;
    if ((++frames & 255) == 0) std::fflush(g_audit);
  }
  static uint64_t reported = 0;
  if (g_valve_frames != reported && g_valve_frames <= 20) {
    reported = g_valve_frames;
    REXLOG_WARN("[time] frame {}: {} reads of the clock outside the waits: past {}, its time went on ({} such frames)",
                FrameCounter(), g_reads, kValve, g_valve_frames);
  }
  // every 600 frames: how many times the game thread reads the clock a frame
  static uint64_t frames = 0, reads = 0, most = 0, waits = 0, most_waits = 0;
  ++frames;
  reads += g_reads;
  most = std::max(most, g_reads);
  waits += g_wait_reads;
  most_waits = std::max(most_waits, g_wait_reads);
  if (frames == 600) {
    uint64_t wall = 0;
    for (const auto& [caller, count] : g_system_time) wall += count;
    REXLOG_INFO("[time] the game thread's clock{}, 600 frames: {:.1f} reads a frame (most {}), {:.0f} a frame in waits "
                "for other threads (most {}); {} frames past the valve so far; {} FMOD event positions made of frames; "
                "the host's wall clock read {} times",
                g_virtual ? " (its frames)" : "", double(reads) / 600.0, most, double(waits) / 600.0, most_waits,
                g_valve_frames, g_positions, wall);
    frames = reads = most = waits = most_waits = 0;
  }
  g_reads = 0;
  g_wait_reads = 0;
}

}  // namespace mk9::virtual_time

// main (sub_823669D8, called by the entry point): the guest's main thread, the game thread, from its first instruction.
extern "C" REX_FUNC(__imp__sub_823669D8);
REX_HOOK_RAW(sub_823669D8) {
  mk9::virtual_time::Install();
  __imp__sub_823669D8(ctx, base);
}

// The tick's wait for UE3's render thread (sub_82360480: until the render command fence passes, with async loading's
// time slices meanwhile): a wait for another thread (WaitZone). The other one, sub_82741630, is mk9_frame_budget.cpp's.
extern "C" REX_FUNC(__imp__sub_82360480);
REX_HOOK_RAW(sub_82360480) {
  mk9::virtual_time::WaitZone wait;
  __imp__sub_82360480(ctx, base);
}

// The game's 7 callers of KeQuerySystemTime (tools/online/import_callers.py): UE3's platform layer (0x8237xxxx) and the
// CRT's time functions (0x82BDxxxx). Counted on the game thread of a session's game (A4).
#define MK9_SYSTEM_TIME_CALLER(name)                \
  extern "C" REX_FUNC(__imp__##name);               \
  REX_HOOK_RAW(name) {                              \
    mk9::virtual_time::NoteSystemTime(#name);       \
    __imp__##name(ctx, base);                       \
  }
MK9_SYSTEM_TIME_CALLER(sub_82373E60)
MK9_SYSTEM_TIME_CALLER(sub_82374208)
MK9_SYSTEM_TIME_CALLER(sub_82374378)
MK9_SYSTEM_TIME_CALLER(sub_82376E98)
MK9_SYSTEM_TIME_CALLER(sub_8237AD50)
MK9_SYSTEM_TIME_CALLER(sub_82BDAE88)
MK9_SYSTEM_TIME_CALLER(sub_82BDD918)
#undef MK9_SYSTEM_TIME_CALLER

// FMOD::Event::getInfo (r3 the event, r4 index, r5 name, r6 the FMOD_EVENT_INFO to fill; r3 back: FMOD_OK = 0): on the
// game thread with the clock made of frames, the event's position made of frames (virtual_time.h, SoundStarted).
extern "C" REX_FUNC(__imp__sub_82E46D40);
REX_HOOK_RAW(sub_82E46D40) {
  const uint32_t event = ctx.r3.u32, info = ctx.r6.u32;
  __imp__sub_82E46D40(ctx, base);
  if (ctx.r3.u32 == 0 && info && mk9::virtual_time::On()) mk9::virtual_time::SoundPosition(base, event, info);
}

// The incremental purge's first pass (sub_82568540, called by IncrementalPurgeGarbage sub_826D3CD8 with its time
// budget): over a list of objects to destroy, it asks each whether it is ready (vtable +44: the render thread has let go
// of its resources) and asks the same one again until it is, reading the clock every 10 asks. How long it spins
// depends on the render thread: a wait for another thread (WaitZone), its budget ending as on the host's clock. On a
// clock that stood still it spun until the render thread let go (the 2026-11-10 audit: up to 17k reads in a frame on
// one machine, a couple on the other; the valve would have opened on one machine only).
extern "C" REX_FUNC(__imp__sub_82568540);
REX_HOOK_RAW(sub_82568540) {
  mk9::virtual_time::WaitZone wait;
  __imp__sub_82568540(ctx, base);
}
