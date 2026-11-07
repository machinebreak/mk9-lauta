// MK9 online, F2: the game's input capture interface for recording and replaying the gamepads frame by frame, and
// per-frame hashes of guest memory for determinism tests (docs/online/engine-map.md, "Entrada").
//
// The game reads its four gamepads once per frame in sub_829D5230: GuardedMain (sub_823667E8) calls it before
// FEngineLoop::Tick, on the game thread. Every block it reads goes to sub_82A1C550(tag, data, size, user): 0x00FF0003
// the XInputGetState result (4 bytes), 0x00FF0002 the raw XINPUT_STATE (16 bytes), 0x00FF0001 the processed buttons
// (12 bytes), per user. The callers read the block again after the call, so it can be replaced in place. The game's
// own object behind it (0x8342D01C, installed by sub_82A1C6D8) is Practice's recorder or the online's gamepads (the
// remote player's blocks replaced with the network's); this hook runs before it.
//
//   mk9_input_record=<file>   every block of every frame, to a file (format below)
//   mk9_input_replay=<file>   the blocks of such a file replace what the controllers gave, frame by frame; the
//                             controllers count again after its last frame
//   mk9_state_hash=<file>     per frame: "<frame> <xxh3 of each range>" for mk9_state_ranges; with
//                             mk9_state_hash_pages one hash per 4 KB page instead (two runs' files say which pages
//                             differ: the way to find the state that is deterministic)
//
// Frame N is the N-th call of sub_829D5230 (1-based). Hashes are taken at its start: the state after frame N-1.
// File format (little-endian): "MK9I", u32 version 1; per frame: u32 frame, u32 block count; per block: u32 tag,
// u32 user, u32 size, the bytes.
//
// New hooks need tools/direct_calls.py --redo (docs/online/README.md, "Enganchar funciones del juego").

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <map>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>

#define XXH_INLINE_ALL
#include <xxhash.h>

REXCVAR_DEFINE_STRING(mk9_input_record, "", "MK9 Online",
                      "Record every gamepad block the game reads (sub_82A1C550), frame by frame, to this file");
REXCVAR_DEFINE_STRING(mk9_input_replay, "", "MK9 Online",
                      "Replay a mk9_input_record file: its blocks replace the controllers' frame by frame");
REXCVAR_DEFINE_STRING(mk9_state_hash, "", "MK9 Online",
                      "Write one line per frame to this file: the frame and an xxh3 of each mk9_state_ranges range");
REXCVAR_DEFINE_STRING(mk9_state_ranges, "83380000-834D0000", "MK9 Online",
                      "Guest ranges for mk9_state_hash, comma-separated hex lo-hi (default: the image's .data/.bss)");
REXCVAR_DEFINE_BOOL(mk9_state_hash_pages, false, "MK9 Online",
                    "mk9_state_hash: one hash per 4 KB page of the ranges instead of one per range");
REXCVAR_DEFINE_INT32(mk9_state_hash_every, 1, "MK9 Online", "mk9_state_hash: every this many frames");

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

void ParseRanges() {
  g_ranges.clear();
  const std::string text = REXCVAR_GET(mk9_state_ranges);
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

void WriteHashes(const uint8_t* base) {
  static const int every = REXCVAR_GET(mk9_state_hash_every) > 0 ? REXCVAR_GET(mk9_state_hash_every) : 1;
  if (g_frame % every) return;
  static const bool pages = REXCVAR_GET(mk9_state_hash_pages);
  std::string line = std::to_string(g_frame);
  char buf[24];
  for (const auto& [lo, hi] : g_ranges) {
    if (pages) {
      for (uint32_t a = lo & ~0xFFFu; a < hi; a += 0x1000) {
        std::snprintf(buf, sizeof(buf), " %016llx", static_cast<unsigned long long>(XXH3_64bits(base + a, 0x1000)));
        line += buf;
      }
    } else {
      std::snprintf(buf, sizeof(buf), " %016llx", static_cast<unsigned long long>(XXH3_64bits(base + lo, hi - lo)));
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
    if (const std::string hash = REXCVAR_GET(mk9_state_hash); Configured(hash)) {
      ParseRanges();
      g_hash_file = std::fopen(hash.c_str(), "w");
      REXLOG_INFO("[capture] state hashes of {} range(s) to {}", g_ranges.size(), hash);
    }
    return true;
  }();
  (void)once;
  FlushRecording();
  ++g_frame;
  g_replay_cursor.clear();
  if (g_hash_file) WriteHashes(base);
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
}  // namespace mk9::online

// The gamepad read of each frame (called from GuardedMain before FEngineLoop::Tick).
extern "C" REX_FUNC(__imp__sub_829D5230);
REX_HOOK_RAW(sub_829D5230) {
  mk9::online::FrameBegin(base);
  __imp__sub_829D5230(ctx, base);
}

// The capture interface's block entry: (tag, guest pointer, size, user).
extern "C" REX_FUNC(__imp__sub_82A1C550);
REX_HOOK_RAW(sub_82A1C550) {
  mk9::online::CaptureBlock(base, ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, ctx.r6.u32);
  __imp__sub_82A1C550(ctx, base);
}
