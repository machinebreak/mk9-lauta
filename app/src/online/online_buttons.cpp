// mk9-lauta (2026-11-16): the player's buttons in the online fights (online_buttons.h).

#include "online/online_buttons.h"

#include <atomic>
#include <cstring>
#include <sstream>

#include <rex/cvar.h>
#include <rex/logging.h>

REXCVAR_DEFINE_STRING(mk9_net_buttons, "", "MK9 Online",
                      "The player's buttons in the online fights (F2 > Controles > Botones en el online): eight of X, "
                      "Y, A, B, RB, LB, RT, LT, comma-separated, for front punch, back punch, front kick, back kick, "
                      "throw, tag, block and flip stance. Empty = the game's own (X,Y,A,B,RB,LB,RT,LT), which a "
                      "session's game always plays with: this machine's pad is turned into it in the fights");

namespace mk9::online_buttons {
namespace {

constexpr const char* kNames[kButtonCount] = {"X", "Y", "A", "B", "RB", "LB", "RT", "LT"};
constexpr const char* kLabels[kButtonCount] = {"X (Cuadrado)", "Y (Triángulo)", "A (Cruz)", "B (Círculo)",
                                               "RB (R1)",      "LB (L1)",       "RT (R2)",  "LT (L2)"};
constexpr const char* kActions[kActionCount] = {
    "Golpe frontal (Front Punch)", "Golpe trasero (Back Punch)", "Patada frontal (Front Kick)",
    "Patada trasera (Back Kick)",  "Agarre (Throw)",             "Relevo (Tag)",
    "Bloquear (Block)",            "Cambiar de postura (Flip Stance)"};

// X_INPUT_GAMEPAD's buttons (wButtons) of the face buttons and the shoulders; the triggers are bytes 2 (left) and 3
// (right).
constexpr uint16_t kBits[kButtonCount] = {0x4000, 0x8000, 0x1000, 0x2000, 0x0200, 0x0100, 0, 0};
constexpr uint8_t kTriggerThreshold = 30;  // XINPUT_GAMEPAD_TRIGGER_THRESHOLD

bool Pressed(const uint8_t* g, Button b) {
  if (b == kLT) return g[2] > kTriggerThreshold;
  if (b == kRT) return g[3] > kTriggerThreshold;
  return ((uint16_t(g[0]) << 8 | g[1]) & kBits[b]) != 0;
}

bool Parse(const std::string& text, Layout* out) {
  Layout layout{};
  bool used[kButtonCount] = {};
  std::stringstream in(text);
  std::string item;
  int n = 0;
  while (std::getline(in, item, ',')) {
    while (!item.empty() && item.front() == ' ') item.erase(item.begin());
    while (!item.empty() && item.back() == ' ') item.pop_back();
    int found = -1;
    for (int b = 0; b < kButtonCount; ++b) {
      if (item == kNames[b]) found = b;
    }
    if (found < 0 || n >= kActionCount || used[found]) return false;
    used[found] = true;
    layout[size_t(n++)] = Button(found);
  }
  if (n != kActionCount) return false;
  *out = layout;
  return true;
}

// The layout in use, packed (3 bits a move, bit 31: set), for the game thread's reads: the cvar's text is read once.
std::atomic<uint32_t> g_packed{0};

uint32_t Pack(const Layout& layout) {
  uint32_t v = 0x80000000u;
  for (int a = 0; a < kActionCount; ++a) v |= uint32_t(layout[size_t(a)]) << (3 * a);
  return v;
}

Layout Unpack(uint32_t v) {
  Layout layout{};
  for (int a = 0; a < kActionCount; ++a) layout[size_t(a)] = Button((v >> (3 * a)) & 7);
  return layout;
}

Layout FromCvar() {
  const std::string text = REXCVAR_GET(mk9_net_buttons);
  Layout layout = kGameLayout;
  if (!text.empty() && !Parse(text, &layout)) {
    REXLOG_WARN("[net] mk9_net_buttons '{}' is not eight different buttons of X, Y, A, B, RB, LB, RT, LT: the "
                "game's own",
                text);
    layout = kGameLayout;
  }
  if (layout != kGameLayout) REXLOG_INFO("[net] the online fights' buttons: {} (mk9_net_buttons)", text);
  return layout;
}

}  // namespace

const char* ActionLabel(int action) { return action >= 0 && action < kActionCount ? kActions[action] : ""; }

const char* ButtonName(Button button) { return button < kButtonCount ? kNames[button] : "?"; }

const char* ButtonLabel(Button button) { return button < kButtonCount ? kLabels[button] : "?"; }

Layout Current() {
  uint32_t v = g_packed.load(std::memory_order_acquire);
  if (!v) {
    uint32_t expected = 0;
    g_packed.compare_exchange_strong(expected, Pack(FromCvar()), std::memory_order_acq_rel);
    v = g_packed.load(std::memory_order_acquire);
  }
  return Unpack(v);
}

void SetCurrent(const Layout& layout) { g_packed.store(Pack(layout), std::memory_order_release); }

std::string Encode(const Layout& layout) {
  if (layout == kGameLayout) return "";
  std::string out;
  for (int a = 0; a < kActionCount; ++a) {
    if (a) out += ',';
    out += kNames[layout[size_t(a)]];
  }
  return out;
}

void ToGameButtons(uint8_t* gamepad) {
  const Layout layout = Current();
  if (layout == kGameLayout) return;
  uint8_t in[12];
  std::memcpy(in, gamepad, sizeof(in));
  // the eight released, then each move's game button pressed when the player's button for it is
  uint16_t buttons = uint16_t(uint16_t(in[0]) << 8 | in[1]);
  for (uint16_t bit : kBits) buttons = uint16_t(buttons & ~bit);
  uint8_t left = 0, right = 0;
  for (int a = 0; a < kActionCount; ++a) {
    if (!Pressed(in, layout[size_t(a)])) continue;
    const Button game = kGameLayout[size_t(a)];
    if (game == kLT) left = 255;
    else if (game == kRT) right = 255;
    else buttons = uint16_t(buttons | kBits[game]);
  }
  gamepad[0] = uint8_t(buttons >> 8);
  gamepad[1] = uint8_t(buttons & 0xFF);
  gamepad[2] = left;
  gamepad[3] = right;
  // the first pad it changed, in the log (a player's report, the tests)
  static std::atomic<bool> logged{false};
  if (std::memcmp(in, gamepad, 4) != 0 && !logged.exchange(true)) {
    REXLOG_INFO("[net] the online fights' buttons ({}): the pad's {:04X} (triggers {} {}) went as the game's {:04X} "
                "(triggers {} {})",
                Encode(layout), uint16_t(uint16_t(in[0]) << 8 | in[1]), in[2], in[3], buttons, left, right);
  }
}

}  // namespace mk9::online_buttons
