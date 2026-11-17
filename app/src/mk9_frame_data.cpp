// mk9-lauta (2026-11-17): frame data in Practice (mk9_frame_data.h).
//
// How a measure goes, by the attacker's and the other fighter's state at each frame's boundary (fight_state.h
// FighterFrame; the frames counted are the dumps' frames, so the numbers are the ones checked against the tables):
//   start    the attacker has a new attack (a move id it did not have, while held by it): its first frame
//   active   the attack's phase (+0x2F74: 1 its startup, 2 active, 3 its recovery; a contact takes it to 3 at once)
//            leaves the startup: its first active frame. Scorpion's 4 is active a frame before it touches: 21, as
//            the tables say
//   contact  the other fighter is newly hit, newly in a block's reaction (its block animation starts again) or loses
//            health: the first such frame. Hit or block: what the other fighter does at it
//   free     the attacker: its first frame not held (its action's kind without 0x40). The other fighter, hit: the
//            same; blocking: its first frame out of the block's reaction (the block's animation starts again, or it
//            leaves the block)
//   startup = active - start + 1, advantage = other's free - attacker's free, total = attacker's free - start + 1
// An attack that never touches is a whiff (its startup and total). A fighter hit while attacking drops its measure. While the
// game is paused nothing moves: a frame where both fighters' animations stand still is not counted.

#include "mk9_frame_data.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <mutex>
#include <string>

#include "imgui.h"

#include <fmt/format.h>
#include <rex/cvar.h>
#include <rex/logging.h>

#include "online/fight_state.h"
#include "online/i18n.h"
#include "online/netplay.h"
#include "options_ui.h"
#include "renut_engine/gpu/overlay/overlay_ui.h"

REXCVAR_DEFINE_BOOL(mk9_frame_data, true, "MK9",
                    "Practice: each attack's startup and its advantage on hit or on block, in a panel at the right "
                    "(F2 > Controles)");
REXCVAR_DEFINE_BOOL(mk9_frame_data_log, false, "MK9", "Tests: each measured attack in the log ([frame data])");
REXCVAR_DEFINE_BOOL(mk9_frame_data_any_mode, false, "MK9",
                    "Tests: the frame data in every offline fight, not only in Practice");

namespace mk9::frame_data {
namespace {

using fight_state::FighterFrame;
using i18n::T;

constexpr int kModePractice = 2;       // fight_state::Mode
constexpr uint32_t kNoMove = 0xFFFFFFFFu;
constexpr uint64_t kGiveUp = 600;      // frames: a measure not done by then is dropped (a long knockdown, a stuck one)

uint8_t Kind(uint32_t action) { return uint8_t(action >> 8); }
bool Held(uint32_t action) { return (Kind(action) & 0x40) != 0; }         // attacking or hit: it cannot act
bool Hit(uint32_t action) { return (Kind(action) & 0x50) == 0x40; }       // 0x46, 0x47: hit
bool Blocking(uint32_t action) { return Kind(action) == 0x0A; }

struct Result {
  int attacker = -1;     // 0 player 1, 1 player 2
  bool touched = false;  // false: a whiff
  bool blocked = false;
  int startup = 0;
  int advantage = 0;
  int total = 0;
  uint32_t serial = 0;   // counts the results (a new one flashes)
};

struct Measure {
  bool on = false;
  uint32_t move = kNoMove;
  uint64_t start = 0, active = 0, contact = 0, last_contact = 0;
  bool blocked = false;
  uint64_t attacker_free = 0, other_free = 0;
};

// the game thread's
Measure g_measure[2];     // by attacker
FighterFrame g_prev[2];
bool g_have_prev = false;
uint64_t g_frame = 0;     // fight frames counted (not the paused ones)

// the panel's (under g_mutex)
std::mutex g_mutex;
Result g_result;
bool g_practice = false;
std::chrono::steady_clock::time_point g_result_at{};

void Publish(const Result& r) {
  if (REXCVAR_GET(mk9_frame_data_log)) {
    if (r.touched) {
      REXLOG_INFO("[frame data] player {}: startup {}, {} {:+d}, total {}", r.attacker + 1, r.startup,
                  r.blocked ? "on block" : "on hit", r.advantage, r.total);
    } else {
      REXLOG_INFO("[frame data] player {}: whiff, startup {}, total {}", r.attacker + 1, r.startup, r.total);
    }
  }
  std::lock_guard lock(g_mutex);
  const uint32_t serial = g_result.serial + 1;
  g_result = r;
  g_result.serial = serial;
  g_result_at = std::chrono::steady_clock::now();
}

// One attacker's measure, this frame: a the attacker, o the other fighter, ap / op the frame before.
void Step(int attacker, const FighterFrame& a, const FighterFrame& o, const FighterFrame& ap, const FighterFrame& op) {
  Measure& m = g_measure[attacker];
  // a new attack: a move id it did not have (each attack, a string's next hit too, has its own) while held by it. An
  // attack in parts (Scorpion's 4, f4: a step, then the kick) changes its id before it touches: the same attack still
  if (a.move != kNoMove && a.move != ap.move && Held(a.action) && !Hit(a.action)) {
    if (m.on && !m.contact && !m.attacker_free) {
      m.move = a.move;
    } else {
      m = Measure{};
      m.on = true;
      m.move = a.move;
      m.start = g_frame;
    }
  }
  if (!m.on) return;
  if (Hit(a.action) || g_frame - m.start > kGiveUp) {  // hit while attacking (a trade, a counter), or stuck
    m.on = false;
    return;
  }
  if (!m.active && a.move == m.move && (a.attack & 2) && !(ap.attack & 2)) m.active = g_frame;
  // the block's animation starts again at a block's contact and again when its reaction ends: only the first, before
  // any contact, is one
  const bool block_again = Blocking(o.action) && Blocking(op.action) && o.animation < 0.0f && op.animation >= 0.0f;
  const bool touched = (Hit(o.action) && !Hit(op.action)) || o.health < op.health || (!m.contact && block_again);
  if (touched) {
    if (!m.contact) {
      m.contact = g_frame;
      m.blocked = !Hit(o.action) && Blocking(o.action);
      if (!m.active) m.active = g_frame;  // its phase not seen to change (a string's next hit): the contact's
    }
    m.last_contact = g_frame;
    m.other_free = 0;  // its stun starts again
  }
  if (!m.contact) {
    if (!Held(a.action)) {  // free without touching: a whiff
      Result r;
      r.attacker = attacker;
      r.startup = m.active ? int(m.active - m.start) + 1 : 0;
      r.total = int(g_frame - m.start) + 1;
      Publish(r);
      m.on = false;
    }
    return;
  }
  if (!m.attacker_free && !Held(a.action)) m.attacker_free = g_frame;
  if (!m.other_free && g_frame > m.last_contact) {
    const bool free = m.blocked ? (!Blocking(o.action) && !Held(o.action)) || (Blocking(o.action) && o.animation < 0.0f)
                                : !Held(o.action);
    if (free) m.other_free = g_frame;
  }
  if (m.attacker_free && m.other_free) {
    Result r;
    r.attacker = attacker;
    r.touched = true;
    r.blocked = m.blocked;
    r.startup = int(m.active - m.start) + 1;
    r.advantage = int(int64_t(m.other_free) - int64_t(m.attacker_free));
    r.total = int(m.attacker_free - m.start) + 1;
    Publish(r);
    m.on = false;
  }
}

void Leave() {
  g_have_prev = false;
  g_measure[0] = g_measure[1] = Measure{};
  std::lock_guard lock(g_mutex);
  g_practice = false;
  g_result = Result{};
}

// The panel: at the right, where the inputs on screen are at the left (mk9_input_display).
class Panel : public renut::gpu::overlay::Dialog {
 public:
  void Draw() override {
    if (!REXCVAR_GET(mk9_frame_data)) return;
    Result r;
    float age = 0.0f;
    {
      std::lock_guard lock(g_mutex);
      if (!g_practice) return;
      r = g_result;
      age = std::chrono::duration<float>(std::chrono::steady_clock::now() - g_result_at).count();
    }
    const float scale = options_ui::UiScale();
    const ImGuiIO& io = ImGui::GetIO();
    ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x - 12.0f * scale, io.DisplaySize.y * 0.32f), ImGuiCond_Always,
                            ImVec2(1.0f, 0.0f));
    ImGui::SetNextWindowBgAlpha(0.55f);
    const ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
                                   ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoMove |
                                   ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoInputs |
                                   ImGuiWindowFlags_NoNav;
    if (ImGui::Begin("##frame_data", nullptr, flags)) {
      const ImVec4 grey(0.68f, 0.66f, 0.66f, 1.0f), gold(1.0f, 0.80f, 0.30f, 1.0f), text(0.93f, 0.91f, 0.89f, 1.0f);
      ImGui::PushFont(renut::gpu::overlay::StrongFont(), 0.0f);
      ImGui::TextColored(gold, "%s", r.attacker == 1 ? T("FRAME DATA · RIVAL") : T("FRAME DATA"));
      ImGui::PopFont();
      const float column = 120.0f * scale;
      auto row = [&](const char* label, const std::string& value, const ImVec4& color) {
        ImGui::TextColored(grey, "%s", label);
        ImGui::SameLine(column);
        ImGui::PushFont(renut::gpu::overlay::StrongFont(), 0.0f);
        ImGui::TextColored(color, "%s", value.c_str());
        ImGui::PopFont();
      };
      // a new result flashes white for a moment, then takes its colour
      const float flash = std::clamp(1.0f - age / 0.35f, 0.0f, 1.0f);
      auto lit = [&](ImVec4 c) {
        return ImVec4(c.x + (1.0f - c.x) * flash, c.y + (1.0f - c.y) * flash, c.z + (1.0f - c.z) * flash, 1.0f);
      };
      if (r.attacker < 0) {
        ImGui::TextColored(grey, "%s", T("Pegá un golpe"));
      } else if (!r.touched) {
        row(T("Arranque"), r.startup ? std::to_string(r.startup) : std::string("-"), lit(text));
        row(T("Total"), std::to_string(r.total), lit(text));
        ImGui::TextColored(grey, "%s", T("No tocó"));
      } else {
        const ImVec4 plus(0.45f, 0.85f, 0.45f, 1.0f), minus(0.95f, 0.40f, 0.35f, 1.0f);
        row(T("Arranque"), std::to_string(r.startup), lit(text));
        row(T(r.blocked ? "Al bloquear" : "Al golpear"), fmt::format("{:+d}", r.advantage),
            lit(r.advantage > 0 ? plus : r.advantage < 0 ? minus : text));
        row(T("Total"), std::to_string(r.total), lit(text));
      }
    }
    ImGui::End();
  }
};

}  // namespace

void OnFrame(uint8_t* base) {
  const bool wanted = REXCVAR_GET(mk9_frame_data) || REXCVAR_GET(mk9_frame_data_log);
  bool practice = false;
  if (wanted && !netplay::SessionGame() && fight_state::State(base) == fight_state::kStatePlay) {
    const int mode = fight_state::Mode(base);
    practice = mode == kModePractice || (REXCVAR_GET(mk9_frame_data_any_mode) && mode >= 0);
  }
  FighterFrame now[2];
  if (!practice || !fight_state::ReadFighter(base, 0, &now[0]) || !fight_state::ReadFighter(base, 1, &now[1])) {
    if (g_have_prev) Leave();
    return;
  }
  if (!g_have_prev) {
    g_prev[0] = now[0];
    g_prev[1] = now[1];
    g_have_prev = true;
    std::lock_guard lock(g_mutex);
    g_practice = true;
    return;
  }
  // paused (the pause menu, a fight's freeze): nothing moved, the frame is not counted
  if (now[0].animation == g_prev[0].animation && now[1].animation == g_prev[1].animation &&
      now[0].action == g_prev[0].action && now[1].action == g_prev[1].action) {
    return;
  }
  ++g_frame;
  Step(0, now[0], now[1], g_prev[0], g_prev[1]);
  Step(1, now[1], now[0], g_prev[1], g_prev[0]);
  g_prev[0] = now[0];
  g_prev[1] = now[1];
}

void Install() {
  static std::once_flag once;
  std::call_once(once, [] {
    static Panel panel;
    renut::gpu::overlay::AddDialog(&panel);
  });
}

}  // namespace mk9::frame_data
