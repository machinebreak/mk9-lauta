// MK9 online: a room of two (room.h).

#include "online/room.h"

#include <chrono>
#include <mutex>
#include <random>
#include <utility>

#include <fmt/format.h>
#include <rex/cvar.h>
#include <rex/logging.h>

#include "online/flags.h"
#include "online/i18n.h"
#include "online/lobby.h"
#include "online/netplay.h"
#include "online/notify.h"
#include "online/session.h"

REXCVAR_DEFINE_STRING(mk9_online_test, "", "MK9 Online",
                      "Tests: 'host' opens a public room, 'quick' looks for one; each is ready once the rival is in");

namespace mk9::room {
namespace {

using i18n::T;

// A runtime format (fmt) of a translated text.
template <typename... Args>
std::string Tf(const char* es, Args&&... args) {
  return fmt::format(fmt::runtime(T(es)), std::forward<Args>(args)...);
}

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
  g.s.title = Tf("Sala de {}", g.s.me);
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
  g.s.status = Tf("Entrando a la sala {}...", code);
  g.guest_id = NewId();
  g.joined_at = Clock::now();
  g.hi_at = {};
  lobby::Join(code, g.guest_id);
  REXLOG_INFO("[room] joining {}", code);
}

// The other player left (or went quiet): the host waits for another, the guest is out. why: a text with the name's
// place ("{} se fue de la sala").
void OtherGoneLocked(const char* why) {
  if (g.s.phase != Phase::kTogether) return;
  const std::string other = g.s.other.empty() ? T("El rival") : g.s.other;
  if (g.s.host && !g.s.in_session) {
    g.s.phase = Phase::kHosting;
    g.s.other.clear();
    g.s.other_country.clear();
    g.s.my_ready = g.s.other_ready = false;
    g.guest_id.clear();
    g.s.status = T("Esperando un rival");
    lobby::Host(g.s.code, g.s.title, g.s.open, false);
  } else {
    g.s.phase = Phase::kNone;
    g.s.status = Tf(why, other);
    if (!g.s.host) lobby::Join("", "");
  }
  AddLine("", Tf(why, other), true);
}

void MaybeStartLocked() {
  if (!g.s.host || g.s.in_session || g.s.phase != Phase::kTogether || !g.s.my_ready || !g.s.other_ready) return;
  SendOther({{"room", "start"}});
  g.s.phase = Phase::kStarting;
  g.s.status = T("Arranca la pelea");
  g.start_pending = true;
  g.start_at = Clock::now() + std::chrono::milliseconds(1200);  // the guest gets the start first
  Touch();
}

void OnMessageLocked(const json& d) {
  const std::string kind = d.value("room", "");
  if (kind.empty()) return;
  g.heard = Clock::now();
  // the other player's country, with its hello and its keepalives (it may have found it after the hello)
  if (const std::string cc = d.value("cc", ""); cc.size() == 2 && g.s.phase == Phase::kTogether) {
    if (g.s.other_country != cc) {
      g.s.other_country = cc;
      Touch();
    }
  }
  if (g.s.host) {
    if (kind == "hi") {
      const std::string id = d.value("id", ""), name = d.value("name", "");
      if (id.empty()) return;
      // mk9-lauta (2026-11-10): a player this one silenced finds the room full
      if (session::IsBlocked(name) && !g.s.in_session) {
        lobby::SigToGuest(g.s.code, id, {{"room", "full"}});
        return;
      }
      if (g.s.phase == Phase::kHosting || (g.s.phase == Phase::kTogether && id == g.guest_id)) {
        const bool fresh = g.s.phase == Phase::kHosting;
        g.guest_id = id;
        g.s.other = name;
        g.s.other_country = d.value("cc", "");
        g.s.phase = Phase::kTogether;
        lobby::SigToGuest(g.s.code, id, {{"room", "welcome"}, {"name", g.s.me}, {"cc", flags::Mine()}});
        if (fresh) {
          g.s.my_ready = g.s.other_ready = false;
          g.s.status = T("Rival en la sala");
          lobby::Host(g.s.code, g.s.title, g.s.open, true);
          AddLine("", Tf("{} entró a la sala", name), true);
          if (!g.s.in_session) notify::Attention(true);
        }
      } else {
        lobby::SigToGuest(g.s.code, id, {{"room", "full"}});
      }
      return;
    }
  } else {
    if (kind == "welcome" && g.s.phase == Phase::kJoining) {
      g.s.other = d.value("name", "");
      g.s.other_country = d.value("cc", "");
      g.s.phase = Phase::kTogether;
      g.s.status = Tf("En la sala de {}", g.s.other);
      AddLine("", Tf("Entraste a la sala de {}", g.s.other), true);
      if (!g.s.in_session) notify::Attention(true);
      return;
    }
    if (kind == "full" && g.s.phase == Phase::kJoining) {
      Reset();
      g.s.status = T("Esa sala ya tiene dos jugadores");
      lobby::Join("", "");
      return;
    }
    if (kind == "start" && g.s.phase == Phase::kTogether && !g.s.in_session) {
      g.s.phase = Phase::kStarting;
      g.s.status = T("Arranca la pelea");
      g.start_pending = true;
      g.start_at = Clock::now() + std::chrono::milliseconds(300);
      Touch();
      return;
    }
  }
  if (g.s.phase != Phase::kTogether) return;
  if (kind == "chat") {
    AddLine(g.s.other, d.value("text", ""), false);
    notify::Attention(false);
  } else if (kind == "ready") {
    g.s.other_ready = d.value("on", false);
    if (g.s.other_ready) notify::Attention(true);
    Touch();
    MaybeStartLocked();
  } else if (kind == "bye") {
    if (g.s.in_session) netplay::PeerLeft();  // its goodbye over the session's own path may not have come
    OtherGoneLocked("{} se fue de la sala");
  }
}

}  // namespace

void CreateRoom(bool open) {
  std::lock_guard<std::mutex> lock(g.mutex);
  HostLocked(open, T(open ? "Sala pública: esperando un rival" : "Sala privada: pasale el código a tu rival"));
}

void JoinRoom(const std::string& code) {
  const std::string c = Normalize(code);
  std::lock_guard<std::mutex> lock(g.mutex);
  if (c.size() < 4) {
    g.s.status = T("El código tiene que tener de 4 a 8 letras o números");
    Touch();
    return;
  }
  JoinLocked(c);
}

void QuickMatch() {
  std::lock_guard<std::mutex> lock(g.mutex);
  Reset();
  g.s.phase = Phase::kSearching;
  g.s.status = T("Buscando rival...");
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
  g.s.status = T("Saliste de la sala");
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
  g.s.status = T("Conectando con el rival");
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
        HostLocked(true, T("Nadie esperando: tu sala quedó abierta para el próximo que busque"));
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
        lobby::SigToHost(g.s.code, {{"room", "hi"}, {"name", g.s.me}, {"id", g.guest_id}, {"cc", flags::Mine()}});
      }
      if (!g.s.in_session && now - g.joined_at > kJoinFor) {
        const std::string code_was = g.s.code;
        Reset();
        g.s.status = Tf("La sala {} no responde", code_was);
        lobby::Join("", "");
      }
    } else if (g.s.phase == Phase::kTogether) {
      if (now - g.said > kPingEvery) {
        g.said = now;
        SendOther({{"room", "ping"}, {"cc", flags::Mine()}});
      }
      // in a session the fight's own path says whether the other player is there (the lobby may be what is down)
      if (now - g.heard > kGone && !g.s.in_session) OtherGoneLocked("{} se desconectó");
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
