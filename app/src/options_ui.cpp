// mk9-nx (2026-11-08): the game's options for the players (options_ui.h).

#include "options_ui.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include "imgui.h"

#include <SDL3/SDL_video.h>
#include <fmt/format.h>

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/ui/keybinds.h>

#include "mk9_controls.h"
#include "mk9_overlay_input.h"
#include "mk9_updater.h"
#include "renut_engine/gpu/overlay/overlay_ui.h"

#if defined(MK9_BUILTIN_CONFIG)
#include "mk9_builtin_config.h"  // the PC build's defaults (app/mk9_pc_defaults.toml): the keyboard's keys
#endif

REXCVAR_DEFINE_BOOL(mk9_options_show, false, "MK9", "The options window (F2) opens at start");
REXCVAR_DEFINE_BOOL(mk9_show_fps, false, "MK9", "The frames a second, top left (the options window, F2)");
REXCVAR_DEFINE_STRING(mk9_ui_scale, "auto", "MK9",
                      "The size of the overlay's windows (F1, F2): auto (by the window's height, 1080 lines = 100 %) or "
                      "a factor (1, 1.25, 1.5, 2)");
extern std::atomic<uint32_t> g_mk9_fps_game, g_mk9_fps_shown;  // mk9_frame_counter.cpp
REXCVAR_DEFINE_STRING(mk9_options_tab, "", "MK9",
                      "The options window's tab shown first (Gráficos, Controles, Pantalla e idioma, Online); empty = "
                      "the first");
REXCVAR_DEFINE_STRING(mk9_options_test_preset, "", "MK9",
                      "Tests: this quality preset (Baja, Consola, Alta, Ultra) is chosen at start, as with its button");

namespace mk9::options_ui {
namespace {

struct Choice {
  const char* value;
  const char* label;
};

struct Option {
  const char* key;    // the cvar
  const char* label;
  std::vector<Choice> choices;
  const char* help;
  bool restart = false;  // the game reads it only when it starts
};

const std::vector<Option>& GraphicsOptions() {
  static const std::vector<Option> options = {
      {"renut_resolution", "Resolución interna",
       {{"480p", "480p (la más liviana)"}, {"720p", "720p (la de la consola)"}, {"1080p", "1080p"}, {"1440p", "1440p"},
        {"4K", "4K"}},
       "La resolución a la que se dibuja el juego. Bajarla es lo que más ayuda en una PC con poca placa de video."},
      {"renut_shadow_resolution", "Sombras",
       {{"Resolution", "Según la resolución"}, {"1x", "Consola (1x)"}, {"2x", "Nítidas (2x)"}, {"3x", "Muy nítidas (3x)"},
        {"4x", "Máximas (4x)"}},
       "La resolución de las sombras. \"Consola\" es la más liviana."},
      {"renut_ambient_occlusion", "Oclusión ambiental",
       {{"Half", "Media resolución (consola)"}, {"Full", "Resolución completa"}},
       "Las sombras de contacto entre objetos."},
      {"renut_anisotropic_filtering", "Filtrado de texturas",
       {{"Game", "El del juego"}, {"2x", "2x"}, {"4x", "4x"}, {"8x", "8x"}, {"16x", "16x"}},
       "Texturas más nítidas vistas de costado (el piso). Cuesta poco."},
      {"renut_post_process_precision", "Brillos y desenfoque",
       {{"Game", "Como en la consola"}, {"High", "Más precisos"}},
       "La precisión del brillo (bloom), la profundidad de campo y la oclusión ambiental."},
      {"mk9_draw_mode", "Cuadros dibujados",
       {{"all", "Todos (60 por segundo)"}, {"auto", "Automático (la mitad si la PC no llega)"},
        {"half", "Uno de cada dos (30 por segundo)"}},
       "Con poca placa de video: el juego sigue a su velocidad (la pelea a 60 cuadros por segundo) y se dibuja uno de "
       "cada dos, la mitad de trabajo para la placa. \"Automático\" lo hace sólo mientras la PC no llega a 60."},
      {"renut_vsync", "Sincronización vertical",
       {{"true", "Activada (sin cortes de imagen)"}, {"false", "Desactivada"}},
       "Activada, la imagen no se corta. Desactivada puede bajar un poco la latencia en monitores de más de 60 Hz."},
      {"mk9_dynamic_resolution", "Resolución dinámica",
       {{"false", "Apagada"}, {"true", "Activada"}},
       "Si la PC no llega a 60 (un X-Ray, muchos efectos), baja la resolución interna un escalón (1080p, 720p, "
       "480p) y la vuelve a subir cuando sobra tiempo; nunca pasa de la que elegiste arriba."},
      {"banjo_renut_memory_policy", "Ahorro de memoria",
       {{"false", "Apagado"}, {"true", "Activado (PCs con 4-6 GB de RAM)"}},
       "Suelta las texturas que no se usan y guarda menos copias: menos memoria a cambio de volver a armarlas cuando "
       "hacen falta. Se activa solo en PCs con 6 GB o menos."},
      {"sa2_renut_msaa", "Antialiasing (MSAA)",
       {{"0", "Apagado (el del juego)"}, {"2", "2x"}, {"4", "4x"}, {"8", "8x"}},
       "Bordes más suaves; cada paso cuesta más placa de video. Se aplica al volver a abrir el juego."},
  };
  return options;
}

const std::vector<Option>& DisplayOptions() {
  static const std::vector<Option> options = {
      {"fullscreen", "Pantalla",
       {{"true", "Pantalla completa"}, {"false", "Ventana"}},
       "F11 o Alt+Enter cambian entre pantalla completa y ventana en cualquier momento."},
      {"renut_gpu_backend", "API gráfica",
       {{"d3d12", "DirectX 12"}, {"vulkan", "Vulkan"}},
       "Si el juego anda mal o se ve raro con una, probá la otra (algunas placas y drivers van mejor con Vulkan). Se "
       "aplica al volver a abrir el juego.",
       true},
      {"mk9_show_fps", "Mostrar FPS",
       {{"false", "No"}, {"true", "Sí, arriba a la izquierda"}},
       "Los cuadros por segundo que se ven y, si se saltean, los que se dibujan."},
      {"mk9_ui_scale", "Tamaño de los menús F1 y F2",
       {{"auto", "Automático (según la pantalla)"}, {"1", "100 %"}, {"1.25", "125 %"}, {"1.5", "150 %"},
        {"2", "200 %"}},
       "El tamaño del texto de estas ventanas. Automático lo agranda en pantallas de más de 1080 líneas (1440p, 4K)."},
      {"user_language", "Idioma del juego",
       {{"5", "Español"}, {"1", "English"}, {"9", "Português"}, {"4", "Français"}, {"3", "Deutsch"},
        {"6", "Italiano"}},
       "Los textos del juego. En español el Modo Historia está doblado. Se aplica al volver a abrir el juego.",
       true},
  };
  return options;
}

const std::vector<Option>& OnlineOptions() {
  static const std::vector<Option> options = {
      {"mk9_net_rollback_delay", "Retraso de entrada en las peleas",
       {{"0", "Ninguno (más correcciones)"}, {"1", "1 cuadro (recomendado)"}, {"2", "2 cuadros (conexiones malas)"}},
       "Cuadros que espera cada golpe antes de verse. Con 0 no hay espera y el rollback corrige todo lo que llega tarde; "
       "con 1 o 2 se ven menos correcciones. Vale el que elige el host de la sala."},
  };
  return options;
}

struct Preset {
  const char* label;
  const char* help;
  std::vector<std::pair<const char*, const char*>> values;
};

const std::vector<Preset>& Presets() {
  static const std::vector<Preset> presets = {
      {"Muy baja",
       "Para netbooks y PCs con gráficos integrados viejos o 4 GB de RAM: 480p, todo como en la consola y, si la PC no "
       "llega a 60, se dibuja uno de cada dos cuadros (el juego sigue a su velocidad).",
       {{"renut_resolution", "480p"}, {"renut_shadow_resolution", "1x"}, {"renut_ambient_occlusion", "Half"},
        {"renut_anisotropic_filtering", "Game"}, {"renut_post_process_precision", "Game"},
        {"mk9_draw_mode", "auto"}, {"mk9_dynamic_resolution", "false"}}},
      {"Baja", "Para PCs con poca placa de video: 480p, sombras y efectos como en la consola.",
       {{"renut_resolution", "480p"}, {"renut_shadow_resolution", "1x"}, {"renut_ambient_occlusion", "Half"},
        {"renut_anisotropic_filtering", "Game"}, {"renut_post_process_precision", "Game"},
        {"mk9_draw_mode", "all"}, {"mk9_dynamic_resolution", "false"}}},
      {"Consola", "Como en la Xbox 360: 720p.",
       {{"renut_resolution", "720p"}, {"renut_shadow_resolution", "Resolution"}, {"renut_ambient_occlusion", "Half"},
        {"renut_anisotropic_filtering", "Game"}, {"renut_post_process_precision", "Game"},
        {"mk9_draw_mode", "all"}, {"mk9_dynamic_resolution", "false"}}},
      {"Alta", "1080p, sombras nítidas, texturas filtradas.",
       {{"renut_resolution", "1080p"}, {"renut_shadow_resolution", "2x"}, {"renut_ambient_occlusion", "Full"},
        {"renut_anisotropic_filtering", "8x"}, {"renut_post_process_precision", "High"},
        {"mk9_draw_mode", "all"}, {"mk9_dynamic_resolution", "true"}}},
      {"Ultra", "1440p y todo al máximo: para placas de video potentes.",
       {{"renut_resolution", "1440p"}, {"renut_shadow_resolution", "4x"}, {"renut_ambient_occlusion", "Full"},
        {"renut_anisotropic_filtering", "16x"}, {"renut_post_process_precision", "High"},
        {"mk9_draw_mode", "all"}, {"mk9_dynamic_resolution", "true"}}},
  };
  return presets;
}

// A key's value in the PC build's compiled-in defaults (app/mk9_pc_defaults.toml), else the cvar's own default.
std::string BuiltInDefault(const std::string& key) {
#if defined(MK9_BUILTIN_CONFIG)
  std::istringstream in{std::string(kMk9BuiltInConfig)};
  for (std::string line; std::getline(in, line);) {
    size_t i = 0;
    while (i < line.size() && (line[i] == ' ' || line[i] == '\t')) ++i;
    if (line.compare(i, key.size(), key) != 0) continue;
    size_t j = i + key.size();
    while (j < line.size() && (line[j] == ' ' || line[j] == '\t')) ++j;
    if (j >= line.size() || line[j] != '=') continue;
    const size_t open = line.find('"', j);
    const size_t close = open == std::string::npos ? std::string::npos : line.find('"', open + 1);
    if (close != std::string::npos) return line.substr(open + 1, close - open - 1);
  }
#endif
  const rex::cvar::FlagEntry* info = rex::cvar::GetFlagInfo(key);
  return info ? info->default_value : std::string();
}

// A bind's keys without the ones that are this virtual key (the key moves to another action).
std::string WithoutKey(const std::string& bind, int virtual_key) {
  std::string out;
  std::istringstream in(bind);
  for (std::string token; std::getline(in, token, ',');) {
    while (!token.empty() && token.front() == ' ') token.erase(token.begin());
    while (!token.empty() && token.back() == ' ') token.pop_back();
    if (token.empty()) continue;
    if (token.find('+') == std::string::npos && int(rex::ui::ParseVirtualKey(token)) == virtual_key) continue;
    if (!out.empty()) out += ",";
    out += token;
  }
  return out;
}

std::filesystem::path ConfigPath() {
  wchar_t path[MAX_PATH] = {};
  GetModuleFileNameW(nullptr, path, MAX_PATH);
  return std::filesystem::path(path).parent_path() / "mk9.toml";
}

// A key's line as TOML (a string quoted).
std::string TomlLine(const std::string& key, const std::string& value) {
  const rex::cvar::FlagEntry* info = rex::cvar::GetFlagInfo(key);
  const bool quoted = info && info->type == rex::cvar::FlagType::String;
  return key + " = " + (quoted ? "\"" + value + "\"" : value);
}

// The keys' lines in mk9.toml replaced (or added at its end) with these values; every other line as it was.
}  // namespace

float UiScale() {
  const std::string value = REXCVAR_GET(mk9_ui_scale);
  if (value.empty() || value == "auto") {
    const float height = ImGui::GetIO().DisplaySize.y;
    return std::clamp(height / 1080.0f, 1.0f, 2.5f);
  }
  const float factor = std::strtof(value.c_str(), nullptr);
  return factor >= 0.5f && factor <= 3.0f ? factor : 1.0f;
}

const std::vector<std::pair<const char*, const char*>>* PresetValues(const std::string& name) {
  for (const Preset& p : Presets()) {
    if (name == p.label) return &p.values;
  }
  return nullptr;
}

bool SaveToConfig(const std::vector<std::pair<std::string, std::string>>& values) {
  const std::filesystem::path path = ConfigPath();
  std::vector<std::string> lines;
  {
    std::ifstream in(path, std::ios::binary);
    for (std::string line; std::getline(in, line);) {
      if (!line.empty() && line.back() == '\r') line.pop_back();
      lines.push_back(line);
    }
  }
  for (const auto& [key, value] : values) {
    bool found = false;
    for (std::string& line : lines) {
      size_t i = 0;
      while (i < line.size() && (line[i] == ' ' || line[i] == '\t')) ++i;
      if (line.compare(i, key.size(), key) != 0) continue;
      size_t j = i + key.size();
      while (j < line.size() && (line[j] == ' ' || line[j] == '\t')) ++j;
      if (j >= line.size() || line[j] != '=') continue;
      line = TomlLine(key, value);
      found = true;
    }
    if (!found) lines.push_back(TomlLine(key, value));
  }
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  if (!out) return false;
  for (const std::string& line : lines) out << line << "\r\n";
  return bool(out);
}

namespace {

class OptionsWindow : public renut::gpu::overlay::Dialog {
 public:
  void Draw() override {
    ImGui::GetStyle().FontScaleMain = UiScale();
    if (static bool tested = false; !tested) {
      tested = true;
      for (const Preset& p : Presets()) {
        if (REXCVAR_GET(mk9_options_test_preset) == p.label) Apply(p);
      }
    }
    if (ImGui::IsKeyPressed(ImGuiKey_F2, false) || overlay_input::ToggleOptionsPressed()) {
      visible_ = !visible_;
      if (visible_) ImGui::SetNextWindowFocus();  // over the online screens if they are open too
      if (!visible_) StopCapture();
    }
    // Full screen or a window, any time (on the window's thread, where the SDK changes the window)
    const ImGuiIO& keys = ImGui::GetIO();
    if (ImGui::IsKeyPressed(ImGuiKey_F11, false) || (keys.KeyAlt && ImGui::IsKeyPressed(ImGuiKey_Enter, false))) {
      const bool full = rex::cvar::GetFlagByName("fullscreen") == "true";
      Set({{"fullscreen", full ? "false" : "true"}}, full ? "Ventana" : "Pantalla completa");
    }
    if (capturing_ >= 0) TakeKey();
    if (REXCVAR_GET(mk9_show_fps)) DrawFps();
    overlay_input::SetOpen(overlay_input::kOptions, visible_);
    if (!visible_) return;
    const ImGuiIO& io = ImGui::GetIO();
    ImGui::SetNextWindowSize(ImVec2(760, 660), ImGuiCond_FirstUseEver);
    // inside the window however small it is (1366x768, a small window)
    ImGui::SetNextWindowSizeConstraints(ImVec2(320, 200),
                                        ImVec2(std::max(320.0f, io.DisplaySize.x - 16), std::max(200.0f, io.DisplaySize.y - 16)));
    ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f), ImGuiCond_FirstUseEver,
                            ImVec2(0.5f, 0.5f));
    if (!ImGui::Begin("Opciones  (F2 para mostrar u ocultar)", &visible_, ImGuiWindowFlags_NoCollapse)) {
      ImGui::End();
      return;
    }
    // the controller: B closes, LB / RB change tabs
    if (overlay_input::ClosePressed()) {
      visible_ = false;
      StopCapture();
    }
    if (const int step = overlay_input::TabStep(); step && capturing_ < 0) {
      static const char* const kTabs[] = {"Gráficos", "Controles", "Pantalla e idioma", "Online", "Acerca de"};
      int current = 0;
      for (int i = 0; i < 5; ++i) {
        if (tab_ == kTabs[i]) current = i;
      }
      tab_ = kTabs[(current + step + 5) % 5];
      select_tab_ = true;
    }
    if (ImGui::BeginTabBar("options_tabs")) {
      if (ImGui::BeginTabItem("Gráficos", nullptr, TabFlags("Gráficos"))) {
        if (!select_tab_) tab_ = "Gráficos";
        DrawPresets();
        ImGui::Separator();
        for (const Option& o : GraphicsOptions()) DrawOption(o);
        ImGui::EndTabItem();
      }
      if (ImGui::BeginTabItem("Controles", nullptr, TabFlags("Controles"))) {
        if (!select_tab_) tab_ = "Controles";
        DrawControls();
        ImGui::EndTabItem();
      } else {
        StopCapture();
      }
      if (ImGui::BeginTabItem("Pantalla e idioma", nullptr, TabFlags("Pantalla e idioma"))) {
        if (!select_tab_) tab_ = "Pantalla e idioma";
        for (const Option& o : DisplayOptions()) DrawOption(o);
        DrawMonitor();
        DrawWindowSize();
        ImGui::EndTabItem();
      }
      if (ImGui::BeginTabItem("Online", nullptr, TabFlags("Online"))) {
        if (!select_tab_) tab_ = "Online";
        for (const Option& o : OnlineOptions()) DrawOption(o);
        ImGui::EndTabItem();
      }
      if (ImGui::BeginTabItem("Acerca de", nullptr, TabFlags("Acerca de"))) {
        if (!select_tab_) tab_ = "Acerca de";
        DrawAbout();
        ImGui::EndTabItem();
      }
      ImGui::EndTabBar();
      first_tabs_ = false;
      select_tab_ = false;
    }
    ImGui::Separator();
    if (!note_.empty()) ImGui::TextColored(note_ok_ ? ImVec4(0.45f, 0.9f, 0.5f, 1) : ImVec4(1, 0.45f, 0.4f, 1), "%s",
                                           note_.c_str());
    ImGui::TextColored(ImVec4(0.65f, 0.65f, 0.7f, 1), "Los cambios se aplican al momento y quedan guardados en mk9.toml.");
    ImGui::End();
  }

 private:
  // The monitor the game opens on (the SDK's monitor: 1-based in SDL's order, 0 the system's default).
  void DrawMonitor() {
    if (!rex::cvar::GetFlagInfo("monitor")) return;
    int count = 0;
    SDL_DisplayID* displays = SDL_GetDisplays(&count);
    std::vector<std::pair<std::string, std::string>> choices = {{"0", "Automático (el principal)"}};
    for (int i = 0; i < count && displays; ++i) {
      const char* name = SDL_GetDisplayName(displays[i]);
      const SDL_DisplayMode* mode = SDL_GetDesktopDisplayMode(displays[i]);
      char label[160];
      std::snprintf(label, sizeof(label), "%d: %s%s", i + 1, name ? name : "monitor",
                    mode ? fmt::format(" ({}x{})", mode->w, mode->h).c_str() : "");
      choices.emplace_back(std::to_string(i + 1), label);
    }
    if (displays) SDL_free(displays);
    if (count < 2) return;  // one monitor: nothing to choose
    DrawChoices("monitor", "Monitor", choices,
                "En qué monitor se abre el juego (en pantalla completa, ese). Se aplica al volver a abrir el juego.",
                {{"monitor", ""}});
  }

  // The window's size at start (the SDK's window_width / window_height, 0 = 1280x720); dragging its border works too.
  void DrawWindowSize() {
    if (!rex::cvar::GetFlagInfo("window_width")) return;
    static const std::vector<std::pair<std::string, std::string>> sizes = {
        {"0x0", "Automático (1280x720)"}, {"1280x720", "1280x720"}, {"1600x900", "1600x900"},
        {"1920x1080", "1920x1080"},       {"2560x1440", "2560x1440"},
    };
    const std::string current = Current("window_width") + "x" + Current("window_height");
    ImGui::PushID("window_size");
    ImGui::AlignTextToFramePadding();
    ImGui::Text("Tamaño de la ventana");
    ImGui::SameLine(250);
    ImGui::SetNextItemWidth(-1);
    const char* shown = current.c_str();
    for (const auto& [value, label] : sizes) {
      if (value == current) shown = label.c_str();
    }
    if (ImGui::BeginCombo("##value", shown)) {
      for (const auto& [value, label] : sizes) {
        if (ImGui::Selectable(label.c_str(), value == current) && value != current) {
          const size_t x = value.find('x');
          Set({{"window_width", value.substr(0, x)}, {"window_height", value.substr(x + 1)}}, "Tamaño de la ventana");
        }
      }
      ImGui::EndCombo();
    }
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.65f, 0.65f, 0.7f, 1));
    ImGui::TextWrapped("El tamaño con el que abre en ventana (también se puede arrastrar el borde). Se aplica al volver a "
                       "abrir el juego.");
    ImGui::PopStyleColor();
    ImGui::Spacing();
    ImGui::PopID();
  }

  // A choice of values made at run time (the monitors).
  void DrawChoices(const char* key, const char* label, const std::vector<std::pair<std::string, std::string>>& choices,
                   const char* help, const std::vector<std::pair<std::string, std::string>>&) {
    const std::string current = Current(key);
    const char* shown = current.c_str();
    for (const auto& [value, text] : choices) {
      if (value == current) shown = text.c_str();
    }
    ImGui::PushID(key);
    ImGui::AlignTextToFramePadding();
    ImGui::Text("%s", label);
    ImGui::SameLine(250);
    ImGui::SetNextItemWidth(-1);
    if (ImGui::BeginCombo("##value", shown)) {
      for (const auto& [value, text] : choices) {
        if (ImGui::Selectable(text.c_str(), value == current) && value != current) Set({{key, value}}, label);
      }
      ImGui::EndCombo();
    }
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.65f, 0.65f, 0.7f, 1));
    ImGui::TextWrapped("%s", help);
    ImGui::PopStyleColor();
    ImGui::Spacing();
    ImGui::PopID();
  }

  // mk9-lauta: the version, the updater, the logs folder and the project's page.
  void DrawAbout() {
    const updater::Status st = updater::GetStatus();
    ImGui::Text("mk9-lauta v%s", st.current.c_str());
    ImGui::TextColored(ImVec4(0.65f, 0.65f, 0.7f, 1), "Mortal Kombat Komplete Edition para PC, con online con rollback.");
    ImGui::Separator();
    bool check = rex::cvar::GetFlagByName("mk9_update_check") != "false";
    if (ImGui::Checkbox("Buscar una versión nueva al iniciar", &check)) {
      Set({{"mk9_update_check", check ? "true" : "false"}}, check ? "Buscar versiones nuevas: sí" : "Buscar versiones nuevas: no");
    }
    const bool busy = st.phase == updater::Phase::kChecking || st.phase == updater::Phase::kDownloading;
    if (busy) ImGui::BeginDisabled();
    if (ImGui::Button("Buscar ahora")) updater::CheckNow();
    if (busy) ImGui::EndDisabled();
    if (!st.message.empty()) {
      ImGui::SameLine();
      ImGui::TextUnformatted(st.message.c_str());
    }
    if (st.phase == updater::Phase::kAvailable) {
      if (ImGui::Button("Actualizar ahora")) updater::Download();
    } else if (st.phase == updater::Phase::kDownloading) {
      ImGui::ProgressBar(float(st.progress), ImVec2(-1, 0));
    } else if (st.phase == updater::Phase::kReady) {
      if (ImGui::Button("Reiniciar ahora")) updater::InstallAndRestart();
    }
    ImGui::Separator();
    ImGui::TextWrapped("Si algo falla, la carpeta logs tiene lo que pasó: mandá el último mk9_*.log (y el crash_*.dmp de "
                       "la misma hora si el juego se cerró de golpe).");
    if (ImGui::Button("Abrir la carpeta de logs")) updater::Open(updater::LogsFolder());
    ImGui::SameLine();
    if (ImGui::Button("Reportar un problema")) updater::Open("https://github.com/machinebreak/mk9-lauta/issues");
    ImGui::SameLine();
    if (ImGui::Button("Página del proyecto")) updater::Open("https://github.com/machinebreak/mk9-lauta");
  }

  // The frames a second, top left: the shown ones, and the game's when they differ (the performance mode).
  void DrawFps() {
    const uint32_t game = g_mk9_fps_game.load(std::memory_order_relaxed);
    const uint32_t shown = g_mk9_fps_shown.load(std::memory_order_relaxed);
    ImGui::SetNextWindowPos(ImVec2(8, 8));
    ImGui::SetNextWindowBgAlpha(0.45f);
    const ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoInputs |
                                   ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings |
                                   ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav;
    if (ImGui::Begin("##fps", nullptr, flags)) {
      if (shown + 2 < game) {
        ImGui::Text("%u FPS (juego a %u)", shown, game);
      } else {
        ImGui::Text("%u FPS", shown);
      }
    }
    ImGui::End();
  }

  // The tab mk9_options_tab names is selected the first time the tabs are drawn.
  // The tab to select: the one LB / RB asked for, or mk9_options_tab the first time.
  ImGuiTabItemFlags TabFlags(const char* tab) const {
    if (select_tab_) return tab_ == tab ? ImGuiTabItemFlags_SetSelected : 0;
    return first_tabs_ && REXCVAR_GET(mk9_options_tab) == tab ? ImGuiTabItemFlags_SetSelected : 0;
  }

  void DrawPresets() {
    ImGui::AlignTextToFramePadding();
    ImGui::Text("Calidad:");
    for (const Preset& p : Presets()) {
      bool active = true;  // every option as the preset has it
      for (const auto& [key, value] : p.values) active &= Current(key) == value;
      ImGui::SameLine();
      if (active) ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.75f, 0.45f, 0.10f, 1.0f));
      if (ImGui::Button(p.label)) Apply(p);
      if (active) ImGui::PopStyleColor();
      if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", p.help);
    }
  }

  void DrawControls() {
    const ImVec4 grey(0.65f, 0.65f, 0.7f, 1);
    bool keyboard = rex::cvar::GetFlagByName("mnk_mode") == "true";
    if (ImGui::Checkbox("Jugar con el teclado", &keyboard)) {
      Set({{"mnk_mode", keyboard ? "true" : "false"}}, keyboard ? "Teclado activado" : "Teclado desactivado");
    }
    if (keyboard) {
      ImGui::SameLine(0, 30);
      ImGui::AlignTextToFramePadding();
      ImGui::Text("El teclado maneja al");
      ImGui::SameLine();
      const std::string player = rex::cvar::GetFlagByName("mk9_keyboard_player");
      ImGui::SetNextItemWidth(150);
      if (ImGui::BeginCombo("##keyboard_player", player == "2" ? "Jugador 2" : "Jugador 1")) {
        if (ImGui::Selectable("Jugador 1", player != "2") && player == "2")
          Set({{"mk9_keyboard_player", "1"}}, "El teclado maneja al Jugador 1");
        if (ImGui::Selectable("Jugador 2", player == "2") && player != "2")
          Set({{"mk9_keyboard_player", "2"}}, "El teclado maneja al Jugador 2");
        ImGui::EndCombo();
      }
      ImGui::PushStyleColor(ImGuiCol_Text, grey);
      ImGui::TextWrapped("Jugador 2: para un Versus en esta PC con el teclado y un mando (el mando es el Jugador 1). "
                         "Online manejás a tu luchador con el teclado y con cualquier mando conectado.");
      ImGui::PopStyleColor();
      ImGui::Spacing();
      DrawKeys();
    }
    ImGui::Separator();
    bool rumble = rex::cvar::GetFlagByName("input_rumble") == "true";
    if (ImGui::Checkbox("Vibración del mando", &rumble)) {
      Set({{"input_rumble", rumble ? "true" : "false"}}, rumble ? "Vibración activada" : "Vibración desactivada");
    }
    ImGui::Text("Mandos conectados:");
    bool any_pad = false;
    for (const controls::Device& d : controls::Devices()) {
      if (d.keyboard) continue;
      any_pad = true;
      if (d.player > 0) ImGui::BulletText("Jugador %d: %s", d.player, d.name.c_str());
      else ImGui::BulletText("%s (sin jugador: ya hay cuatro)", d.name.c_str());
    }
    ImGui::PushStyleColor(ImGuiCol_Text, grey);
    if (!any_pad) {
      ImGui::TextWrapped("Ninguno. Los de Xbox, PlayStation y Switch Pro andan apenas se conectan; el primero que se "
                         "conecta es el Jugador 1.");
    }
    ImGui::TextWrapped("Qué golpe hace cada botón del mando se cambia en el menú del juego: OPCIONES > MANDO. El "
                       "teclado lo sigue: cada tecla aprieta el botón del mando que dice la tabla.");
    ImGui::PopStyleColor();
  }

  void DrawKeys() {
    const auto& actions = controls::Actions();
    if (ImGui::BeginTable("keys", 4, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH |
                                         ImGuiTableFlags_SizingStretchProp)) {
      ImGui::TableSetupColumn("Acción", ImGuiTableColumnFlags_WidthStretch, 2.3f);
      ImGui::TableSetupColumn("Mando", ImGuiTableColumnFlags_WidthStretch, 1.0f);
      ImGui::TableSetupColumn("Teclas", ImGuiTableColumnFlags_WidthStretch, 1.8f);
      ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed, 110.0f);
      ImGui::TableHeadersRow();
      for (int i = 0; i < int(actions.size()); ++i) {
        const controls::Action& a = actions[size_t(i)];
        ImGui::PushID(i);
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted(a.label);
        ImGui::TableNextColumn();
        ImGui::TextColored(ImVec4(0.65f, 0.65f, 0.7f, 1), "%s", a.button);
        ImGui::TableNextColumn();
        if (capturing_ == i) {
          ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.3f, 1), "Apretá una tecla...");
        } else {
          const std::string keys = controls::KeysLabel(rex::cvar::GetFlagByName(a.cvar));
          ImGui::TextUnformatted(keys.empty() ? "(ninguna)" : keys.c_str());
        }
        ImGui::TableNextColumn();
        if (capturing_ == i) {
          if (ImGui::Button("Cancelar", ImVec2(-1, 0))) StopCapture();
        } else if (ImGui::Button("Cambiar", ImVec2(-1, 0))) {
          capturing_ = i;
          controls::CaptureNextKey(true);
        }
        ImGui::PopID();
      }
      ImGui::EndTable();
    }
    if (ImGui::Button("Volver a las teclas de fábrica")) {
      StopCapture();
      std::vector<std::pair<std::string, std::string>> values;
      for (const controls::Action& a : actions) values.emplace_back(a.cvar, BuiltInDefault(a.cvar));
      Set(values, "Teclas de fábrica");
    }
  }

  // The key pressed for the action waiting for one: it is that action's only key now, and no other action's.
  void TakeKey() {
    const int vk = controls::TakeCapturedKey();
    if (vk < 0) return;
    const auto& actions = controls::Actions();
    const controls::Action& action = actions[size_t(capturing_)];
    capturing_ = -1;
    const std::string name = controls::KeyName(vk);
    if (name.empty()) {
      note_ok_ = false;
      note_ = "Esa tecla no se puede usar: elegí otra";
      return;
    }
    std::vector<std::pair<std::string, std::string>> values{{action.cvar, name}};
    std::string moved;
    for (const controls::Action& other : actions) {
      if (&other == &action) continue;
      const std::string bind = rex::cvar::GetFlagByName(other.cvar);
      const std::string without = WithoutKey(bind, vk);
      if (without != bind) {
        values.emplace_back(other.cvar, without);
        moved = other.label;
      }
    }
    Set(values, std::string(action.label) + ": " + controls::KeysLabel(name) +
                    (moved.empty() ? std::string() : " (se la saqué a " + moved + ")"));
  }

  void StopCapture() {
    if (capturing_ < 0) return;
    capturing_ = -1;
    controls::CaptureNextKey(false);
  }

  void Apply(const Preset& p) {
    std::vector<std::pair<std::string, std::string>> values;
    for (const auto& [key, value] : p.values) values.emplace_back(key, value);
    Set(values, std::string("Calidad ") + p.label);
  }

  // The value shown: one kept for the next start (an option read only then), else the one in use.
  std::string Current(const char* key) const {
    if (auto it = pending_.find(key); it != pending_.end()) return it->second;
    return rex::cvar::GetFlagByName(key);
  }

  void DrawOption(const Option& o) {
    if (!rex::cvar::GetFlagInfo(o.key)) return;  // not in this build
    const std::string current = Current(o.key);
    const char* shown = current.c_str();
    for (const Choice& c : o.choices) {
      if (current == c.value) shown = c.label;
    }
    ImGui::PushID(o.key);
    ImGui::AlignTextToFramePadding();
    ImGui::Text("%s", o.label);
    ImGui::SameLine(250);
    ImGui::SetNextItemWidth(-1);
    if (ImGui::BeginCombo("##value", shown)) {
      for (const Choice& c : o.choices) {
        if (ImGui::Selectable(c.label, current == c.value) && current != c.value) Set({{o.key, c.value}}, o.label);
      }
      ImGui::EndCombo();
    }
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.65f, 0.65f, 0.7f, 1));
    ImGui::TextWrapped("%s", o.help);
    ImGui::PopStyleColor();
    ImGui::Spacing();
    ImGui::PopID();
  }

  // Applied now where the option allows it (else kept for the next start), and written to mk9.toml.
  void Set(const std::vector<std::pair<std::string, std::string>>& values, const std::string& what) {
    bool later = false;
    for (const auto& [key, value] : values) {
      if (rex::cvar::SetFlagByName(key, value)) {
        pending_.erase(key);
      } else {
        pending_[key] = value;
        later = true;
      }
      for (const Option& o : DisplayOptions()) later |= o.restart && key == o.key;
    }
    note_ok_ = SaveToConfig(values);
    note_ = !note_ok_ ? "No se pudo guardar en mk9.toml"
            : later   ? what + ": guardado (se aplica al volver a abrir el juego)"
                      : what + ": guardado";
    REXLOG_INFO("[options] {}{} ({})", what, later ? " (next start)" : "",
                note_ok_ ? "saved to mk9.toml" : "could not save mk9.toml");
  }

  bool visible_ = REXCVAR_GET(mk9_options_show), note_ok_ = true;
  int capturing_ = -1;  // the action waiting for its key (Controles), -1: none
  bool first_tabs_ = true;
  bool select_tab_ = false;            // LB / RB asked for tab_
  std::string tab_ = "Gráficos";       // the tab showing (as the last frame drew it)
  std::string note_;
  std::map<std::string, std::string> pending_;
};

}  // namespace

void Install() {
  static std::once_flag once;
  std::call_once(once, [] {
    static OptionsWindow window;
    renut::gpu::overlay::AddDialog(&window);
  });
}

}  // namespace mk9::options_ui
