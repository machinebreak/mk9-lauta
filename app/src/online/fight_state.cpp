// MK9 online: what a fight's state says about the match (fight_state.h).
//
// The match's info (0x833C6C54, engine-map.md) holds a block of 3212 bytes a player, as the PC game's (MK9Hook) with
// other offsets on the 360 (found with mk9_diag_fighters, 2026-11-10): the character's name ("CHAR_SCORPION") at
// +0xB6C, the fighter's object at [+0xDF4], its health a float at +0x78C0 (1 full, 0 out).

#include "online/fight_state.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <cctype>
#include <cstring>
#include <deque>
#include <mutex>
#include <string>

#include <rex/cvar.h>
#include <rex/logging.h>

REXCVAR_DEFINE_BOOL(mk9_diag_fighters, false, "MK9 Online",
                    "Diagnostic: each fighter's character and health in the log every 10 s of an online fight");

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

}  // namespace mk9::fight_state
