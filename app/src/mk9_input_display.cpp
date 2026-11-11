// mk9-lauta (2026-11-11, PLAN-0.4 H2): the inputs on screen in a fight (mk9_input_display.h).

#include "mk9_input_display.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <deque>
#include <mutex>
#include <vector>

#include "imgui.h"

#include <rex/cvar.h>

#include "online/fight_state.h"
#include "online/netplay.h"
#include "options_ui.h"
#include "renut_engine/gpu/overlay/overlay_ui.h"
#include "renut_engine/gpu/sa2_renut_pacing.h"

REXCVAR_DEFINE_BOOL(mk9_input_display, false, "MK9",
                    "The inputs on screen in a fight (directions and buttons as the game got them, with the frames "
                    "each was held): to practise combos");

namespace mk9::input_display {
namespace {

// a user's XINPUT_STATE as XInputGetState gave it (16 bytes, big-endian: the packet number, then the X_INPUT_GAMEPAD).
// Not the processed block (0x00FF0001, 12 bytes): the game lays its buttons out its own way there (a test fight's
// face buttons came out as trigger values).
constexpr uint32_t kRawState = 0x00FF0002;
constexpr uint32_t kGamepadOffset = 4;
constexpr int kStatePlay = 8;                   // GS_PLAY (fight_state.h State)
constexpr size_t kRows = 12;  // between the combo counter and the meter, at 720 lines

// The buttons shown, in this order (an X_INPUT_GAMEPAD bit, or the triggers past 30 of 255).
enum Button : uint16_t {
  kA = 1 << 0, kB = 1 << 1, kX = 1 << 2, kY = 1 << 3, kLB = 1 << 4, kRB = 1 << 5, kLT = 1 << 6, kRT = 1 << 7,
};

struct Entry {
  uint8_t dir = 5;      // numpad notation from the player's screen: 8 up, 2 down, 4 left, 6 right, 5 none
  uint16_t buttons = 0;
  uint32_t frames = 1;  // held so long (live frames)
};

std::mutex g_mutex;
std::deque<Entry> g_history;  // the newest first
bool g_in_fight = false;

uint16_t Be16(const uint8_t* p) { return uint16_t(p[0] << 8 | p[1]); }

// The pad of a frame as the display shows it.
Entry Read(const uint8_t* pad) {
  const uint16_t bits = Be16(pad);
  const uint8_t lt = pad[2], rt = pad[3];
  const int16_t lx = int16_t(Be16(pad + 4)), ly = int16_t(Be16(pad + 6));
  constexpr int kStick = 16384;  // half way: what the fights take as a direction
  const bool up = (bits & 0x0001) || ly > kStick;
  const bool down = (bits & 0x0002) || ly < -kStick;
  const bool left = (bits & 0x0004) || lx < -kStick;
  const bool right = (bits & 0x0008) || lx > kStick;
  Entry e;
  e.dir = uint8_t(5 + (up ? 3 : 0) - (down ? 3 : 0) - (left ? 1 : 0) + (right ? 1 : 0));
  e.buttons = uint16_t((bits & 0x1000 ? kA : 0) | (bits & 0x2000 ? kB : 0) | (bits & 0x4000 ? kX : 0) |
                       (bits & 0x8000 ? kY : 0) | (bits & 0x0100 ? kLB : 0) | (bits & 0x0200 ? kRB : 0) |
                       (lt > 30 ? kLT : 0) | (rt > 30 ? kRT : 0));
  return e;
}

// An arrow in a square of side s at p (dir in numpad notation; 5: a dot).
void DrawArrow(ImDrawList* list, ImVec2 p, float s, uint8_t dir, ImU32 color) {
  const ImVec2 c(p.x + s * 0.5f, p.y + s * 0.5f);
  if (dir == 5) {
    list->AddCircleFilled(c, s * 0.12f, IM_COL32(150, 140, 135, 255));
    return;
  }
  // numpad: 1-3 the bottom row (down), 7-9 the top one (up): -1, 0, 1 with y up
  const float dx = float((dir - 1) % 3) - 1.0f, dy = float((dir - 1) / 3) - 1.0f;
  const float len = std::sqrt(dx * dx + dy * dy);
  const ImVec2 d(dx / len, -dy / len);  // screen: y down
  const ImVec2 n(-d.y, d.x);
  const float r = s * 0.40f;
  const ImVec2 tip(c.x + d.x * r, c.y + d.y * r);
  const ImVec2 back(c.x - d.x * r, c.y - d.y * r);
  const ImVec2 neck(c.x + d.x * r * 0.05f, c.y + d.y * r * 0.05f);
  list->AddLine(back, neck, color, s * 0.16f);
  list->AddTriangleFilled(tip, ImVec2(neck.x + n.x * r * 0.62f, neck.y + n.y * r * 0.62f),
                          ImVec2(neck.x - n.x * r * 0.62f, neck.y - n.y * r * 0.62f), color);
}

// A button as a chip: the face buttons round in their Xbox colours, the others grey and as wide as their text. Returns
// its width.
float DrawButton(ImDrawList* list, ImVec2 p, float s, const char* label, ImU32 fill, bool round) {
  const ImVec2 t = ImGui::CalcTextSize(label);
  const ImVec2 size(round ? s : std::max(s * 1.35f, t.x + s * 0.5f), s);
  if (round) {
    list->AddCircleFilled(ImVec2(p.x + s * 0.5f, p.y + s * 0.5f), s * 0.48f, fill);
  } else {
    list->AddRectFilled(p, ImVec2(p.x + size.x, p.y + size.y), fill, s * 0.25f);
  }
  list->AddText(ImVec2(p.x + (size.x - t.x) * 0.5f, p.y + (size.y - t.y) * 0.5f), IM_COL32(16, 12, 10, 255), label);
  return size.x;
}

class InputDisplay : public renut::gpu::overlay::Dialog {
 public:
  void Draw() override {
    if (!REXCVAR_GET(mk9_input_display)) return;
    std::vector<Entry> rows;
    {
      std::lock_guard lock(g_mutex);
      if (!g_in_fight) return;
      rows.assign(g_history.begin(), g_history.end());
    }
    if (rows.empty()) return;
    const float scale = options_ui::UiScale();
    const ImGuiIO& io = ImGui::GetIO();
    // below the game's combo counter ("3 GOLPES", top left) and above its meter (bottom left)
    ImGui::SetNextWindowPos(ImVec2(12.0f * scale, io.DisplaySize.y * 0.32f), ImGuiCond_Always);
    ImGui::SetNextWindowBgAlpha(0.55f);
    const ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
                                   ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoMove |
                                   ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoInputs |
                                   ImGuiWindowFlags_NoNav;
    if (ImGui::Begin("##input_display", nullptr, flags)) {
      ImGui::PushFont(renut::gpu::overlay::StrongFont(), 0.0f);
      const float s = ImGui::GetTextLineHeight() * 1.15f;
      ImDrawList* list = ImGui::GetWindowDrawList();
      static const struct {
        uint16_t bit;
        const char* label;
        ImU32 fill;
        bool round;
      } kButtons[] = {
          // MK's own notation with the default buttons (the game's OPCIONES > MANDO; the keyboard follows them): 1 front
          // punch (X), 2 back punch (Y), 3 front kick (A), 4 back kick (B), in those buttons' colours; BL block (RT),
          // TH throw (RB), SS switch stance (LT), TAG (LB)
          {kX, "1", IM_COL32(64, 128, 230, 255), true},  {kY, "2", IM_COL32(236, 196, 60, 255), true},
          {kA, "3", IM_COL32(90, 190, 80, 255), true},   {kB, "4", IM_COL32(220, 70, 60, 255), true},
          {kRT, "BL", IM_COL32(190, 180, 175, 255), false}, {kRB, "TH", IM_COL32(190, 180, 175, 255), false},
          {kLT, "SS", IM_COL32(190, 180, 175, 255), false}, {kLB, "TAG", IM_COL32(190, 180, 175, 255), false},
      };
      for (size_t i = 0; i < rows.size(); ++i) {
        const Entry& e = rows[i];
        char frames[8];
        std::snprintf(frames, sizeof(frames), "%u", std::min<uint32_t>(e.frames, 999));
        // the newest in full colour, the older ones dimmer
        const float fade = i == 0 ? 1.0f : std::max(0.45f, 1.0f - float(i) * 0.04f);
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.75f, 0.72f, 0.70f, fade));
        const float number_w = ImGui::CalcTextSize("999").x;
        const ImVec2 row = ImGui::GetCursorScreenPos();
        ImGui::SetCursorScreenPos(ImVec2(row.x + number_w - ImGui::CalcTextSize(frames).x, row.y + (s - ImGui::GetTextLineHeight()) * 0.5f));
        ImGui::TextUnformatted(frames);
        ImGui::PopStyleColor();
        float x = row.x + number_w + s * 0.45f;
        DrawArrow(list, ImVec2(x, row.y), s, e.dir, IM_COL32(245, 205, 90, int(255 * fade)));
        x += s * 1.25f;
        for (const auto& b : kButtons) {
          if (!(e.buttons & b.bit)) continue;
          x += DrawButton(list, ImVec2(x, row.y), s, b.label, b.fill, b.round) + s * 0.2f;
        }
        ImGui::SetCursorScreenPos(ImVec2(row.x, row.y));
        ImGui::Dummy(ImVec2(std::max(x - row.x, number_w + s * 6.5f), s + 2.0f * scale));
      }
      ImGui::PopFont();
    }
    ImGui::End();
  }
};

}  // namespace

void Install() {
  static std::once_flag once;
  std::call_once(once, [] {
    static InputDisplay display;
    renut::gpu::overlay::AddDialog(&display);
  });
}

void NoteBlock(uint8_t* base, uint32_t tag, uint32_t data, uint32_t size, uint32_t user) {
  // exactly 16 bytes: the same tag also hands over a 20-byte block of each user (01010000 ffffffff 7fff7fff...: not a
  // gamepad), which read as one gave every button held every other frame
  if (tag != kRawState || size != kGamepadOffset + 12 || !data || !REXCVAR_GET(mk9_input_display)) return;
  const int shown = netplay::SessionGame() ? std::max(0, netplay::LocalPlayer()) : 0;
  if (int(user) != shown) return;
  if (sa2::renut::FrameHidden()) return;  // a rollback's frame run again: the live one said it already
  const bool fight = fight_state::State(base) == kStatePlay;
  std::lock_guard lock(g_mutex);
  if (!fight) {
    if (g_in_fight) g_history.clear();
    g_in_fight = false;
    return;
  }
  g_in_fight = true;
  const Entry e = Read(base + data + kGamepadOffset);
  if (!g_history.empty() && g_history.front().dir == e.dir && g_history.front().buttons == e.buttons) {
    ++g_history.front().frames;
    return;
  }
  g_history.push_front(e);
  if (g_history.size() > kRows) g_history.pop_back();
}

}  // namespace mk9::input_display
