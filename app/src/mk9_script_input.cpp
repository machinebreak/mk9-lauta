// Scripted gamepad (see mk9_script_input.h).
//
// A script is a list of events, one per line in mk9_input_script_file or separated by ';' in mk9_input_script:
//
//     <seconds after start> <BUTTON[+BUTTON...]> [hold seconds, default 0.15]
//
//   20 START          press Start at 20 s for 0.15 s
//   25.5 DOWN         d-pad down
//   30 A+B 0.5        A and B together for half a second
//   40 LS_RIGHT 2     left stick fully right for 2 s
//
// Buttons: A B X Y START BACK LB RB LT RT UP DOWN LEFT RIGHT LS RS (stick clicks), LS_UP LS_DOWN LS_LEFT LS_RIGHT,
// RS_UP RS_DOWN RS_LEFT RS_RIGHT. mk9-lauta (2026-11-13): "P2:" before them (P2:A, P3:LEFT, P4:X+Y) is the second,
// third or fourth scripted pad, a device each, on players 2-4 the way pads are (mk9_controls.cpp); none is the
// first, on player 1. '#' starts a comment. Times count from the creation of the input system, which is
// within a second of the process start (the snapshot times of mk9_snapshot_s count from about the same point).
// Every press and release is logged ("[script] ..."), so the log lines up with snapshots.
//
// mk9-lauta (2026-11-21): "@<anchor> <seconds> <BUTTONS> [hold]" counts from an anchor instead of the start: "@fight"
// the first frame the fight is in play (fight_state kStatePlay), "@<package>" the first time the game asks for that
// package (online/loading.cpp: "@FatalityArt" the Finish Him). Whatever the loads before took, the inputs land at the
// same moment of the fight. "!<anchor>" after the buttons: the event does not start once that anchor came (a fight's
// attacks "!FatalityArt": none in the Finish Him).

#include "mk9_script_input.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstring>
#include <fstream>
#include <mutex>
#include <sstream>
#include <string>
#include <vector>

#include <rex/cvar.h>
#include <rex/input/input_driver.h>
#include <rex/input/input_system.h>
#include <rex/logging.h>
#include <fmt/format.h>

#include "mk9_controls.h"
#include "online/fight_state.h"
#include "online/snapshot.h"

extern std::atomic<uint64_t> g_mk9_game_frames;  // src/mk9_frame_counter.cpp

REXCVAR_DEFINE_STRING(mk9_input_script, "", "MK9",
                      "Scripted gamepad events for tests, ';'-separated: \"<seconds> <BUTTON[+BUTTON]> [hold]\" "
                      "(e.g. \"20 START;25 A\"); empty = off");
REXCVAR_DEFINE_BOOL(mk9_input_script_frames, false, "MK9",
                    "Scripted gamepad times count game frames / 60 (presented frames, src/mk9_frame_counter.cpp) "
                    "instead of wall-clock seconds: with vblank_per_swap two runs press at the same frames");
REXCVAR_DEFINE_STRING(mk9_input_script_file, "", "MK9",
                      "File with scripted gamepad events, one per line (same format as mk9_input_script)");

namespace mk9::script_input {
namespace {

using namespace rex;  // X_STATUS, X_RESULT and their values

using rex::input::DeviceId;
using rex::input::DeviceInfo;

constexpr DeviceId kScriptDevice = static_cast<DeviceId>(0x55534634);  // "MK9": pad 1; pad N is this + N - 1
constexpr int kScriptPads = 4;

// The scripted pad a device is (0-3), or -1 when it is not one.
int PadOf(DeviceId id) {
  const uint32_t n = uint32_t(id) - uint32_t(kScriptDevice);
  return n < uint32_t(kScriptPads) ? int(n) : -1;
}

struct Event {
  std::string anchor;  // lower case: its time counts from that anchor's ("": from the start)
  std::string until;   // lower case: it does not start once that anchor came ("": always)
  int pad = 0;  // 0-3: P1 (none), P2:, P3:, P4:
  double start_s = 0;
  double hold_s = 0.15;
  uint16_t buttons = 0;
  uint8_t left_trigger = 0, right_trigger = 0;
  int16_t lx = 0, ly = 0, rx = 0, ry = 0;
  std::string text;
};

bool ApplyName(const std::string& name, Event& e) {
  struct Button {
    const char* name;
    uint16_t bit;
  };
  static const Button kButtons[] = {
      {"UP", rex::input::X_INPUT_GAMEPAD_DPAD_UP},       {"DOWN", rex::input::X_INPUT_GAMEPAD_DPAD_DOWN},
      {"LEFT", rex::input::X_INPUT_GAMEPAD_DPAD_LEFT},   {"RIGHT", rex::input::X_INPUT_GAMEPAD_DPAD_RIGHT},
      {"START", rex::input::X_INPUT_GAMEPAD_START},      {"BACK", rex::input::X_INPUT_GAMEPAD_BACK},
      {"LS", rex::input::X_INPUT_GAMEPAD_LEFT_THUMB},    {"RS", rex::input::X_INPUT_GAMEPAD_RIGHT_THUMB},
      {"LB", rex::input::X_INPUT_GAMEPAD_LEFT_SHOULDER}, {"RB", rex::input::X_INPUT_GAMEPAD_RIGHT_SHOULDER},
      {"A", rex::input::X_INPUT_GAMEPAD_A},              {"B", rex::input::X_INPUT_GAMEPAD_B},
      {"X", rex::input::X_INPUT_GAMEPAD_X},              {"Y", rex::input::X_INPUT_GAMEPAD_Y},
  };
  for (const auto& b : kButtons) {
    if (name == b.name) {
      e.buttons |= b.bit;
      return true;
    }
  }
  constexpr int16_t kFull = 32767, kFullNeg = -32768;
  if (name == "LT") e.left_trigger = 255;
  else if (name == "RT") e.right_trigger = 255;
  else if (name == "LS_UP") e.ly = kFull;
  else if (name == "LS_DOWN") e.ly = kFullNeg;
  else if (name == "LS_LEFT") e.lx = kFullNeg;
  else if (name == "LS_RIGHT") e.lx = kFull;
  else if (name == "RS_UP") e.ry = kFull;
  else if (name == "RS_DOWN") e.ry = kFullNeg;
  else if (name == "RS_LEFT") e.rx = kFullNeg;
  else if (name == "RS_RIGHT") e.rx = kFull;
  else return false;
  return true;
}

// The anchors the script names (lower case) and when each came (the script's clock; < 0: not yet).
std::mutex g_anchor_mutex;
std::vector<std::pair<std::string, double>> g_anchors;
std::atomic<bool> g_has_anchors{false};
std::chrono::steady_clock::time_point g_start = std::chrono::steady_clock::now();

// The script's clock: game frames / 60 with mk9_input_script_frames (the SyncTest's frame in its runs), else seconds
// since the input system was made.
double Now() {
  const int64_t synctest_frame = mk9::snapshot::SyncTestScriptFrame();
  return synctest_frame >= 0 && REXCVAR_GET(mk9_input_script_frames) ? double(synctest_frame) / 60.0
         : REXCVAR_GET(mk9_input_script_frames)
             ? double(g_mk9_game_frames.load(std::memory_order_relaxed)) / 60.0
             : std::chrono::duration<double>(std::chrono::steady_clock::now() - g_start).count();
}

std::string Lower(std::string s) {
  std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return char(std::tolower(c)); });
  return s;
}

void Want(const std::string& anchor) {
  std::lock_guard lock(g_anchor_mutex);
  for (const auto& [name, at] : g_anchors) {
    if (name == anchor) return;
  }
  g_anchors.emplace_back(anchor, -1.0);
  g_has_anchors = true;
}

// An anchor's time (< 0: not yet).
double AnchorTime(const std::string& anchor) {
  std::lock_guard lock(g_anchor_mutex);
  for (const auto& [name, at] : g_anchors) {
    if (name == anchor) return at;
  }
  return -1.0;
}

void Reach(const std::string& anchor) {
  if (!g_has_anchors.load(std::memory_order_relaxed)) return;
  const std::string key = Lower(anchor);
  std::lock_guard lock(g_anchor_mutex);
  for (auto& [name, at] : g_anchors) {
    if (name == key && at < 0) {
      at = Now();
      REXLOG_INFO("[script] anchor '{}' at {:.2f} s", anchor, at);
    }
  }
}

void ParseLine(std::string line, std::vector<Event>& out) {
  if (auto hash = line.find('#'); hash != std::string::npos) line.resize(hash);
  std::istringstream in(line);
  Event e;
  std::string names;
  if (const size_t first = line.find_first_not_of(" \t"); first != std::string::npos && line[first] == '@') {
    in >> e.anchor;
    e.anchor = Lower(e.anchor.substr(1));
    if (e.anchor.empty()) return;
    Want(e.anchor);
  }
  if (!(in >> e.start_s >> names)) return;
  for (std::string word; in >> word;) {  // the hold, "!<anchor>"
    if (word[0] == '!' && word.size() > 1) {
      e.until = Lower(word.substr(1));
      Want(e.until);
    } else {
      try {
        e.hold_s = std::stod(word);
      } catch (...) {
      }
    }
  }
  if (names.size() > 3 && (names[0] == 'P' || names[0] == 'p') && names[1] >= '1' && names[1] <= '4' &&
      names[2] == ':') {
    e.pad = names[1] - '1';
    names = names.substr(3);
  }
  std::istringstream parts(names);
  for (std::string name; std::getline(parts, name, '+');) {
    std::transform(name.begin(), name.end(), name.begin(), ::toupper);
    if (!ApplyName(name, e)) {
      REXLOG_WARN("[script] unknown button '{}' in \"{}\"", name, line);
      return;
    }
  }
  e.text = names;
  out.push_back(e);
}

class ScriptInputDriver final : public rex::input::InputDriver {
 public:
  explicit ScriptInputDriver(std::vector<Event> events)
      : InputDriver(nullptr, 0), events_(std::move(events)), logged_(events_.size(), 0) {
    g_start = std::chrono::steady_clock::now();
    used_[0] = true;  // pad 1 always (the tests' player 1)
    for (const Event& e : events_) used_[e.pad] = true;
  }

  X_STATUS Setup() override { return X_STATUS_SUCCESS; }

  // A device for each pad the script uses: "Script" (pad 1), "Script 2"... (mk9_controls.cpp seats them).
  void EnumerateDevices(std::vector<DeviceInfo>& out) override {
    for (int pad = 0; pad < kScriptPads; ++pad) {
      if (!used_[pad]) continue;
      DeviceInfo info;
      info.id = static_cast<DeviceId>(uint32_t(kScriptDevice) + uint32_t(pad));
      info.name = pad ? "Script " + std::to_string(pad + 1) : std::string("Script");
      info.synthetic = true;
      out.push_back(info);
    }
  }

  X_RESULT GetDeviceState(DeviceId id, rex::input::X_INPUT_STATE* out_state) override {
    const int pad = PadOf(id);
    if (pad < 0 || !used_[pad]) return X_ERROR_DEVICE_NOT_CONNECTED;
    if (!out_state) return X_ERROR_SUCCESS;
    const double now = Now();
    uint16_t buttons = 0;
    uint8_t lt = 0, rt = 0;
    int16_t lx = 0, ly = 0, rx = 0, ry = 0;
    std::lock_guard lock(mutex_);
    for (size_t i = 0; i < events_.size(); ++i) {
      const auto& e = events_[i];
      if (e.pad != pad) continue;
      double start = e.start_s;
      if (!e.anchor.empty()) {  // from its anchor, once it came
        const double at = AnchorTime(e.anchor);
        if (at < 0) continue;
        start += at;
      }
      if (!e.until.empty() && logged_[i] == 0) {  // not started yet, and its "until" anchor came before its start
        const double until = AnchorTime(e.until);
        if (until >= 0 && start >= until) continue;
      }
      const bool active = now >= start && now < start + e.hold_s;
      if (active) {
        buttons |= e.buttons;
        lt = std::max(lt, e.left_trigger);
        rt = std::max(rt, e.right_trigger);
        if (e.lx) lx = e.lx;
        if (e.ly) ly = e.ly;
        if (e.rx) rx = e.rx;
        if (e.ry) ry = e.ry;
      }
      // 0 = not yet, 1 = pressed, 2 = released: log each transition once.
      if (active && logged_[i] == 0) {
        logged_[i] = 1;
        REXLOG_INFO("[script] {:.2f} s: press {}{} (for {:.2f} s)", now, e.pad ? fmt::format("P{}:", e.pad + 1) : "",
                    e.text, e.hold_s);
      } else if (!active && now >= start + e.hold_s && logged_[i] < 2) {
        logged_[i] = 2;
        REXLOG_INFO("[script] {:.2f} s: release {}{}", now, e.pad ? fmt::format("P{}:", e.pad + 1) : "", e.text);
      }
    }
    const uint64_t key = uint64_t(buttons) | uint64_t(lt) << 16 | uint64_t(rt) << 24 |
                         uint64_t(uint16_t(lx)) << 32 | uint64_t(uint16_t(ly)) << 48;
    PadState& last = last_[pad];
    if (key != last.key || rx != last.rx || ry != last.ry) {
      ++last.packet;
      last.key = key;
      last.rx = rx;
      last.ry = ry;
    }
    std::memset(out_state, 0, sizeof(*out_state));
    out_state->packet_number = last.packet;
    out_state->gamepad.buttons = buttons;
    out_state->gamepad.left_trigger = lt;
    out_state->gamepad.right_trigger = rt;
    out_state->gamepad.thumb_lx = lx;
    out_state->gamepad.thumb_ly = ly;
    out_state->gamepad.thumb_rx = rx;
    out_state->gamepad.thumb_ry = ry;
    return X_ERROR_SUCCESS;
  }

  X_RESULT GetDeviceCapabilities(DeviceId id, uint32_t, rex::input::X_INPUT_CAPABILITIES* out_caps) override {
    if (PadOf(id) < 0 || !used_[PadOf(id)]) return X_ERROR_DEVICE_NOT_CONNECTED;
    if (out_caps) {
      std::memset(out_caps, 0, sizeof(*out_caps));
      out_caps->type = 0x01;      // XINPUT_DEVTYPE_GAMEPAD
      out_caps->sub_type = 0x01;  // XINPUT_DEVSUBTYPE_GAMEPAD
      out_caps->gamepad.buttons = 0xFFFF;
      out_caps->gamepad.left_trigger = 0xFF;
      out_caps->gamepad.right_trigger = 0xFF;
      out_caps->gamepad.thumb_lx = static_cast<int16_t>(0x7FFF);
      out_caps->gamepad.thumb_ly = static_cast<int16_t>(0x7FFF);
      out_caps->gamepad.thumb_rx = static_cast<int16_t>(0x7FFF);
      out_caps->gamepad.thumb_ry = static_cast<int16_t>(0x7FFF);
    }
    return X_ERROR_SUCCESS;
  }

  X_RESULT SetDeviceVibration(DeviceId id, rex::input::X_INPUT_VIBRATION*) override {
    return PadOf(id) >= 0 && used_[PadOf(id)] ? X_ERROR_SUCCESS : X_ERROR_DEVICE_NOT_CONNECTED;
  }

  X_RESULT GetDeviceKeystroke(DeviceId id, uint32_t, rex::input::X_INPUT_KEYSTROKE*) override {
    return PadOf(id) >= 0 && used_[PadOf(id)] ? X_ERROR_EMPTY : X_ERROR_DEVICE_NOT_CONNECTED;
  }

 private:
  std::vector<Event> events_;
  std::mutex mutex_;
  std::vector<uint8_t> logged_;
  struct PadState {
    uint32_t packet = 1;
    uint64_t key = 0;
    int16_t rx = 0, ry = 0;
  };
  PadState last_[kScriptPads];
  bool used_[kScriptPads] = {};
};

std::vector<Event> LoadScript() {
  std::vector<Event> events;
  std::istringstream inline_script{std::string(REXCVAR_GET(mk9_input_script))};
  for (std::string line; std::getline(inline_script, line, ';');) ParseLine(line, events);
  const std::string path = REXCVAR_GET(mk9_input_script_file);
  if (!path.empty()) {
    std::ifstream file(path);
    if (!file) REXLOG_WARN("[script] cannot open {}", path);
    for (std::string line; std::getline(file, line);) ParseLine(line, events);
  }
  return events;
}

}  // namespace

void NotePackage(const std::string& name) { Reach(name); }

void OnFrame(uint8_t* base) {
  if (!g_has_anchors.load(std::memory_order_relaxed)) return;
  if (mk9::fight_state::State(base) == mk9::fight_state::kStatePlay) Reach("fight");
}

std::unique_ptr<rex::system::IInputSystem> CreateInputSystem(bool tool_mode) {
  mk9::controls::FindMappingsFile();  // before SDL starts and reads it
  auto input = rex::input::CreateDefaultInputSystem(tool_mode);
  // mk9-nx (2026-11-08): the keyboard on the player the options say (mk9_controls.h)
  input->SetDeviceAssignment(mk9::controls::CreateAssignment());
  if (!tool_mode) {
    auto events = LoadScript();
    if (!events.empty()) {
      int pads = 1;
      for (const Event& e : events) pads = std::max(pads, e.pad + 1);
      REXLOG_INFO("[script] scripted gamepad on player 1{}: {} events, last at {:.1f} s",
                  pads > 1 ? fmt::format(" (and pads 2-{} on the next players)", pads) : std::string(), events.size(),
                  events.back().start_s);
      input->AddDriver(std::make_unique<ScriptInputDriver>(std::move(events)));
    }
  }
  return input;
}

}  // namespace mk9::script_input
