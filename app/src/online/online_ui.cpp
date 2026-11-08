// MK9 online: the online screens in the game (online_ui.h).

#include "online/online_ui.h"

#include <chrono>
#include <cstring>
#include <ctime>
#include <mutex>
#include <string>

#include "imgui.h"

#include <fmt/format.h>
#include <rex/cvar.h>

#include "online/lobby.h"
#include "online/netplay.h"
#include "online/room.h"
#include "online/session.h"
#include "renut_engine/gpu/overlay/overlay_ui.h"

REXCVAR_DEFINE_BOOL(mk9_online_show, false, "MK9 Online",
                    "The online screens open at start (the game started again back from a room's session)");
REXCVAR_DEFINE_INT32(mk9_online_test_leave_s, 0, "MK9 Online",
                     "Tests: in a room's session, this player leaves this many seconds after the game's first frame "
                     "(0 = never)");

namespace mk9::online_ui {
namespace {

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
  if (code == "slow") return "Más despacio: un mensaje por segundo.";
  if (code == "links") return "En el chat no se pueden pasar links.";
  if (code == "muted") return "Estás silenciado por un rato.";
  if (code == "banned") return "No podés escribir en el chat.";
  if (code == "reserved") return "Ese nombre está reservado.";
  if (code == "empty") return "El nombre no puede quedar vacío.";
  return code;
}

class OnlineWindow : public renut::gpu::overlay::Dialog {
 public:
  OnlineWindow() {
    name_ = session::LoadName();
    std::snprintf(name_buf_, sizeof(name_buf_), "%s", name_.c_str());
    if (!name_.empty() && lobby::Configured()) lobby::Start(name_);
    // with no name yet the window opens by itself (it has to be chosen to play online), and so it does back from a
    // room's session: the player is in the lobby again
    visible_ = lobby::Configured() && (name_.empty() || REXCVAR_GET(mk9_online_show));
  }

  void Draw() override {
    room::Pump();
    if (ImGui::IsKeyPressed(ImGuiKey_F1, false)) {
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
    ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f), ImGuiCond_FirstUseEver,
                            ImVec2(0.5f, 0.5f));
    if (!ImGui::Begin("Online  (F1 para mostrar u ocultar)", &visible_, ImGuiWindowFlags_NoCollapse)) {
      ImGui::End();
      return;
    }
    if (!lobby::Configured()) {
      ImGui::TextWrapped("El online está apagado (mk9_lobby_url vacío).");
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
      if (in_room) {
        const ImGuiTabItemFlags flags = in_room && !was_in_room_ ? ImGuiTabItemFlags_SetSelected : 0;
        if (ImGui::BeginTabItem("Sala", nullptr, flags)) {
          DrawRoom(room);
          ImGui::EndTabItem();
        }
      }
      was_in_room_ = in_room;
      if (!room.in_session && ImGui::BeginTabItem("Salas")) {
        DrawRooms(lobby, room);
        ImGui::EndTabItem();
      }
      if (ImGui::BeginTabItem("Chat global")) {
        DrawGlobalChat(lobby);
        ImGui::EndTabItem();
      }
      ImGui::EndTabBar();
    }
    ImGui::End();
  }

 private:
  void DrawName(const lobby::State& lobby) {
    ImGui::TextWrapped("Elegí tu nombre de usuario para jugar online. Es el que ven los demás en las salas y en el "
                       "chat (de 2 a 20 letras).");
    ImGui::Spacing();
    ImGui::SetNextItemWidth(260);
    const bool enter = ImGui::InputText("##name", name_buf_, sizeof(name_buf_),
                                        ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_AutoSelectAll);
    ImGui::SameLine();
    const bool save = ImGui::Button("Guardar");
    if (!editing_name_) {
      // nothing else until there is one
    } else {
      ImGui::SameLine();
      if (ImGui::Button("Cancelar")) {
        editing_name_ = false;
        std::snprintf(name_buf_, sizeof(name_buf_), "%s", name_.c_str());
      }
    }
    if (enter || save) {
      const std::string name = Trim(name_buf_);
      if (name.size() < 2) {
        name_note_ = "El nombre tiene que tener por lo menos 2 letras.";
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
        ImGui::TextColored(kGreen, "Conectado");
        ImGui::SameLine();
        ImGui::Text("- %d en línea", lobby.online);
        break;
      case lobby::Link::kConnecting:
        ImGui::TextColored(kGold, "Conectando al lobby...");
        break;
      case lobby::Link::kRetrying:
        ImGui::TextColored(kRed, "Sin conexión con el lobby: reintentando");
        break;
      default:
        ImGui::TextColored(kGrey, "Desconectado");
        break;
    }
    ImGui::SameLine(ImGui::GetWindowWidth() - 230);
    ImGui::Text("Sos: %s", lobby.name.empty() ? name_.c_str() : lobby.name.c_str());
    ImGui::SameLine();
    if (ImGui::SmallButton("Cambiar")) {
      editing_name_ = true;
      std::snprintf(name_buf_, sizeof(name_buf_), "%s", name_.c_str());
    }
    if (!lobby.name_error.empty()) ImGui::TextColored(kRed, "%s", ErrorText(lobby.name_error).c_str());
    if (!lobby.notice.empty()) ImGui::TextColored(kGold, "Aviso: %s", lobby.notice.c_str());
    ImGui::Separator();
  }

  void DrawRooms(const lobby::State& lobby, const room::State& room) {
    const bool online = lobby.link == lobby::Link::kOnline;
    ImGui::BeginDisabled(!online || room.phase != room::Phase::kNone);
    if (ImGui::Button("Partida rápida")) room::QuickMatch();
    ImGui::SameLine();
    if (ImGui::Button("Crear sala pública")) room::CreateRoom(true);
    ImGui::SameLine();
    if (ImGui::Button("Crear sala privada")) room::CreateRoom(false);
    ImGui::Text("Unirse con código:");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(120);
    const bool enter = ImGui::InputText("##code", code_buf_, sizeof(code_buf_),
                                        ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_CharsUppercase);
    ImGui::SameLine();
    if ((ImGui::Button("Unirse") || enter) && std::strlen(code_buf_)) room::JoinRoom(code_buf_);
    ImGui::EndDisabled();
    if (!room.status.empty()) ImGui::TextColored(kGold, "%s", room.status.c_str());
    ImGui::Spacing();
    ImGui::Text("Salas abiertas (%d)", int(lobby.rooms.size()));
    if (ImGui::BeginTable("rooms", 4, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_ScrollY,
                          ImVec2(0, 0))) {
      ImGui::TableSetupColumn("Sala", ImGuiTableColumnFlags_WidthStretch);
      ImGui::TableSetupColumn("Host", ImGuiTableColumnFlags_WidthFixed, 150);
      ImGui::TableSetupColumn("Estado", ImGuiTableColumnFlags_WidthFixed, 90);
      ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed, 70);
      ImGui::TableHeadersRow();
      for (const lobby::Room& r : lobby.rooms) {
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        ImGui::Text("%s", r.title.empty() ? r.code.c_str() : r.title.c_str());
        ImGui::TableNextColumn();
        ImGui::Text("%s%s%s", r.host.c_str(), r.country.empty() ? "" : " - ", r.country.c_str());
        ImGui::TableNextColumn();
        ImGui::TextColored(r.playing ? kGrey : kGreen, "%s", r.playing ? "jugando" : "esperando");
        ImGui::TableNextColumn();
        if (!r.playing && room.phase == room::Phase::kNone && online) {
          ImGui::PushID(r.code.c_str());
          if (ImGui::SmallButton("Entrar")) room::JoinRoom(r.code);
          ImGui::PopID();
        }
      }
      ImGui::EndTable();
    }
  }

  void DrawRoom(const room::State& room) {
    ImGui::Text("Sala %s", room.code.c_str());
    ImGui::SameLine();
    ImGui::TextColored(kGrey, "(%s)", room.open ? "pública" : "privada");
    ImGui::SameLine();
    if (ImGui::SmallButton("Copiar código")) ImGui::SetClipboardText(room.code.c_str());
    if (room.host) {
      ImGui::Text("Host: vos (%s)", room.me.c_str());
      ImGui::Text("Rival: %s", room.other.empty() ? "-" : room.other.c_str());
    } else {
      ImGui::Text("Host: %s", room.other.empty() ? "-" : room.other.c_str());
      ImGui::Text("Vos: %s", room.me.c_str());
    }
    if (!room.status.empty()) ImGui::TextColored(kGold, "%s", room.status.c_str());
    if (room.in_session) {
      const netplay::SessionInfo info = netplay::Info();
      if (info.active) {
        ImGui::Text("Ping: %.0f ms - %s", info.rtt_ms, info.rollback ? "pelea con rollback" : "menús en lockstep");
      }
      ImGui::TextWrapped("El host (jugador 1) lleva los menús hasta VERSUS; el rival se suma con START en la "
                         "pantalla de 2 jugadores. Después cada uno elige su luchador.");
      if (ImGui::Button("Terminar la sesión y volver al lobby")) LeaveSession();
    } else if (room.phase == room::Phase::kTogether) {
      if (ImGui::Button(room.my_ready ? "Cancelar listo" : "Estoy listo")) room::SetReady(!room.my_ready);
      ImGui::SameLine();
      ImGui::TextColored(room.other_ready ? kGreen : kGrey, "%s %s", room.other.c_str(),
                         room.other_ready ? "está listo" : "todavía no está listo");
      ImGui::TextColored(kGrey, "Cuando los dos estén listos, el juego arranca de nuevo ya conectado.");
    } else if (room.phase == room::Phase::kStarting) {
      ImGui::TextColored(kGreen, "Arrancando la pelea...");
    }
    if (!room.in_session && room.phase != room::Phase::kStarting) {
      if (ImGui::Button("Salir de la sala")) room::Leave();
    }
    ImGui::Separator();
    ImGui::Text("Chat de la sala");
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
    if ((ImGui::Button("Enviar##room") || enter) && std::strlen(room_buf_)) {
      room::Say(Trim(room_buf_));
      room_buf_[0] = '\0';
      ImGui::SetKeyboardFocusHere(-1);
    }
    ImGui::EndDisabled();
  }

  void DrawGlobalChat(const lobby::State& lobby) {
    const float input_h = ImGui::GetFrameHeightWithSpacing() * (lobby.error.empty() ? 1.0f : 2.0f) + 4;
    if (ImGui::BeginChild("global_chat", ImVec2(0, -input_h), ImGuiChildFlags_Borders)) {
      for (const lobby::ChatLine& l : lobby.chat) {
        ImGui::TextColored(kGrey, "%s", Clock24(l.at).c_str());
        ImGui::SameLine();
        if (l.id == 0 && l.name.empty()) {
          ImGui::TextColored(kGold, "%s", l.text.c_str());
          continue;
        }
        ImGui::TextColored(l.staff ? kGold : kGreen, "%s%s:", l.staff ? "[staff] " : "", l.name.c_str());
        ImGui::SameLine();
        ImGui::TextWrapped("%s", l.text.c_str());
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
    if ((ImGui::Button("Enviar##global") || enter) && std::strlen(global_buf_)) {
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

  static std::string Other(const room::State& room) { return room.other.empty() ? "el rival" : room.other; }

  // In a session, a corner of the screen says who the rival is and the ping (and when the game waits for the rival).
  void DrawSessionBadge(const room::State& room) {
    if (!room.in_session) return;
    const netplay::SessionInfo info = netplay::Info();
    if (info.ended) return;
    const ImGuiIO& io = ImGui::GetIO();
    ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x - 12, 12), ImGuiCond_Always, ImVec2(1, 0));
    ImGui::SetNextWindowBgAlpha(0.45f);
    const ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
                                   ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing |
                                   ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoInputs;
    if (ImGui::Begin("##session_badge", nullptr, flags)) {
      if (info.active) {
        ImGui::Text("vs %s - %.0f ms", Other(room).c_str(), info.rtt_ms);
        if (info.waiting) ImGui::TextColored(kGold, "Esperando a %s... %.0f s", Other(room).c_str(), info.waiting_s);
      } else {
        ImGui::TextColored(kGold, "Conectando con %s...", Other(room).c_str());
      }
    }
    ImGui::End();
  }

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
    switch (info.end_reason) {
      case netplay::EndReason::kLeft: why = "Saliste de la sesión."; break;
      case netplay::EndReason::kPeerLeft: why = Other(room) + " se fue de la sesión."; break;
      case netplay::EndReason::kPeerLost: why = "Se cortó la conexión con " + Other(room) + "."; break;
      case netplay::EndReason::kDesync: why = "Las dos partidas dejaron de coincidir (se desincronizaron)."; break;
      case netplay::EndReason::kFailed: why = "No se pudo conectar con " + Other(room) + "."; break;
      default: why = "La sesión terminó."; break;
    }
    const ImGuiIO& io = ImGui::GetIO();
    ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.4f), ImGuiCond_Always,
                            ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowBgAlpha(0.85f);
    const ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
                                   ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoMove;
    if (ImGui::Begin("##session_end", nullptr, flags)) {
      ImGui::TextColored(kGold, "Fin de la sesión");
      ImGui::Spacing();
      ImGui::Text("%s", why.c_str());
      ImGui::TextColored(kGrey, "Volvés al lobby en %d s.", left);
      ImGui::Spacing();
      if (ImGui::Button("Volver al lobby ahora")) LeaveSession();
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
  std::string name_, name_note_, toast_;
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
