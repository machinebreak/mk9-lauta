// MK9 online: what a fight's state says about the match (fight_state.h).
//
// The match's info (0x833C6C54, engine-map.md) holds a block of 3212 bytes a player, as the PC game's (MK9Hook) with
// other offsets on the 360 (found with mk9_diag_fighters, 2026-11-10): the character's name ("CHAR_SCORPION") at
// +0xB6C, the fighter's object at [+0xDF4], its health a float at +0x78C0 (1 full, 0 out).

#include "online/fight_state.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <deque>
#include <map>
#include <mutex>
#include <string>
#include <vector>

#include <fmt/format.h>
#include <rex/cvar.h>
#include <rex/logging.h>
#include <xxhash.h>

REXCVAR_DEFINE_BOOL(mk9_diag_fighters, false, "MK9 Online",
                    "Diagnostic: each fighter's character and health in the log every 10 s of an online fight");
REXCVAR_DEFINE_STRING(mk9_fighter_dump, "", "MK9 Online",
                      "Tests: every mk9_fighter_dump_every frames of a fight, each player's block of the match's info "
                      "and the start of their fighter's object, to this file (tools/online/dev/fighter_diff.py compares "
                      "two machines')");
REXCVAR_DEFINE_INT32(mk9_fighter_dump_every, 10, "MK9 Online", "mk9_fighter_dump: every this many frames");
REXCVAR_DEFINE_INT32(mk9_net_test_desync_frame, 0, "MK9 Online",
                     "Tests: at the start of this game frame (GuardedMain's counter), on every run of it, the guest's "
                     "player 1 fighter loses 5 % of its health: a desync for the sync hash to find (0: never)");

namespace rex::memory {
void BeginQuietGuestFaults() noexcept;
void EndQuietGuestFaults() noexcept;
}  // namespace rex::memory

namespace mk9::fight_state {
namespace {

constexpr uint32_t kGameInfo = 0x833C6C54;  // the match's info (static), engine-map.md
constexpr uint32_t kPlayerStride = 0xC8C;   // a player's block in it
constexpr uint32_t kName = 0xB6C;           // player 1's character name
constexpr uint32_t kObject = 0xDF4;         // player 1's fighter object (a pointer)
constexpr uint32_t kHealth = 0x78C0;        // the health float in a fighter's object
constexpr uint32_t kObjectDump = 0x9000;    // mk9_fighter_dump: a fighter object's first bytes (its health inside)
constexpr uint32_t kActorDump = 0x800;      // and its actor's
constexpr uint32_t kTagMode = 13436;        // nonzero: a tag match (engine-map.md)
// mk9-lauta (2026-11-10): the game object (the one sub_829B02E8 reads), whose +4 is the state: GS_INIT 0,
// GS_LOADING_FIGHT 1, GS_ATTRACT 2, GS_MAINMENU 3, GS_ONLINE 4, GS_KOTH 5, GS_PROFILES 6, GS_PSELECT 7 (the character
// select), GS_PLAY 8, GS_GAME_OVER 9, GS_MOVIE 10, GS_FATALITY 11, GS_LADDER 12, GS_INVALID 13 (sub_829B01D8)
constexpr uint32_t kGameObject = 0x833848D4;
constexpr unsigned long kAccessViolation = 0xC0000005ul;

uint8_t* Raw(uint8_t* base, uint32_t a) { return base + a + (a >= 0xE0000000u ? 0x1000u : 0u); }

bool Read32(uint8_t* base, uint32_t a, uint32_t* out) {
  __try {
    uint32_t v;
    std::memcpy(&v, Raw(base, a), 4);
    *out = _byteswap_ulong(v);
  } __except (GetExceptionCode() == kAccessViolation ? 1 : 0) {
    return false;
  }
  return true;
}

bool ReadByte(uint8_t* base, uint32_t a, char* out) {
  __try {
    *out = char(*Raw(base, a));
  } __except (GetExceptionCode() == kAccessViolation ? 1 : 0) {
    return false;
  }
  return true;
}

struct QuietFaults {
  QuietFaults() { rex::memory::BeginQuietGuestFaults(); }
  ~QuietFaults() { rex::memory::EndQuietGuestFaults(); }
};

std::mutex g_mutex;
std::deque<Result> g_results;

}  // namespace

std::string CharacterName(uint8_t* base, int player) {
  QuietFaults quiet;
  std::string name;
  const uint32_t at = kGameInfo + kName + uint32_t(player) * kPlayerStride;
  for (uint32_t i = 0; i < 40; ++i) {
    char c = 0;
    if (!ReadByte(base, at + i, &c) || c < 32 || c >= 127) break;
    name += c;
  }
  return name;
}

std::string DisplayName(const std::string& game_name) {
  std::string key = game_name;
  if (key.rfind("CHAR_", 0) == 0) key = key.substr(5);
  if (const size_t under = key.find('_'); under != std::string::npos) key = key.substr(0, under);  // a costume
  for (char& c : key) c = char(std::toupper(static_cast<unsigned char>(c)));
  static const struct {
    const char* key;
    const char* name;
  } kNames[] = {{"SUBZERO", "Sub-Zero"},       {"CYBERSUBZERO", "Cyber Sub-Zero"}, {"NOOBSAIBOT", "Noob Saibot"},
                {"LIUKANG", "Liu Kang"},       {"KUNGLAO", "Kung Lao"},            {"JOHNNYCAGE", "Johnny Cage"},
                {"SHANGTSUNG", "Shang Tsung"}, {"QUANCHI", "Quan Chi"},            {"SHAOKAHN", "Shao Kahn"},
                {"FREDDY", "Freddy Krueger"},  {"FREDDYKRUEGER", "Freddy Krueger"}};
  for (const auto& n : kNames) {
    if (key == n.key) return n.name;
  }
  if (key.empty()) return {};
  std::string out = key;
  for (size_t i = 1; i < out.size(); ++i) out[i] = char(std::tolower(static_cast<unsigned char>(out[i])));
  return out;
}

float Health(uint8_t* base, int player) {
  QuietFaults quiet;
  uint32_t object = 0;
  if (!Read32(base, kGameInfo + kObject + uint32_t(player) * kPlayerStride, &object) || object < 0x30000000u ||
      object >= 0xA0000000u) {
    return -1;
  }
  uint32_t bits = 0;
  if (!Read32(base, object + kHealth, &bits)) return -1;
  float health;
  std::memcpy(&health, &bits, 4);
  return health >= 0.0f && health <= 1.0f ? health : -1;
}

int State(uint8_t* base) {
  QuietFaults quiet;
  uint32_t game = 0, state = 0;
  if (!Read32(base, kGameObject, &game) || game < 0x30000000u || game >= 0xA0000000u) return -1;
  if (!Read32(base, game + 4, &state) || state > 13) return -1;
  return int(state);
}

void Report(uint8_t* base, int me) {
  Result r;
  r.me = me;
  for (int p = 0; p < 2; ++p) {
    r.characters[p] = DisplayName(CharacterName(base, p));
    r.health[p] = Health(base, p);
  }
  uint32_t tag = 0;
  {
    QuietFaults quiet;
    r.tag = Read32(base, kGameInfo + kTagMode, &tag) && tag != 0;
  }
  REXLOG_INFO("[fight] the match is decided: {} {:.3f} vs {} {:.3f}{}", r.characters[0], r.health[0], r.characters[1],
              r.health[1], r.tag ? " (tag)" : "");
  std::lock_guard<std::mutex> lock(g_mutex);
  g_results.push_back(std::move(r));
  while (g_results.size() > 8) g_results.pop_front();
}

bool Take(Result* out) {
  std::lock_guard<std::mutex> lock(g_mutex);
  if (g_results.empty()) return false;
  *out = std::move(g_results.front());
  g_results.pop_front();
  return true;
}

void Diagnose(uint8_t* base) {
  if (!REXCVAR_GET(mk9_diag_fighters)) return;
  REXLOG_INFO("[fight] {} {:.3f} vs {} {:.3f}", CharacterName(base, 0), Health(base, 0), CharacterName(base, 1),
              Health(base, 1));
}

namespace {

bool ReadBlock(uint8_t* base, uint32_t a, uint32_t size, uint8_t* out) {
  __try {
    std::memcpy(out, Raw(base, a), size);
  } __except (GetExceptionCode() == kAccessViolation ? 1 : 0) {
    return false;
  }
  return true;
}

}  // namespace

namespace {

// mk9_fighter_dump's file (the game thread's only), and the frame to label a record with when this frame is dumped
// (the game's counter: the same at the boundary's start and at its sync hash), 0 when it is not.
FILE* DumpFile() {
  static FILE* file = nullptr;
  static bool opened = false;
  if (!opened) {
    opened = true;
    const std::string path = REXCVAR_GET(mk9_fighter_dump);
    if (!path.empty() && !(file = std::fopen(path.c_str(), "wb"))) {
      REXLOG_ERROR("[fight] mk9_fighter_dump: cannot write {}", path);
    }
  }
  return file;
}

uint32_t DumpFrame(uint8_t* base) {
  uint32_t game = 0, frame = 0;
  if (!Read32(base, kGameObject, &game) || !Read32(base, game + 108, &frame)) return 0;
  return frame % uint32_t(std::max(1, REXCVAR_GET(mk9_fighter_dump_every))) == 0 ? frame : 0;
}

void DumpRecord(FILE* file, uint32_t frame, uint32_t player, uint32_t kind, uint32_t address, uint32_t size,
                const uint8_t* bytes) {
  const uint32_t header[6] = {0x504D4446u, frame, player, kind, address, size};
  std::fwrite(header, sizeof(header), 1, file);
  std::fwrite(bytes, size, 1, file);
}

}  // namespace

namespace {

// What of the fighters goes into a fight's sync hash (A2): ranges of each player's block of the match's info (kind 0),
// of their fighter's object (1) and of its actor (2), the same on both machines at every frame and holding the fight's
// state (tools/online/dev/fighter_diff.py over mk9_fighter_dump sessions; no pointers: the two machines' heaps are laid
// out apart).
struct HashedRange {
  uint8_t kind;
  uint32_t offset, size;
};
constexpr HashedRange kHashed[] = {
#include "online/fight_hash_ranges.inc"
};

// How much of each block the ranges reach (read once a frame, then hashed from the copy).
constexpr uint32_t Reach(uint8_t kind) {
  uint32_t end = 0;
  for (const auto& r : kHashed) {
    if (r.kind == kind && r.offset + r.size > end) end = r.offset + r.size;
  }
  return end;
}
constexpr uint32_t kReach[3] = {Reach(0), Reach(1), Reach(2)};
static_assert(kReach[0] <= kPlayerStride && kReach[1] <= kObjectDump && kReach[2] <= kActorDump);
constexpr uint32_t RangeBytes() {
  uint32_t n = 0;
  for (const auto& r : kHashed) n += r.size;
  return n;
}
constexpr uint32_t kRangeBytes = RangeBytes();  // a player's hashed bytes

// The latest frames' hashed bytes (A3), by frame (epoch << 32 | frame; a frame run again: its latest run), for a
// desync's report. The game thread's only.
constexpr size_t kKept = 64;
struct KeptFrame {
  uint64_t key = ~0ull;
  uint8_t have[2] = {0, 0};  // per player: bit k, block k was there
  uint8_t bytes[2][kRangeBytes];
};
KeptFrame g_kept[kKept];

}  // namespace

uint64_t HashFighters(uint8_t* base, uint64_t seed, uint64_t key) {
  if (State(base) != kStatePlay) return seed;
  QuietFaults quiet;
  static uint8_t copy[3][kObjectDump];  // the game thread's only
  KeptFrame& kept = g_kept[key % kKept];
  kept.key = key;
  uint64_t h = seed;
  for (uint32_t p = 0; p < 2; ++p) {
    uint32_t at[3] = {kGameInfo + p * kPlayerStride, 0, 0};
    if (Read32(base, at[0] + kObject, &at[1]) && at[1] >= 0x30000000u && at[1] < 0xA0000000u) {
      if (!Read32(base, at[1] + 8, &at[2]) || at[2] < 0x30000000u || at[2] >= 0xA0000000u) at[2] = 0;
    } else {
      at[1] = 0;
    }
    bool have[3];
    kept.have[p] = 0;
    for (int k = 0; k < 3; ++k) {
      have[k] = at[k] && kReach[k] && ReadBlock(base, at[k], kReach[k], copy[k]);
      if (have[k]) kept.have[p] |= uint8_t(1u << k);
    }
    // tests (mk9_fighter_dump): what this moment read, beside the boundary's start (DumpFighters): a word that changed
    // in between was written by another thread or by the boundary's own work, not by the frame
    if (FILE* file = DumpFile()) {
      if (const uint32_t frame = DumpFrame(base)) {
        for (uint32_t k = 0; k < 3; ++k) {
          if (have[k]) DumpRecord(file, frame, p, k + 3, at[k], kReach[k], copy[k]);
        }
      }
    }
    uint8_t* out = kept.bytes[p];
    for (const auto& r : kHashed) {
      if (have[r.kind]) {
        h = XXH3_64bits_withSeed(copy[r.kind] + r.offset, r.size, h);
        std::memcpy(out, copy[r.kind] + r.offset, r.size);
      } else {
        std::memset(out, 0, r.size);
      }
      out += r.size;
    }
  }
  return h;
}

bool WriteFighters(const std::filesystem::path& file, uint64_t upto) {
  FILE* f = _wfopen(file.c_str(), L"wb");
  if (!f) return false;
  // "FHSH", u32 version 1, u32 range count, the ranges (u8 kind, u32 offset, u32 size), then per kept frame of the
  // epoch up to `upto`, in order: u64 key, u8 have[2], player 1's bytes, player 2's (the ranges' order)
  const uint32_t header[3] = {0x48534846u, 1u, uint32_t(std::size(kHashed))};
  std::fwrite(header, sizeof(header), 1, f);
  for (const auto& r : kHashed) {
    std::fwrite(&r.kind, 1, 1, f);
    std::fwrite(&r.offset, 4, 1, f);
    std::fwrite(&r.size, 4, 1, f);
  }
  std::vector<const KeptFrame*> frames;
  for (const auto& k : g_kept) {
    if (k.key != ~0ull && (k.key >> 32) == (upto >> 32) && k.key <= upto) frames.push_back(&k);
  }
  std::sort(frames.begin(), frames.end(), [](const KeptFrame* a, const KeptFrame* b) { return a->key < b->key; });
  for (const KeptFrame* k : frames) {
    std::fwrite(&k->key, 8, 1, f);
    std::fwrite(k->have, 2, 1, f);
    std::fwrite(k->bytes, sizeof(k->bytes), 1, f);
  }
  std::fclose(f);
  return true;
}

namespace {

// SyncTestFighters: the latest frames' whole fighter state (both players: info block, object, actor), by frame.
constexpr uint32_t kWhole = kPlayerStride + kObjectDump + kActorDump;
struct WholeFrame {
  uint32_t key = 0;
  uint8_t have[2] = {0, 0};
  std::vector<uint8_t> bytes = std::vector<uint8_t>(2 * kWhole);
};
WholeFrame g_whole[16];
std::map<std::pair<uint32_t, uint32_t>, uint64_t> g_whole_diffs;  // (kind, offset) -> runs again that differed there
uint64_t g_whole_compared = 0, g_whole_differed = 0;

}  // namespace

void SyncTestFighters(uint8_t* base, uint32_t key) {
  if (!key || State(base) != kStatePlay) return;
  QuietFaults quiet;
  static std::vector<uint8_t> now(2 * kWhole);
  uint8_t have[2] = {0, 0};
  for (uint32_t p = 0; p < 2; ++p) {
    uint8_t* out = now.data() + p * kWhole;
    std::memset(out, 0, kWhole);
    const uint32_t block = kGameInfo + p * kPlayerStride;
    uint32_t object = 0, actor = 0;
    if (ReadBlock(base, block, kPlayerStride, out)) have[p] |= 1;
    if (Read32(base, block + kObject, &object) && object >= 0x30000000u && object < 0xA0000000u) {
      if (ReadBlock(base, object, kObjectDump, out + kPlayerStride)) have[p] |= 2;
      if (Read32(base, object + 8, &actor) && actor >= 0x30000000u && actor < 0xA0000000u &&
          ReadBlock(base, actor, kActorDump, out + kPlayerStride + kObjectDump)) {
        have[p] |= 4;
      }
    }
  }
  WholeFrame& slot = g_whole[key % std::size(g_whole)];
  if (slot.key != key) {  // the first run of this frame (or a slot taken by a newer frame)
    slot.key = key;
    slot.have[0] = have[0];
    slot.have[1] = have[1];
    slot.bytes = now;
    return;
  }
  ++g_whole_compared;
  bool differed = false;
  for (uint32_t p = 0; p < 2; ++p) {
    const uint8_t both = have[p] & slot.have[p];
    const uint32_t starts[3] = {0, kPlayerStride, kPlayerStride + kObjectDump};
    const uint32_t sizes[3] = {kPlayerStride, kObjectDump, kActorDump};
    for (uint32_t k = 0; k < 3; ++k) {
      if (!(both & (1u << k))) continue;
      const uint8_t* a = slot.bytes.data() + p * kWhole + starts[k];
      const uint8_t* b = now.data() + p * kWhole + starts[k];
      if (std::memcmp(a, b, sizes[k]) == 0) continue;
      for (uint32_t o = 0; o + 4 <= sizes[k]; o += 4) {
        if (std::memcmp(a + o, b + o, 4) == 0) continue;
        differed = true;
        if (++g_whole_diffs[{k, o}] == 1 && g_whole_diffs.size() <= 40) {
          const uint32_t x = uint32_t(a[o]) << 24 | uint32_t(a[o + 1]) << 16 | uint32_t(a[o + 2]) << 8 | a[o + 3];
          const uint32_t y = uint32_t(b[o]) << 24 | uint32_t(b[o + 1]) << 16 | uint32_t(b[o + 2]) << 8 | b[o + 3];
          float fx, fy;
          std::memcpy(&fx, &x, 4);
          std::memcpy(&fy, &y, 4);
          REXLOG_WARN("[synctest] fighters: game frame {} ran again with player {}'s {} +{:#x} otherwise: {:08x} ({:g}) "
                      "first, {:08x} ({:g}) now",
                      key, p + 1, k == 0 ? "match info block" : k == 1 ? "fighter object" : "fighter actor", o, x, fx,
                      y, fy);
        }
      }
    }
  }
  if (differed) ++g_whole_differed;
  slot.have[0] = have[0];
  slot.have[1] = have[1];
  slot.bytes = now;  // the latest run is the one a later run again is compared with
}

std::string SyncTestFighterSummary() {
  std::string top;
  std::vector<std::pair<uint64_t, std::pair<uint32_t, uint32_t>>> sorted;
  for (const auto& [where, count] : g_whole_diffs) sorted.push_back({count, where});
  std::sort(sorted.rbegin(), sorted.rend());
  for (size_t i = 0; i < sorted.size() && i < 8; ++i) {
    top += fmt::format(" {}+{:#x} x{}", sorted[i].second.first, sorted[i].second.second, sorted[i].first);
  }
  return fmt::format("fighters: {} frames run again compared, {} with words otherwise ({} words in all){}",
                     g_whole_compared, g_whole_differed, g_whole_diffs.size(), top);
}

void TestDesync(uint8_t* base, int player) {
  static const int at = REXCVAR_GET(mk9_net_test_desync_frame);
  if (at <= 0 || player != 1 || State(base) != kStatePlay) return;
  QuietFaults quiet;
  uint32_t game = 0, frame = 0, object = 0, bits = 0;
  if (!Read32(base, kGameObject, &game) || !Read32(base, game + 108, &frame) || frame != uint32_t(at)) return;
  if (!Read32(base, kGameInfo + kObject, &object) || object < 0x30000000u || object >= 0xA0000000u ||
      !Read32(base, object + kHealth, &bits)) {
    return;
  }
  float health;
  std::memcpy(&health, &bits, 4);
  health = std::max(0.0f, health - 0.05f);
  std::memcpy(&bits, &health, 4);
  bits = _byteswap_ulong(bits);
  __try {
    std::memcpy(Raw(base, object + kHealth), &bits, 4);
  } __except (GetExceptionCode() == kAccessViolation ? 1 : 0) {
    return;
  }
  REXLOG_WARN("[fight] mk9_net_test_desync_frame: player 1's fighter down to {:.3f} at game frame {} (this machine "
              "only)",
              health, frame);
}

void DumpFighters(uint8_t* base) {
  FILE* file = DumpFile();
  if (!file || State(base) != kStatePlay) return;
  QuietFaults quiet;
  const uint32_t frame = DumpFrame(base);
  if (!frame) return;
  // a record: "FDMP", the game's frame, the player, what (0 the match info's block, 1 the fighter's object, 2 its
  // actor; 3-5 the same at the sync hash's moment, HashFighters), its address and size (little-endian), then its
  // bytes as the guest has them
  static uint8_t buffer[kObjectDump];
  auto write = [&](uint32_t player, uint32_t kind, uint32_t address, uint32_t size) {
    if (ReadBlock(base, address, size, buffer)) DumpRecord(file, frame, player, kind, address, size, buffer);
  };
  for (uint32_t p = 0; p < 2; ++p) {
    const uint32_t block = kGameInfo + p * kPlayerStride;
    write(p, 0, block, kPlayerStride);
    uint32_t object = 0, actor = 0;
    if (Read32(base, block + kObject, &object) && object >= 0x30000000u && object < 0xA0000000u) {
      write(p, 1, object, kObjectDump);
      // its actor (object +8; the position at +0xDC, engine-map.md)
      if (Read32(base, object + 8, &actor) && actor >= 0x30000000u && actor < 0xA0000000u) {
        write(p, 2, actor, kActorDump);
      }
    }
  }
  static uint32_t records = 0;
  if ((++records & 63) == 0) std::fflush(file);
}

}  // namespace mk9::fight_state
