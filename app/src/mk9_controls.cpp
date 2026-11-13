// mk9-nx (2026-11-08): the PC's controls (mk9_controls.h).

#include "mk9_controls.h"

#include "mk9_platform.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#if defined(_WIN32)
#include <windows.h>
#endif
#if !defined(_WIN32)
#include <SDL3/SDL.h>
#endif


#include <atomic>
#include <filesystem>
#include <mutex>
#include <string_view>

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/input/device.h>
#include <rex/input/device_assignment.h>
#include <rex/ui/keybinds.h>
#include <rex/ui/ui_event.h>
#include <rex/ui/virtual_key.h>
#include <rex/ui/window.h>
#include <rex/ui/window_listener.h>

REXCVAR_DEFINE_INT32(mk9_keyboard_player, 1, "MK9",
                     "The player the keyboard plays offline (1 or 2; 2 lets a local Versus use the keyboard and one "
                     "pad). Online, every device of the PC plays its player")
    .range(1, 4);

namespace mk9::controls {
namespace {

using rex::input::DeviceId;
using rex::input::DeviceInfo;
using rex::input::kMaxGuestUsers;

constexpr std::string_view kKeyboardName = "Keyboard and Mouse";  // the SDK's keyboard device (mnk_input_driver.cpp)

std::mutex g_list_mutex;
std::vector<Device> g_list;

uint32_t KeyboardUser() {
  const int32_t player = REXCVAR_GET(mk9_keyboard_player);
  return player >= 1 && player <= int32_t(kMaxGuestUsers) ? uint32_t(player - 1) : 0;
}

// As the SDK's SlotAssignment (pad N plays guest user N, synthetic devices user 0), with the keyboard on its player.
class Assignment final : public rex::input::DeviceAssignment {
 public:
  void OnDevicesChanged(const std::vector<DeviceInfo>& devices) override {
    for (auto& user : users_) user.clear();
    keyboard_ = DeviceId::kInvalid;
    std::vector<Device> listed;
    for (const DeviceInfo& device : devices) {
      if (device.synthetic && device.name == kKeyboardName) {
        keyboard_ = device.id;
        listed.push_back({"Teclado", 0, true});
      } else if (device.synthetic) {
        // the stand-in, the test script (pad 1); mk9-lauta (2026-11-13): its pads 2-4, "Script N", on player N
        const size_t n = device.name.rfind("Script ", 0) == 0 && device.name.size() == 8 ? size_t(device.name[7] - '1')
                                                                                          : 0;
        users_[n < kMaxGuestUsers ? n : 0].push_back(device.id);
      } else {
        const bool seated = device.ordinal < kMaxGuestUsers;
        if (seated) users_[device.ordinal].push_back(device.id);
        listed.push_back({device.name.empty() ? std::string("Mando") : device.name,
                          seated ? int(device.ordinal) + 1 : 0, false});
      }
    }
    std::string seen;
    for (const Device& d : listed) {
      seen += (seen.empty() ? "" : ", ") + d.name + " (" +
              (d.keyboard ? "player " + std::to_string(KeyboardUser() + 1) + " offline"
                          : d.player ? "player " + std::to_string(d.player) : std::string("no player")) + ")";
    }
    REXLOG_INFO("[controls] {}", seen.empty() ? std::string("no keyboard, no pad") : seen);
    std::lock_guard lock(g_list_mutex);
    g_list = std::move(listed);
  }

  void DevicesForUser(uint32_t user_index, std::vector<DeviceId>& out) const override {
    out.clear();
    if (user_index >= kMaxGuestUsers) return;
    out = users_[user_index];
    if (keyboard_ != DeviceId::kInvalid && user_index == KeyboardUser()) out.push_back(keyboard_);
  }

 private:
  std::vector<std::vector<DeviceId>> users_ = std::vector<std::vector<DeviceId>>(kMaxGuestUsers);
  DeviceId keyboard_ = DeviceId::kInvalid;
};

#if defined(_WIN32)
std::string Utf8(const std::wstring& text) {
  if (text.empty()) return {};
  const int size = WideCharToMultiByte(CP_UTF8, 0, text.data(), int(text.size()), nullptr, 0, nullptr, nullptr);
  std::string out(size_t(std::max(size, 0)), '\0');
  WideCharToMultiByte(CP_UTF8, 0, text.data(), int(text.size()), out.data(), size, nullptr, nullptr);
  return out;
}
#endif

// One key as the player's keyboard prints it.
std::string KeyLabel(std::string_view name) {
  static const std::pair<std::string_view, const char*> kNamed[] = {
      {"Return", "Enter"},
      {"Escape", "Esc"},
      {"Space", "Espacio"},
      {"Backspace", "Retroceso"},
      {"Tab", "Tab"},
      {"Up", "Flecha arriba"},
      {"Down", "Flecha abajo"},
      {"Left", "Flecha izquierda"},
      {"Right", "Flecha derecha"},
      {"Shift", "Shift"},
      {"Control", "Ctrl"},
      {"Alt", "Alt"},
      {"Insert", "Insert"},
      {"Delete", "Supr"},
      {"Home", "Inicio"},
      {"End", "Fin"},
      {"PageUp", "Re Pág"},
      {"PageDown", "Av Pág"},
      {"CapsLock", "Bloq Mayús"},
      {"NumpadPlus", "Num +"},
      {"NumpadMinus", "Num -"},
      {"NumpadStar", "Num *"},
      {"NumpadSlash", "Num /"},
      {"NumpadEnter", "Enter"},
      {"LMB", "Clic izquierdo"},
      {"RMB", "Clic derecho"},
      {"MMB", "Clic de la rueda"},
  };
  for (const auto& [key, label] : kNamed) {
    if (name == key) return label;
  }
  if (name.size() == 7 && name.substr(0, 6) == "Numpad") return "Num " + std::string(name.substr(6));
  // Letters, digits and the punctuation keys: the SDK names them by their place on an English keyboard (window_sdl.cpp
  // reads SDL's scan codes); shown as the character this keyboard's layout has in that place.
#if defined(_WIN32)
  static const std::pair<std::string_view, UINT> kScanCodes[] = {
      {"Q", 0x10}, {"W", 0x11}, {"E", 0x12}, {"R", 0x13}, {"T", 0x14}, {"Y", 0x15}, {"U", 0x16}, {"I", 0x17},
      {"O", 0x18}, {"P", 0x19}, {"A", 0x1E}, {"S", 0x1F}, {"D", 0x20}, {"F", 0x21}, {"G", 0x22}, {"H", 0x23},
      {"J", 0x24}, {"K", 0x25}, {"L", 0x26}, {"Z", 0x2C}, {"X", 0x2D}, {"C", 0x2E}, {"V", 0x2F}, {"B", 0x30},
      {"N", 0x31}, {"M", 0x32}, {"1", 0x02}, {"2", 0x03}, {"3", 0x04}, {"4", 0x05}, {"5", 0x06}, {"6", 0x07},
      {"7", 0x08}, {"8", 0x09}, {"9", 0x0A}, {"0", 0x0B}, {"Minus", 0x0C}, {"Plus", 0x0D}, {"LBracket", 0x1A},
      {"RBracket", 0x1B}, {"Semicolon", 0x27}, {"Quote", 0x28}, {"Backtick", 0x29}, {"Backslash", 0x2B},
      {"Comma", 0x33}, {"Period", 0x34}, {"Slash", 0x35},
  };
  for (const auto& [key, scan_code] : kScanCodes) {
    if (name != key) continue;
    const UINT layout_vk = MapVirtualKeyW(scan_code, MAPVK_VSC_TO_VK);
    const UINT ch = layout_vk ? MapVirtualKeyW(layout_vk, MAPVK_VK_TO_CHAR) & 0x7FFF : 0;
    if (ch > 32) {
      wchar_t text[2] = {wchar_t(ch), 0};
      CharUpperW(text);
      return Utf8(text);
    }
    break;
  }
#else
  // mk9-lauta (2026-11-13): Linux: the same places, as SDL's scan codes; the key this layout has there, by SDL's name
  // for it (a letter in capitals, the ñ of a Spanish keyboard as Ñ)
  static const std::pair<std::string_view, SDL_Scancode> kScanCodes[] = {
      {"Q", SDL_SCANCODE_Q}, {"W", SDL_SCANCODE_W}, {"E", SDL_SCANCODE_E}, {"R", SDL_SCANCODE_R},
      {"T", SDL_SCANCODE_T}, {"Y", SDL_SCANCODE_Y}, {"U", SDL_SCANCODE_U}, {"I", SDL_SCANCODE_I},
      {"O", SDL_SCANCODE_O}, {"P", SDL_SCANCODE_P}, {"A", SDL_SCANCODE_A}, {"S", SDL_SCANCODE_S},
      {"D", SDL_SCANCODE_D}, {"F", SDL_SCANCODE_F}, {"G", SDL_SCANCODE_G}, {"H", SDL_SCANCODE_H},
      {"J", SDL_SCANCODE_J}, {"K", SDL_SCANCODE_K}, {"L", SDL_SCANCODE_L}, {"Z", SDL_SCANCODE_Z},
      {"X", SDL_SCANCODE_X}, {"C", SDL_SCANCODE_C}, {"V", SDL_SCANCODE_V}, {"B", SDL_SCANCODE_B},
      {"N", SDL_SCANCODE_N}, {"M", SDL_SCANCODE_M}, {"1", SDL_SCANCODE_1}, {"2", SDL_SCANCODE_2},
      {"3", SDL_SCANCODE_3}, {"4", SDL_SCANCODE_4}, {"5", SDL_SCANCODE_5}, {"6", SDL_SCANCODE_6},
      {"7", SDL_SCANCODE_7}, {"8", SDL_SCANCODE_8}, {"9", SDL_SCANCODE_9}, {"0", SDL_SCANCODE_0},
      {"Minus", SDL_SCANCODE_MINUS}, {"Plus", SDL_SCANCODE_EQUALS}, {"LBracket", SDL_SCANCODE_LEFTBRACKET},
      {"RBracket", SDL_SCANCODE_RIGHTBRACKET}, {"Semicolon", SDL_SCANCODE_SEMICOLON},
      {"Quote", SDL_SCANCODE_APOSTROPHE}, {"Backtick", SDL_SCANCODE_GRAVE}, {"Backslash", SDL_SCANCODE_BACKSLASH},
      {"Comma", SDL_SCANCODE_COMMA}, {"Period", SDL_SCANCODE_PERIOD}, {"Slash", SDL_SCANCODE_SLASH},
  };
  for (const auto& [key, scan_code] : kScanCodes) {
    if (name != key) continue;
    const SDL_Keycode code = SDL_GetKeyFromScancode(scan_code, SDL_KMOD_NONE, false);
    if (code > 32 && code < SDLK_SCANCODE_MASK) {
      const char* label = SDL_GetKeyName(code);
      if (label && *label) return label;
    }
    break;
  }
#endif
  return std::string(name);
}

// The key capture (CaptureNextKey): waiting for a key, and the one that came (-1: none yet).
std::atomic<bool> g_capturing{false};
std::atomic<int> g_captured_key{-1};

// Ahead of the game's keyboard (the SDK's keyboard driver listens at z-order 0).
class KeyCapture final : public rex::ui::WindowInputListener {
 public:
  void OnKeyDown(rex::ui::KeyEvent& e) override {
    if (!g_capturing.load(std::memory_order_relaxed)) return;
    using rex::ui::VirtualKey;
    switch (e.virtual_key()) {
      case VirtualKey::kF1:
      case VirtualKey::kF2:
      case VirtualKey::kF11:
      case VirtualKey::kShift:
      case VirtualKey::kLShift:
      case VirtualKey::kRShift:
      case VirtualKey::kControl:
      case VirtualKey::kLControl:
      case VirtualKey::kRControl:
      case VirtualKey::kMenu:
      case VirtualKey::kLMenu:
      case VirtualKey::kRMenu:
      case VirtualKey::kLWin:
      case VirtualKey::kRWin:
        return;
      default:
        break;
    }
    g_captured_key.store(int(e.virtual_key()), std::memory_order_relaxed);
    g_capturing.store(false, std::memory_order_relaxed);
    e.set_handled(true);  // bound, not played
  }
};

}  // namespace

std::unique_ptr<rex::input::DeviceAssignment> CreateAssignment() { return std::make_unique<Assignment>(); }

void FindMappingsFile() {
  const std::filesystem::path file(rex::cvar::GetFlagByName("hid_mappings_file"));
  std::error_code ec;
  if (file.empty() || file.is_absolute() || std::filesystem::exists(file, ec)) return;
  const std::filesystem::path beside = mk9::platform::ExeDir() / file;
  if (std::filesystem::exists(beside, ec)) rex::cvar::SetFlagByName("hid_mappings_file", beside.string());
}

void AttachWindow(rex::ui::Window* window) {
  static KeyCapture capture;
  if (window) window->AddInputListener(&capture, 1000);
}

void CaptureNextKey(bool on) {
  g_captured_key.store(-1, std::memory_order_relaxed);
  g_capturing.store(on, std::memory_order_relaxed);
}

int TakeCapturedKey() { return g_captured_key.exchange(-1, std::memory_order_relaxed); }

std::vector<Device> Devices() {
  std::vector<Device> list;
  {
    std::lock_guard lock(g_list_mutex);
    list = g_list;
  }
  for (Device& d : list) {
    if (d.keyboard) d.player = int(KeyboardUser()) + 1;
  }
  return list;
}

const std::vector<Action>& Actions() {
  static const std::vector<Action> actions = {
      {"keybind_lstick_up", "Arriba (saltar)", "Stick arriba"},
      {"keybind_lstick_down", "Abajo (agacharse)", "Stick abajo"},
      {"keybind_lstick_left", "Izquierda", "Stick izquierda"},
      {"keybind_lstick_right", "Derecha", "Stick derecha"},
      {"keybind_x", "Puño frontal (1)", "X"},
      {"keybind_y", "Puño trasero (2)", "Y"},
      {"keybind_a", "Patada frontal (3) / aceptar", "A"},
      {"keybind_b", "Patada trasera (4) / volver", "B"},
      {"keybind_right_shoulder", "Agarre", "RB"},
      {"keybind_right_trigger", "Cubrirse", "RT"},
      {"keybind_left_trigger", "Cambio de guardia", "LT"},
      {"keybind_left_shoulder", "Relevo (peleas por equipos)", "LB"},
      {"keybind_start", "Pausa (Start)", "Start"},
      {"keybind_back", "Back", "Back"},
  };
  return actions;
}

std::string KeysLabel(const std::string& bind) {
  std::string out;
  std::string_view rest(bind);
  while (!rest.empty()) {
    const size_t comma = rest.find(',');
    std::string_view token = rest.substr(0, comma);
    rest = comma == std::string_view::npos ? std::string_view() : rest.substr(comma + 1);
    while (!token.empty() && token.front() == ' ') token.remove_prefix(1);
    while (!token.empty() && token.back() == ' ') token.remove_suffix(1);
    if (token.empty()) continue;
    std::string shown;
    // modifier prefixes ("Shift+Up")
    for (size_t plus; (plus = token.find('+')) != std::string_view::npos && plus > 0;) {
      shown += KeyLabel(token.substr(0, plus)) + " + ";
      token.remove_prefix(plus + 1);
    }
    shown += KeyLabel(token);
    if (!out.empty()) out += ", ";
    out += shown;
  }
  return out;
}

std::string KeyName(int virtual_key) {
  if (virtual_key <= 0 || virtual_key > 0xFF) return {};
  std::string name = rex::ui::VirtualKeyToString(rex::ui::VirtualKey(uint16_t(virtual_key)));
  return name == "NumpadEnter" ? "Return" : name;  // the two names of one code
}

}  // namespace mk9::controls
