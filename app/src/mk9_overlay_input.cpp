// mk9-lauta: the overlay windows with a controller (mk9_overlay_input.h).

#include "mk9_overlay_input.h"

#include <algorithm>
#include <atomic>
#include <mutex>

#include "imgui.h"

#include <rex/input/input_system.h>
#include <rex/logging.h>

#include "renut_engine/gpu/overlay/overlay_ui.h"

namespace mk9::overlay_input {
namespace {

constexpr uint16_t kStart = 0x0010, kBack = 0x0020, kY = 0x8000;

rex::input::InputSystem* g_input = nullptr;
thread_local bool t_bypass = false;      // this thread's read is the overlay's (OnFrame)
std::atomic<uint32_t> g_open{0};          // bit per Window
std::atomic<bool> g_combo_frame{false};   // this frame's buttons are a combo: not the game's
std::atomic<bool> g_have_pad{false};
std::atomic<uint64_t> g_pad{0};           // buttons, lt << 16, rt << 24, lx << 32, ly << 48
std::atomic<bool> g_close_asked{false};
std::atomic<bool> g_closing{false};

// The input system's active callback: the drivers answer with the player's input only while it says yes.
bool GameMayRead() {
  if (t_bypass) return true;
  return !g_open.load(std::memory_order_relaxed) && !g_combo_frame.load(std::memory_order_relaxed);
}

// The overlay's gamepad (its ImGui's gamepad keys), as the last frame's start read it.
bool ReadPad(renut::gpu::overlay::Pad* out) {
  if (!g_have_pad.load(std::memory_order_relaxed)) return false;
  const uint64_t v = g_pad.load(std::memory_order_relaxed);
  out->buttons = uint16_t(v);
  out->lt = uint8_t(v >> 16);
  out->rt = uint8_t(v >> 24);
  out->lx = int16_t(uint16_t(v >> 32));
  out->ly = int16_t(uint16_t(v >> 48));
  return true;
}

// The window's close (its X, Alt+F4): the confirmation first.
bool CloseFilter() {
  if (g_closing.load(std::memory_order_relaxed)) return true;
  g_close_asked.store(true, std::memory_order_relaxed);
  return false;
}

class ConfirmClose : public renut::gpu::overlay::Dialog {
 public:
  void Draw() override {
    const bool asked = g_close_asked.load(std::memory_order_relaxed);
    SetOpen(kConfirm, asked);
    if (!asked) return;
    const ImGuiIO& io = ImGui::GetIO();
    ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f), ImGuiCond_Always,
                            ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowFocus();
    const ImGuiWindowFlags flags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                                   ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings |
                                   ImGuiWindowFlags_NoCollapse;
    if (ImGui::Begin("##confirm_close", nullptr, flags)) {
      ImGui::TextUnformatted("¿Cerrar el juego?");
      ImGui::Spacing();
      if (ImGui::Button("Seguir jugando")) g_close_asked.store(false);
      ImGui::SetItemDefaultFocus();
      ImGui::SameLine();
      if (ImGui::Button("Cerrar el juego")) {
        REXLOG_INFO("[overlay] the player closes the game");
        g_closing.store(true);
        renut::gpu::overlay::CloseNow();
      }
      if (ImGui::IsKeyPressed(ImGuiKey_Escape, false) || ImGui::IsKeyPressed(ImGuiKey_GamepadFaceRight, false)) {
        g_close_asked.store(false);
      }
    }
    ImGui::End();
  }
};

}  // namespace

void Install(rex::input::InputSystem* input) {
  static std::once_flag once;
  std::call_once(once, [input] {
    g_input = input;
    if (input) input->SetActiveCallback(&GameMayRead);
    renut::gpu::overlay::SetPadReader(&ReadPad);
    renut::gpu::overlay::SetCloseFilter(&CloseFilter);
    static ConfirmClose confirm;
    renut::gpu::overlay::AddDialog(&confirm);
    REXLOG_INFO("[overlay] controllers drive F1 and F2 (Back + Y, Back + Start); the window's close asks first");
  });
}

void OnFrame() {
  if (!g_input) return;
  rex::input::X_INPUT_STATE state{};
  t_bypass = true;
  const bool have = g_input->GetControllersState(&state);
  t_bypass = false;
  const auto& pad = state.gamepad;
  const uint16_t buttons = have ? uint16_t(pad.buttons) : 0;
  g_pad.store(uint64_t(buttons) | uint64_t(uint8_t(pad.left_trigger)) << 16 |
                  uint64_t(uint8_t(pad.right_trigger)) << 24 | uint64_t(uint16_t(int16_t(pad.thumb_lx))) << 32 |
                  uint64_t(uint16_t(int16_t(pad.thumb_ly))) << 48,
              std::memory_order_relaxed);
  g_have_pad.store(have, std::memory_order_relaxed);
  g_combo_frame.store(have && (buttons & kBack) && (buttons & (kStart | kY)), std::memory_order_relaxed);
}

void SetOpen(Window window, bool open) {
  const uint32_t bit = 1u << uint32_t(window);
  if (open) {
    g_open.fetch_or(bit, std::memory_order_relaxed);
  } else {
    g_open.fetch_and(~bit, std::memory_order_relaxed);
  }
}

bool ToggleOptionsPressed() {
  return ImGui::IsKeyDown(ImGuiKey_GamepadBack) && ImGui::IsKeyPressed(ImGuiKey_GamepadStart, false);
}

bool ToggleOnlinePressed() {
  return ImGui::IsKeyDown(ImGuiKey_GamepadBack) && ImGui::IsKeyPressed(ImGuiKey_GamepadFaceUp, false);
}

int TabStep() {
  if (ImGui::IsKeyPressed(ImGuiKey_GamepadL1, false)) return -1;
  if (ImGui::IsKeyPressed(ImGuiKey_GamepadR1, false)) return 1;
  return 0;
}

void PageScroll() {
  const bool up = ImGui::IsKeyPressed(ImGuiKey_GamepadL2, true);
  const bool down = ImGui::IsKeyPressed(ImGuiKey_GamepadR2, true);
  if ((!up && !down) || !ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows)) return;
  const float page = ImGui::GetWindowHeight() * 0.85f;
  ImGui::SetScrollY(std::clamp(ImGui::GetScrollY() + (down ? page : -page), 0.0f, ImGui::GetScrollMaxY()));
}

bool ClosePressed() {
  return ImGui::IsKeyPressed(ImGuiKey_GamepadFaceRight, false) && !ImGui::IsAnyItemActive() &&
         !ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId) &&
         ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows);
}

}  // namespace mk9::overlay_input
