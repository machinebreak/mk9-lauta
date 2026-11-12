// MK9 online, F2: the game's input capture interface for recording and replaying the gamepads frame by frame, and
// per-frame hashes of guest memory for determinism tests (docs/online/engine-map.md, "Entrada").
//
// The game reads its four gamepads once per frame in sub_829D5230: GuardedMain (sub_823667E8) calls it before
// FEngineLoop::Tick, on the game thread. Every block it reads goes to sub_82A1C550(tag, data, size, user): 0x00FF0003
// the XInputGetState result (4 bytes), 0x00FF0002 the raw XINPUT_STATE (16 bytes), 0x00FF0001 the processed buttons
// (12 bytes), per user; and, outside the gamepad read, 0x00FF0004 the random seed of a mode starting (4 bytes, user
// 0; seed.h). The callers read the block again after the call, so it can be replaced in place. The game's own object
// behind it (0x8342D01C, installed by sub_82A1C6D8) is Practice's recorder or the online's gamepads (the remote
// player's blocks replaced with the network's); this hook runs before it. Its sibling sub_82A1C5C0(tag, value) hands
// that object values to compare between the two machines: MK9Game.cpp's tick (sub_82382E40) gives tags 9 and 8 (two
// frame counters) and 10 (a draw of the game's random number generator) every frame.
//
//   mk9_input_record=<file>   every block of every frame, to a file (format below)
//   mk9_input_replay=<file>   the blocks of such a file replace what the controllers gave, frame by frame; the
//                             controllers count again after its last frame
//   mk9_check_log=<file>      per frame: "<frame> <tag>=<value> ..." of those checks (hex)
//   mk9_rand_log=<file>       every draw of the game's random number generator (seed.h): "<frame> <thread> <source
//                             file>:<line> <result> <host stack offsets...>" (g = the game thread; the source line is
//                             the one the game passes along; tools/online/rand_report.py names the stacks and compares
//                             two machines' draws)
//   mk9_script_log=<file>     every script function the game's C++ starts (NetherRealm's MKO scripts, sub_82A23D50):
//                             "<frame> <thread> <name>", the flow of menus and fights
//   mk9_native_log=<file>     the script natives called in the frames of mk9_native_log_frames (a-b): "<frame> <native
//                             address> <the VM's result register after it>", one per call (the MKO VM's native call,
//                             sub_82A12118: the native is the word after the instruction, VM at [0x8342CDF0], its pc
//                             at +60, the result register at +28)
//   mk9_state_hash=<file>     per frame: "<frame> <xxh3 of each range>" for mk9_state_ranges; with
//                             mk9_state_hash_pages one hash per 4 KB page instead (two runs' files say which pages
//                             differ: the way to find the state that is deterministic)
//
// Frame N is the N-th call of sub_829D5230 (1-based). Hashes are taken at its start: the state after frame N-1.
// File format (little-endian): "MK9I", u32 version 1; per frame: u32 frame, u32 block count; per block: u32 tag,
// u32 user, u32 size, the bytes.
//
// New hooks need tools/direct_calls.py --redo (docs/online/README.md, "Enganchar funciones del juego").

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/system/guest_park.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xmemory.h>

#include "online/fight_state.h"
#include "online/guest_memory.h"
#include "online/host_stack.h"
#include "online/loading.h"
#include "online/netplay.h"
#include "first_run_ui.h"
#include "mk9_brand.h"
#include "mk9_input_display.h"
#include "online/online_ui.h"
#include "options_ui.h"
#include "online/quiesce.h"
#include "online/seed.h"
#include "online/snapshot.h"
#include "online/virtual_time.h"
#include "mk9_frame_budget.h"
#include "mk9_overlay_input.h"
#include "mk9_renderer_start.h"
#include "mk9_updater.h"

#define XXH_INLINE_ALL
#include <xxhash.h>

REXCVAR_DEFINE_STRING(mk9_input_record, "", "MK9 Online",
                      "Record every gamepad block the game reads (sub_82A1C550), frame by frame, to this file");
REXCVAR_DEFINE_STRING(mk9_input_replay, "", "MK9 Online",
                      "Replay a mk9_input_record file: its blocks replace the controllers' frame by frame");
REXCVAR_DEFINE_STRING(mk9_check_log, "", "MK9 Online",
                      "Write the game's own per-frame sync checks (sub_82A1C5C0: tag and value) to this file, one line "
                      "per frame");
REXCVAR_DEFINE_BOOL(mk9_script_clock, true, "MK9 Online",
                    "In a session the scripts' clock (native 824b75f8, the engine's appSeconds) is the game's frame "
                    "count over 60: the same on both machines (false: each machine's own clock)");
REXCVAR_DEFINE_STRING(mk9_rand_log, "", "MK9 Online",
                      "Write every draw of the game's random number generator to this file (frame, thread, the source "
                      "line the game passes, the result and the host call stack)");
REXCVAR_DEFINE_STRING(mk9_script_log, "", "MK9 Online",
                      "Write every MKO script function the game's C++ starts (frame, thread, name) to this file");
REXCVAR_DEFINE_STRING(mk9_native_log, "", "MK9 Online",
                      "Write the MKO script natives called in the frames of mk9_native_log_frames to this file");
REXCVAR_DEFINE_STRING(mk9_native_log_frames, "", "MK9 Online", "mk9_native_log: the frames, a-b");
REXCVAR_DEFINE_STRING(mk9_state_hash, "", "MK9 Online",
                      "Write one line per frame to this file: the frame and an xxh3 of each mk9_state_ranges range");
REXCVAR_DEFINE_STRING(mk9_state_ranges, "83380000-834D0000", "MK9 Online",
                      "Guest ranges for mk9_state_hash, comma-separated hex lo-hi (default: the image's .data/.bss), "
                      "or \"auto\": every committed writable region of the game's heaps when the hashing starts");
REXCVAR_DEFINE_BOOL(mk9_state_hash_pages, false, "MK9 Online",
                    "mk9_state_hash: one hash per 4 KB page of the ranges instead of one per range");
REXCVAR_DEFINE_INT32(mk9_state_hash_every, 1, "MK9 Online", "mk9_state_hash: every this many frames");
REXCVAR_DEFINE_INT32(mk9_state_hash_from, 1, "MK9 Online",
                     "mk9_state_hash: the first frame hashed (the ranges are taken then, for \"auto\" too)");
REXCVAR_DEFINE_STRING(mk9_state_pages_at, "", "MK9 Online",
                      "mk9_state_hash: at these frames (comma-separated) also write one hash per 4 KB page of the "
                      "ranges to <mk9_state_hash>.pages (\"<frame> <page address> <hash>\"), to compare two runs");
REXCVAR_DEFINE_INT32(mk9_memmap_at, 0, "MK9 Online",
                     "At this frame, write every committed region of the guest heaps to logs/memmap_<frame>.txt "
                     "(heap, start, size, protection); 0 = never");
REXCVAR_DEFINE_STRING(mk9_memdump_at, "", "MK9 Online",
                      "At these frames (comma-separated), write every committed writable region of the guest heaps "
                      "to logs/memdump_<frame>.bin (analysis: tools/online/memsearch.py)");
REXCVAR_DEFINE_STRING(mk9_memdump_prefix, "logs/memdump_", "MK9 Online",
                      "mk9_memdump_at: the dumps' path before \"<frame>.bin\" (two instances need different ones)");

namespace mk9::online {
namespace {

constexpr uint32_t kVersion = 1;

struct Block {
  uint32_t tag, user;
  std::vector<uint8_t> bytes;
};

struct Replay {
  std::vector<std::vector<Block>> frames;  // [frame - 1]
  bool loaded = false;
  uint64_t replaced = 0, mismatched = 0, missing = 0;
};

std::mutex g_mutex;
uint64_t g_frame = 0;               // the current frame (1-based); 0 before the first pad read
std::vector<Block> g_recording;     // this frame's blocks (mk9_input_record)
FILE* g_record_file = nullptr;
FILE* g_hash_file = nullptr;
FILE* g_check_file = nullptr;
std::string g_check_line;           // this frame's checks (mk9_check_log)
FILE* g_rand_file = nullptr;
FILE* g_script_file = nullptr;
FILE* g_native_file = nullptr;
uint64_t g_native_from = 0, g_native_to = 0;
std::thread::id g_game_thread;
Replay g_replay;
std::map<std::pair<uint32_t, uint32_t>, size_t> g_replay_cursor;  // (tag, user) -> next block of this frame
std::vector<std::pair<uint32_t, uint32_t>> g_ranges;

bool Configured(const std::string& value) { return !value.empty(); }

void LoadReplay(const std::string& path) {
  g_replay.loaded = true;
  FILE* f = std::fopen(path.c_str(), "rb");
  if (!f) {
    REXLOG_ERROR("[capture] replay: cannot open {}", path);
    return;
  }
  char magic[4];
  uint32_t version = 0;
  if (std::fread(magic, 1, 4, f) != 4 || std::memcmp(magic, "MK9I", 4) != 0 || std::fread(&version, 4, 1, f) != 1 ||
      version != kVersion) {
    REXLOG_ERROR("[capture] replay: {} is not a version {} input recording", path, kVersion);
    std::fclose(f);
    return;
  }
  uint32_t header[2];
  while (std::fread(header, 4, 2, f) == 2) {
    const uint32_t frame = header[0], count = header[1];
    if (frame == 0 || frame > 10'000'000 || count > 4096) break;
    if (g_replay.frames.size() < frame) g_replay.frames.resize(frame);
    auto& blocks = g_replay.frames[frame - 1];
    for (uint32_t i = 0; i < count; ++i) {
      uint32_t b[3];
      if (std::fread(b, 4, 3, f) != 3 || b[2] > 4096) break;
      Block block{b[0], b[1], std::vector<uint8_t>(b[2])};
      if (b[2] && std::fread(block.bytes.data(), 1, b[2], f) != b[2]) break;
      blocks.push_back(std::move(block));
    }
  }
  std::fclose(f);
  REXLOG_INFO("[capture] replay: {} frames from {}", g_replay.frames.size(), path);
}

// A guest address as the host sees it (the physical views have their own host offsets).
const uint8_t* Host(uint32_t address) {
  return rex::system::kernel_memory()->TranslateVirtual<const uint8_t*>(address);
}

// The committed regions of the guest heaps (guest_memory.h). The first MB of v00000000 holds the store cards
// (tools/store_cards.py), not game state, and is left out.
using guest::CommittedRegions;
using guest::Region;

void DumpMemMap() {
  const auto regions = CommittedRegions(false);
  const std::string path = "logs/memmap_" + std::to_string(g_frame) + ".txt";
  FILE* f = std::fopen(path.c_str(), "w");
  if (!f) return;
  std::map<uint32_t, uint64_t> per_heap;
  for (const Region& r : regions) {
    std::fprintf(f, "%08X %08X-%08X %10u %c%c\n", r.heap, r.start, r.end, r.end - r.start,
                 (r.protect & rex::memory::kMemoryProtectRead) ? 'r' : '-',
                 (r.protect & rex::memory::kMemoryProtectWrite) ? 'w' : '-');
    per_heap[r.heap] += r.end - r.start;
  }
  std::fclose(f);
  std::string summary;
  for (const auto& [heap, bytes] : per_heap) {
    char buf[64];
    std::snprintf(buf, sizeof(buf), " %08X: %.1f MB", heap, bytes / 1048576.0);
    summary += buf;
  }
  REXLOG_INFO("[capture] frame {}: {} committed regions to {};{}", g_frame, regions.size(), path, summary);
}

std::vector<uint64_t> ParseFrames(const std::string& text) {
  std::vector<uint64_t> frames;
  for (size_t pos = 0; pos < text.size();) {
    const size_t end = text.find(',', pos);
    const uint64_t frame = std::strtoull(text.substr(pos, end - pos).c_str(), nullptr, 10);
    if (frame) frames.push_back(frame);
    if (end == std::string::npos) break;
    pos = end + 1;
  }
  return frames;
}

// logs/memdump_<frame>.bin: "MK9D", u32 version 1, u32 region count, then per region u32 start, u32 size and the
// bytes (guest byte order). Taken on the game thread at the frame's start; other threads keep running meanwhile.
void DumpMemory() {
  const auto regions = CommittedRegions(true);
  const std::string path = REXCVAR_GET(mk9_memdump_prefix) + std::to_string(g_frame) + ".bin";
  FILE* f = std::fopen(path.c_str(), "wb");
  if (!f) return;
  const uint32_t header[3] = {0x44394B4Du, 1u, static_cast<uint32_t>(regions.size())};
  std::fwrite(header, 4, 3, f);
  uint64_t total = 0;
  for (const Region& r : regions) {
    const uint32_t h[2] = {r.start, r.end - r.start};
    std::fwrite(h, 4, 2, f);
    std::fwrite(Host(r.start), 1, r.end - r.start, f);
    total += r.end - r.start;
  }
  std::fclose(f);
  REXLOG_INFO("[capture] frame {}: {} regions, {:.1f} MB written to {}", g_frame, regions.size(), total / 1048576.0,
              path);
}

void ParseRanges() {
  g_ranges.clear();
  const std::string text = REXCVAR_GET(mk9_state_ranges);
  if (text == "auto") {
    uint64_t total = 0;
    for (const Region& r : CommittedRegions(true)) {
      g_ranges.emplace_back(r.start, r.end);
      total += r.end - r.start;
    }
    REXLOG_INFO("[capture] state ranges: {} committed writable regions, {:.1f} MB", g_ranges.size(),
                total / 1048576.0);
    return;
  }
  size_t pos = 0;
  while (pos < text.size()) {
    const size_t end = text.find(',', pos);
    const std::string item = text.substr(pos, end == std::string::npos ? std::string::npos : end - pos);
    unsigned lo = 0, hi = 0;
    if (std::sscanf(item.c_str(), "%x-%x", &lo, &hi) == 2 && hi > lo) g_ranges.emplace_back(lo, hi);
    if (end == std::string::npos) break;
    pos = end + 1;
  }
}

FILE* g_pages_file = nullptr;
std::vector<uint64_t> g_pages_at;  // frames whose per-page hashes go to <mk9_state_hash>.pages

void WriteHashes() {
  static const int every = REXCVAR_GET(mk9_state_hash_every) > 0 ? REXCVAR_GET(mk9_state_hash_every) : 1;
  const bool pages_now = g_pages_file && std::find(g_pages_at.begin(), g_pages_at.end(), g_frame) != g_pages_at.end();
  if (pages_now) {
    for (const auto& [lo, hi] : g_ranges) {
      for (uint32_t a = lo & ~0xFFFu; a < hi; a += 0x1000) {
        std::fprintf(g_pages_file, "%llu %08X %016llx\n", static_cast<unsigned long long>(g_frame), a,
                     static_cast<unsigned long long>(XXH3_64bits(Host(a), 0x1000)));
      }
    }
    std::fflush(g_pages_file);
  }
  if (g_frame % every) return;
  static const bool pages = REXCVAR_GET(mk9_state_hash_pages);
  std::string line = std::to_string(g_frame);
  char buf[24];
  for (const auto& [lo, hi] : g_ranges) {
    if (pages) {
      for (uint32_t a = lo & ~0xFFFu; a < hi; a += 0x1000) {
        std::snprintf(buf, sizeof(buf), " %016llx", static_cast<unsigned long long>(XXH3_64bits(Host(a), 0x1000)));
        line += buf;
      }
    } else {
      std::snprintf(buf, sizeof(buf), " %016llx", static_cast<unsigned long long>(XXH3_64bits(Host(lo), hi - lo)));
      line += buf;
    }
  }
  line += '\n';
  std::fwrite(line.data(), 1, line.size(), g_hash_file);
}

void FlushRecording() {
  if (!g_record_file || g_frame == 0) return;
  const uint32_t header[2] = {static_cast<uint32_t>(g_frame), static_cast<uint32_t>(g_recording.size())};
  std::fwrite(header, 4, 2, g_record_file);
  for (const Block& b : g_recording) {
    const uint32_t h[3] = {b.tag, b.user, static_cast<uint32_t>(b.bytes.size())};
    std::fwrite(h, 4, 3, g_record_file);
    if (!b.bytes.empty()) std::fwrite(b.bytes.data(), 1, b.bytes.size(), g_record_file);
  }
  g_recording.clear();
  if ((g_frame % 60) == 0) std::fflush(g_record_file);  // test runs end with a forced stop: lose 1 s at most
}

void FlushChecks() {
  if (!g_check_file || g_check_line.empty()) return;
  const std::string line = std::to_string(g_frame) + g_check_line + '\n';
  std::fwrite(line.data(), 1, line.size(), g_check_file);
  g_check_line.clear();
  if ((g_frame % 60) == 0) std::fflush(g_check_file);
}

void CaptureCheck(uint32_t tag, uint32_t value) {
  if (!g_check_file) return;
  std::lock_guard lock(g_mutex);
  char buf[32];
  std::snprintf(buf, sizeof(buf), " %x=%08x", tag & 0xFFFF, value);
  g_check_line += buf;
}

// A draw of the game's random number generator (mk9_rand_log): file and line are what the caller passed.
void CaptureDraw(const uint8_t* base, uint32_t file, uint32_t line, uint32_t result) {
  if (!g_rand_file) return;
  uint64_t stack[40];
  const int depth = host_stack::Capture(stack, 40, 2);
  std::string source = "?";
  if (file >= 0x82000000u && file < 0x84000000u) {
    const char* text = reinterpret_cast<const char*>(base + file);
    const size_t len = strnlen(text, 260);
    source.assign(text, len);
    if (const size_t slash = source.find_last_of("\\/"); slash != std::string::npos) source.erase(0, slash + 1);
  }
  static thread_local int thread_index = [] {
    static std::atomic<int> next{1};
    return next++;
  }();
  char head[160];
  std::snprintf(head, sizeof(head), "%llu %s%d %s:%u %08x", static_cast<unsigned long long>(g_frame),
                std::this_thread::get_id() == g_game_thread ? "g" : "t", thread_index, source.c_str(), line, result);
  std::string text = head;
  char buf[24];
  for (int i = 0; i < depth; ++i) {
    std::snprintf(buf, sizeof(buf), " %llx", static_cast<unsigned long long>(stack[i]));
    text += buf;
  }
  text += '\n';
  std::lock_guard lock(g_mutex);
  std::fwrite(text.data(), 1, text.size(), g_rand_file);
}

uint32_t Load32(const uint8_t* base, uint32_t addr) {
  const uint8_t* p = base + addr;
  return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3];
}

bool GuestPointer(uint32_t addr) { return addr >= 0x40000000u && addr < 0xF0000000u; }

// A script function started (mk9_script_log): the script object and the function's number (1-based) in its table
// (+32 count, +140 the entries; an entry's first word is its name).
void CaptureScriptCall(const uint8_t* base, uint32_t script, uint32_t index) {
  if (!g_script_file) return;
  std::string name = "?";
  if (GuestPointer(script) && index >= 1 && index <= Load32(base, script + 32)) {
    const uint32_t table = Load32(base, script + 140);
    const uint32_t entry = GuestPointer(table) ? Load32(base, table + (index - 1) * 4) : 0;
    const uint32_t text = GuestPointer(entry) ? Load32(base, entry) : 0;
    if (GuestPointer(text)) {
      const char* chars = reinterpret_cast<const char*>(base + text);
      name.assign(chars, strnlen(chars, 128));
    }
  }
  char head[64];
  std::snprintf(head, sizeof(head), "%llu %s ", static_cast<unsigned long long>(g_frame),
                std::this_thread::get_id() == g_game_thread ? "g" : "t");
  const std::string line = head + name + '\n';
  std::lock_guard lock(g_mutex);
  std::fwrite(line.data(), 1, line.size(), g_script_file);
}

constexpr uint32_t kScriptVm = 0x8342CDF0;

// The script native about to run (0 = none or outside mk9_native_log_frames), and its first argument: the bytecode's
// word after the native's (the native reads it at [[vm + 60] + 4] once the call moved the pc past its own word).
thread_local uint32_t t_native_arg = 0;

uint32_t NativeAboutToRun(const uint8_t* base) {
  if (!g_native_file || g_frame < g_native_from || g_frame > g_native_to) return 0;
  const uint32_t vm = Load32(base, kScriptVm);
  const uint32_t pc = GuestPointer(vm) ? Load32(base, vm + 60) : 0;
  t_native_arg = GuestPointer(pc) ? Load32(base, pc + 8) : 0;
  return GuestPointer(pc) ? Load32(base, pc + 4) : 0;
}

// The guest bytes [addr, addr + n) are mapped and readable: a native's first argument is not always a pointer, and
// a word that looks like one can point at a page nobody committed.
bool Readable(uint32_t addr, uint32_t n) {
  if (!GuestPointer(addr) || !n || addr + n < addr) return false;
  auto* memory = rex::system::kernel_memory();
  auto* heap = memory ? memory->LookupHeap(addr) : nullptr;
  return heap && heap->QueryRangeAccess(addr, addr + n - 1) != rex::memory::PageAccess::kNoAccess;
}

// Up to 40 printable characters at a guest address ("" when it is not text).
std::string GuestText(const uint8_t* base, uint32_t addr) {
  std::string out;
  for (uint32_t i = 0; i < 40; ++i) {
    if ((i == 0 || ((addr + i) & 0xFFF) == 0) && !Readable(addr + i, 1)) return std::string();
    const char c = char(base[addr + i]);
    if (c == 0) break;
    if (c < 32 || c > 126) return std::string();
    out += c;
  }
  return out;
}

// A script native that ran (mk9_native_log), with the VM's result register after it, its first argument and the
// text it points at (directly, or through one pointer: a script string variable).
void CaptureNative(const uint8_t* base, uint32_t native) {
  const uint32_t vm = Load32(base, kScriptVm);
  const uint32_t arg = t_native_arg;
  std::string text = GuestText(base, arg);
  if (text.empty() && Readable(arg, 4)) text = GuestText(base, Load32(base, arg));
  char line[128];
  const int n = std::snprintf(line, sizeof(line), "%llu %08x %x %08x %s\n", static_cast<unsigned long long>(g_frame),
                              native, GuestPointer(vm) ? Load32(base, vm + 28) : 0u, arg, text.c_str());
  std::lock_guard lock(g_mutex);
  std::fwrite(line, 1, size_t(n), g_native_file);
}

// Start of frame N (sub_829D5230's entry): the previous frame's record goes out, the state after it is hashed.
void FrameBegin(const uint8_t* base) {
  std::lock_guard lock(g_mutex);
  static const bool once = [] {
    if (const std::string rec = REXCVAR_GET(mk9_input_record); Configured(rec)) {
      g_record_file = std::fopen(rec.c_str(), "wb");
      if (g_record_file) {
        std::fwrite("MK9I", 1, 4, g_record_file);
        std::fwrite(&kVersion, 4, 1, g_record_file);
        REXLOG_INFO("[capture] recording the gamepad blocks to {}", rec);
      } else {
        REXLOG_ERROR("[capture] cannot write {}", rec);
      }
    }
    if (const std::string play = REXCVAR_GET(mk9_input_replay); Configured(play)) LoadReplay(play);
    if (const std::string checks = REXCVAR_GET(mk9_check_log); Configured(checks)) {
      g_check_file = std::fopen(checks.c_str(), "w");
      if (!g_check_file) REXLOG_ERROR("[capture] cannot write {}", checks);
    }
    if (const std::string scripts = REXCVAR_GET(mk9_script_log); Configured(scripts)) {
      g_script_file = std::fopen(scripts.c_str(), "w");
      if (!g_script_file) REXLOG_ERROR("[capture] cannot write {}", scripts);
    }
    if (const std::string natives = REXCVAR_GET(mk9_native_log); Configured(natives)) {
      g_native_file = std::fopen(natives.c_str(), "w");
      unsigned long long from = 0, to = 0;
      if (std::sscanf(REXCVAR_GET(mk9_native_log_frames).c_str(), "%llu-%llu", &from, &to) == 2) {
        g_native_from = from;
        g_native_to = to;
      }
    }
    if (const std::string draws = REXCVAR_GET(mk9_rand_log); Configured(draws)) {
      g_rand_file = std::fopen(draws.c_str(), "w");
      if (!g_rand_file) REXLOG_ERROR("[capture] cannot write {}", draws);
    }
    if (const std::string hash = REXCVAR_GET(mk9_state_hash); Configured(hash)) {
      g_hash_file = std::fopen(hash.c_str(), "w");
      const std::string at = REXCVAR_GET(mk9_state_pages_at);
      for (size_t pos = 0; pos < at.size();) {
        const size_t end = at.find(',', pos);
        const uint64_t frame = std::strtoull(at.substr(pos, end - pos).c_str(), nullptr, 10);
        if (frame) g_pages_at.push_back(frame);
        if (end == std::string::npos) break;
        pos = end + 1;
      }
      if (!g_pages_at.empty()) g_pages_file = std::fopen((hash + ".pages").c_str(), "w");
      REXLOG_INFO("[capture] state hashes to {} from frame {} ({} frames with page hashes)", hash,
                  REXCVAR_GET(mk9_state_hash_from), g_pages_at.size());
    }
    return true;
  }();
  (void)once;
  (void)base;
  FlushRecording();
  FlushChecks();
  if (g_rand_file && (g_frame % 60) == 0) std::fflush(g_rand_file);
  if (g_script_file && (g_frame % 60) == 0) std::fflush(g_script_file);
  if (g_native_file && (g_frame % 60) == 0) std::fflush(g_native_file);
  g_game_thread = std::this_thread::get_id();
  ++g_frame;
  g_replay_cursor.clear();
  if (const int at = REXCVAR_GET(mk9_memmap_at); at > 0 && g_frame == uint64_t(at)) DumpMemMap();
  static const std::vector<uint64_t> dump_at = ParseFrames(REXCVAR_GET(mk9_memdump_at));
  if (std::find(dump_at.begin(), dump_at.end(), g_frame) != dump_at.end()) DumpMemory();
  if (g_hash_file && g_frame >= uint64_t(std::max(1, REXCVAR_GET(mk9_state_hash_from)))) {
    // The ranges are taken when the hashing starts ("auto": the heaps as they are then).
    static const bool ranges = [] {
      ParseRanges();
      REXLOG_INFO("[capture] hashing {} range(s) from frame {}", g_ranges.size(), g_frame);
      return true;
    }();
    (void)ranges;
    WriteHashes();
  }
  if (g_replay.loaded && g_frame == g_replay.frames.size() + 1) {
    REXLOG_INFO("[capture] replay ended at frame {}: {} blocks replaced, {} mismatched, {} missing", g_frame,
                g_replay.replaced, g_replay.mismatched, g_replay.missing);
  }
  if ((g_frame % 60) == 0 && g_hash_file) std::fflush(g_hash_file);
}

// A block the game has just read (sub_82A1C550): recorded, or replaced from the replay.
void CaptureBlock(uint8_t* base, uint32_t tag, uint32_t data, uint32_t size, uint32_t user) {
  if (size == 0 || size > 4096 || data == 0) return;
  std::lock_guard lock(g_mutex);
  if (g_replay.loaded && g_frame >= 1 && g_frame <= g_replay.frames.size()) {
    const auto& blocks = g_replay.frames[g_frame - 1];
    size_t& cursor = g_replay_cursor[{tag, user}];
    size_t seen = 0;
    const Block* match = nullptr;
    for (const Block& b : blocks) {
      if (b.tag != tag || b.user != user) continue;
      if (seen++ == cursor) {
        match = &b;
        break;
      }
    }
    ++cursor;
    if (!match) {
      ++g_replay.missing;
    } else if (match->bytes.size() != size) {
      ++g_replay.mismatched;
    } else {
      std::memcpy(base + data, match->bytes.data(), size);
      ++g_replay.replaced;
    }
  }
  if (g_record_file) {
    Block b{tag, user, std::vector<uint8_t>(base + data, base + data + size)};
    g_recording.push_back(std::move(b));
  }
}

}  // namespace

uint64_t CurrentFrame() { return g_frame; }

}  // namespace mk9::online

// The gamepad read of each frame (called from GuardedMain before FEngineLoop::Tick): the frame's start for the capture
// tools, then the online session's input exchange (netplay.cpp; outside the capture lock, as it may wait), at its
// first frame the shared seed (seed.h; mk9_seed outside a session), and the pending loads finished (loading.h).
extern "C" REX_FUNC(__imp__sub_829D5230);
REX_HOOK_RAW(sub_829D5230) {
  const auto boundary_start = std::chrono::steady_clock::now();
  mk9::virtual_time::BoundaryStart(base);  // the frame before's logic is over (its clock reads, virtual_time.h)
  mk9::fight_state::DumpFighters(base);    // tests: the fighters' state after it (mk9_fighter_dump)
  mk9::overlay_input::OnFrame();  // the controllers for the overlay's windows (and the combos' frames kept from the game)
  mk9::brand::Install();         // once: the main menu's corner (mk9_brand.h), under the windows
  mk9::first_run_ui::Install();  // once: the first start's steps (before the online window, which waits for them)
  mk9::online_ui::Install();   // once: the online screens (F1)
  mk9::options_ui::Install();  // and the options (F2)
  mk9::input_display::Install();  // and the inputs on screen (mk9_input_display)
  mk9::updater::Install();     // and the updater's banner (a newer release on GitHub)
  mk9::renderer_start::Install();  // and the notice of a graphics API swapped at this start
  mk9::online::FrameBegin(base);
  mk9::quiesce::Observe(base, mk9::online::CurrentFrame());
  mk9::snapshot::OnFrame(ctx, base, mk9::online::CurrentFrame());
  mk9::netplay::OnFrame(ctx, base, mk9::online::CurrentFrame());
  uint32_t seed = 0;
  if (mk9::netplay::TakeSessionSeed(&seed) ||
      (!mk9::netplay::Active() && mk9::seed::FixedSeedFor(mk9::online::CurrentFrame(), &seed))) {
    mk9::seed::Start(base, seed);
  }
  if (mk9::loading::Enabled() || mk9::netplay::Active()) mk9::loading::FlushPending(ctx, base);
  mk9::frame_budget::NoteBoundary(
      std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - boundary_start).count());
  mk9::virtual_time::FrameStart(base);  // the frame's clock reads count from here (after the boundary's own)
  mk9::fight_state::TestDesync(base, mk9::netplay::LocalPlayer());  // tests (mk9_net_test_desync_frame)
  __imp__sub_829D5230(ctx, base);
}

// The game's random number generator (seed.h): an integer (sub_826D91C0) or a float made integer (sub_826D91E8), each
// given the caller's source file and line (r3, r4); mk9_rand_log writes every draw.
extern "C" REX_FUNC(__imp__sub_826D91C0);
REX_HOOK_RAW(sub_826D91C0) {
  const uint32_t file = ctx.r3.u32, line = ctx.r4.u32;
  __imp__sub_826D91C0(ctx, base);
  mk9::online::CaptureDraw(base, file, line, ctx.r3.u32);
}
extern "C" REX_FUNC(__imp__sub_826D91E8);
REX_HOOK_RAW(sub_826D91E8) {
  const uint32_t file = ctx.r3.u32, line = ctx.r4.u32;
  __imp__sub_826D91E8(ctx, base);
  mk9::online::CaptureDraw(base, file, line, ctx.r3.u32);
}

// A fight's start, once it has loaded: the scripts call this native as the stage's scripts start (it loads the
// Toasty! easter egg's Toasty.mko and runs toasty_init: sub_823B3DA0), once per fight. The session's barrier there
// (netplay.cpp) is where the game's own online waited for both players and a "go" (GAME_START_GO_REQUEST,
// sub_824088A0).
extern "C" REX_FUNC(__imp__sub_82495660);
REX_HOOK_RAW(sub_82495660) {
  mk9::netplay::Barrier(base, "fight start");
  mk9::snapshot::SyncTestFightStart(mk9::online::CurrentFrame());
  __imp__sub_82495660(ctx, base);
}

// What a rollback boundary waits for (quiesce.h): a thread pool's thread handing back its work (UE3's
// FQueuedThreadPoolWin::ReturnToPoolOrGetNextJob: r3 the pool, r4 the thread), and a batch of PhysX scene queries
// starting (NxSceneQuery::execute: r3 the query object, whose pending batch the boundary collects).
extern "C" REX_FUNC(__imp__sub_827798D8);
REX_HOOK_RAW(sub_827798D8) {
  mk9::quiesce::NotePoolThread(ctx.r3.u32, ctx.r4.u32);
  __imp__sub_827798D8(ctx, base);
}
extern "C" REX_FUNC(__imp__sub_830AC338);
REX_HOOK_RAW(sub_830AC338) {
  mk9::quiesce::NoteSceneQuery(base, ctx.r3.u32);
  __imp__sub_830AC338(ctx, base);
}

// Tasks a rollback boundary's park must not find a thread halfway through (quiesce.h; rex/system/guest_park.h,
// WorkScope): what FMOD's threads run each turn of their loop (FMOD::Thread, sub_82DEB0B8: a callback at +288 or the
// thread's first virtual function; sound_memory.cpp logs each thread's), and the game's job system (sub_8255F188's
// threads take each job with sub_827783B0, which first finishes off the one before and takes the queue's lock, and
// run it with sub_82778508: a thread parked waiting for that lock held a job of the memory from before a rollback,
// and the queue's lock-free list came out broken).
#define MK9_WORK_HOOK(name)                       extern "C" REX_FUNC(__imp__##name);             REX_HOOK_RAW(name) {                              rex::system::park::WorkScope work;              __imp__##name(ctx, base);                     }
MK9_WORK_HOOK(sub_82E213C8)  // FMOD: the thread waiting on a semaphore (F80001FC at boot)
MK9_WORK_HOOK(sub_82DFB4C8)  // FMOD: a thread's virtual work, every 1 ms
MK9_WORK_HOOK(sub_82DD4310)  // FMOD: a thread's callback, every 10 ms
MK9_WORK_HOOK(sub_82DF9B50)  // FMOD: a semaphore thread started in a fight
MK9_WORK_HOOK(sub_82DDDCB8)  // FMOD: another one
MK9_WORK_HOOK(sub_827783B0)  // the job system: the next job taken
MK9_WORK_HOOK(sub_82778508)  // the job system: one job
#undef MK9_WORK_HOOK

// IsMoviePlaying (GFullScreenMovie at 0x834246A8; the engine's tick and the scripts, native 824747c0, poll it): a
// movie ends on each machine's own clock, so in a session the game sees it end once it has ended on both (Busy).
extern "C" REX_FUNC(__imp__sub_829CE600);
REX_HOOK_RAW(sub_829CE600) {
  __imp__sub_829CE600(ctx, base);
  ctx.r3.u64 = mk9::netplay::Busy(mk9::netplay::kBusyMovie, ctx.r3.u32);
}

namespace mk9::online {
namespace {

// A script native that waits on something ending on each machine's own clock just ran: its answer, in the VM's result
// register, becomes the one both machines agree on (netplay.h, Ready). busy_style: the native says nonzero while not
// done yet (else nonzero once ready).
void AgreeOnNativeResult(uint8_t* base, netplay::ReadyFlag flag, bool busy_style) {
  const uint32_t vm = Load32(base, kScriptVm);
  if (!GuestPointer(vm)) return;
  const uint32_t local = Load32(base, vm + 28);
  uint32_t agreed;
  if (busy_style) {
    agreed = netplay::Ready(flag, local == 0 ? 1u : 0u) ? 0u : (local ? local : 1u);
  } else {
    agreed = netplay::Ready(flag, local);
  }
  uint8_t* result = base + vm + 28;
  result[0] = uint8_t(agreed >> 24);
  result[1] = uint8_t(agreed >> 16);
  result[2] = uint8_t(agreed >> 8);
  result[3] = uint8_t(agreed);
}

// mk9-lauta (2026-11-10): the scripts' clock. Native 824b75f8 answers appSeconds() (the clock, sub_82366BF8, times
// GSecondsPerCycle at 0x833DD0A0), each machine's own wall clock: a menu's script timer (the title's, the main menu's)
// then ran out at different frames on the two machines whenever their frames' wall-clock times differed (lockstep's
// waits, a PC under load), and a spectator catching up (its frames fast, hidden) reached the main menu 209 frames
// after the players. In a session's game it answers the game's frame count over 60 (GuardedMain's counter at
// [0x833848D4]+108, the sync checks' tag 9): the same on both machines and a spectator, and gone back with a
// rollback's memory. From the game's first frame, not the session's: a time a script kept from before the session
// started (each machine's own clock then) made the VS screen's wait end a frame apart on the two machines. With the
// game thread's clock made of its frames (virtual_time.h) the native's own answer is the same already: kept, so the
// scripts and the engine read one clock.
void ScriptClock(uint8_t* base) {
  static const bool on = REXCVAR_GET(mk9_script_clock);
  if (!on || !netplay::SessionGame() || virtual_time::On()) return;
  const uint32_t vm = Load32(base, kScriptVm), game = Load32(base, 0x833848D4);
  if (!GuestPointer(vm) || !GuestPointer(game)) return;
  const float seconds = float(double(Load32(base, game + 108)) / 60.0);
  uint32_t bits;
  std::memcpy(&bits, &seconds, 4);
  uint8_t* result = base + vm + 28;
  result[0] = uint8_t(bits >> 24);
  result[1] = uint8_t(bits >> 16);
  result[2] = uint8_t(bits >> 8);
  result[3] = uint8_t(bits);
}

}  // namespace
}  // namespace mk9::online

// mk9-lauta (2026-11-10): native 8247b658 asks the engine's task holder at 0x8341EF1C (made by FEngineLoop::PreInit,
// vtable 0x82001E58) whether its task is done (+16, sub_826B26E8: no task, or the task's own answer) and, once it is,
// finishes it (+20, sub_826BF3B8: the task's result kept, the task deleted, what waited on it run). The task ends on
// each machine's own clock (the title's START: the profile and its settings read from storage): the scripts went on a
// frame apart on the two machines, and a spectator catching up went on hundreds of frames after the players. In a
// session the task is asked without being finished, the two machines agree (netplay.h, Ready), and the native runs
// (finishing it) at that frame on both; a spectator whose own task is not done by then waits for it (20 s at most).
extern "C" REX_FUNC(__imp__sub_8247B658);
extern "C" REX_FUNC(__imp__sub_826B26E8);
REX_HOOK_RAW(sub_8247B658) {
  constexpr uint32_t kTaskHolder = 0x8341EF1C, kHolderVtable = 0x82001E58;
  const uint32_t holder = mk9::online::Load32(base, kTaskHolder);
  if (mk9::netplay::FrameRunning(nullptr, nullptr) < 0 || !mk9::online::GuestPointer(holder) ||
      mk9::online::Load32(base, holder) != kHolderVtable) {
    __imp__sub_8247B658(ctx, base);
    return;
  }
  auto done = [&] {
    rex::CallFrame ask(ctx);
    ask.ctx.r3.u64 = holder;
    __imp__sub_826B26E8(ask, base);
    return ask.ctx.r3.u32 != 0;
  };
  bool local = done();
  const bool agreed = mk9::netplay::Ready(mk9::netplay::kReadyTask, local ? 1u : 0u) != 0;
  if (agreed) {
    const auto start = std::chrono::steady_clock::now();
    while (!local && std::chrono::steady_clock::now() - start < std::chrono::seconds(20)) {
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
      local = done();
    }
    __imp__sub_8247B658(ctx, base);  // finishes it, and answers 1
    return;
  }
  const uint32_t vm = mk9::online::Load32(base, mk9::online::kScriptVm);
  if (mk9::online::GuestPointer(vm)) std::memset(base + vm + 28, 0, 4);  // not yet, on both
}

extern "C" REX_FUNC(__imp__sub_824B75F8);
REX_HOOK_RAW(sub_824B75F8) {
  __imp__sub_824B75F8(ctx, base);
  mk9::online::ScriptClock(base);
}

// The scripts' waits that end on each machine's own clock (in a session the game sees them end once they have on both
// machines): native 824a56d0, sub_82A311D8 on a thing found by name, the gate from the Versus selection to the fight
// mode (nonzero = ready); native 824a5268, an object found by name, then sub_82A30588 asks the world and the object
// whether the scene is ready, the last wait before a fight starts (nonzero = ready); native 8247bf38, sub_82A2EAB8: the
// engine's streaming still busy (the manager at 0x8341ECF4 behind what the level wants, or another manager's busy
// check; nonzero = busy).
extern "C" REX_FUNC(__imp__sub_824A56D0);
REX_HOOK_RAW(sub_824A56D0) {
  __imp__sub_824A56D0(ctx, base);
  mk9::online::AgreeOnNativeResult(base, mk9::netplay::kReadyModeGate, false);
}
extern "C" REX_FUNC(__imp__sub_824A5268);
REX_HOOK_RAW(sub_824A5268) {
  __imp__sub_824A5268(ctx, base);
  mk9::online::AgreeOnNativeResult(base, mk9::netplay::kReadyScene, false);
}
extern "C" REX_FUNC(__imp__sub_8247BF38);
REX_HOOK_RAW(sub_8247BF38) {
  __imp__sub_8247BF38(ctx, base);
  mk9::online::AgreeOnNativeResult(base, mk9::netplay::kReadyStreaming, true);
}

// The MKO VM's native call: the native's address is in the bytecode after the instruction (mk9_native_log).
extern "C" REX_FUNC(__imp__sub_82A12118);
REX_HOOK_RAW(sub_82A12118) {
  const uint32_t native = mk9::online::NativeAboutToRun(base);
  __imp__sub_82A12118(ctx, base);
  if (native) mk9::online::CaptureNative(base, native);
}

// Starts a function of an MKO script (r3 the script object, r4 the function's number): mk9_script_log.
extern "C" REX_FUNC(__imp__sub_82A23D50);
REX_HOOK_RAW(sub_82A23D50) {
  mk9::online::CaptureScriptCall(base, ctx.r3.u32, ctx.r4.u32);
  __imp__sub_82A23D50(ctx, base);
}

// The capture interface's check entry: (tag, value), the values the game's own online compares between the two
// machines; the session compares them too (netplay.cpp).
extern "C" REX_FUNC(__imp__sub_82A1C5C0);
REX_HOOK_RAW(sub_82A1C5C0) {
  mk9::netplay::OnCheck(ctx.r3.u32, ctx.r4.u32);
  mk9::snapshot::OnCheck(ctx.r3.u32, ctx.r4.u32);
  mk9::online::CaptureCheck(ctx.r3.u32, ctx.r4.u32);
  __imp__sub_82A1C5C0(ctx, base);
}

// The capture interface's block entry: (tag, guest pointer, size, user). The session's inputs and the shared mode
// seed go in first, then the capture tools see (and record) what the game will read.
extern "C" REX_FUNC(__imp__sub_82A1C550);
REX_HOOK_RAW(sub_82A1C550) {
  const uint32_t tag = ctx.r3.u32, data = ctx.r4.u32, size = ctx.r5.u32, user = ctx.r6.u32;
  mk9::netplay::OverrideBlock(base, tag, data, size, user, ctx.r1.u32);
  mk9::seed::OverrideModeSeed(base, tag, data, size);
  mk9::online::CaptureBlock(base, tag, data, size, user);
  __imp__sub_82A1C550(ctx, base);
  mk9::input_display::NoteBlock(base, tag, data, size, user);  // H2: the block as the game takes it
}
