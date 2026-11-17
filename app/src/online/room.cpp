// MK9 online: a room of two, or of four (room.h).

#include "online/room.h"

#include <algorithm>
#include <chrono>
#include <initializer_list>
#include <mutex>
#include <random>
#include <unordered_map>
#include <utility>

#include <fmt/format.h>
#include <rex/cvar.h>
#include <rex/logging.h>

#include "mk9_kratos.h"
#include "online/avatars.h"
#include "online/feed.h"
#include "online/flags.h"
#include "online/i18n.h"
#include "online/lobby.h"
#include "online/netplay.h"
#include "online/notify.h"
#include "online/session.h"
#include "online/stats.h"

REXCVAR_DEFINE_STRING(mk9_online_test, "", "MK9 Online",
                      "Tests: 'host' opens a public room, 'quick' looks for one; each is ready once the rival is in; "
                      "'watch' watches the first room playing (its Watch button); 'host4' opens a public room of four, "
                      "'quick4' joins a room of four (or opens one); 'hostk' opens a public room of King of the Hill, "
                      "'joink' joins the first one in the list");
REXCVAR_DEFINE_INT32(mk9_online_test_players, 1, "MK9 Online",
                     "Tests: with mk9_online_test, the people at this PC the room says before it is ready (2: the "
                     "\"Somos dos en esta PC\" box)");
REXCVAR_DEFINE_INT32(mk9_online_test_start, 4, "MK9 Online",
                     "Tests: the host of a room of four (mk9_online_test=host4) starts the fight once this many "
                     "machines are in it, all of them ready")
    .range(2, 4);

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
constexpr int kMaxMachines = 4;
// mk9-lauta (2026-11-17): the session protocol of the releases that play rooms of four (netplay.cpp's 12), of the
// ones that play King of the Hill (13), and this release's (said in its hello)
constexpr int kFourProto = 12;
constexpr int kKothProto = 13;
constexpr int kProto = 13;

struct Room {
  std::mutex mutex;
  State s;
  std::string guest_id;            // the host's: its guest's (a room of two); the guest's: its own
  Clock::time_point heard{}, said{}, hi_at{}, joined_at{};
  bool start_pending = false;
  Clock::time_point start_at{};
  // mk9-lauta (2026-11-17)
  std::vector<std::string> start_machines;  // the machines a room of four's start named (its session's)
  std::vector<std::string> session_machines;  // the host in such a session: the guests it welcomes back
  std::unordered_map<std::string, Clock::time_point> heard_from;  // the host of a room of four: each guest's last word
  int want = 0;  // a guest: what it joined for (a quick one against one: 2; a quick two against two: 4)
  bool start_koth = false;  // mk9-lauta (2026-11-17): the start's session is King of the Hill's (mk9_net_koth)
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

// mk9-lauta (2026-11-11): this game's content beyond the disc, as the rooms say it ("kratos"; "" none).
std::string Mods() { return mk9::kratos::Installed() ? "kratos" : ""; }

// This player's country and record, for the other player (with the hello, the welcome and the keepalives).
// mk9-lauta (2026-11-11): and its picture's hash (avatars.h; "" none): the picture itself only if the other lacks it.
json AboutMe(json message) {
  const stats::Summary s = stats::Totals();
  message["cc"] = flags::Mine();
  message["won"] = s.won;
  message["lost"] = s.lost;
  message["streak"] = s.streak;
  message["av"] = avatars::MineHash();
  message["mods"] = Mods();
  return message;
}

// A text field of the other game's message ("" when missing or not a text).
std::string Text(const json& d, const char* key) {
  const auto it = d.find(key);
  return it != d.end() && it->is_string() ? it->get<std::string>() : std::string();
}

// mk9-lauta (2026-11-11): the pictures in a room (avatars.h). A hash this game lacks is asked for ("av?", with the
// asker's id when a spectator asks the host); the picture goes as "av" {h, d: base64}, each asker's at most every 15 s.
std::unordered_map<std::string, Clock::time_point> g_pictures_sent;

bool PictureDue(const std::string& who) {
  const auto now = Clock::now();
  auto& at = g_pictures_sent[who];
  if (at != Clock::time_point{} && now - at < std::chrono::seconds(15)) return false;
  at = now;
  return true;
}

// The picture of a hash this game has (its own, or the rival's for a spectator), as the relay's message; null if none.
json PictureMessage(const std::string& hash) {
  const std::string data = avatars::Read(hash);
  if (data.empty()) return nullptr;
  return {{"room", "av"}, {"h", hash}, {"d", avatars::Base64(data)}};
}

// A picture that came: kept if it is one of the expected hashes.
void TakePicture(const json& d, const std::vector<std::string>& expected) {
  const std::string hash = avatars::CleanHash(Text(d, "h"));
  if (hash.empty()) return;
  for (const std::string& e : expected) {
    if (e == hash) {
      const std::string data = Text(d, "d");
      if (data.size() <= (avatars::kMaxBytes + 2) / 3 * 4) avatars::Store(hash, avatars::Unbase64(data));
      return;
    }
  }
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

bool Four() { return g.s.capacity == 4; }

// mk9-lauta (2026-11-17): the room's people and machines.
int People() {
  int n = 0;
  for (const Member& m : g.s.members) n += m.players;
  return n;
}

Member* FindMember(const std::string& id) {
  for (Member& m : g.s.members) {
    if (!m.me && m.id == id) return &m;
  }
  return nullptr;
}

Member* MeMember() {
  for (Member& m : g.s.members) {
    if (m.me) return &m;
  }
  return nullptr;
}

// The members' pictures (the ones a picture that comes may be).
std::vector<std::string> MemberPictures() {
  std::vector<std::string> out;
  for (const Member& m : g.s.members) {
    if (!m.me && !m.avatar.empty()) out.push_back(m.avatar);
  }
  if (!g.s.other_avatar.empty()) out.push_back(g.s.other_avatar);
  return out;
}

// This game's own member, as it is now (its name, its record, its picture, its people, its ready).
Member MeNow() {
  const stats::Summary st = stats::Totals();
  Member me;
  me.id = g.s.host ? std::string() : g.guest_id;
  me.name = g.s.me;
  me.country = flags::Mine();
  me.avatar = avatars::MineHash();
  me.mods = Mods();
  me.won = st.won;
  me.lost = st.lost;
  me.streak = st.streak;
  me.players = g.s.my_players;
  me.ready = g.s.my_ready;
  me.me = true;
  return me;
}

// The "other" fields (a room of two's screens: the rival) from the first other member; whether any member's content
// differs from this game's.
void SyncOther() {
  if (Member* mine = MeMember()) *mine = MeNow();
  const Member* o = nullptr;
  bool differs = false;
  for (const Member& m : g.s.members) {
    if (m.me) continue;
    if (!o) o = &m;
    if (m.mods != Mods()) differs = true;
  }
  if (o) {
    g.s.other = o->name;
    g.s.other_country = o->country;
    g.s.other_won = o->won;
    g.s.other_lost = o->lost;
    g.s.other_streak = o->streak;
    g.s.other_avatar = o->avatar;
    g.s.other_players = o->players;
    g.s.other_ready = o->ready;
    g.s.other_mods = o->mods;
  }
  if (Four()) g.s.content_differs = differs;
  Touch();
}

// The other's content (a hello, a welcome, a keepalive; none from an older release): when it differs from this game's
// the room says so once and the fight does not start.
void TakeMods(const json& d, const std::string& who) {
  const std::string mods = Text(d, "mods");
  const bool differs = mods != Mods();
  if (!Four()) {
    if (mods == g.s.other_mods && differs == g.s.content_differs) return;
    g.s.other_mods = mods;
    g.s.content_differs = differs;
  }
  Touch();
  if (!differs) return;
  const std::string other = who.empty() ? T("El rival") : who;
  const std::string line = Mods() == "kratos"
                               ? Tf("{} todavía no tiene a Kratos (se le descarga solo): cuando le termine, tiene que "
                                    "reiniciar el juego para pelear online con vos.",
                                    other)
                               : Tf("{} ya tiene a Kratos y vos todavía no: se está descargando solo (abajo a la "
                                    "derecha del menú). Cuando termine, reiniciá el juego.",
                                    other);
  AddLine("", line, true);
  g.s.status = line;
  REXLOG_WARN("[room] {}'s game has content \"{}\", this one \"{}\": the fight cannot start", other, mods, Mods());
}

// A message to the other side: the host's to its guests (every one of them), a guest's to the host.
void SendOther(const json& data) {
  if (g.s.host) {
    if (Four()) {
      for (const Member& m : g.s.members) {
        if (!m.me && !m.id.empty()) lobby::SigToGuest(g.s.code, m.id, data);
      }
    } else if (!g.guest_id.empty()) {
      lobby::SigToGuest(g.s.code, g.guest_id, data);
    }
  } else {
    lobby::SigToHost(g.s.code, data);
  }
}

// mk9-lauta (2026-11-17): the host of a room of four tells every guest who is in it.
json MembersMessage() {
  json list = json::array();
  for (const Member& m : g.s.members) {
    list.push_back({{"id", m.id},
                    {"name", m.name},
                    {"cc", m.country},
                    {"won", m.won},
                    {"lost", m.lost},
                    {"streak", m.streak},
                    {"av", m.avatar},
                    {"mods", m.mods},
                    {"pl", m.players},
                    {"ready", m.ready}});
  }
  return {{"room", "members"}, {"cap", g.s.capacity}, {"koth", g.s.koth}, {"list", std::move(list)}};
}

// The host of a room of four: is it full now, and can its fight start (everyone in it ready, two machines or more).
bool FullNow() { return People() >= g.s.capacity || int(g.s.members.size()) >= kMaxMachines; }

void UpdateCanStart() {
  bool all = g.s.host && Four() && !g.s.in_session && g.s.phase == Phase::kTogether && g.s.members.size() >= 2 &&
             People() <= g.s.capacity && !g.s.content_differs;
  for (const Member& m : g.s.members) all = all && m.ready;
  if (all != g.s.can_start) {
    g.s.can_start = all;
    Touch();
  }
}

void BroadcastMembers() {
  if (!g.s.host || !Four()) return;
  SyncOther();
  const json message = MembersMessage();
  for (const Member& m : g.s.members) {
    if (!m.me && !m.id.empty()) lobby::SigToGuest(g.s.code, m.id, message);
  }
  UpdateCanStart();
}

// The host of a room of four says to the lobby whether it still waits for players (a full one is not in the list's
// waiting rooms; the lobby's quick match marks a room it gives out as playing: this side says again what it is).
void AnnounceFour() {
  if (!g.s.host || !Four() || g.s.in_session) return;
  lobby::Host(g.s.code, g.s.title, g.s.open, FullNow());
}

void Reset() {
  const bool in_session = g.s.in_session;
  std::vector<Line> keep;  // nothing: a new room starts with its own chat
  g.s = State{};
  g.s.in_session = in_session;
  g.guest_id.clear();
  g.start_pending = false;
  g.start_machines.clear();
  g.start_koth = false;
  g.heard_from.clear();
  g.want = 0;
  g.s.chat = std::move(keep);
  Touch();
}

void HostLocked(bool open, int capacity, const std::string& status, bool koth = false) {
  Reset();
  g.s.phase = Phase::kHosting;
  g.s.code = NewCode();
  g.s.open = open;
  g.s.host = true;
  g.s.capacity = capacity == 4 ? 4 : 2;
  g.s.koth = koth && Four();
  g.s.me = MyName();
  g.s.title = std::string(g.s.koth ? kKothTitle : Four() ? kFourTitle : "") + Tf("Sala de {}", g.s.me);
  g.s.status = status;
  g.s.members = {MeNow()};
  lobby::Host(g.s.code, g.s.title, open, false);
  REXLOG_INFO("[room] hosting {} ({}, {})", g.s.code, open ? "open" : "private",
              g.s.koth ? "King of the Hill, up to four people" : Four() ? "two against two, four people" : "one against one");
}

void JoinLocked(const std::string& code, int want) {
  Reset();
  g.s.phase = Phase::kJoining;
  g.s.code = code;
  g.s.host = false;
  g.s.me = MyName();
  g.s.status = Tf("Entrando a la sala {}...", code);
  g.guest_id = NewId();
  g.want = want;
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
    g.s.other_avatar.clear();
    g.s.my_ready = g.s.other_ready = false;
    g.guest_id.clear();
    g.s.members = {MeNow()};
    g.s.status = T("Esperando un rival");
    lobby::Host(g.s.code, g.s.title, g.s.open, false);
  } else {
    g.s.phase = Phase::kNone;
    g.s.status = Tf(why, other);
    if (!g.s.host) lobby::Join("", "");
  }
  AddLine("", Tf(why, other), true);
}

// mk9-lauta (2026-11-17): a guest of the host's room of four left (or went quiet): out of the list; the session it
// played in ends (for every machine).
void GuestGoneLocked(const std::string& id, const char* why) {
  Member* m = FindMember(id);
  if (!m) return;
  const std::string name = m->name.empty() ? T("Alguien") : m->name;
  g.s.members.erase(g.s.members.begin() + (m - g.s.members.data()));
  g.heard_from.erase(id);
  AddLine("", Tf(why, name), true);
  if (g.s.in_session) {
    netplay::PeerLeft();
    return;
  }
  // the others are not ready any more: who plays changed
  for (Member& o : g.s.members) {
    if (!o.me) o.ready = false;
  }
  if (g.s.members.size() <= 1) {
    g.s.phase = Phase::kHosting;
    g.s.my_ready = false;
    g.s.status = T("Esperando jugadores");
  }
  BroadcastMembers();
  AnnounceFour();
}

void LaunchSoon(int ms, std::vector<std::string> machines) {
  g.s.phase = Phase::kStarting;
  g.s.status = T("Arranca la pelea");
  g.start_pending = true;
  g.start_machines = std::move(machines);
  g.start_at = Clock::now() + std::chrono::milliseconds(ms);
  Touch();
}

void MaybeStartLocked() {
  if (Four()) {
    UpdateCanStart();  // its host starts it (Start)
    return;
  }
  if (!g.s.host || g.s.in_session || g.s.phase != Phase::kTogether || !g.s.my_ready || !g.s.other_ready) return;
  if (g.s.content_differs) return;  // one has Kratos and the other not (TakeMods said so)
  SendOther({{"room", "start"}});
  LaunchSoon(1200, {});  // the guest gets the start first
}

void OnMessageLocked(const json& d) {
  const std::string kind = d.value("room", "");
  if (kind.empty()) return;
  g.heard = Clock::now();
  const std::string from = Text(d, "id");
  // a room of four's host: what a guest says goes with its id (the relay does not say who sent it)
  Member* sender = g.s.host && Four() ? FindMember(from) : nullptr;
  if (sender) g.heard_from[from] = Clock::now();
  // the other player's country and record, with its hello and its keepalives (it may have found it after the hello)
  if (!Four()) {
    if (const std::string cc = d.value("cc", ""); cc.size() == 2 && g.s.phase == Phase::kTogether) {
      if (g.s.other_country != cc) {
        g.s.other_country = cc;
        Touch();
      }
    }
    if (d.contains("won") && g.s.phase == Phase::kTogether) {
      g.s.other_won = d.value("won", -1);
      g.s.other_lost = d.value("lost", -1);
      g.s.other_streak = d.value("streak", 0);
    }
    if (d.contains("won") && g.s.phase == Phase::kTogether) TakeMods(d, g.s.other);
    if (d.contains("av") && g.s.phase == Phase::kTogether) {
      const std::string hash = avatars::CleanHash(Text(d, "av"));
      if (hash != g.s.other_avatar) {
        g.s.other_avatar = hash;
        Touch();
      }
      if (!hash.empty() && avatars::ShowOthers() && !session::IsBlocked(g.s.other) && avatars::Wanted(hash)) {
        SendOther({{"room", "av?"}, {"h", hash}});
      }
    }
  } else if (sender && d.contains("won")) {
    // a guest of the host's room of four: its keepalive's news
    const std::string cc = d.value("cc", "");
    if (cc.size() == 2) sender->country = cc;
    sender->won = d.value("won", -1);
    sender->lost = d.value("lost", -1);
    sender->streak = d.value("streak", 0);
    const std::string mods = Text(d, "mods");
    if (mods != sender->mods) {
      sender->mods = mods;
      TakeMods(d, sender->name);
      BroadcastMembers();
    }
    const std::string hash = avatars::CleanHash(Text(d, "av"));
    if (hash != sender->avatar) {
      sender->avatar = hash;
      BroadcastMembers();
    }
    if (!hash.empty() && avatars::ShowOthers() && !session::IsBlocked(sender->name) && avatars::Wanted(hash)) {
      lobby::SigToGuest(g.s.code, from, {{"room", "av?"}, {"h", hash}});
    }
  }
  if (g.s.host) {
    // mk9-lauta (2026-11-10): someone watching the session (its own id): the two players, then the match (feed.h)
    if (kind == "watch") {
      const std::string id = d.value("id", "");
      if (id.empty() || !g.s.in_session) return;
      // a player this one silenced does not watch either (session.h, IsBlocked)
      if (session::IsBlocked(d.value("name", "")) ||
          !feed::Heard(id, d.value("have", uint64_t(0)), d.value("z", 0) == 1)) {
        lobby::SigToGuest(g.s.code, id, {{"room", "watch_full"}});
        return;
      }
      // mk9-lauta (2026-11-11): the match plays with this game's content: a spectator needs the same (mk9_kratos.h)
      if (Text(d, "mods") != Mods()) {
        lobby::SigToGuest(g.s.code, id, {{"room", "watch_mods"}, {"mods", Mods()}});
        return;
      }
      // the players' names and flags as this side has them now (the other's may come later). mk9-lauta (2026-11-17):
      // a room of four: the host and the others together ("p2"), each one in "list"
      std::string others = g.s.other, others_cc = g.s.other_country, others_av = g.s.other_avatar;
      json list = json::array();
      if (Four()) {
        others.clear();
        for (const Member& m : g.s.members) {
          list.push_back({{"name", m.name}, {"cc", m.country}, {"av", m.avatar}});
          if (m.me) continue;
          others += (others.empty() ? "" : " · ") + m.name;
        }
      }
      lobby::SigToGuest(g.s.code, id,
                        {{"room", "watching"}, {"p1", g.s.me}, {"p2", others}, {"cc1", flags::Mine()},
                         {"cc2", others_cc}, {"av1", avatars::MineHash()}, {"av2", others_av}, {"list", list}});
      if (g.s.watchers != feed::Watchers()) {
        g.s.watchers = feed::Watchers();
        Touch();
      }
      return;
    }
    // mk9-lauta (2026-11-11): a spectator asks for a player's picture (this one's, or the rival's kept here)
    if (kind == "av?" && !Text(d, "id").empty() && !sender) {
      const std::string id = Text(d, "id"), hash = avatars::CleanHash(Text(d, "h"));
      const std::vector<std::string> pictures = MemberPictures();
      const bool known = hash == avatars::MineHash() || std::find(pictures.begin(), pictures.end(), hash) != pictures.end();
      if (!hash.empty() && known && PictureDue(id + hash)) {
        if (json picture = PictureMessage(hash); !picture.is_null()) lobby::SigToGuest(g.s.code, id, picture);
      }
      return;
    }
    if (kind == "hi") {
      const std::string id = d.value("id", ""), name = d.value("name", "");
      if (id.empty()) return;
      // mk9-lauta (2026-11-10): a player this one silenced finds the room full
      if (session::IsBlocked(name) && !g.s.in_session) {
        lobby::SigToGuest(g.s.code, id, {{"room", "full"}});
        return;
      }
      if (Four()) {
        // mk9-lauta (2026-11-17): a room of four: a guest already in it says hello again (or comes back in its
        // session); a new one, while there is room for one more machine and a person more, from a release that
        // plays it, looking for it (a quick match one against one is turned away)
        const int want = d.value("want", 0);
        Member* known = FindMember(id);
        bool fits = known != nullptr;
        if (!fits && g.s.in_session) {
          fits = std::find(g.session_machines.begin(), g.session_machines.end(), id) != g.session_machines.end();
        } else if (!fits) {
          fits = (g.s.phase == Phase::kHosting || g.s.phase == Phase::kTogether) && !FullNow() &&
                 d.value("proto", 0) >= (g.s.koth ? kKothProto : kFourProto) &&
                 (want == 0 || (want == 4 && !g.s.koth));
        }
        if (!fits) {
          lobby::SigToGuest(g.s.code, id, {{"room", "full"}, {"cap", 4}, {"koth", g.s.koth}});
          AnnounceFour();  // the lobby's quick match marked it playing when it gave it out
          return;
        }
        if (!known) {
          Member m;
          m.id = id;
          g.s.members.push_back(m);
          known = &g.s.members.back();
          AddLine("", Tf("{} entró a la sala", name), true);
          if (!g.s.in_session) notify::Attention(true);
        }
        known->name = name;
        known->country = d.value("cc", "");
        known->won = d.value("won", -1);
        known->lost = d.value("lost", -1);
        known->streak = d.value("streak", 0);
        known->avatar = avatars::CleanHash(Text(d, "av"));
        known->mods = Text(d, "mods");
        g.heard_from[id] = Clock::now();
        if (known->mods != Mods()) TakeMods(d, name);
        g.s.phase = Phase::kTogether;
        g.s.status = Tf("{} de {} personas en la sala", People(), g.s.capacity);
        lobby::SigToGuest(g.s.code, id,
                          AboutMe({{"room", "welcome"}, {"name", g.s.me}, {"cap", 4}, {"koth", g.s.koth}}));
        if (!known->avatar.empty() && avatars::ShowOthers() && avatars::Wanted(known->avatar)) {
          lobby::SigToGuest(g.s.code, id, {{"room", "av?"}, {"h", known->avatar}});
        }
        BroadcastMembers();
        AnnounceFour();
        return;
      }
      if (g.s.phase == Phase::kHosting || (g.s.phase == Phase::kTogether && id == g.guest_id)) {
        // a room of two: a quick match two against two is turned away
        if (d.value("want", 0) == 4) {
          lobby::SigToGuest(g.s.code, id, {{"room", "full"}, {"cap", 2}});
          lobby::Host(g.s.code, g.s.title, g.s.open, false);
          return;
        }
        const bool fresh = g.s.phase == Phase::kHosting;
        g.guest_id = id;
        g.s.other = name;
        g.s.other_country = d.value("cc", "");
        g.s.other_won = d.value("won", -1);
        g.s.other_lost = d.value("lost", -1);
        g.s.other_streak = d.value("streak", 0);
        g.s.other_avatar = avatars::CleanHash(Text(d, "av"));
        g.s.phase = Phase::kTogether;
        TakeMods(d, name);
        Member guest;
        guest.id = id;
        guest.name = name;
        guest.country = g.s.other_country;
        guest.won = g.s.other_won;
        guest.lost = g.s.other_lost;
        guest.streak = g.s.other_streak;
        guest.avatar = g.s.other_avatar;
        guest.mods = Text(d, "mods");
        guest.players = g.s.other_players;
        guest.ready = g.s.other_ready;
        g.s.members = {MeNow(), guest};
        lobby::SigToGuest(g.s.code, id, AboutMe({{"room", "welcome"}, {"name", g.s.me}}));
        if (!g.s.other_avatar.empty() && avatars::ShowOthers() && avatars::Wanted(g.s.other_avatar)) {
          lobby::SigToGuest(g.s.code, id, {{"room", "av?"}, {"h", g.s.other_avatar}});
        }
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
    if (Four()) {
      // mk9-lauta (2026-11-17): a guest of a room of four (only those in it are heard)
      if (!sender) return;
      if (kind == "chat") {
        const std::string text = d.value("text", "").substr(0, 200);
        AddLine(sender->name, text, false);
        notify::Attention(false);
        const json relayed = {{"room", "chat"}, {"text", text}, {"name", sender->name}};
        for (const Member& m : g.s.members) {
          if (!m.me && m.id != from && !m.id.empty()) lobby::SigToGuest(g.s.code, m.id, relayed);
        }
      } else if (kind == "ready") {
        sender->ready = d.value("on", false);
        const int players = d.value("pl", 1) == 2 ? 2 : 1;
        // two at that PC only while the room has the room for it
        sender->players = !g.s.koth && players == 2 && People() - sender->players + 2 <= g.s.capacity ? 2 : 1;
        if (sender->ready) notify::Attention(true);
        g.s.status = Tf("{} de {} personas en la sala", People(), g.s.capacity);
        BroadcastMembers();
        AnnounceFour();
      } else if (kind == "bye") {
        GuestGoneLocked(from, "{} se fue de la sala");
      } else if (kind == "av?") {
        // the guest lacks a picture: this one's, or another member's kept here
        const std::string hash = avatars::CleanHash(Text(d, "h"));
        const std::vector<std::string> pictures = MemberPictures();
        const bool known =
            hash == avatars::MineHash() || std::find(pictures.begin(), pictures.end(), hash) != pictures.end();
        if (!hash.empty() && known && PictureDue(from + hash)) {
          if (json picture = PictureMessage(hash); !picture.is_null()) lobby::SigToGuest(g.s.code, from, picture);
        }
      } else if (kind == "av") {
        TakePicture(d, MemberPictures());
      }
      return;
    }
  } else {
    if (g.s.phase == Phase::kWatching) {
      if (kind == "watching") {
        const std::string p1 = d.value("p1", ""), p2 = d.value("p2", ""), cc1 = d.value("cc1", ""),
                          cc2 = d.value("cc2", "");
        const std::string av1 = avatars::CleanHash(Text(d, "av1")), av2 = avatars::CleanHash(Text(d, "av2"));
        if (!g.s.watching || p1 != g.s.p1 || p2 != g.s.p2 || cc1 != g.s.p1_country || cc2 != g.s.p2_country ||
            av1 != g.s.p1_avatar || av2 != g.s.p2_avatar) {
          g.s.watching = true;
          g.s.p1 = p1;
          g.s.p2 = p2;
          g.s.p1_country = cc1;
          g.s.p2_country = cc2;
          g.s.p1_avatar = av1;
          g.s.p2_avatar = av2;
          Touch();
        }
        // the players' pictures this game lacks, from the host (mk9-lauta, 2026-11-11)
        for (const std::string& hash : {av1, av2}) {
          if (!hash.empty() && avatars::ShowOthers() && avatars::Wanted(hash)) {
            lobby::SigToHost(g.s.code, {{"room", "av?"}, {"h", hash}, {"id", g.guest_id}});
          }
        }
      } else if (kind == "feed") {
        // (mk9-lauta, 2026-11-17: a deflated piece, "z" with its raw size "n")
        if (d.contains("z")) {
          feed::ReceivedZip(d.value("at", uint64_t(0)), d.value("z", ""), d.value("n", uint64_t(0)));
        } else {
          feed::Received(d.value("at", uint64_t(0)), d.value("data", ""));
        }
      } else if (kind == "av") {
        TakePicture(d, {g.s.p1_avatar, g.s.p2_avatar});
      } else if (kind == "feed_end") {
        feed::ReceivedEnd();
      } else if (kind == "watch_full") {
        g.s.watch_full = true;
        feed::ReceivedEnd();
        Touch();
      } else if (kind == "watch_mods") {  // mk9-lauta (2026-11-11): the match has other content than this game
        g.s.watch_full = true;            // no more asking
        g.s.watch_mods = Text(d, "mods").empty() ? "none" : Text(d, "mods");
        feed::ReceivedEnd();
        Touch();
      }
      return;
    }
    if (kind == "welcome" && g.s.phase == Phase::kJoining) {
      g.s.capacity = d.value("cap", 2) == 4 ? 4 : 2;
      g.s.koth = Four() && d.value("koth", false);
      if (g.s.koth) g.s.my_players = 1;
      g.s.other = d.value("name", "");
      g.s.other_country = d.value("cc", "");
      g.s.other_won = d.value("won", -1);
      g.s.other_lost = d.value("lost", -1);
      g.s.other_streak = d.value("streak", 0);
      g.s.other_avatar = avatars::CleanHash(Text(d, "av"));
      g.s.phase = Phase::kTogether;
      g.s.status = Tf("En la sala de {}", g.s.other);
      if (!Four()) {
        TakeMods(d, g.s.other);
        Member host;
        host.name = g.s.other;
        host.country = g.s.other_country;
        host.won = g.s.other_won;
        host.lost = g.s.other_lost;
        host.streak = g.s.other_streak;
        host.avatar = g.s.other_avatar;
        host.mods = Text(d, "mods");
        g.s.members = {host, MeNow()};
      }
      if (!g.s.other_avatar.empty() && avatars::ShowOthers() && avatars::Wanted(g.s.other_avatar)) {
        lobby::SigToHost(g.s.code, {{"room", "av?"}, {"h", g.s.other_avatar}, {"id", g.guest_id}});
      }
      AddLine("", Tf("Entraste a la sala de {}", g.s.other), true);
      if (!g.s.in_session) notify::Attention(true);
      return;
    }
    if (kind == "members" && (g.s.phase == Phase::kTogether || g.s.phase == Phase::kStarting)) {
      // mk9-lauta (2026-11-17): a room of four's host says who is in it
      g.s.capacity = 4;
      g.s.koth = d.value("koth", false);
      std::vector<Member> members;
      const json& list = d.contains("list") && d["list"].is_array() ? d["list"] : json::array();
      for (const json& e : list) {
        if (!e.is_object() || members.size() >= size_t(kMaxMachines)) continue;
        Member m;
        m.id = Text(e, "id");
        m.name = Text(e, "name");
        m.country = Text(e, "cc");
        m.won = e.value("won", -1);
        m.lost = e.value("lost", -1);
        m.streak = e.value("streak", 0);
        m.avatar = avatars::CleanHash(Text(e, "av"));
        m.mods = Text(e, "mods");
        m.players = e.value("pl", 1) == 2 ? 2 : 1;
        m.ready = e.value("ready", false);
        m.me = !m.id.empty() && m.id == g.guest_id;
        if (m.me) {
          // this one's own word on its ready and its people is the one it sent last
          m.ready = g.s.my_ready;
          m.players = g.s.my_players;
        }
        for (const std::string& hash : {m.avatar}) {
          if (!m.me && !hash.empty() && avatars::ShowOthers() && avatars::Wanted(hash) && PictureDue("ask" + hash)) {
            lobby::SigToHost(g.s.code, {{"room", "av?"}, {"h", hash}, {"id", g.guest_id}});
          }
        }
        members.push_back(std::move(m));
      }
      g.s.members = std::move(members);
      SyncOther();
      g.s.status = Tf("{} de {} personas en la sala", People(), g.s.capacity);
      return;
    }
    if (kind == "full" && g.s.phase == Phase::kJoining) {
      const int cap = d.value("cap", 2);
      const bool koth = d.value("koth", false);
      Reset();
      g.s.status = koth       ? T("Esa sala de Rey de la colina ya está completa (o es de otra versión)")
                   : cap == 4 ? T("Esa sala de 2 contra 2 ya está completa (o es de otra versión)")
                              : T("Esa sala ya tiene dos jugadores");
      lobby::Join("", "");
      return;
    }
    if (kind == "start" && g.s.phase == Phase::kTogether && !g.s.in_session) {
      std::vector<std::string> machines;
      if (d.contains("machines") && d["machines"].is_array()) {
        for (const json& id : d["machines"]) {
          if (id.is_string() && machines.size() < size_t(kMaxMachines)) machines.push_back(id.get<std::string>());
        }
      }
      const bool koth = d.value("koth", false) && machines.size() > 2;
      LaunchSoon(300, std::move(machines));
      g.start_koth = koth;
      return;
    }
  }
  if (g.s.phase != Phase::kTogether) return;
  if (kind == "av?") {  // mk9-lauta (2026-11-11): the other lacks this one's picture
    const std::string hash = avatars::CleanHash(Text(d, "h"));
    if (!hash.empty() && hash == avatars::MineHash() && PictureDue("other")) {
      if (json picture = PictureMessage(hash); !picture.is_null()) SendOther(picture);
    }
    return;
  }
  if (kind == "av") {
    TakePicture(d, MemberPictures());
    return;
  }
  if (kind == "chat") {
    // (a room of four: the host passes it on with its sender's name)
    AddLine(Text(d, "name").empty() ? g.s.other : Text(d, "name"), d.value("text", ""), false);
    notify::Attention(false);
  } else if (kind == "ready") {
    g.s.other_ready = d.value("on", false);
    g.s.other_players = d.value("pl", 1) == 2 ? 2 : 1;
    for (Member& m : g.s.members) {
      if (!m.me) {
        m.ready = g.s.other_ready;
        m.players = g.s.other_players;
        break;
      }
    }
    if (g.s.other_ready) notify::Attention(true);
    Touch();
    MaybeStartLocked();
  } else if (kind == "bye") {
    if (g.s.in_session) netplay::PeerLeft();  // its goodbye over the session's own path may not have come
    OtherGoneLocked("{} se fue de la sala");
  }
}

}  // namespace

void CreateRoom(bool open, int capacity, bool koth) {
  std::lock_guard<std::mutex> lock(g.mutex);
  if (koth) {
    HostLocked(open, 4,
               T(open ? "Sala pública de Rey de la colina: esperando jugadores (de 3 a 4)"
                      : "Sala privada de Rey de la colina: pasales el código (de 3 a 4 personas)"),
               true);
  } else if (capacity == 4) {
    HostLocked(open, 4,
               T(open ? "Sala pública de 2 contra 2: esperando jugadores"
                      : "Sala privada de 2 contra 2: pasales el código a los otros tres"));
  } else {
    HostLocked(open, 2, T(open ? "Sala pública: esperando un rival" : "Sala privada: pasale el código a tu rival"));
  }
}

void JoinRoom(const std::string& code, int want) {
  const std::string c = Normalize(code);
  std::lock_guard<std::mutex> lock(g.mutex);
  if (c.size() < 4) {
    g.s.status = T("El código tiene que tener de 4 a 8 letras o números");
    Touch();
    return;
  }
  JoinLocked(c, want);
}

void QuickMatch() {
  std::lock_guard<std::mutex> lock(g.mutex);
  Reset();
  g.s.phase = Phase::kSearching;
  g.s.status = T("Buscando rival...");
  g.want = 2;
  lobby::Quick();
}

void Start() {
  std::vector<std::string> machines;
  {
    std::lock_guard<std::mutex> lock(g.mutex);
    UpdateCanStart();
    if (!g.s.can_start) return;
    for (const Member& m : g.s.members) machines.push_back(m.me ? std::string() : m.id);
    // (two machines: a session of two, as a room of two's; King of the Hill with two is one against one)
    const bool koth = g.s.koth && machines.size() > 2;
    const json start = machines.size() > 2 ? json{{"room", "start"}, {"machines", machines}, {"koth", koth}}
                                           : json{{"room", "start"}};
    SendOther(start);
    REXLOG_INFO("[room] {} starts its fight with {} machines, {} people{}", g.s.code, machines.size(), People(),
                koth ? " (King of the Hill)" : "");
    g.s.can_start = false;
    LaunchSoon(1200, machines.size() > 2 ? machines : std::vector<std::string>{});  // the guests get the start first
    g.start_koth = koth;
  }
}

void Leave() {
  std::lock_guard<std::mutex> lock(g.mutex);
  if (g.s.phase == Phase::kNone) return;
  if (g.s.phase == Phase::kWatching) {
    // a spectator says nothing to the host: its "bye" would read as the other player's
    lobby::Join("", "");
    Reset();
    return;
  }
  SendOther({{"room", "bye"}, {"id", g.s.host ? std::string() : g.guest_id}});
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
  if (Four() && g.s.host) {
    BroadcastMembers();
  } else {
    SendOther({{"room", "ready"}, {"on", ready}, {"pl", g.s.my_players}, {"id", g.guest_id}});
    SyncOther();
  }
  Touch();
  MaybeStartLocked();
}

void SetPlayers(int players) {
  std::lock_guard<std::mutex> lock(g.mutex);
  if (g.s.phase != Phase::kTogether || g.s.in_session || g.s.my_ready) return;
  g.s.my_players = players == 2 && !g.s.koth ? 2 : 1;  // (King of the Hill: one at each PC)
  if (Four() && g.s.host) {
    // the host's own: only while the room has the room for it
    if (People() - (MeMember() ? MeMember()->players : 1) + g.s.my_players > g.s.capacity) g.s.my_players = 1;
    BroadcastMembers();
  } else {
    SendOther({{"room", "ready"}, {"on", false}, {"pl", g.s.my_players}, {"id", g.guest_id}});
    SyncOther();
  }
  Touch();
}

void Say(const std::string& text) {
  if (text.empty()) return;
  std::lock_guard<std::mutex> lock(g.mutex);
  if (g.s.phase != Phase::kTogether) return;
  SendOther({{"room", "chat"}, {"text", text.substr(0, 200)}, {"name", g.s.me}, {"id", g.guest_id}});
  AddLine(g.s.me, text.substr(0, 200), false);
}

State Snapshot() {
  std::lock_guard<std::mutex> lock(g.mutex);
  return g.s;
}

void Watch(const std::string& code) {
  std::string name;
  {
    std::lock_guard<std::mutex> lock(g.mutex);
    name = MyName();
  }
  lobby::Stop();
  session::Watch(Normalize(code), name);
}

void EnterWatch(const std::string& code, const std::string& me, const std::string& id) {
  std::lock_guard<std::mutex> lock(g.mutex);
  g.s = State{};
  g.s.in_session = true;
  g.s.code = code;
  g.s.me = me;
  g.s.phase = Phase::kWatching;
  g.s.status = T("Pidiendo la partida al host...");
  g.guest_id = id;
  g.joined_at = Clock::now();
  g.heard = Clock::now();
  Touch();
}

void EnterSession(const std::string& code, bool host, const std::string& me, const std::string& id,
                  const std::string& title, bool open, const std::vector<std::string>& machines) {
  std::lock_guard<std::mutex> lock(g.mutex);
  g.s = State{};
  g.s.in_session = true;
  g.s.code = code;
  g.s.host = host;
  g.s.me = me;
  g.s.title = title;
  g.s.open = open;
  g.s.phase = host ? Phase::kHosting : Phase::kJoining;
  g.s.status = T("Conectando con el rival");
  g.guest_id = host ? std::string() : id;  // the session (netplay.cpp) has hosted or joined the room already
  // mk9-lauta (2026-11-17): a session of more than two machines: a room of four, its guests welcomed back by id
  g.session_machines = machines;
  if (machines.size() > 2) {
    g.s.capacity = 4;
    g.s.koth = rex::cvar::GetFlagByName("mk9_net_koth") == "true";  // (netplay.cpp's, from the room's start)
    g.s.members = {MeNow()};
  }
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
    if (test == "quick4") {
      // mk9-lauta (2026-11-17): a room of four waiting in the list (as the screens' quick match two against two), or
      // a new one after 6 s of none
      static const auto first = Clock::now();
      for (const lobby::Room& r : lobby.rooms) {
        if (r.playing || r.title.rfind(kFourTitle, 0) != 0) continue;
        started = true;
        REXLOG_INFO("[room] mk9_online_test=quick4: room {}", r.code);
        JoinRoom(r.code, 4);
        return;
      }
      if (Clock::now() - first < std::chrono::seconds(6)) return;
      started = true;
      CreateRoom(true, 4);
      return;
    }
    if (test == "joink") {
      // mk9-lauta (2026-11-17): the first room of King of the Hill waiting in the list
      for (const lobby::Room& r : lobby.rooms) {
        if (r.playing || r.title.rfind(kKothTitle, 0) != 0) continue;
        started = true;
        REXLOG_INFO("[room] mk9_online_test=joink: room {}", r.code);
        JoinRoom(r.code, 0);
        return;
      }
      return;
    }
    started = true;
    REXLOG_INFO("[room] mk9_online_test={}", test);
    if (test == "host") CreateRoom(true);
    if (test == "host4") CreateRoom(true, 4);
    if (test == "hostk") CreateRoom(true, 4, true);
    if (test == "quick") QuickMatch();
    return;
  }
  if (test == "watch" && s.phase == Phase::kNone) {
    for (const lobby::Room& r : lobby::Snapshot().rooms) {
      if (!r.playing) continue;
      REXLOG_INFO("[room] mk9_online_test=watch: room {} plays", r.code);
      Watch(r.code);
    }
    return;
  }
  if (s.phase == Phase::kTogether && !s.my_ready) {
    if (const int players = REXCVAR_GET(mk9_online_test_players); players != s.my_players) SetPlayers(players);
    SetReady(true);
  }
  // the host of a room of four starts once the test's machines are in, all ready
  if (s.host && s.capacity == 4 && s.can_start && int(s.members.size()) >= REXCVAR_GET(mk9_online_test_start)) Start();
}

void Pump() {
  TestStep();
  lobby::Event event;
  while (lobby::PollEvent(&event)) {
    std::lock_guard<std::mutex> lock(g.mutex);
    if (event.kind == lobby::Event::Kind::kMatch) {
      if (g.s.phase != Phase::kSearching) continue;
      if (!event.room.empty()) {
        JoinLocked(event.room, 2);
      } else {
        HostLocked(true, 2, T("Nadie esperando: tu sala quedó abierta para el próximo que busque"));
      }
      continue;
    }
    if (event.room == g.s.code && event.data.is_object()) OnMessageLocked(event.data);
  }
  bool launch = false;
  std::string code, me, title, my_id;
  bool host = false, listed = false, koth = false;
  int players = 1;
  std::vector<std::string> machines;
  {
    std::lock_guard<std::mutex> lock(g.mutex);
    const auto now = Clock::now();
    if (g.s.phase == Phase::kJoining) {
      if (now - g.hi_at > kHiEvery) {
        g.hi_at = now;
        lobby::SigToHost(g.s.code, AboutMe({{"room", "hi"},
                                            {"name", g.s.me},
                                            {"id", g.guest_id},
                                            {"want", g.want},
                                            {"proto", kProto}}));
      }
      if (!g.s.in_session && now - g.joined_at > kJoinFor) {
        const std::string code_was = g.s.code;
        Reset();
        g.s.status = Tf("La sala {} no responde", code_was);
        lobby::Join("", "");
      }
    } else if (g.s.phase == Phase::kWatching) {
      // every 2 s: still here, with this much of the match (the host answers with the players' names)
      if (!g.s.watch_full && now - g.hi_at > std::chrono::seconds(2)) {
        g.hi_at = now;
        lobby::SigToHost(g.s.code, {{"room", "watch"},
                                    {"id", g.guest_id},
                                    {"name", g.s.me},
                                    {"have", feed::Size()},
                                    {"mods", Mods()},
                                    {"z", 1}});  // (mk9-lauta, 2026-11-17: it takes the recording deflated)
      }
    } else if (g.s.phase == Phase::kTogether) {
      if (now - g.said > kPingEvery) {
        g.said = now;
        SendOther(AboutMe({{"room", "ping"}, {"id", g.guest_id}}));
        if (g.s.host && Four()) BroadcastMembers();  // (the members again: a guest that missed one)
      }
      // in a session the fight's own path says whether the other players are there (the lobby may be what is down)
      if (!g.s.in_session) {
        if (g.s.host && Four()) {
          std::vector<std::string> quiet;
          for (const Member& m : g.s.members) {
            if (m.me) continue;
            const auto it = g.heard_from.find(m.id);
            if (it != g.heard_from.end() && now - it->second > kGone) quiet.push_back(m.id);
          }
          for (const std::string& id : quiet) GuestGoneLocked(id, "{} se desconectó");
        } else if (now - g.heard > kGone) {
          OtherGoneLocked("{} se desconectó");
        }
      }
    }
    if (g.start_pending && now >= g.start_at) {
      g.start_pending = false;
      launch = true;
      code = g.s.code;
      me = g.s.me;
      host = g.s.host;
      title = g.s.title;
      listed = g.s.open;
      players = g.s.my_players;
      machines = g.start_machines;
      my_id = g.guest_id;
      koth = g.start_koth;
    }
  }
  if (launch) {
    lobby::Stop();
    session::Launch(code, host, me, title, listed, players, machines, my_id, koth);
  }
  // mk9-lauta (2026-11-10): the session's host passes the match to whoever watches it (feed.h)
  {
    std::string room_code;
    bool feeding = false;
    {
      std::lock_guard<std::mutex> lock(g.mutex);
      feeding = g.s.host && g.s.in_session;
      room_code = g.s.code;
    }
    if (feeding) {
      const netplay::SessionInfo info = netplay::Info();
      const size_t budget = !info.rollback ? feed::kBudgetMenus
                            : info.rtt_ms > 150 || info.loss_pct > 2 ? feed::kBudgetFightSlow
                                                                     : feed::kBudgetFight;
      feed::Pump(room_code, info.ended, budget);
      std::lock_guard<std::mutex> lock(g.mutex);
      if (g.s.watchers != feed::Watchers()) {
        g.s.watchers = feed::Watchers();  // one that stopped asking is gone
        Touch();
      }
    }
  }
}

}  // namespace mk9::room
