// MK9 online: the online screens in the game (online_ui.h).

#include "online/online_ui.h"

#include "mk9_overlay_input.h"
#include "mk9_updater.h"
#include "options_ui.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include "imgui.h"

#include <fmt/format.h>
#include <rex/cvar.h>

#include "online/discord.h"
#include "online/fight_state.h"
#include "online/flags.h"
#include "online/i18n.h"
#include "online/lobby.h"
#include "online/netplay.h"
#include "online/room.h"
#include "online/session.h"
#include "online/stats.h"
#include "renut_engine/gpu/overlay/overlay_ui.h"

REXCVAR_DEFINE_BOOL(mk9_online_show, false, "MK9 Online",
                    "The online screens open at start (the game started again back from a room's session)");
REXCVAR_DEFINE_STRING(mk9_online_test_tab, "", "MK9 Online",
                      "Tests: the online window opens on this tab (Sala, Salas, Chat global, Perfil)");
REXCVAR_DEFINE_INT32(mk9_online_test_leave_s, 0, "MK9 Online",
                     "Tests: in a room's session, this player leaves this many seconds after the game's first frame "
                     "(0 = never)");

#if defined(MK9_LAUTA_VERSION)
#define MK9_LAUTA_VERSION_TEXT MK9_LAUTA_VERSION
#else
#define MK9_LAUTA_VERSION_TEXT "0.0.0"
#endif

namespace mk9::online_ui {
namespace {

using i18n::T;

// A runtime format (fmt) of a translated text.
template <typename... Args>
std::string Tf(const char* es, Args&&... args) {
  return fmt::format(fmt::runtime(T(es)), std::forward<Args>(args)...);
}

// A label in the player's language with a fixed ImGui id (the Spanish key).
std::string Label(const char* es) { return std::string(T(es)) + "###" + es; }

using Clock = std::chrono::steady_clock;

const ImVec4 kGrey(0.65f, 0.65f, 0.70f, 1.0f);
const ImVec4 kGold(1.00f, 0.80f, 0.30f, 1.0f);
const ImVec4 kGreen(0.45f, 0.90f, 0.50f, 1.0f);
const ImVec4 kRed(1.00f, 0.45f, 0.40f, 1.0f);

std::string Trim(const char* text) {
  std::string s = text;
  while (!s.empty() && (s.back() == ' ' || s.back() == '\t')) s.pop_back();
  size_t i = 0;
  while (i < s.size() && (s[i] == ' ' || s[i] == '\t')) ++i;
  return s.substr(i);
}

std::string Clock24(int64_t ms) {
  if (ms <= 0) return "--:--";
  const std::time_t t = std::time_t(ms / 1000);
  std::tm local{};
  localtime_s(&local, &t);
  return fmt::format("{:02}:{:02}", local.tm_hour, local.tm_min);
}

std::string ErrorText(const std::string& code) {
  if (code == "slow") return T("Más despacio: un mensaje por segundo.");
  if (code == "links") return T("En el chat no se pueden pasar links.");
  if (code == "muted") return T("Estás silenciado por un rato.");
  if (code == "banned") return T("No podés escribir en el chat.");
  if (code == "reserved") return T("Ese nombre está reservado.");
  if (code == "empty") return T("El nombre no puede quedar vacío.");
  return code;
}

// mk9-lauta (2026-11-10): a country's flag inline, before what comes next on the line (nothing for none).
void FlagInline(const std::string& country) {
  if (flags::Draw(country, ImGui::GetTextLineHeight() * 0.85f)) ImGui::SameLine(0, 5);
}

// mk9-lauta (2026-11-10): the connection as four bars, from its round trip, how much that moves and the pings lost:
// 4 good, 1 bad.
int Quality(const netplay::SessionInfo& info) {
  int q = 4;
  if (info.rtt_ms > 120) --q;
  if (info.rtt_ms > 200) --q;
  if (info.rtt_ms > 300) --q;
  if (info.jitter_ms > 20) --q;
  if (info.jitter_ms > 50) --q;
  if (info.loss_pct > 2) --q;
  if (info.loss_pct > 8) --q;
  return std::clamp(q, 1, 4);
}

void SignalBars(const netplay::SessionInfo& info) {
  const int q = Quality(info);
  const float h = ImGui::GetTextLineHeight();
  const float bar = h * 0.22f, gap = h * 0.12f;
  const ImVec2 at = ImGui::GetCursorScreenPos();
  ImDrawList* list = ImGui::GetWindowDrawList();
  const ImU32 on = q >= 4   ? IM_COL32(110, 220, 120, 255)
                   : q == 3 ? IM_COL32(190, 220, 90, 255)
                   : q == 2 ? IM_COL32(240, 170, 60, 255)
                            : IM_COL32(240, 90, 80, 255);
  const ImU32 off = IM_COL32(120, 120, 130, 110);
  for (int i = 0; i < 4; ++i) {
    const float bh = h * (0.35f + 0.65f * float(i + 1) / 4.0f);
    const ImVec2 a(at.x + float(i) * (bar + gap), at.y + h - bh), b(a.x + bar, at.y + h);
    list->AddRectFilled(a, b, i < q ? on : off, 1.0f);
  }
  ImGui::Dummy(ImVec2(4 * bar + 3 * gap, h));
}

// The connection in words (the room's tab).
std::string ConnectionText(const netplay::SessionInfo& info) {
  return Tf("Ping {:.0f} ms - variación {:.0f} ms - {:.0f}% perdidos - {}", info.rtt_ms, info.jitter_ms,
            info.loss_pct, T(info.relayed ? "por relay (TURN)" : "conexión directa"));
}

class OnlineWindow : public renut::gpu::overlay::Dialog {
 public:
  OnlineWindow() {
    name_ = session::LoadName();
    std::snprintf(name_buf_, sizeof(name_buf_), "%s", name_.c_str());
    flags::Detect();  // this player's country, for the lobby and the rival (a thread: Cloudflare's trace)
    if (!name_.empty() && lobby::Configured()) lobby::Start(name_);
    // with no name yet the window opens by itself (it has to be chosen to play online), and so it does back from a
    // room's session: the player is in the lobby again
    visible_ = lobby::Configured() && (name_.empty() || REXCVAR_GET(mk9_online_show));
  }

  void Draw() override {
    ImGui::GetStyle().FontScaleMain = options_ui::UiScale();
    if (const std::string country = flags::Mine(); !country.empty()) lobby::SetCountry(country);
    room::Pump();
    if (ImGui::IsKeyPressed(ImGuiKey_F1, false) || overlay_input::ToggleOnlinePressed()) {
      visible_ = !visible_;
      focus_ = visible_;  // over the options if they are open too
    }
    const room::State room = room::Snapshot();
    if (room.in_session) {
      const netplay::SessionInfo info = netplay::Info();
      if (const int leave_s = REXCVAR_GET(mk9_online_test_leave_s);
          leave_s > 0 && !leaving_ && Clock::now() - started_ > std::chrono::seconds(leave_s)) {
        LeaveSession();
      }
      if (leaving_) {
        // the goodbye has gone out (the session ended) or the game thread is busy elsewhere: back to the lobby
        if (info.ended || Clock::now() > leave_deadline_) session::Back();
      } else if (info.ended) {
        DrawSessionEnd(room, info);
        return;
      }
    }
    DrawSessionBadge(room);
    DrawNamePlates(room);
    UpdatePresence(room);
    TakeResults(room);
    overlay_input::SetOpen(overlay_input::kOnline, visible_);
    if (!visible_) {
      DrawToast(room);
      return;
    }
    last_seen_lines_ = room.chat.size();
    const ImGuiIO& io = ImGui::GetIO();
    if (focus_) {
      ImGui::SetNextWindowFocus();
      focus_ = false;
    }
    ImGui::SetNextWindowSize(ImVec2(620, 560), ImGuiCond_FirstUseEver);
    // inside the window however small it is (1366x768, a small window)
    ImGui::SetNextWindowSizeConstraints(ImVec2(320, 200),
                                        ImVec2(std::max(320.0f, io.DisplaySize.x - 16), std::max(200.0f, io.DisplaySize.y - 16)));
    ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f), ImGuiCond_FirstUseEver,
                            ImVec2(0.5f, 0.5f));
    if (!ImGui::Begin((std::string(T("Online  (F1 para mostrar u ocultar)")) + "###online").c_str(), &visible_,
                      ImGuiWindowFlags_NoCollapse)) {
      ImGui::End();
      return;
    }
    // the controller: B closes, LB / RB change tabs
    if (overlay_input::ClosePressed()) visible_ = false;
    tab_step_ = overlay_input::TabStep();
    if (!lobby::Configured()) {
      ImGui::TextWrapped("%s", T("El online está apagado (mk9_lobby_url vacío)."));
      ImGui::End();
      return;
    }
    const lobby::State lobby = lobby::Snapshot();
    if (name_.empty() || editing_name_) {
      DrawName(lobby);
      ImGui::End();
      return;
    }
    DrawHeader(lobby);
    if (ImGui::BeginTabBar("online_tabs")) {
      const bool in_room = room.phase != room::Phase::kNone && room.phase != room::Phase::kSearching;
      // the tabs there are, for LB / RB
      std::vector<const char*> tabs;
      if (in_room) tabs.push_back("Sala");
      if (!room.in_session) tabs.push_back("Salas");
      tabs.push_back("Chat global");
      tabs.push_back("Perfil");
      if (!room.in_session) tabs.push_back("Replays");
      const char* wanted = nullptr;
      if (static bool once = false; !once && !REXCVAR_GET(mk9_online_test_tab).empty()) {
        once = true;
        for (const char* t : tabs) {
          if (REXCVAR_GET(mk9_online_test_tab) == t) wanted = t;
        }
      }
      if (tab_step_) {
        int current = 0;
        for (int i = 0; i < int(tabs.size()); ++i) {
          if (tab_ == tabs[i]) current = i;
        }
        wanted = tabs[(current + tab_step_ + int(tabs.size())) % int(tabs.size())];
      }
      auto tab_flags = [&](const char* name, ImGuiTabItemFlags flags) -> ImGuiTabItemFlags {
        if (!wanted) return flags;
        return std::string(wanted) == name ? ImGuiTabItemFlags_SetSelected : 0;
      };
      if (in_room) {
        const ImGuiTabItemFlags flags = in_room && !was_in_room_ ? ImGuiTabItemFlags_SetSelected : 0;
        if (ImGui::BeginTabItem(Label("Sala").c_str(), nullptr, tab_flags("Sala", flags))) {
          tab_ = "Sala";
          DrawRoom(room);
          ImGui::EndTabItem();
        }
      }
      was_in_room_ = in_room;
      if (!room.in_session && ImGui::BeginTabItem(Label("Salas").c_str(), nullptr, tab_flags("Salas", 0))) {
        tab_ = "Salas";
        DrawRooms(lobby, room);
        ImGui::EndTabItem();
      }
      if (ImGui::BeginTabItem(Label("Chat global").c_str(), nullptr, tab_flags("Chat global", 0))) {
        tab_ = "Chat global";
        DrawGlobalChat(lobby);
        ImGui::EndTabItem();
      }
      if (ImGui::BeginTabItem(Label("Perfil").c_str(), nullptr, tab_flags("Perfil", 0))) {
        tab_ = "Perfil";
        DrawProfile(lobby);
        ImGui::EndTabItem();
      }
      if (!room.in_session && ImGui::BeginTabItem(Label("Replays").c_str(), nullptr, tab_flags("Replays", 0))) {
        tab_ = "Replays";
        DrawReplays();
        ImGui::EndTabItem();
      }
      ImGui::EndTabBar();
    }
    ImGui::End();
  }

 private:
  void DrawName(const lobby::State& lobby) {
    ImGui::TextWrapped("%s", T("Elegí tu nombre de usuario para jugar online. Es el que ven los demás en las salas y "
                               "en el chat (de 2 a 20 letras)."));
    ImGui::Spacing();
    ImGui::SetNextItemWidth(260);
    const bool enter = ImGui::InputText("##name", name_buf_, sizeof(name_buf_),
                                        ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_AutoSelectAll);
    ImGui::SameLine();
    const bool save = ImGui::Button(T("Guardar"));
    if (!editing_name_) {
      // nothing else until there is one
    } else {
      ImGui::SameLine();
      if (ImGui::Button(T("Cancelar"))) {
        editing_name_ = false;
        std::snprintf(name_buf_, sizeof(name_buf_), "%s", name_.c_str());
      }
    }
    if (enter || save) {
      const std::string name = Trim(name_buf_);
      if (name.size() < 2) {
        name_note_ = T("El nombre tiene que tener por lo menos 2 letras.");
      } else {
        name_ = name;
        name_note_.clear();
        editing_name_ = false;
        session::SaveName(name_);
        lobby::Start(name_);  // connects, or renames
      }
    }
    if (!name_note_.empty()) ImGui::TextColored(kRed, "%s", name_note_.c_str());
    if (!lobby.name_error.empty()) ImGui::TextColored(kRed, "%s", ErrorText(lobby.name_error).c_str());
  }

  void DrawHeader(const lobby::State& lobby) {
    switch (lobby.link) {
      case lobby::Link::kOnline:
        ImGui::TextColored(kGreen, "%s", T("Conectado"));
        ImGui::SameLine();
        ImGui::Text(T("- %d en línea"), lobby.online);
        break;
      case lobby::Link::kConnecting:
        ImGui::TextColored(kGold, "%s", T("Conectando al lobby..."));
        break;
      case lobby::Link::kRetrying:
        ImGui::TextColored(kRed, "%s", T("Sin conexión con el lobby: reintentando"));
        break;
      default:
        ImGui::TextColored(kGrey, "%s", T("Desconectado"));
        break;
    }
    ImGui::SameLine(ImGui::GetWindowWidth() - 250);
    ImGui::Text("%s", T("Sos:"));
    ImGui::SameLine();
    FlagInline(flags::Mine());
    ImGui::Text("%s", lobby.name.empty() ? name_.c_str() : lobby.name.c_str());
    ImGui::SameLine();
    if (ImGui::SmallButton(T("Cambiar"))) {
      editing_name_ = true;
      std::snprintf(name_buf_, sizeof(name_buf_), "%s", name_.c_str());
    }
    if (!lobby.name_error.empty()) ImGui::TextColored(kRed, "%s", ErrorText(lobby.name_error).c_str());
    if (!lobby.notice.empty()) ImGui::TextColored(kGold, T("Aviso: %s"), lobby.notice.c_str());
    ImGui::Separator();
  }

  void DrawRooms(const lobby::State& lobby, const room::State& room) {
    const bool online = lobby.link == lobby::Link::kOnline;
    ImGui::BeginDisabled(!online || room.phase != room::Phase::kNone);
    if (ImGui::Button(T("Partida rápida"))) QuickMatch(lobby);
    ImGui::SameLine();
    if (ImGui::Button(T("Crear sala pública"))) room::CreateRoom(true);
    ImGui::SameLine();
    if (ImGui::Button(T("Crear sala privada"))) room::CreateRoom(false);
    ImGui::Text("%s", T("Unirse con código:"));
    ImGui::SameLine();
    ImGui::SetNextItemWidth(120);
    const bool enter = ImGui::InputText("##code", code_buf_, sizeof(code_buf_),
                                        ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_CharsUppercase);
    ImGui::SameLine();
    if ((ImGui::Button(T("Unirse")) || enter) && std::strlen(code_buf_)) room::JoinRoom(code_buf_);
    ImGui::EndDisabled();
    if (!room.status.empty()) ImGui::TextColored(kGold, "%s", room.status.c_str());
    ImGui::Spacing();
    ImGui::Text(T("Salas abiertas (%d)"), int(lobby.rooms.size()));
    if (ImGui::BeginTable("rooms", 5, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_ScrollY,
                          ImVec2(0, 0))) {
      ImGui::TableSetupColumn(T("Sala"), ImGuiTableColumnFlags_WidthStretch);
      ImGui::TableSetupColumn("Host", ImGuiTableColumnFlags_WidthFixed, 150);
      ImGui::TableSetupColumn(T("Ping aprox."), ImGuiTableColumnFlags_WidthFixed, 90);
      ImGui::TableSetupColumn(T("Estado"), ImGuiTableColumnFlags_WidthFixed, 90);
      ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed, 70);
      ImGui::TableHeadersRow();
      for (const lobby::Room& r : lobby.rooms) {
        if (session::IsBlocked(r.host)) continue;  // mk9-lauta: a silenced player's room is not offered
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        ImGui::Text("%s", r.title.empty() ? r.code.c_str() : r.title.c_str());
        ImGui::TableNextColumn();
        FlagInline(r.country);
        ImGui::Text("%s", r.host.c_str());
        ImGui::TableNextColumn();
        // mk9-lauta (2026-11-10): the round trip to expect, from the two countries (the real one shows in the room)
        if (const double ms = flags::EstimatedRoundTripMs(flags::Mine(), r.country); ms > 0) {
          ImGui::TextColored(ms < 100 ? kGreen : ms < 180 ? kGold : kRed, "~%.0f ms", ms);
          if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("%s", T("Estimado por la distancia entre los dos países; el real se ve al entrar."));
          }
        } else {
          ImGui::TextColored(kGrey, "-");
        }
        ImGui::TableNextColumn();
        ImGui::TextColored(r.playing ? kGrey : kGreen, "%s", T(r.playing ? "jugando" : "esperando"));
        ImGui::TableNextColumn();
        if (!r.playing && room.phase == room::Phase::kNone && online) {
          ImGui::PushID(r.code.c_str());
          if (ImGui::SmallButton(T("Entrar"))) room::JoinRoom(r.code);
          ImGui::PopID();
        } else if (r.playing && room.phase == room::Phase::kNone && online) {
          // mk9-lauta (2026-11-10): a playing room can be watched (the game starts again watching it: feed.h)
          ImGui::PushID(r.code.c_str());
          if (ImGui::SmallButton(T("Mirar"))) room::Watch(r.code);
          ImGui::PopID();
        }
      }
      ImGui::EndTable();
    }
  }

  void DrawRoom(const room::State& room) {
    if (room.phase == room::Phase::kWatching) {
      ImGui::Text(T("Sala %s"), room.code.c_str());
      if (room.watching) {
        ImGui::Text("%s", T("Mirando:"));
        ImGui::SameLine();
        FlagInline(room.p1_country);
        ImGui::Text("%s  vs", room.p1.c_str());
        ImGui::SameLine();
        FlagInline(room.p2_country);
        ImGui::Text("%s", room.p2.c_str());
      } else {
        ImGui::TextColored(kGold, "%s", T("Pidiendo la partida al host..."));
      }
      if (ImGui::Button(T("Dejar de mirar"))) session::Back();
      return;
    }
    ImGui::Text(T("Sala %s"), room.code.c_str());
    ImGui::SameLine();
    ImGui::TextColored(kGrey, "(%s)", T(room.open ? "pública" : "privada"));
    ImGui::SameLine();
    if (ImGui::SmallButton(T("Copiar código"))) ImGui::SetClipboardText(room.code.c_str());
    if (room.host) {
      ImGui::Text("%s", T("Host: vos ("));
      ImGui::SameLine(0, 0);
      FlagInline(flags::Mine());
      ImGui::SameLine(0, 0);
      ImGui::Text("%s)", room.me.c_str());
      ImGui::Text("%s", T("Rival:"));
      ImGui::SameLine();
      if (!room.other.empty()) FlagInline(room.other_country);
      ImGui::Text("%s", room.other.empty() ? "-" : room.other.c_str());
      RecordInline(room);
    } else {
      ImGui::Text("Host:");
      ImGui::SameLine();
      if (!room.other.empty()) FlagInline(room.other_country);
      ImGui::Text("%s", room.other.empty() ? "-" : room.other.c_str());
      RecordInline(room);
      ImGui::Text("%s", T("Vos:"));
      ImGui::SameLine();
      FlagInline(flags::Mine());
      ImGui::Text("%s", room.me.c_str());
    }
    if (!room.status.empty()) ImGui::TextColored(kGold, "%s", room.status.c_str());
    if (room.in_session) {
      const netplay::SessionInfo info = netplay::Info();
      if (info.active) {
        SignalBars(info);
        ImGui::SameLine();
        ImGui::Text("%s", ConnectionText(info).c_str());
        ImGui::TextColored(kGrey, "%s", T(info.rollback ? "Pelea con rollback" : "Menús en lockstep"));
      }
      ImGui::TextWrapped("%s", T("El host (jugador 1) lleva los menús hasta VERSUS; el rival se suma con START en la "
                                 "pantalla de 2 jugadores. Después cada uno elige su luchador."));
      if (ImGui::Button(T("Terminar la sesión y volver al lobby"))) LeaveSession();
    } else if (room.phase == room::Phase::kTogether) {
      if (ImGui::Button(Label(room.my_ready ? "Cancelar listo" : "Estoy listo").c_str())) room::SetReady(!room.my_ready);
      ImGui::SameLine();
      ImGui::TextColored(room.other_ready ? kGreen : kGrey, T(room.other_ready ? "%s está listo" : "%s todavía no está listo"),
                         room.other.c_str());
      ImGui::TextColored(kGrey, "%s", T("Cuando los dos estén listos, el juego arranca de nuevo ya conectado."));
    } else if (room.phase == room::Phase::kStarting) {
      ImGui::TextColored(kGreen, "%s", T("Arrancando la pelea..."));
    }
    if (!room.in_session && room.phase != room::Phase::kStarting) {
      if (ImGui::Button(T("Salir de la sala"))) room::Leave();
    }
    ImGui::Separator();
    ImGui::Text("%s", T("Chat de la sala"));
    const float input_h = ImGui::GetFrameHeightWithSpacing() + 4;
    if (ImGui::BeginChild("room_chat", ImVec2(0, -input_h), ImGuiChildFlags_Borders)) {
      for (const room::Line& l : room.chat) {
        if (l.system) {
          ImGui::TextColored(kGrey, "%s", l.text.c_str());
        } else {
          ImGui::TextColored(l.name == room.me ? kGold : kGreen, "%s:", l.name.c_str());
          ImGui::SameLine();
          ImGui::TextWrapped("%s", l.text.c_str());
        }
      }
      if (room.chat.size() != room_lines_) {
        room_lines_ = room.chat.size();
        ImGui::SetScrollHereY(1.0f);
      }
    }
    ImGui::EndChild();
    ImGui::BeginDisabled(room.phase != room::Phase::kTogether);
    ImGui::SetNextItemWidth(-80);
    const bool enter = ImGui::InputText("##room_say", room_buf_, sizeof(room_buf_), ImGuiInputTextFlags_EnterReturnsTrue);
    ImGui::SameLine();
    if ((ImGui::Button((std::string(T("Enviar")) + "##room").c_str()) || enter) && std::strlen(room_buf_)) {
      room::Say(Trim(room_buf_));
      room_buf_[0] = '\0';
      ImGui::SetKeyboardFocusHere(-1);
    }
    ImGui::EndDisabled();
  }

  void DrawGlobalChat(const lobby::State& lobby) {
    // mk9-lauta (2026-11-10): the players silenced on this machine, and taking it back
    const std::vector<std::string> blocked = session::Blocked();
    if (!blocked.empty() && ImGui::TreeNode("silenced", T("Silenciados (%d)"), int(blocked.size()))) {
      for (const std::string& name : blocked) {
        ImGui::PushID(name.c_str());
        ImGui::Text("%s", name.c_str());
        ImGui::SameLine();
        if (ImGui::SmallButton(T("Quitar"))) session::SetBlocked(name, false);
        ImGui::PopID();
      }
      ImGui::TreePop();
    }
    const float input_h = ImGui::GetFrameHeightWithSpacing() * (lobby.error.empty() ? 1.0f : 2.0f) + 4;
    if (ImGui::BeginChild("global_chat", ImVec2(0, -input_h), ImGuiChildFlags_Borders)) {
      const std::string me = lobby.name.empty() ? name_ : lobby.name;
      for (const lobby::ChatLine& l : lobby.chat) {
        if (session::IsBlocked(l.name)) continue;
        ImGui::TextColored(kGrey, "%s", Clock24(l.at).c_str());
        ImGui::SameLine();
        if (l.id == 0 && l.name.empty()) {
          ImGui::TextColored(kGold, "%s", l.text.c_str());
          continue;
        }
        FlagInline(l.country);
        ImGui::TextColored(l.staff ? kGold : kGreen, "%s%s:", l.staff ? "[staff] " : "", l.name.c_str());
        // a click on a name: silence that player (on this machine)
        if (l.name != me && ImGui::IsItemClicked()) {
          person_ = l.name;
          ImGui::OpenPopup("chat_person");
        }
        if (l.name != me && ImGui::IsItemHovered()) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
        ImGui::SameLine();
        ImGui::TextWrapped("%s", l.text.c_str());
      }
      if (ImGui::BeginPopup("chat_person")) {
        ImGui::TextColored(kGold, "%s", person_.c_str());
        ImGui::Separator();
        if (ImGui::MenuItem(T("Silenciar (no ver sus mensajes ni sus salas)"))) session::SetBlocked(person_, true);
        ImGui::EndPopup();
      }
      if (lobby.chat.size() != global_lines_) {
        global_lines_ = lobby.chat.size();
        ImGui::SetScrollHereY(1.0f);
      }
    }
    ImGui::EndChild();
    if (!lobby.error.empty()) ImGui::TextColored(kRed, "%s", ErrorText(lobby.error).c_str());
    ImGui::BeginDisabled(lobby.link != lobby::Link::kOnline);
    ImGui::SetNextItemWidth(-80);
    const bool enter = ImGui::InputText("##global_say", global_buf_, sizeof(global_buf_),
                                        ImGuiInputTextFlags_EnterReturnsTrue);
    ImGui::SameLine();
    if ((ImGui::Button((std::string(T("Enviar")) + "##global").c_str()) || enter) && std::strlen(global_buf_)) {
      lobby::Chat(Trim(global_buf_));
      global_buf_[0] = '\0';
      ImGui::SetKeyboardFocusHere(-1);
    }
    ImGui::EndDisabled();
  }

  // The way out of a session (the button, the end's window): the goodbye goes to the rival over the room and the
  // session's own path, then the game starts again as it was (Draw: once the session ended, or after a moment).
  void LeaveSession() {
    if (leaving_) return;
    leaving_ = true;
    leave_deadline_ = Clock::now() + std::chrono::milliseconds(500);
    room::Leave();
    netplay::Leave();
  }

  static std::string Other(const room::State& room) { return room.other.empty() ? T("el rival") : room.other; }

  // mk9-lauta (2026-11-10): quick match picks, of the rooms waiting, the one with the shortest round trip to expect
  // (then the one waiting longest), leaving out the silenced players'; with none, the lobby's own quick match (it opens
  // a room for the next one who looks).
  void QuickMatch(const lobby::State& lobby) {
    const lobby::Room* best = nullptr;
    double best_ms = 0;
    for (const lobby::Room& r : lobby.rooms) {
      if (r.playing || session::IsBlocked(r.host)) continue;
      double ms = flags::EstimatedRoundTripMs(flags::Mine(), r.country);
      if (ms <= 0) ms = 200;  // a country not said: as a far one
      if (!best || ms < best_ms - 15 || (std::abs(ms - best_ms) <= 15 && r.since < best->since)) {
        best = &r;
        best_ms = ms;
      }
    }
    if (best) {
      room::JoinRoom(best->code);
    } else {
      room::QuickMatch();
    }
  }

  // In a session, a corner of the screen says who the rival is and the ping (and when the game waits for the rival).
  // mk9-lauta (2026-11-10): a session's corner, the top right; in a fight the bottom center, between the two super
  // meters (the top right is player 2's health bar and name).
  static void PlaceBadge(const netplay::SessionInfo& info) {
    const ImGuiIO& io = ImGui::GetIO();
    if (info.in_fight) {
      ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y - 8), ImGuiCond_Always, ImVec2(0.5f, 1));
    } else {
      ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x - 12, 12), ImGuiCond_Always, ImVec2(1, 0));
    }
  }

  void DrawSessionBadge(const room::State& room) {
    if (!room.in_session) return;
    const netplay::SessionInfo info = netplay::Info();
    if (info.ended) return;
    if (info.watching) {
      DrawWatchBadge(room, info);
      return;
    }
    PlaceBadge(info);
    ImGui::SetNextWindowBgAlpha(0.45f);
    const ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
                                   ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing |
                                   ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoInputs;
    if (ImGui::Begin("##session_badge", nullptr, flags)) {
      if (info.active) {
        ImGui::Text("vs");
        ImGui::SameLine();
        FlagInline(room.other_country);
        ImGui::Text("%s", Other(room).c_str());
        ImGui::SameLine();
        SignalBars(info);
        ImGui::SameLine();
        std::string line = fmt::format("{:.0f} ms", info.rtt_ms);
        if (info.rollback && info.speculating) line += Tf(" - retraso {}", info.delay);
        if (info.relayed) line += " - relay";
        ImGui::Text("%s", line.c_str());
        if (info.loss_pct >= 3 || info.jitter_ms >= 30) {
          ImGui::TextColored(kGold, T("Conexión inestable (%.0f%% perdidos, variación %.0f ms)"), info.loss_pct,
                             info.jitter_ms);
        }
        if (room.host && room.watchers > 0) ImGui::TextColored(kGrey, T("%d mirando"), room.watchers);
        if (info.waiting) ImGui::TextColored(kGold, T("Esperando a %s... %.0f s"), Other(room).c_str(), info.waiting_s);
        // mk9-lauta (2026-11-10): the two games apart (the session goes on): each player sees a different fight, so
        // they know it and can send their logs (logs/session_*.mk9net)
        if (info.desynced) {
          ImGui::TextColored(ImVec4(1.0f, 0.45f, 0.35f, 1), "%s",
                             T("Las dos partidas dejaron de coincidir: mandá logs\\session_*.mk9net"));
        }
      } else {
        ImGui::TextColored(kGold, T("Conectando con %s..."), Other(room).c_str());
      }
    }
    ImGui::End();
  }

  // mk9-lauta (2026-11-10): a spectator's corner: who plays, how far behind the match, catching up.
  void DrawWatchBadge(const room::State& room, const netplay::SessionInfo& info) {
    PlaceBadge(info);
    ImGui::SetNextWindowBgAlpha(0.45f);
    const ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
                                   ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing |
                                   ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoInputs;
    if (ImGui::Begin("##watch_badge", nullptr, flags)) {
      if (info.replay) {  // a replay: no room, no live match
        ImGui::TextColored(kGold, "%s", T("Replay"));
        if (info.catching_up) ImGui::TextColored(kGrey, "%s", T("Adelantando los menús..."));
        if (info.watch_diffs > 0) ImGui::TextColored(kRed, "%s", T("La repetición se desvió de la partida."));
        ImGui::End();
        return;
      }
      ImGui::Text("%s", T("Mirando:"));
      ImGui::SameLine();
      FlagInline(room.p1_country);
      ImGui::Text("%s  vs", room.p1.empty() ? "-" : room.p1.c_str());
      ImGui::SameLine();
      FlagInline(room.p2_country);
      ImGui::Text("%s", room.p2.empty() ? "-" : room.p2.c_str());
      if (!info.active) {
        ImGui::TextColored(kGold, "%s", T("Pidiendo la partida al host..."));
      } else if (info.catching_up) {
        ImGui::TextColored(kGold, T("Alcanzando el vivo... (%.0f s)"), info.behind_s);
      } else {
        ImGui::TextColored(kGrey, T("%.1f s detrás del vivo"), info.behind_s);
      }
      if (info.watch_diffs > 0) ImGui::TextColored(kRed, "%s", T("La repetición se desvió de la partida."));
    }
    ImGui::End();
  }

  // mk9-lauta (2026-11-10): in a session's fight, each player's name and flag under their health bar (player 1, the
  // room's host, on the left), as the game's own online showed its gamertags. Drawn over the frame where the last
  // present put it (any window shape), at its scale; behind the online windows.
  // mk9-lauta (2026-11-10): and in the character select, over each side's fighter name (where the frame has it at
  // 1280x720: 195 and 1085, the names' banners at 578).
  void DrawNamePlates(const room::State& room) {
    if (!room.in_session) return;
    const netplay::SessionInfo info = netplay::Info();
    if (!info.active) return;
    const bool select = !info.fight_on && info.game_state == fight_state::kStateSelect;
    if (!info.fight_on && !select) return;
    const float u1 = select ? 0.152f : 0.118f, u2 = select ? 0.848f : 0.882f, v = select ? 0.752f : 0.086f;
    float frame_h = renut::gpu::overlay::FrameHeightInOutput();
    float lx = 0, ly = 0, rx = 0, ry = 0;
    if (frame_h <= 0 || !renut::gpu::overlay::FramePointToOutput(u1, v, &lx, &ly) ||
        !renut::gpu::overlay::FramePointToOutput(u2, v, &rx, &ry)) {
      // nothing presented yet (a minimized window: the tests' captures): the frame over the whole overlay
      const ImGuiIO& io = ImGui::GetIO();
      frame_h = io.DisplaySize.y;
      lx = u1 * io.DisplaySize.x;
      rx = u2 * io.DisplaySize.x;
      ly = ry = v * io.DisplaySize.y;
      if (frame_h <= 0) return;
    }
    const bool watching = room.phase == room::Phase::kWatching;
    const std::string& p1 = watching ? room.p1 : room.host ? room.me : room.other;
    const std::string& p2 = watching ? room.p2 : room.host ? room.other : room.me;
    const std::string c1 = watching ? room.p1_country : room.host ? flags::Mine() : room.other_country;
    const std::string c2 = watching ? room.p2_country : room.host ? room.other_country : flags::Mine();
    ImDrawList* list = ImGui::GetBackgroundDrawList();
    ImFont* font = ImGui::GetFont();
    const float size = std::max(12.0f, frame_h * (select ? 0.036f : 0.030f));
    const float gap = size * 0.35f, flag_w = size * 40.0f / 30.0f;
    const ImU32 shadow = IM_COL32(0, 0, 0, 200), text = IM_COL32(255, 236, 190, 255);
    auto plate = [&](const std::string& name, const std::string& country, float x, float y, bool right) {
      if (name.empty()) return;
      const ImVec2 extent = font->CalcTextSizeA(size, FLT_MAX, 0.0f, name.c_str());
      const bool has_flag = flags::Known(country);
      const float total = extent.x + (has_flag ? flag_w + gap : 0.0f);
      float at = right ? x - total : x;
      if (has_flag && !right) {
        flags::DrawAt(list, country, ImVec2(at, y + (extent.y - size) * 0.5f), size);
        at += flag_w + gap;
      }
      const float d = std::max(1.0f, size / 14.0f);
      list->AddText(font, size, ImVec2(at + d, y + d), shadow, name.c_str());
      list->AddText(font, size, ImVec2(at, y), text, name.c_str());
      if (has_flag && right) {
        flags::DrawAt(list, country, ImVec2(at + extent.x + gap, y + (extent.y - size) * 0.5f), size);
      }
    };
    plate(p1, c1, lx, ly, false);
    plate(p2, c2, rx, ry, true);
  }

  // mk9-lauta (2026-11-10): what Discord shows of the player (discord.h): the menus, the lobby, a room, a session's
  // menus or a fight (since it began).
  void UpdatePresence(const room::State& room) {
    std::string details = T("En los menús"), state;
    int64_t since = 0;
    if (room.phase == room::Phase::kWatching) {
      details = T("Mirando una partida online");
      if (room.watching) state = fmt::format("{} vs {}", room.p1, room.p2);
    } else if (room.in_session) {
      const netplay::SessionInfo info = netplay::Info();
      state = Tf("vs {}", Other(room));
      if (info.active && info.rollback) {
        if (!fight_since_) fight_since_ = int64_t(std::time(nullptr));
        details = T("Peleando online");
        since = fight_since_;
      } else {
        fight_since_ = 0;
        details = T("Online: en los menús");
      }
    } else if (room.phase == room::Phase::kHosting || room.phase == room::Phase::kJoining ||
               room.phase == room::Phase::kSearching) {
      details = T("En una sala online");
      state = T("Esperando un rival");
    } else if (room.phase == room::Phase::kTogether || room.phase == room::Phase::kStarting) {
      details = T("En una sala online");
      state = Tf("con {}", Other(room));
    } else if (visible_ && lobby::Snapshot().link == lobby::Link::kOnline) {
      details = T("En el lobby online");
    }
    discord::SetPresence(details, state, since);
  }

  // mk9-lauta (2026-11-10): the other player's online record, after its name.
  static void RecordInline(const room::State& room) {
    if (room.other.empty() || room.other_won < 0) return;
    ImGui::SameLine();
    ImGui::TextColored(kGrey, T("(%d V - %d D)"), room.other_won, room.other_lost);
  }

  // The session's matches as they are decided (fight_state.h) into the player's record, with the rival's name and
  // country.
  void TakeResults(const room::State& room) {
    fight_state::Result r;
    while (fight_state::Take(&r)) {
      if (!room.in_session) continue;
      const int me = r.me, them = 1 - r.me;
      stats::Match m;
      m.at = int64_t(std::time(nullptr));
      m.rival = Other(room);
      m.rival_country = room.other_country;
      m.mine = r.characters[me];
      m.theirs = r.characters[them];
      if (!r.tag && r.health[me] >= 0 && r.health[them] >= 0 && r.health[me] != r.health[them]) {
        m.result = r.health[me] > r.health[them] ? 1 : -1;
      }
      stats::Add(m);
    }
  }

  // The player's online record: matches, wins and losses, characters, the latest matches.
  void DrawProfile(const lobby::State& lobby) {
    const stats::Summary s = stats::Totals();
    FlagInline(flags::Mine());
    ImGui::Text("%s", lobby.name.empty() ? name_.c_str() : lobby.name.c_str());
    if (s.played == 0) {
      ImGui::TextColored(kGrey, "%s", T("Todavía no jugaste partidas online."));
      return;
    }
    const int decided = s.won + s.lost;
    ImGui::Text(T("Partidas: %d - Ganadas: %d - Perdidas: %d"), s.played, s.won, s.lost);
    if (decided > 0) {
      ImGui::SameLine();
      ImGui::TextColored(kGold, T("(%.0f%% ganadas)"), 100.0 * s.won / decided);
    }
    if (!s.characters.empty()) {
      std::string list;
      for (size_t i = 0; i < s.characters.size() && i < 4; ++i) {
        list += fmt::format("{}{} ({})", i ? ", " : "", s.characters[i].first, s.characters[i].second);
      }
      ImGui::TextWrapped("%s %s", T("Tus personajes:"), list.c_str());
    }
    ImGui::Spacing();
    ImGui::Text("%s", T("Últimas partidas"));
    if (ImGui::BeginTable("history", 4, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_ScrollY,
                          ImVec2(0, 0))) {
      ImGui::TableSetupColumn(T("Fecha"), ImGuiTableColumnFlags_WidthFixed, 110);
      ImGui::TableSetupColumn(T("Resultado"), ImGuiTableColumnFlags_WidthFixed, 90);
      ImGui::TableSetupColumn(T("Rival"), ImGuiTableColumnFlags_WidthStretch);
      ImGui::TableSetupColumn(T("Personajes"), ImGuiTableColumnFlags_WidthStretch);
      ImGui::TableHeadersRow();
      for (const stats::Match& m : stats::Recent(30)) {
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        std::tm local{};
        const std::time_t t = std::time_t(m.at);
        localtime_s(&local, &t);
        ImGui::TextColored(kGrey, "%02d/%02d %02d:%02d", local.tm_mday, local.tm_mon + 1, local.tm_hour, local.tm_min);
        ImGui::TableNextColumn();
        if (m.result > 0) {
          ImGui::TextColored(kGreen, "%s", T("Ganaste"));
        } else if (m.result < 0) {
          ImGui::TextColored(kRed, "%s", T("Perdiste"));
        } else {
          ImGui::TextColored(kGrey, "-");
        }
        ImGui::TableNextColumn();
        FlagInline(m.rival_country);
        ImGui::Text("%s", m.rival.c_str());
        ImGui::TableNextColumn();
        ImGui::Text("%s vs %s", m.mine.c_str(), m.theirs.c_str());
      }
      ImGui::EndTable();
    }
  }

  // mk9-lauta (2026-11-11, PLAN-0.4 F5): the sessions recorded (logs\session_*.mk9net, the newest 10), newest first,
  // with the rival of the matches played in each (the profile's records), and "Ver": the game starts again playing it
  // back (a spectator whose match is the file: the menus fast, the fights at their pace).
  void DrawReplays() {
    ImGui::PushStyleColor(ImGuiCol_Text, kGrey);
    ImGui::TextWrapped("%s", T("Cada sesión online queda grabada (las últimas 10): verla la juega de nuevo desde el "
                               "principio, los menús rápido y las peleas a su ritmo."));
    ImGui::PopStyleColor();
    ImGui::Spacing();
    if (Clock::now() - replays_listed_ > std::chrono::seconds(5)) ListReplays();
    if (replays_.empty()) {
      ImGui::TextColored(kGrey, "%s", T("Todavía no hay sesiones grabadas."));
      return;
    }
    if (ImGui::BeginTable("replays", 3, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_ScrollY,
                          ImVec2(0, 0))) {
      ImGui::TableSetupColumn(T("Fecha"), ImGuiTableColumnFlags_WidthFixed, 130);
      ImGui::TableSetupColumn(T("Rival"), ImGuiTableColumnFlags_WidthStretch);
      ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed, 80);
      ImGui::TableHeadersRow();
      for (size_t i = 0; i < replays_.size(); ++i) {
        const Replay& r = replays_[i];
        ImGui::PushID(int(i));
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        ImGui::TextColored(kGrey, "%s", r.when.c_str());
        ImGui::TableNextColumn();
        ImGui::Text("%s", r.rival.empty() ? "-" : r.rival.c_str());
        ImGui::TableNextColumn();
        if (ImGui::SmallButton(T("Ver"))) session::Replay(r.path);
        ImGui::PopID();
      }
      ImGui::EndTable();
    }
  }

  struct Replay {
    std::string path;    // UTF-8
    std::string when;    // "dd/mm hh:mm"
    std::string rival;   // the rivals of the matches recorded in it, from the profile's records
    int64_t start = 0;   // seconds since 1970 (its name's date)
  };
  void ListReplays() {
    replays_listed_ = Clock::now();
    replays_.clear();
    wchar_t exe[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    const std::filesystem::path logs = std::filesystem::path(exe).parent_path() / L"logs";
    std::error_code ec;
    for (const auto& entry : std::filesystem::directory_iterator(logs, ec)) {
      const std::string name = entry.path().filename().string();
      int y, mo, d, h, mi, s;
      if (entry.path().extension() != L".mk9net" ||
          std::sscanf(name.c_str(), "session_%4d%2d%2d_%2d%2d%2d", &y, &mo, &d, &h, &mi, &s) != 6) {
        continue;
      }
      Replay r;
      const std::u8string u8 = entry.path().u8string();
      r.path.assign(reinterpret_cast<const char*>(u8.data()), u8.size());
      r.when = fmt::format("{:02d}/{:02d} {:02d}:{:02d}", d, mo, h, mi);
      std::tm t{};
      t.tm_year = y - 1900;
      t.tm_mon = mo - 1;
      t.tm_mday = d;
      t.tm_hour = h;
      t.tm_min = mi;
      t.tm_sec = s;
      t.tm_isdst = -1;
      r.start = int64_t(std::mktime(&t));
      replays_.push_back(std::move(r));
    }
    // one a session (two games on one PC, a test's, leave both players' files: player 1's kept)
    std::sort(replays_.begin(), replays_.end(), [](const Replay& a, const Replay& b) {
      return a.start != b.start ? a.start > b.start : a.path < b.path;
    });
    replays_.erase(std::unique(replays_.begin(), replays_.end(),
                               [](const Replay& a, const Replay& b) { return a.start == b.start; }),
                   replays_.end());
    // the matches each one holds: those that ended after it began and before the next one began
    const std::vector<stats::Match> matches = stats::Recent(100);
    for (size_t i = 0; i < replays_.size(); ++i) {
      const int64_t until = i == 0 ? INT64_MAX : replays_[i - 1].start;
      for (const stats::Match& m : matches) {
        if (m.at < replays_[i].start || m.at >= until || m.rival.empty()) continue;
        if (replays_[i].rival.find(m.rival) == std::string::npos) {
          replays_[i].rival += (replays_[i].rival.empty() ? "" : ", ") + m.rival;
        }
      }
    }
  }
  std::vector<Replay> replays_;
  Clock::time_point replays_listed_{};

  // The session is over: why, and back to the lobby (by itself after a few seconds).
  void DrawSessionEnd(const room::State& room, const netplay::SessionInfo& info) {
    if (ended_at_ == Clock::time_point{}) ended_at_ = Clock::now();
    constexpr int kBackAfterS = 10;
    const int left = kBackAfterS - int(std::chrono::duration_cast<std::chrono::seconds>(Clock::now() - ended_at_).count());
    if (left <= 0) {
      LeaveSession();
      return;
    }
    std::string why;
    if (info.replay) {
      why = info.end_reason == netplay::EndReason::kOtherVersion
                ? T("Este replay es de otra versión de mk9-lauta: no se puede ver con esta.")
            : info.end_reason == netplay::EndReason::kFailed ? T("No se pudo leer el replay.")
                                                             : T("El replay terminó.");
    } else if (info.watching) {
      if (room.watch_full) why = T("La sala ya tiene el máximo de espectadores.");
      else if (info.end_reason == netplay::EndReason::kFailed) why = T("No se pudo traer la partida.");
      else if (info.end_reason == netplay::EndReason::kOtherVersion)
        why = T("La partida se juega con otra versión de mk9-lauta: para mirarla hay que tener la misma (F2 > Acerca "
                "de).");
      else why = T("La partida que mirabas terminó.");
    } else switch (info.end_reason) {
      case netplay::EndReason::kLeft: why = T("Saliste de la sesión."); break;
      case netplay::EndReason::kPeerLeft: why = Tf("{} se fue de la sesión.", Other(room)); break;
      case netplay::EndReason::kPeerLost: why = Tf("Se cortó la conexión con {}.", Other(room)); break;
      case netplay::EndReason::kDesync: why = T("Las dos partidas dejaron de coincidir (se desincronizaron)."); break;
      case netplay::EndReason::kFailed: why = Tf("No se pudo conectar con {}.", Other(room)); break;
      case netplay::EndReason::kOtherVersion:
        why = Tf("{} tiene otra versión de mk9-lauta: {} (vos: {}).\nPara jugar online tienen que tener la misma: "
                 "actualicen los dos (F2 > Acerca de).",
                 Other(room), info.peer_release.empty() ? std::string(T("0.2.0 o anterior")) : info.peer_release,
                 MK9_LAUTA_VERSION_TEXT);
        break;
      default: why = T("La sesión terminó."); break;
    }
    // mk9-lauta (2026-11-11, A3): a desync during the session left a report (netplay.cpp WriteDesyncReport)
    if (!info.desync_report.empty()) why += std::string("\n") + T("Hubo un desync: el reporte quedó en la carpeta logs.");
    const ImGuiIO& io = ImGui::GetIO();
    ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.4f), ImGuiCond_Always,
                            ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowBgAlpha(0.85f);
    const ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
                                   ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoMove;
    if (ImGui::Begin("##session_end", nullptr, flags)) {
      ImGui::PushFont(nullptr, 30.0f);
      ImGui::TextColored(kGold, "%s", T("Fin de la sesión"));
      ImGui::PopFont();
      ImGui::Spacing();
      ImGui::PushFont(nullptr, 22.0f);
      ImGui::Text("%s", why.c_str());
      ImGui::PopFont();
      ImGui::TextColored(kGrey, T("Volvés al lobby en %d s."), left);
      ImGui::Spacing();
      if (ImGui::Button(T("Volver al lobby ahora"), ImVec2(260, 0))) LeaveSession();
      if (!info.desync_report.empty()) {
        ImGui::SameLine();
        if (ImGui::Button(T("Abrir el reporte del desync"), ImVec2(260, 0))) updater::Open(info.desync_report);
      }
    }
    ImGui::End();
  }

  // Hidden: a new line in the room's chat, or a rival in the room, shows for a few seconds.
  void DrawToast(const room::State& room) {
    if (room.chat.size() > last_seen_lines_) {
      const room::Line& l = room.chat.back();
      toast_ = l.system ? l.text : l.name + ": " + l.text;
      toast_until_ = Clock::now() + std::chrono::seconds(6);
      last_seen_lines_ = room.chat.size();
    }
    if (toast_.empty() || Clock::now() > toast_until_) return;
    const ImGuiIO& io = ImGui::GetIO();
    ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f, 16), ImGuiCond_Always, ImVec2(0.5f, 0));
    ImGui::SetNextWindowBgAlpha(0.6f);
    const ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
                                   ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing |
                                   ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoInputs;
    if (ImGui::Begin("##online_toast", nullptr, flags)) {
      ImGui::Text("%s", toast_.c_str());
      ImGui::TextColored(kGrey, "F1: online");
    }
    ImGui::End();
  }

  bool visible_ = false, editing_name_ = false, was_in_room_ = false, leaving_ = false, focus_ = false;
  int tab_step_ = 0;                 // LB / RB this frame
  std::string tab_ = "Salas";        // the tab showing
  std::string name_, name_note_, toast_;
  std::string person_;               // the chat's name clicked (its menu)
  int64_t fight_since_ = 0;          // the session's fight began (seconds since 1970; Discord's timer)
  char name_buf_[24] = {}, code_buf_[12] = {}, room_buf_[220] = {}, global_buf_[220] = {};
  size_t room_lines_ = 0, global_lines_ = 0, last_seen_lines_ = 0;
  Clock::time_point toast_until_{}, leave_deadline_{}, ended_at_{};
  const Clock::time_point started_ = Clock::now();
};

}  // namespace

void Install() {
  static std::once_flag once;
  std::call_once(once, [] {
    static OnlineWindow window;
    renut::gpu::overlay::AddDialog(&window);
  });
}

}  // namespace mk9::online_ui
