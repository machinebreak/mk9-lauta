// mk9-lauta (2026-11-18): see mk9_practice_record.h.
#include "mk9_practice_record.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <climits>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

#include "imgui.h"
#include <fmt/format.h>
#include <rex/cvar.h>
#include <rex/logging.h>

#include "mk9_overlay_input.h"
#include "online/fight_state.h"
#include "online/i18n.h"
#include "online/netplay.h"
#include "options_ui.h"
#include "renut_engine/gpu/overlay/overlay_ui.h"

REXCVAR_DEFINE_BOOL(mk9_practice_record, true, "MK9",
                    "Practice: the opponent recorded (F5 or BACK + LB: the player's controller drives the dummy) and "
                    "played back on it (F6 or BACK + RB; F7 or BACK + X: in a loop). F2 > Controles");

REXCVAR_DEFINE_STRING(mk9_practice_record_test, "", "MK9",
                      "Tests: the keys pressed at these seconds of the game, \"82:record,88:record,90:play,95:loop\"")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

namespace mk9::practice_record {
namespace {

using i18n::T;

constexpr int kModePractice = 2;  // the game object's +8 (fight_state::Mode)
// X_INPUT_GAMEPAD's buttons
constexpr uint16_t kLeft = 0x0004, kRight = 0x0008, kStart = 0x0010, kBack = 0x0020;
constexpr size_t kMaxFrames = 60 * 60;  // 60 s at the game's 60 frames a second

enum class Mode : int { kIdle = 0, kRecording = 1, kPlaying = 2 };
enum Request : int { kNone = 0, kRecord = 1, kPlay = 2, kLoop = 3 };

struct Frame {
  uint8_t pad[12];   // X_INPUT_GAMEPAD as the guest has it (big-endian)
  bool dummy_right;  // the dummy to the right of player 1 when it was recorded
};

// The game thread's.
Mode g_mode = Mode::kIdle;
std::vector<Frame> g_frames;
size_t g_pos = 0;
uint8_t g_live[12] = {};      // player 1's pad this frame (recording: the dummy's)
uint8_t g_given[12] = {};     // the last pad given to player 2
uint32_t g_packet = 1;        // player 2's packet number: a new one at each change, as XInput counts
uint16_t g_prev_buttons = 0;  // player 1's buttons the frame before (START's press)
bool g_practice = false;      // a Practice fight's frame (the feature on, no online session)
bool g_dummy_right = true;    // the dummy (player 2) to the right of player 1, this frame
// The fight's side to side: the actors' location's axis on which the fighters stand apart at the fight's start, and its
// sign toward the screen's right (player 1 starts on the left). MK9's: y, -125 and 125 (the log's "[practice] side").
int g_axis = 1;
float g_sign = 1.0f;
bool g_oriented = false;

// The keys' requests (UI thread) and what the panel shows.
std::atomic<int> g_request{kNone};
std::atomic<bool> g_loop{false};
std::atomic<int> g_view_mode{0};
std::atomic<uint32_t> g_view_frames{0}, g_view_pos{0};
std::atomic<bool> g_view_practice{false};

uint16_t Be16(const uint8_t* p) { return uint16_t(uint16_t(p[0]) << 8 | p[1]); }
void PutBe16(uint8_t* p, uint16_t v) {
  p[0] = uint8_t(v >> 8);
  p[1] = uint8_t(v);
}
void PutBe32(uint8_t* p, uint32_t v) {
  p[0] = uint8_t(v >> 24);
  p[1] = uint8_t(v >> 16);
  p[2] = uint8_t(v >> 8);
  p[3] = uint8_t(v);
}

void SetMode(Mode mode, const char* why) {
  if (g_mode == mode) return;
  g_mode = mode;
  g_pos = 0;
  REXLOG_INFO("[practice] {} ({} frames recorded)", why, g_frames.size());
}

// A recorded pad as it plays now: left and right swapped (the d-pad and the left stick) when the dummy is on the other
// side of player 1 than when it was recorded, so forward stays forward.
void Mirror(uint8_t* pad) {
  uint16_t buttons = Be16(pad);
  const bool left = (buttons & kLeft) != 0, right = (buttons & kRight) != 0;
  buttons = uint16_t((buttons & ~(kLeft | kRight)) | (left ? kRight : 0) | (right ? kLeft : 0));
  PutBe16(pad, buttons);
  const int16_t lx = int16_t(Be16(pad + 4));
  PutBe16(pad + 4, uint16_t(lx == INT16_MIN ? INT16_MAX : int16_t(-lx)));
}

// Player 2's X_INPUT_STATE: the pad, its packet number new when the pad changed.
void Give(uint8_t* out, const uint8_t* pad) {
  if (std::memcmp(pad, g_given, sizeof(g_given)) != 0) {
    std::memcpy(g_given, pad, sizeof(g_given));
    ++g_packet;
  }
  PutBe32(out, g_packet);
  std::memcpy(out + 4, pad, 12);
}

std::string Seconds(uint32_t frames) { return fmt::format("{:.1f} s", double(frames) / 60.0); }

// The panel: at the right, under the frame data's.
class Panel : public renut::gpu::overlay::Dialog {
 public:
  void Draw() override {
    if (!REXCVAR_GET(mk9_practice_record) || !g_view_practice.load(std::memory_order_relaxed)) return;
    // the keys: F5 / F6 / F7, or BACK + LB / RB / X on a controller (overlay_input keeps those from the game)
    if (ImGui::IsKeyPressed(ImGuiKey_F5, false) || overlay_input::PracticeRecordPressed()) g_request = kRecord;
    if (ImGui::IsKeyPressed(ImGuiKey_F6, false) || overlay_input::PracticePlayPressed()) g_request = kPlay;
    if (ImGui::IsKeyPressed(ImGuiKey_F7, false) || overlay_input::PracticeLoopPressed()) g_request = kLoop;
    const Mode mode = Mode(g_view_mode.load(std::memory_order_relaxed));
    const uint32_t frames = g_view_frames.load(std::memory_order_relaxed);
    const uint32_t pos = g_view_pos.load(std::memory_order_relaxed);
    const bool loop = g_loop.load(std::memory_order_relaxed);

    const float scale = options_ui::UiScale();
    const ImGuiIO& io = ImGui::GetIO();
    ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x - 12.0f * scale, io.DisplaySize.y * 0.58f), ImGuiCond_Always,
                            ImVec2(1.0f, 0.0f));
    ImGui::SetNextWindowBgAlpha(0.55f);
    const ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
                                   ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoMove |
                                   ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoInputs |
                                   ImGuiWindowFlags_NoNav;
    if (ImGui::Begin("##practice_record", nullptr, flags)) {
      const ImVec4 grey(0.68f, 0.66f, 0.66f, 1.0f), gold(1.0f, 0.80f, 0.30f, 1.0f), text(0.93f, 0.91f, 0.89f, 1.0f);
      const ImVec4 red(0.95f, 0.35f, 0.30f, 1.0f), green(0.45f, 0.85f, 0.45f, 1.0f);
      ImGui::PushFont(renut::gpu::overlay::StrongFont(), 0.0f);
      ImGui::TextColored(gold, "%s", T("GRABAR AL RIVAL"));
      ImGui::PopFont();
      if (mode == Mode::kRecording) {
        ImGui::PushFont(renut::gpu::overlay::StrongFont(), 0.0f);
        ImGui::TextColored(red, "%s %s", T("GRABANDO"), Seconds(frames).c_str());
        ImGui::PopFont();
        ImGui::TextColored(text, "%s", T("Tu mando maneja al rival. F5 termina."));
        ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + 260.0f * scale);
        ImGui::TextColored(grey, "%s",
                           T("¿El rival no se mueve? Tiene que estar en HUMANA: Pausa > Opciones de IA > Tipo de "
                             "enemigo."));
        ImGui::PopTextWrapPos();
      } else if (mode == Mode::kPlaying) {
        ImGui::PushFont(renut::gpu::overlay::StrongFont(), 0.0f);
        ImGui::TextColored(green, "%s %s / %s", T("REPRODUCIENDO"), Seconds(pos).c_str(), Seconds(frames).c_str());
        ImGui::PopFont();
        ImGui::TextColored(text, "%s · %s", T(loop ? "En bucle" : "Una vez"), T("F6 la para"));
      } else {
        if (frames) {
          ImGui::TextColored(text, "%s %s", T("Grabación:"), Seconds(frames).c_str());
          ImGui::TextColored(text, "%s", T("F6 (BACK + RB): reproducir"));
          ImGui::TextColored(text, "%s %s", T("F7 (BACK + X): en bucle:"), T(loop ? "sí" : "no"));
        }
        ImGui::TextColored(text, "%s", T(frames ? "F5 (BACK + LB): grabar de nuevo" : "F5 (BACK + LB): grabar al rival"));
        ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + 260.0f * scale);
        ImGui::TextColored(grey, "%s",
                           T("El rival tiene que estar en HUMANA: Pausa > Opciones de IA > Tipo de enemigo."));
        ImGui::PopTextWrapPos();
      }
    }
    ImGui::End();
  }
};

}  // namespace

void OnFrame(uint8_t* base) {
  const bool fight = !netplay::SessionGame() && fight_state::State(base) == fight_state::kStatePlay &&
                     fight_state::Mode(base) == kModePractice;
  const bool practice = fight && REXCVAR_GET(mk9_practice_record);
  overlay_input::SetPracticeCombos(practice);
  // the fighters' places (the fight's sides taken at its start, the feature on or not: it can be turned on in a fight)
  float p1[3], p2[3];
  const bool placed = fight && fight_state::Position(base, 0, p1) && fight_state::Position(base, 1, p2);
  if (!fight) g_oriented = false;
  if (placed && !g_oriented) {
    const float dx = p2[0] - p1[0], dy = p2[1] - p1[1];
    if (std::fabs(dx) + std::fabs(dy) > 10.0f) {
      g_axis = std::fabs(dx) > std::fabs(dy) ? 0 : 1;
      g_sign = (g_axis == 0 ? dx : dy) < 0.0f ? -1.0f : 1.0f;
      g_oriented = true;
      g_dummy_right = true;
      REXLOG_INFO("[practice] side: the fighters at ({:.1f}, {:.1f}, {:.1f}) and ({:.1f}, {:.1f}, {:.1f}): axis {}{}", p1[0],
                  p1[1], p1[2], p2[0], p2[1], p2[2], g_sign < 0.0f ? "-" : "+", g_axis == 0 ? "x" : "y");
    }
  }
  if (placed && g_oriented) {
    const float apart = g_sign * (p2[g_axis] - p1[g_axis]);
    if (std::fabs(apart) > 1.0f && (apart > 0.0f) != g_dummy_right) {  // (one over the other: the side kept)
      g_dummy_right = apart > 0.0f;
      if (g_mode != Mode::kIdle) {
        REXLOG_INFO("[practice] the dummy now on the {} of player 1", g_dummy_right ? "right" : "left");
      }
    }
  }
  if (!practice) {
    if (g_practice) SetMode(Mode::kIdle, "out of a Practice fight: the recording kept, stopped");
    g_practice = false;
    g_request = kNone;
    g_view_practice = false;
    return;
  }
  g_practice = true;
  // tests: the keys at given seconds (mk9_practice_record_test)
  static const std::string test = REXCVAR_GET(mk9_practice_record_test);
  if (!test.empty()) {
    static const auto started = std::chrono::steady_clock::now();
    static size_t next = 0;
    const double now = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
    while (next < test.size()) {
      const size_t colon = test.find(':', next), comma = test.find(',', next);
      if (colon == std::string::npos) break;
      const double at = std::atof(test.substr(next, colon - next).c_str());
      if (at > now) break;
      const std::string what = test.substr(colon + 1, comma == std::string::npos ? std::string::npos : comma - colon - 1);
      g_request = what == "record" ? kRecord : what == "play" ? kPlay : what == "loop" ? kLoop : kNone;
      next = comma == std::string::npos ? test.size() : comma + 1;
      break;
    }
  }
  switch (g_request.exchange(kNone)) {
    case kRecord:
      if (g_mode == Mode::kRecording) {
        SetMode(Mode::kIdle, "recording ended (F5)");
      } else {
        g_frames.clear();
        SetMode(Mode::kRecording, g_dummy_right ? "recording the dummy (F5), on the right: player 1's controller drives it"
                                                : "recording the dummy (F5), on the left: player 1's controller drives it");
      }
      break;
    case kPlay:
      if (g_mode == Mode::kPlaying) {
        SetMode(Mode::kIdle, "playback stopped (F6)");
      } else if (!g_frames.empty()) {
        SetMode(Mode::kPlaying, g_dummy_right ? (g_loop ? "the recording plays on the dummy, on the right, in a loop (F6)"
                                                        : "the recording plays on the dummy, on the right (F6)")
                                              : (g_loop ? "the recording plays on the dummy, on the left, in a loop (F6)"
                                                        : "the recording plays on the dummy, on the left (F6)"));
      }
      break;
    case kLoop: g_loop = !g_loop; break;
    default: break;
  }
  g_view_mode = int(g_mode);
  g_view_frames = uint32_t(g_frames.size());
  g_view_pos = uint32_t(g_pos);
  g_view_practice = true;
}

void OnPadBlock(uint8_t* base, uint32_t tag, uint32_t data, uint32_t size, uint32_t user, uint32_t r1) {
  if (!g_practice || !data) return;
  uint8_t* out = base + data;
  if (tag == 0x00FF0002 && size == 16 && user == 0) {
    // player 1: START's press ends a recording or a playback (and goes on to the game: the pause is the player's)
    const uint16_t buttons = Be16(out + 4);
    const bool start = (buttons & kStart) && !(g_prev_buttons & kStart);
    g_prev_buttons = buttons;
    if (g_mode == Mode::kIdle) return;
    if (start) {
      SetMode(Mode::kIdle, g_mode == Mode::kRecording ? "recording ended (START)" : "playback stopped (START)");
      return;
    }
    if (g_mode == Mode::kRecording) {
      std::memcpy(g_live, out + 4, 12);
      std::memset(out + 4, 0, 12);  // player 1's own fighter stands still
    }
    return;
  }
  if (user != 1 || g_mode == Mode::kIdle) return;
  if (tag == 0x00FF0003 && size == 4 && data - r1 < 0x400) {
    PutBe32(out, 0);  // XInputGetState's result: player 2's controller is there
    return;
  }
  if (tag == 0x00FF0002 && size == 20) {
    // X_INPUT_CAPABILITIES of a standard gamepad (netplay::OverrideBlock's)
    static const uint8_t caps[20] = {1,    1,    0,    0,    0xF3, 0xFF, 0xFF, 0xFF, 0xFF, 0xC0,
                                     0xFF, 0xC0, 0xFF, 0xC0, 0xFF, 0xC0, 0xFF, 0xFF, 0xFF, 0xFF};
    std::memcpy(out, caps, sizeof(caps));
    return;
  }
  if (tag != 0x00FF0002 || size != 16) return;
  uint8_t pad[12];
  if (g_mode == Mode::kRecording) {
    std::memcpy(pad, g_live, sizeof(pad));
    PutBe16(pad, uint16_t(Be16(pad) & ~(kStart | kBack)));  // the dummy never pauses
    Frame f;
    std::memcpy(f.pad, pad, sizeof(pad));
    f.dummy_right = g_dummy_right;
    g_frames.push_back(f);
    if (g_frames.size() >= kMaxFrames) SetMode(Mode::kIdle, "recording ended (60 s)");
  } else {
    if (g_frames.empty()) {
      SetMode(Mode::kIdle, "nothing to play");
      return;
    }
    const Frame& f = g_frames[std::min(g_pos, g_frames.size() - 1)];
    std::memcpy(pad, f.pad, sizeof(pad));
    if (f.dummy_right != g_dummy_right) Mirror(pad);
    if (++g_pos >= g_frames.size()) {
      if (g_loop) {
        g_pos = 0;
      } else {
        SetMode(Mode::kIdle, "playback ended");
      }
    }
  }
  Give(out, pad);
}

void Install() {
  static std::once_flag once;
  std::call_once(once, [] {
    static Panel panel;
    renut::gpu::overlay::AddDialog(&panel);
  });
}

}  // namespace mk9::practice_record
