// MK9 online: a room of two (room.h).

#include "online/room.h"

#include <chrono>
#include <mutex>
#include <random>

#include <fmt/format.h>
#include <rex/cvar.h>
#include <rex/logging.h>

#include "online/lobby.h"
#include "online/session.h"

REXCVAR_DEFINE_STRING(mk9_online_test, "", "MK9 Online",
                      "Tests: 'host' opens a public room, 'quick' looks for one; each is ready once the rival is in");

namespace mk9::room {
namespace {

using json = nlohmann::json;
using Clock = std::chrono::steady_clock;

constexpr auto kHiEvery = std::chrono::seconds(1);
constexpr auto kPingEvery = std::chrono::seconds(3);
constexpr auto kGone = std::chrono::seconds(12);
constexpr auto kJoinFor = std::chrono::seconds(10);
constexpr size_t kMaxLines = 100;

struct Room {
  std::mutex mutex;
  State s;
  std::string guest_id;            // the host's: its guest's; the guest's: its own
  Clock::time_point heard{}, said{}, hi_at{}, joined_at{};
  bool start_pending = false;
  Clock::time_point start_at{};
};
Room g;

std::string NewCode() {
  static const char kLetters[] = "ABCDEFGHJKLMNPQRSTUVWXYZ23456789";  // no I, O, 0 or 1
  std::random_device rd;
  std::string code;
  for (int i = 0; i < 6; ++i) code += kLetters[rd() % 32];
  return code;
}

std::string NewId() {
  std::random_device rd;
  return fmt::format("{:08x}{:08x}", rd(), rd());
}

std::string Normalize(const std::string& code) {
  std::string out;
  for (char c : code) {
    if (c >= 'a' && c <= 'z') c = char(c - 'a' + 'A');
    if ((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')) out += c;
  }
  return out.substr(0, 8);
}

std::string MyName() {
  const lobby::State lobby = lobby::Snapshot();
  return lobby.name.empty() ? session::LoadName() : lobby.name;
}

// The mutex held from here on.
void Touch() { ++g.s.version; }

void AddLine(const std::string& name, const std::string& text, bool system) {
  g.s.chat.push_back({name, text, system});
  while (g.s.chat.size() > kMaxLines) g.s.chat.erase(g.s.chat.begin());
  Touch();
}

void SendOther(const json& data) {
  if (g.s.host) {
    if (!g.guest_id.empty()) lobby::SigToGuest(g.s.code, g.guest_id, data);
  } else {
    lobby::SigToHost(g.s.code, data);
  }
}

void Reset() {
  const bool in_session = g.s.in_session;
  std::vector<Line> keep;  // nothing: a new room starts with its own chat
  g.s = State{};
  g.s.in_session = in_session;
  g.guest_id.clear();
  g.start_pending = false;
  g.s.chat = std::move(keep);
  Touch();
}

void HostLocked(bool open, const std::string& status) {
  Reset();
  g.s.phase = Phase::kHosting;
  g.s.code = NewCode();
  g.s.open = open;
  g.s.host = true;
  g.s.me = MyName();
  g.s.title = "Sala de " + g.s.me;
  g.s.status = status;
  lobby::Host(g.s.code, g.s.title, open, false);
  REXLOG_INFO("[room] hosting {} ({})", g.s.code, open ? "open" : "private");
}

void JoinLocked(const std::string& code) {
  Reset();
  g.s.phase = Phase::kJoining;
  g.s.code = code;
  g.s.host = false;
  g.s.me = MyName();
  g.s.status = "Entrando a la sala " + code + "...";
  g.guest_id = NewId();
  g.joined_at = Clock::now();
  g.hi_at = {};
  lobby::Join(code, g.guest_id);
  REXLOG_INFO("[room] joining {}", code);
}

// The other player left (or went quiet): the host waits for another, the guest is out.
void OtherGoneLocked(const std::string& why) {
  if (g.s.phase != Phase::kTogether) return;
  const std::string other = g.s.other.empty() ? "El rival" : g.s.other;
  if (g.s.host && !g.s.in_session) {
    g.s.phase = Phase::kHosting;
    g.s.other.clear();
    g.s.my_ready = g.s.other_ready = false;
    g.guest_id.clear();
    g.s.status = "Esperando un rival";
    lobby::Host(g.s.code, g.s.title, g.s.open, false);
  } else {
    g.s.phase = Phase::kNone;
    g.s.status = other + " " + why;
    if (!g.s.host) lobby::Join("", "");
  }
  AddLine("", other + " " + why, true);
}

void MaybeStartLocked() {
  if (!g.s.host || g.s.in_session || g.s.phase != Phase::kTogether || !g.s.my_ready || !g.s.other_ready) return;
  SendOther({{"room", "start"}});
  g.s.phase = Phase::kStarting;
  g.s.status = "Arranca la pelea";
  g.start_pending = true;
  g.start_at = Clock::now() + std::chrono::milliseconds(1200);  // the guest gets the start first
  Touch();
}

void OnMessageLocked(const json& d) {
  const std::string kind = d.value("room", "");
  if (kind.empty()) return;
  g.heard = Clock::now();
  if (g.s.host) {
    if (kind == "hi") {
      const std::string id = d.value("id", ""), name = d.value("name", "");
      if (id.empty()) return;
      if (g.s.phase == Phase::kHosting || (g.s.phase == Phase::kTogether && id == g.guest_id)) {
        const bool fresh = g.s.phase == Phase::kHosting;
        g.guest_id = id;
        g.s.other = name;
        g.s.phase = Phase::kTogether;
        lobby::SigToGuest(g.s.code, id, {{"room", "welcome"}, {"name", g.s.me}});
        if (fresh) {
          g.s.my_ready = g.s.other_ready = false;
          g.s.status = "Rival en la sala";
          lobby::Host(g.s.code, g.s.title, g.s.open, true);
          AddLine("", name + " entró a la sala", true);
        }
      } else {
        lobby::SigToGuest(g.s.code, id, {{"room", "full"}});
      }
      return;
    }
  } else {
    if (kind == "welcome" && g.s.phase == Phase::kJoining) {
      g.s.other = d.value("name", "");
      g.s.phase = Phase::kTogether;
      g.s.status = "En la sala de " + g.s.other;
      AddLine("", "Entraste a la sala de " + g.s.other, true);
      return;
    }
    if (kind == "full" && g.s.phase == Phase::kJoining) {
      Reset();
      g.s.status = "Esa sala ya tiene dos jugadores";
      lobby::Join("", "");
      return;
    }
    if (kind == "start" && g.s.phase == Phase::kTogether && !g.s.in_session) {
      g.s.phase = Phase::kStarting;
      g.s.status = "Arranca la pelea";
      g.start_pending = true;
      g.start_at = Clock::now() + std::chrono::milliseconds(300);
      Touch();
      return;
    }
  }
  if (g.s.phase != Phase::kTogether) return;
  if (kind == "chat") {
    AddLine(g.s.other, d.value("text", ""), false);
  } else if (kind == "ready") {
    g.s.other_ready = d.value("on", false);
    Touch();
    MaybeStartLocked();
  } else if (kind == "bye") {
    OtherGoneLocked("se fue de la sala");
  }
}

}  // namespace

void CreateRoom(bool open) {
  std::lock_guard<std::mutex> lock(g.mutex);
  HostLocked(open, open ? "Sala pública: esperando un rival" : "Sala privada: pasale el código a tu rival");
}

void JoinRoom(const std::string& code) {
  const std::string c = Normalize(code);
  std::lock_guard<std::mutex> lock(g.mutex);
  if (c.size() < 4) {
    g.s.status = "El código tiene que tener de 4 a 8 letras o números";
    Touch();
    return;
  }
  JoinLocked(c);
}

void QuickMatch() {
  std::lock_guard<std::mutex> lock(g.mutex);
  Reset();
  g.s.phase = Phase::kSearching;
  g.s.status = "Buscando rival...";
  lobby::Quick();
}

void Leave() {
  std::lock_guard<std::mutex> lock(g.mutex);
  if (g.s.phase == Phase::kNone) return;
  SendOther({{"room", "bye"}});
  if (g.s.host) {
    lobby::Unhost();
  } else {
    lobby::Join("", "");
  }
  Reset();
  g.s.status = "Saliste de la sala";
}

void SetReady(bool ready) {
  std::lock_guard<std::mutex> lock(g.mutex);
  if (g.s.phase != Phase::kTogether || g.s.in_session) return;
  g.s.my_ready = ready;
  SendOther({{"room", "ready"}, {"on", ready}});
  Touch();
  MaybeStartLocked();
}

void Say(const std::string& text) {
  if (text.empty()) return;
  std::lock_guard<std::mutex> lock(g.mutex);
  if (g.s.phase != Phase::kTogether) return;
  SendOther({{"room", "chat"}, {"text", text.substr(0, 200)}});
  AddLine(g.s.me, text.substr(0, 200), false);
}

State Snapshot() {
  std::lock_guard<std::mutex> lock(g.mutex);
  return g.s;
}

void EnterSession(const std::string& code, bool host, const std::string& me, const std::string& id) {
  std::lock_guard<std::mutex> lock(g.mutex);
  g.s = State{};
  g.s.in_session = true;
  g.s.code = code;
  g.s.host = host;
  g.s.me = me;
  g.s.phase = host ? Phase::kHosting : Phase::kJoining;
  g.s.status = "Conectando con el rival";
  g.guest_id = host ? std::string() : id;  // the session (netplay.cpp) has hosted or joined the room already
  g.joined_at = Clock::now();
  g.heard = Clock::now();
  Touch();
}

// mk9_online_test: what a player would click, once the lobby has taken the name.
static void TestStep() {
  static const std::string test = REXCVAR_GET(mk9_online_test);
  static bool started = false;
  if (test.empty()) return;
  const State s = Snapshot();
  if (s.in_session) return;
  if (!started) {
    const lobby::State lobby = lobby::Snapshot();
    if (lobby.link != lobby::Link::kOnline || !lobby.named) return;
    started = true;
    REXLOG_INFO("[room] mk9_online_test={}", test);
    if (test == "host") CreateRoom(true);
    if (test == "quick") QuickMatch();
    return;
  }
  if (s.phase == Phase::kTogether && !s.my_ready) SetReady(true);
}

void Pump() {
  TestStep();
  lobby::Event event;
  while (lobby::PollEvent(&event)) {
    std::lock_guard<std::mutex> lock(g.mutex);
    if (event.kind == lobby::Event::Kind::kMatch) {
      if (g.s.phase != Phase::kSearching) continue;
      if (!event.room.empty()) {
        JoinLocked(event.room);
      } else {
        HostLocked(true, "Nadie esperando: tu sala quedó abierta para el próximo que busque");
      }
      continue;
    }
    if (event.room == g.s.code && event.data.is_object()) OnMessageLocked(event.data);
  }
  bool launch = false;
  std::string code, me;
  bool host = false;
  {
    std::lock_guard<std::mutex> lock(g.mutex);
    const auto now = Clock::now();
    if (g.s.phase == Phase::kJoining) {
      if (now - g.hi_at > kHiEvery) {
        g.hi_at = now;
        lobby::SigToHost(g.s.code, {{"room", "hi"}, {"name", g.s.me}, {"id", g.guest_id}});
      }
      if (!g.s.in_session && now - g.joined_at > kJoinFor) {
        const std::string code_was = g.s.code;
        Reset();
        g.s.status = "La sala " + code_was + " no responde";
        lobby::Join("", "");
      }
    } else if (g.s.phase == Phase::kTogether) {
      if (now - g.said > kPingEvery) {
        g.said = now;
        SendOther({{"room", "ping"}});
      }
      if (now - g.heard > kGone) OtherGoneLocked("se desconectó");
    }
    if (g.start_pending && now >= g.start_at) {
      g.start_pending = false;
      launch = true;
      code = g.s.code;
      me = g.s.me;
      host = g.s.host;
    }
  }
  if (launch) {
    lobby::Stop();
    session::Launch(code, host, me);
  }
}

}  // namespace mk9::room
