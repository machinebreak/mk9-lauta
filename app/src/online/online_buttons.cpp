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

bool Usable(const AnalogLimits& limits) {
  for (const float v : {limits.left_dead_zone, limits.right_dead_zone, limits.trigger_threshold}) {
    if (!(v >= 0.0f && v < 1.0f)) return false;  // NaN too
  }
  return true;
}

namespace {

// sub_829D4978, step by step in its single precision: an axis made a value from -1 to 1 past the dead zone. Its sign is
// the direction sub_829D4ED8 sets. (Its negative side adds 1 before dividing: the first value past the dead zone on that
// side can come out a hair above 0, a direction the other way, and so it does here too.)
float GameAxis(int16_t axis, float dead_zone) {
  const double half = 32767.0;
  const double scaled = float(double(dead_zone) * half);
  const double shifted = float(double(float(axis)) + half);
  const double upper = float(scaled + half);
  const double lower = float(half - scaled);
  if (shifted > upper) return float(float(shifted - upper) / lower);
  if (shifted < lower) {
    const double past = float(float(shifted + scaled) + 1.0);
    return float(-double(float(float(half - past) / lower)));
  }
  return 0.0f;
}

// The steadied axis at bytes p (big-endian): 32767 and -32768 give the game exactly 1.0 and -1.0.
bool SteadyAxis(uint8_t* p, float dead_zone) {
  const int16_t axis = int16_t(uint16_t(p[0]) << 8 | p[1]);
  const float value = GameAxis(axis, dead_zone);
  const int sign = value > 0.0f ? 1 : value < 0.0f ? -1 : 0;
  const int16_t steady = sign > 0 ? int16_t(32767) : sign < 0 ? int16_t(-32768) : int16_t(0);
  if (steady == axis) return false;
  p[0] = uint8_t(uint16_t(steady) >> 8);
  p[1] = uint8_t(uint16_t(steady) & 0xFF);
  return true;
}

// sub_829D4ED8's trigger: pressed when its value is past the threshold times 255 (single precision).
bool SteadyTrigger(uint8_t* p, float threshold) {
  const float limit = float(double(threshold) * 255.0);
  const uint8_t steady = float(*p) > limit ? 255 : 0;
  if (steady == *p) return false;
  *p = steady;
  return true;
}

}  // namespace

bool ToGameAnalog(uint8_t* gamepad, const AnalogLimits& limits) {
  if (!Usable(limits)) return false;
  bool changed = SteadyTrigger(gamepad + 2, limits.trigger_threshold);
  changed |= SteadyTrigger(gamepad + 3, limits.trigger_threshold);
  changed |= SteadyAxis(gamepad + 4, limits.left_dead_zone);
  changed |= SteadyAxis(gamepad + 6, limits.left_dead_zone);
  changed |= SteadyAxis(gamepad + 8, limits.right_dead_zone);
  changed |= SteadyAxis(gamepad + 10, limits.right_dead_zone);
  return changed;
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
