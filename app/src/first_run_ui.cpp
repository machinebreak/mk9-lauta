// mk9-lauta (2026-11-11, PLAN-0.4 F3): the first start's steps (first_run_ui.h).

#include "first_run_ui.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include "imgui.h"
#include "imgui_internal.h"  // SetFocusID: a step's main button gets the controller's focus

#include <rex/cvar.h>
#include <rex/logging.h>

#include "mk9_controls.h"
#include "mk9_hardware.h"
#include "mk9_overlay_input.h"
#include "mk9_updater.h"
#include "online/i18n.h"
#include "online/netplay.h"
#include "online/online_ui.h"
#include "options_ui.h"
#include "renut_engine/gpu/overlay/overlay_ui.h"

REXCVAR_DEFINE_BOOL(mk9_first_run_done, true, "MK9",
                    "The first start's steps (language, online name, quality, controller) are done: the first start "
                    "writes false to mk9.toml, and they show until finished or skipped");
REXCVAR_DEFINE_INT32(mk9_first_run_test_cycle_s, 0, "MK9",
                     "Tests: the first start's steps go on by themselves every this many seconds (a capture of each, "
                     "mk9_snapshot_s); 0: off");

namespace mk9::first_run_ui {
namespace {

using mk9::i18n::T;

const ImVec4 kGold(1.00f, 0.80f, 0.30f, 1.0f);
const ImVec4 kGrey(0.68f, 0.66f, 0.66f, 1.0f);
const ImVec4 kGreen(0.45f, 0.85f, 0.45f, 1.0f);

enum Step { kWelcome, kLanguage, kName, kQuality, kController, kDone };

// The game's languages (user_language), each named in itself, and the online screens' language that goes with it.
struct Language {
  const char* code;
  const char* name;
  const char* online;
};
const Language kLanguages[] = {{"5", "Español", "es"},  {"1", "English", "en"}, {"9", "Português", "pt"},
                               {"4", "Français", "en"}, {"3", "Deutsch", "en"}, {"6", "Italiano", "en"}};

const char* const kPresets[] = {"Muy baja", "Baja", "Consola", "Alta", "Ultra"};

// What each preset is, for the players (the options window's texts, shorter).
const char* PresetHelp(const std::string& name) {
  if (name == "Muy baja") return T("Para PCs con gráficos integrados viejos o 4 GB de RAM: 480p, y si no llega a 60 se "
                                   "dibuja uno de cada dos cuadros.");
  if (name == "Baja") return T("Para PCs con poca placa de video: 480p, como en la consola.");
  if (name == "Consola") return T("Como en la Xbox 360: 720p.");
  if (name == "Alta") return T("1080p, sombras nítidas, texturas filtradas.");
  return T("1440p y todo al máximo: para placas de video potentes.");
}

std::atomic<bool> g_active{false};

// The steps show on a player's own start: never in a session's game (a room's restart, a spectator, a replay: its
// input must reach the game) nor in a scripted test.
bool Wanted() {
  if (REXCVAR_GET(mk9_first_run_done) || netplay::SessionGame()) return false;
  return rex::cvar::GetFlagByName("mk9_input_script").empty() &&
         rex::cvar::GetFlagByName("mk9_input_script_file").empty();
}

std::string Trim(const char* text) {
  std::string s(text);
  while (!s.empty() && s.front() == ' ') s.erase(s.begin());
  while (!s.empty() && s.back() == ' ') s.pop_back();
  return s;
}

class FirstRunWindow : public renut::gpu::overlay::Dialog {
 public:
  void Draw() override {
    const bool show = Wanted();
    g_active.store(show, std::memory_order_relaxed);
    overlay_input::SetOpen(overlay_input::kWizard, show);
    if (!show) return;
    if (!started_) Prepare();
    if (const int cycle = REXCVAR_GET(mk9_first_run_test_cycle_s); cycle > 0) {
      const auto seconds = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now() - since_);
      const Step step = Step(std::min<int64_t>(seconds.count() / cycle, kDone));
      if (step != step_) Go(step);
    }
    ImGui::GetStyle().FontScaleMain = options_ui::UiScale();
    const float scale = options_ui::UiScale();
    ImGuiIO& io = ImGui::GetIO();
    // the game behind, dimmed
    ImGui::GetBackgroundDrawList()->AddRectFilled(ImVec2(0, 0), io.DisplaySize, IM_COL32(0, 0, 0, 150));
    const ImVec2 size(std::min(700.0f * scale, io.DisplaySize.x - 24), std::min(470.0f * scale, io.DisplaySize.y - 24));
    ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f), ImGuiCond_Always,
                            ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(size, ImGuiCond_Always);
    if (focus_window_) {
      ImGui::SetNextWindowFocus();
      focus_window_ = false;
    }
    // the controller's step: its buttons are tested, not used to move (START goes on, the mouse too)
    ImGuiWindowFlags flags = ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                             ImGuiWindowFlags_NoSavedSettings;
    if (step_ == kController) flags |= ImGuiWindowFlags_NoNavInputs;
    ImGui::SetNextWindowBgAlpha(1.0f);  // the game's own texts and logos showed through
    ImGui::PushFont(renut::gpu::overlay::TitleFont(), 0.0f);
    const bool open = ImGui::Begin("mk9-lauta###first_run", nullptr, flags);
    ImGui::PopFont();
    if (open) {
      const bool back = step_ != kController && overlay_input::ClosePressed();
      if (back && step_ > kWelcome) Go(Step(step_ - 1));
      switch (step_) {
        case kWelcome: DrawWelcome(); break;
        case kLanguage: DrawLanguage(); break;
        case kName: DrawName(); break;
        case kQuality: DrawQuality(); break;
        case kController: DrawController(); break;
        case kDone: DrawDone(); break;
      }
    }
    ImGui::End();
  }

 private:
  void Prepare() {
    started_ = true;
    REXLOG_INFO("[first run] the first start's steps");
    language_ = rex::cvar::GetFlagByName("user_language");
    started_language_ = language_;
    preset_ = rex::cvar::GetFlagByName("mk9_preset_detected");
    detected_ = preset_;
    const hardware::Info info = hardware::Detect();
    pc_ = info.gpu.empty() ? std::string(T("placa de video desconocida")) : info.gpu;
    // a card's own memory; an integrated one's is what the firmware set aside (411 MB on a Radeon 760M): left out
    if (info.vram >= (1ull << 30)) pc_ += " (" + std::to_string((info.vram + (1ull << 29)) >> 30) + " GB)";
    if (info.ram) pc_ += std::string(", ") + std::to_string((info.ram + (1ull << 29)) >> 30) + T(" GB de RAM");
  }

  void Go(Step step) {
    step_ = step;
    focus_main_ = step != kName;  // the name's step: its box, to type at once
    focus_name_ = step == kName;
    note_.clear();
  }

  // The step's title, with how far along it is.
  void Title(const char* title, int number) {
    if (number > 0) ImGui::TextColored(kGrey, T("Paso %d de 4"), number);
    ImGui::PushFont(renut::gpu::overlay::TitleFont(), 34.0f);
    ImGui::TextColored(kGold, "%s", title);
    ImGui::PopFont();
    ImGui::Spacing();
  }

  void Paragraph(const char* text, const ImVec4* color = nullptr) {
    if (color) ImGui::PushStyleColor(ImGuiCol_Text, *color);
    ImGui::TextWrapped("%s", text);
    if (color) ImGui::PopStyleColor();
  }

  // The buttons at the bottom: back (none on the first step) and the main one, which the controller's A gets first.
  // Returns 1 for the main one, -1 for back, 0 for none.
  int Buttons(const char* main, const char* back = nullptr, const char* other = nullptr, bool* other_pressed = nullptr) {
    const float height = ImGui::GetFrameHeight();
    const float bottom = ImGui::GetWindowHeight() - ImGui::GetStyle().WindowPadding.y - height;
    if (ImGui::GetCursorPosY() < bottom) ImGui::SetCursorPosY(bottom);
    const float main_width = 200 * options_ui::UiScale();
    const float right = ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x - main_width;
    int pressed = 0;
    bool on_line = false;  // a button is on the bottom line already (SameLine would go back to the text's line)
    // back and the other choice in a quieter style than the main button
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.20f, 0.13f, 0.11f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.32f, 0.18f, 0.14f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.42f, 0.22f, 0.16f, 1.0f));
    if (back) {
      if (ImGui::Button(back, ImVec2(150 * options_ui::UiScale(), 0))) pressed = -1;
      on_line = true;
    }
    if (other) {
      const float other_width = 200 * options_ui::UiScale();
      const float x = right - other_width - ImGui::GetStyle().ItemSpacing.x;
      if (on_line) ImGui::SameLine(x);
      else ImGui::SetCursorPosX(x);
      if (ImGui::Button(other, ImVec2(other_width, 0)) && other_pressed) *other_pressed = true;
      on_line = true;
    }
    ImGui::PopStyleColor(3);
    if (on_line) ImGui::SameLine(right);
    else ImGui::SetCursorPosX(right);
    if (ImGui::Button(main, ImVec2(main_width, 0))) pressed = 1;
    if (focus_main_) {
      ImGui::SetFocusID(ImGui::GetItemID(), ImGui::GetCurrentWindow());
      focus_main_ = false;
    }
    return pressed;
  }

  void DrawWelcome() {
    Title(T("¡Bienvenido a mk9-lauta!"), 0);
    Paragraph(T("Mortal Kombat Komplete Edition en tu PC, con online y rollback."));
    ImGui::Spacing();
    Paragraph(T("Cuatro pasos y a jugar: el idioma, tu nombre para el online, la calidad de los gráficos y el mando. "
                "Todo se puede cambiar después: F2 abre las opciones y F1 el online (con el mando, BACK + START y "
                "BACK + Y)."));
    bool skip = false;
    const int pressed = Buttons(T("Empezar"), nullptr, T("Saltar"), &skip);
    if (skip) Finish(false);
    else if (pressed > 0) Go(kLanguage);
  }

  void DrawLanguage() {
    Title(T("Idioma"), 1);
    Paragraph(T("El idioma de los textos del juego (en español el Modo Historia está doblado). Las pantallas del "
                "online van en español, inglés o portugués."));
    ImGui::Spacing();
    for (const Language& l : kLanguages) {
      if (ImGui::Selectable(l.name, language_ == l.code, 0, ImVec2(260 * options_ui::UiScale(), 0)) &&
          language_ != l.code) {
        language_ = l.code;
        options_ui::SaveToConfig({{"user_language", language_}});
        rex::cvar::SetFlagByName("user_language", language_);  // the game reads it at its start: the next one
        i18n::SetLanguage(l.online);
        REXLOG_INFO("[first run] the game's language: {}", l.name);
      }
    }
    if (language_ != started_language_) {
      ImGui::Spacing();
      Paragraph(T("El juego cambia de idioma al volver a abrirlo: al final podés reiniciarlo."), &kGold);
    }
    const int pressed = Buttons(T("Siguiente"), T("Atrás"));
    if (pressed > 0) Go(kName);
    if (pressed < 0) Go(kWelcome);
  }

  void DrawName() {
    Title(T("Tu nombre online"), 2);
    Paragraph(T("Es el que ven los demás en las salas y en el chat (de 2 a 20 letras). Si lo dejás vacío, lo elegís "
                "después en F1."));
    ImGui::Spacing();
    ImGui::SetNextItemWidth(320 * options_ui::UiScale());
    if (focus_name_) {
      ImGui::SetKeyboardFocusHere();
      focus_name_ = false;
    }
    ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 1.0f);  // a box to see on the dark window
    const bool enter = ImGui::InputText("##first_run_name", name_buf_, sizeof(name_buf_),
                                        ImGuiInputTextFlags_EnterReturnsTrue);
    ImGui::PopStyleVar();
    if (!note_.empty()) Paragraph(note_.c_str(), &kGold);
    const int pressed = Buttons(T("Siguiente"), T("Atrás"));
    if (pressed < 0) Go(kLanguage);
    if (pressed > 0 || enter) {
      const std::string name = Trim(name_buf_);
      if (!name.empty() && name.size() < 2) {
        note_ = T("El nombre tiene que tener por lo menos 2 letras.");
      } else {
        if (!name.empty()) {
          online_ui::UseName(name);
          REXLOG_INFO("[first run] the online name chosen");
        }
        Go(kQuality);
      }
    }
  }

  void DrawQuality() {
    Title(T("Calidad de los gráficos"), 3);
    ImGui::TextWrapped(T("Tu PC: %s."), pc_.c_str());
    if (!detected_.empty()) {
      ImGui::TextWrapped(T("Para ella elegimos «%s». Si el juego no va fluido, probá una más baja."),
                         T(detected_.c_str()));
    }
    ImGui::Spacing();
    std::string help = preset_;  // the one under the mouse or the controller's focus, else the chosen one
    for (const char* name : kPresets) {
      const bool chosen = preset_ == name;
      if (ImGui::Selectable(T(name), chosen, 0, ImVec2(260 * options_ui::UiScale(), 0)) && !chosen) Choose(name);
      if (ImGui::IsItemHovered() || ImGui::IsItemFocused()) help = name;
    }
    ImGui::Spacing();
    if (!help.empty()) Paragraph(PresetHelp(help), &kGrey);
    const int pressed = Buttons(T("Siguiente"), T("Atrás"));
    if (pressed > 0) Go(kController);
    if (pressed < 0) Go(kName);
  }

  void Choose(const char* name) {
    const auto* values = options_ui::PresetValues(name);
    if (!values) return;
    std::vector<std::pair<std::string, std::string>> saved;
    for (const auto& [key, value] : *values) {
      rex::cvar::SetFlagByName(key, value);
      saved.emplace_back(key, value);
    }
    options_ui::SaveToConfig(saved);
    preset_ = name;
    REXLOG_INFO("[first run] quality preset \"{}\"", name);
  }

  void DrawController() {
    Title(T("El mando"), 4);
    bool any = false;
    for (const controls::Device& d : controls::Devices()) {
      if (d.keyboard) continue;
      any = true;
      if (d.player > 0) ImGui::BulletText(T("Jugador %d: %s"), d.player, d.name.c_str());
      else ImGui::BulletText("%s", d.name.c_str());
    }
    if (!any) {
      Paragraph(T("Ningún mando conectado. Los de Xbox, PlayStation y Switch Pro andan apenas se conectan. Sin mando "
                  "jugás con el teclado: F2 > Controles muestra y cambia las teclas."),
                &kGrey);
    }
    if (any || REXCVAR_GET(mk9_first_run_test_cycle_s) > 0) {  // the test: the buttons' chips without a controller
      ImGui::Spacing();
      Paragraph(T("Apretá los botones: se encienden los que el juego recibe."));
      ImGui::Spacing();
      DrawPadTest();
      ImGui::Spacing();
      Paragraph(T("Qué golpe hace cada botón se cambia en el menú del juego: OPCIONES > MANDO."), &kGrey);
    }
    const bool start = ImGui::IsKeyPressed(ImGuiKey_GamepadStart, false);
    if (any) Paragraph(T("START sigue."), &kGrey);
    const int pressed = Buttons(T("Siguiente"), T("Atrás"));
    if (pressed > 0 || start) Go(kDone);
    if (pressed < 0) Go(kQuality);
  }

  // The controller's buttons as chips, lit while held (the overlay's ImGui gets the pads as its gamepad keys).
  void DrawPadTest() {
    struct Chip {
      ImGuiKey key;
      const char* label;
    };
    const Chip rows[3][6] = {  // each frame: the labels in the language chosen a step before
        {{ImGuiKey_GamepadFaceDown, "A"}, {ImGuiKey_GamepadFaceRight, "B"}, {ImGuiKey_GamepadFaceLeft, "X"},
         {ImGuiKey_GamepadFaceUp, "Y"}, {ImGuiKey_GamepadBack, "BACK"}, {ImGuiKey_GamepadStart, "START"}},
        {{ImGuiKey_GamepadL1, "LB"}, {ImGuiKey_GamepadR1, "RB"}, {ImGuiKey_GamepadL2, "LT"},
         {ImGuiKey_GamepadR2, "RT"}, {ImGuiKey_GamepadL3, "L3"}, {ImGuiKey_GamepadR3, "R3"}},
        {{ImGuiKey_GamepadDpadUp, T("Arriba")}, {ImGuiKey_GamepadDpadDown, T("Abajo")},
         {ImGuiKey_GamepadDpadLeft, T("Izquierda")}, {ImGuiKey_GamepadDpadRight, T("Derecha")},
         {ImGuiKey_GamepadLStickUp, T("Stick")}, {ImGuiKey_None, nullptr}},
    };
    const float width = 92 * options_ui::UiScale();
    ImDrawList* list = ImGui::GetWindowDrawList();
    for (const auto& row : rows) {
      for (int i = 0; i < 6 && row[i].label; ++i) {
        if (i) ImGui::SameLine();
        bool down = ImGui::IsKeyDown(row[i].key);
        if (row[i].key == ImGuiKey_GamepadLStickUp) {
          for (ImGuiKey k : {ImGuiKey_GamepadLStickUp, ImGuiKey_GamepadLStickDown, ImGuiKey_GamepadLStickLeft,
                             ImGuiKey_GamepadLStickRight}) {
            down |= ImGui::IsKeyDown(k);
          }
        }
        const ImVec2 at = ImGui::GetCursorScreenPos();
        const ImVec2 size(width, ImGui::GetFrameHeight());
        list->AddRectFilled(at, ImVec2(at.x + size.x, at.y + size.y),
                            down ? IM_COL32(242, 184, 64, 255) : IM_COL32(60, 30, 26, 255), 5.0f);
        const ImVec2 text = ImGui::CalcTextSize(row[i].label);
        list->AddText(ImVec2(at.x + (size.x - text.x) * 0.5f, at.y + (size.y - text.y) * 0.5f),
                      down ? IM_COL32(20, 12, 8, 255) : IM_COL32(220, 210, 205, 255), row[i].label);
        ImGui::Dummy(size);
      }
    }
  }

  void DrawDone() {
    Title(T("¡Listo!"), 0);
    Paragraph(T("Para tener a mano:"));
    ImGui::Spacing();
    if (ImGui::BeginTable("##keys", 2, ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_RowBg |
                                           ImGuiTableFlags_PadOuterX)) {
      const std::pair<const char*, const char*> rows[] = {
          {"F1", T("El online: salas, chat global, tu perfil y los replays")},
          {"F2", T("Las opciones: gráficos, controles, pantalla e idioma")},
          {"F11  /  Alt + Enter", T("Pantalla completa o ventana")},
          {T("BACK + Y (mando)"), T("El online")},
          {T("BACK + START (mando)"), T("Las opciones")},
          {"LB  /  RB", T("Cambiar de pestaña")},
          {"B", T("Volver o cerrar")},
      };
      for (const auto& [key, what] : rows) {
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        ImGui::PushFont(renut::gpu::overlay::StrongFont(), 0.0f);
        ImGui::TextColored(kGold, "%s", key);
        ImGui::PopFont();
        ImGui::TableNextColumn();
        ImGui::TextUnformatted(what);
      }
      ImGui::EndTable();
    }
    const bool restart = language_ != started_language_;
    if (restart) {
      ImGui::Spacing();
      Paragraph(T("El idioma nuevo se ve al volver a abrir el juego."), &kGold);
    }
    bool restart_now = false;
    const int pressed = restart ? Buttons(T("Jugar"), T("Atrás"), T("Reiniciar ahora"), &restart_now)
                                : Buttons(T("Jugar"), T("Atrás"));
    if (pressed < 0) Go(kController);
    if (pressed > 0) Finish(true);
    if (restart_now) {
      Finish(true);
      updater::Restart();
    }
  }

  void Finish(bool done) {
    options_ui::SaveToConfig({{"mk9_first_run_done", "true"}});
    rex::cvar::SetFlagByName("mk9_first_run_done", "true");
    overlay_input::SetOpen(overlay_input::kWizard, false);
    g_active.store(false, std::memory_order_relaxed);
    REXLOG_INFO("[first run] {}", done ? "done" : "skipped");
  }

  const std::chrono::steady_clock::time_point since_ = std::chrono::steady_clock::now();
  bool started_ = false, focus_window_ = true, focus_main_ = true, focus_name_ = true;
  Step step_ = kWelcome;
  std::string language_, started_language_, preset_, detected_, pc_, note_;
  char name_buf_[24] = {};
};

}  // namespace

void Install() {
  static std::once_flag once;
  std::call_once(once, [] {
    // before its first frame: the online window, which opens by itself with no name yet, waits from the start
    g_active.store(Wanted(), std::memory_order_relaxed);
    static FirstRunWindow window;
    renut::gpu::overlay::AddDialog(&window);
  });
}

bool Active() { return g_active.load(std::memory_order_relaxed); }

}  // namespace mk9::first_run_ui
