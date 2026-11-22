// mk9-nx (2026-11-08): the game's options for the players (options_ui.h).

#include "options_ui.h"

#include "mk9_platform.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#if defined(_WIN32)
#include <windows.h>
#endif

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
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

#include "mk9_brand.h"
#include "mk9_controls.h"
#include "mk9_overlay_input.h"
#include "mk9_plugins.h"
#include "mk9_updater.h"
#include "mk9_vita_costumes.h"
#include "online/i18n.h"
#include "online/online_buttons.h"
#include "renut_engine/gpu/backend/backend.h"
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
REXCVAR_DEFINE_DOUBLE(mk9_options_test_cycle_s, 0.0, "MK9",
                      "Tests: every this many seconds the next choice of the next option of F2 (Gráficos, then Pantalla "
                      "e idioma, then the presets) is taken, as a player's click would, round after round (0: off)");

namespace mk9::options_ui {
namespace {

// mk9-lauta (2026-11-17): F2 in the player's language (i18n.h: the texts here are the Spanish keys)
using i18n::T;

// A runtime format (fmt) of a translated text.
template <typename... Args>
std::string Tf(const char* es, Args&&... args) {
  return fmt::format(fmt::runtime(T(es)), std::forward<Args>(args)...);
}

// A translated label with its Spanish id (ImGui keeps an item's state by its id: a language change keeps it).
std::string Tid(const char* es) { return std::string(T(es)) + "###" + es; }

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

// mk9-lauta (2026-11-11, PLAN-0.4 E1): the graphics card, when the PC has more than one (a notebook's integrated and
// dedicated ones): the names the API listed when the device was made (null until there are two).
const Option* GpuOption() {
  static std::vector<std::string> names;
  static Option option{"renut_gpu_adapter", "Placa de video", {},
                       "Automática usa la que tiene más memoria (en una notebook, la dedicada). Se aplica al volver a "
                       "abrir el juego.",
                       true};
  static bool built = false;
  if (!built) {
    std::vector<std::string> listed = renut::gpu::AdapterNames();
    if (listed.size() < 2) return nullptr;
    names = std::move(listed);  // not touched again: the choices point into it
    option.choices.push_back({"", "Automática"});
    for (const std::string& name : names) option.choices.push_back({name.c_str(), name.c_str()});
    built = true;
  }
  return &option;
}

const std::vector<Option>& DisplayOptions() {
  static const std::vector<Option> options = {
      {"fullscreen", "Pantalla",
       {{"true", "Pantalla completa"}, {"false", "Ventana"}},
       "F11 o Alt+Enter cambian entre pantalla completa y ventana en cualquier momento."},
#if defined(_WIN32)  // mk9-lauta (2026-11-13): Linux has Vulkan only
      {"renut_gpu_backend", "API gráfica",
       {{"d3d12", "DirectX 12"}, {"vulkan", "Vulkan"}, {"d3d11", "DirectX 11 (placas o drivers viejos)"}},
       "Si el juego anda mal o se ve raro con una, probá otra (algunas placas y drivers van mejor con Vulkan). DirectX "
       "11 es para placas o drivers viejos donde las otras no arrancan o se cierran. Se aplica al volver a abrir el "
       "juego.",
       true},
#endif
      {"mk9_show_fps", "Mostrar FPS",
       {{"false", "No"}, {"true", "Sí, arriba a la izquierda"}},
       "Los cuadros por segundo que se ven y, si se saltean, los que se dibujan."},
      {"mk9_ui_scale", "Tamaño de los menús F1 y F2",
       {{"auto", "Automático (según la pantalla)"}, {"1", "100 %"}, {"1.25", "125 %"}, {"1.5", "150 %"},
        {"2", "200 %"}},
#if defined(_WIN32)
       "El tamaño del texto de estas ventanas. Automático lo agranda en pantallas de más de 1080 líneas (1440p, 4K) y "
       "sigue la escala de Windows (125 %, 150 %)."},
#else
       "El tamaño del texto de estas ventanas. Automático lo agranda en pantallas de más de 1080 líneas (1440p, 4K) y "
       "sigue la escala del escritorio (125 %, 150 %)."},
#endif
      {"mk9_brand_menu", "Redes en el menú principal",
       {{"true", "Mostrar"}, {"false", "Ocultar"}},
       "MK9 Online by Lauta y los botones de Discord, WhatsApp, Instagram, Kick, YouTube y X, abajo a la derecha del "
       "menú principal."},
      // mk9-lauta (2026-11-17, issue #10): the boot logos (mk9_capture.cpp SkipLogosBlock)
      {"mk9_skip_logos", "Logos al iniciar",
       {{"false", "Mostrar"}, {"true", "Saltear"}},
       "Los videos de WB y NetherRealm del arranque (16 s). Al volver de una sesión online se saltean siempre. Se "
       "aplica al volver a abrir el juego.",
       true},
      // mk9-lauta (2026-11-17, issue #11): the cutscenes' pictures from the PC version's .bik (mk9_movie.cpp)
      {"mk9_movie_hq", "Videos de las cinemáticas",
       {{"true", "Los .bik si están (HD)"}, {"false", "Los del juego"}},
       "Si copiaste en la carpeta Movies del juego los .bik de la versión de PC (por ejemplo el mod de videos HQ "
       "sacados de PS3), las cinemáticas se ven con esos; el sonido sigue siendo el del juego."},
      // mk9-lauta (2026-11-17, issue #12): the cutscenes skipped (mk9_movie.cpp)
      {"mk9_skip_cutscenes", "Cinemáticas",
       {{"true", "Se saltean con START o A"}, {"false", "Como el original"}},
       "Las del Modo Historia: START o A las saltean, también la primera vez (el original no deja)."},
      {"user_language", "Idioma del juego",
       {{"5", "Español"}, {"1", "English"}, {"9", "Português"}, {"4", "Français"}, {"3", "Deutsch"},
        {"6", "Italiano"}},
       "Los textos del juego y de estas ventanas (estas cambian al momento; el juego, al volver a abrirlo). Si no "
       "elegís uno, es el idioma de tu PC. En español el Modo Historia está doblado.",
       true},
      // mk9-lauta (2026-11-22): the game's 5.1 folded to stereo whatever the device says (the SDK's audio_output): a
      // player heard no announcer and no intros, and low voices in the fights
      {"audio_output", "Sonido",
       {{"auto", "Automático (según la placa de sonido)"}, {"stereo", "Estéreo (parlantes o auriculares)"}},
       "Si no se escuchan el locutor ni las presentaciones de los personajes, o las voces suenan bajas, elegí "
       "Estéreo: tu PC dice tener parlantes 5.1 o 7.1 y el juego manda las voces al parlante del centro. Se aplica al "
       "volver a abrir el juego.",
       true},
  };
  return options;
}

// mk9-lauta: the game's button prompts (F2 > Controles).
const Option kButtonIconsOption = {
    "mk9_button_icons", "Íconos de los botones",
    {{"auto", "Automático (según el mando)"}, {"xbox", "Xbox"}, {"playstation", "PlayStation"}, {"switch", "Switch"},
     {"keyboard", "Teclado"}},
    "Los botones que muestra el juego en sus menús y en la lista de movimientos. Automático: los del primer mando "
    "conectado al abrir el juego, o las teclas si no hay ninguno. Se aplica al volver a abrir el juego.",
    true};

const std::vector<Option>& OnlineOptions() {
  static const std::vector<Option> options = {
      {"mk9_net_rollback_delay", "Retraso de entrada en las peleas",
       {{"-1", "Automático (recomendado)"}, {"0", "Ninguno"}, {"1", "1 cuadro"}, {"2", "2 cuadros"},
        {"3", "3 cuadros"}, {"4", "4 cuadros"}, {"5", "5 cuadros"}, {"6", "6 cuadros"}, {"7", "7 cuadros"}},
       "Cuadros que espera cada golpe tuyo antes de verse. Con menos retraso tus golpes salen antes pero el rollback "
       "corrige más (los personajes \"saltan\"); con más, al revés. Automático: 1 cuadro con ping bajo y más con ping "
       "alto, ajustándose durante toda la pelea. Cada jugador elige el suyo."},
      {"mk9_online_badge", "Cartel de la conexión en las peleas",
       {{"full", "Completo (rival, señal, ping y retraso)"}, {"bars", "Sólo la señal"}, {"off", "Oculto"}},
       "Lo que se ve abajo durante una pelea online. Los avisos (conexión inestable, esperando al rival) aparecen "
       "siempre; en los menús se ve completo."},
      // mk9-lauta (2026-11-11)
      {"mk9_plates_names", "Nombres en la pelea",
       {{"true", "Mostrar"}, {"false", "Ocultar"}},
       "El nombre y la bandera de cada jugador debajo de su barra de vida en las peleas online (y en la selección de "
       "personajes)."},
      {"mk9_plates_avatars", "Fotos de perfil en la pelea",
       {{"true", "Mostrar"}, {"false", "Ocultar"}},
       "La foto de cada jugador al lado de su nombre. Se cargan antes de la pelea: no cuestan nada mientras se juega."},
      // mk9-lauta (2026-11-17)
      {"mk9_plates_record", "Récord y racha en la pelea",
       {{"true", "Mostrar"}, {"false", "Ocultar"}},
       "Al lado del nombre de cada jugador, sus victorias y derrotas online y, si viene ganando, cuántas seguidas."},
      {"mk9_avatars_others", "Fotos de los otros jugadores",
       {{"true", "Mostrar"}, {"false", "No mostrar ninguna"}},
       "Las fotos de perfil de los demás, en la sala, en la pelea y en los perfiles. Las de los jugadores que "
       "silenciaste no se ven nunca."},
      {"mk9_net_record", "Grabar replays",
       {{"false", "No"}, {"true", "Sí, mis sesiones online"}},
       "Guarda las entradas de cada sesión online (unos 3 KB por segundo, en la carpeta logs; las últimas 10) para "
       "verlas en F1 > Replays. No usa internet ni se nota en el juego. Vale desde la próxima sesión."},
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

// mk9-lauta (2026-11-22, issue #18): a bind without any key the given binds have (the second keyboard player's keys
// leave the first's).
std::string WithoutKeysOf(std::string bind, const std::vector<std::string>& others) {
  for (const std::string& other : others) {
    std::istringstream in(other);
    for (std::string token; std::getline(in, token, ',');) {
      while (!token.empty() && token.front() == ' ') token.erase(token.begin());
      while (!token.empty() && token.back() == ' ') token.pop_back();
      if (token.empty() || token.find('+') != std::string::npos) continue;
      const int vk = int(rex::ui::ParseVirtualKey(token));
      if (vk > 0) bind = WithoutKey(bind, vk);
    }
  }
  return bind;
}

std::filesystem::path ConfigPath() {
  return mk9::platform::ExeDir() / "mk9.toml";
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
    // the screen's lines past 1080, or Windows' scale (E6, 2026-11-11: a 1080p notebook at 150 % had menus two thirds
    // the size of its own), whichever is larger
    const float height = ImGui::GetIO().DisplaySize.y;
    return std::clamp(std::max(height / 1080.0f, renut::gpu::overlay::DpiScale()), 1.0f, 2.5f);
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

// mk9-lauta (2026-11-16): OpenSupport asked for the window on its Apóyame tab (the main menu's Apoyar, Acerca de's).
std::atomic<bool> g_open_support{false};

// The tabs in order (LB / RB step through them).
constexpr const char* kTabs[] = {"Gráficos", "Controles", "Pantalla e idioma", "Online", "Acerca de", "Apóyame"};
constexpr int kTabCount = int(std::size(kTabs));

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
    TestCycle();
    if (ImGui::IsKeyPressed(ImGuiKey_F2, false) || overlay_input::ToggleOptionsPressed()) {
      visible_ = !visible_;
      if (visible_) ImGui::SetNextWindowFocus();  // over the online screens if they are open too
      if (!visible_) StopCapture();
    }
    if (g_open_support.exchange(false)) {
      if (!visible_) ImGui::SetNextWindowFocus();
      visible_ = true;
      tab_ = "Apóyame";
      select_tab_ = true;
    }
    // Full screen or a window, any time (on the window's thread, where the SDK changes the window)
    const ImGuiIO& keys = ImGui::GetIO();
    if (ImGui::IsKeyPressed(ImGuiKey_F11, false) || (keys.KeyAlt && ImGui::IsKeyPressed(ImGuiKey_Enter, false))) {
      const bool full = rex::cvar::GetFlagByName("fullscreen") == "true";
      Set({{"fullscreen", full ? "false" : "true"}}, T(full ? "Ventana" : "Pantalla completa"));
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
    // the title in the titles' font (F1), the rest in the text's
    ImGui::PushFont(renut::gpu::overlay::TitleFont(), 0.0f);
    const bool open = ImGui::Begin((std::string(T("Opciones  (F2 para mostrar u ocultar)")) + "###mk9_options").c_str(),
                                   &visible_, ImGuiWindowFlags_NoCollapse);
    ImGui::PopFont();
    if (!open) {
      ImGui::End();
      return;
    }
    overlay_input::PageScroll();  // LT / RT: a page of the window (F2 of the plan)
    // the controller: B closes, LB / RB change tabs
    if (overlay_input::ClosePressed()) {
      visible_ = false;
      StopCapture();
    }
    if (const int step = overlay_input::TabStep(); step && capturing_ < 0) {
      int current = 0;
      for (int i = 0; i < kTabCount; ++i) {
        if (tab_ == kTabs[i]) current = i;
      }
      tab_ = kTabs[(current + step + kTabCount) % kTabCount];
      select_tab_ = true;
    }
    if (ImGui::BeginTabBar("options_tabs")) {
      if (ImGui::BeginTabItem(Tid("Gráficos").c_str(), nullptr, TabFlags("Gráficos"))) {
        if (!select_tab_) tab_ = "Gráficos";
        DrawPresets();
        ImGui::Separator();
        for (const Option& o : GraphicsOptions()) DrawOption(o);
        ImGui::EndTabItem();
      }
      if (ImGui::BeginTabItem(Tid("Controles").c_str(), nullptr, TabFlags("Controles"))) {
        if (!select_tab_) tab_ = "Controles";
        DrawControls();
        ImGui::EndTabItem();
      } else {
        StopCapture();
      }
      if (ImGui::BeginTabItem(Tid("Pantalla e idioma").c_str(), nullptr, TabFlags("Pantalla e idioma"))) {
        if (!select_tab_) tab_ = "Pantalla e idioma";
        for (const Option& o : DisplayOptions()) DrawOption(o);
        if (const Option* gpu = GpuOption()) DrawOption(*gpu);
        DrawMonitor();
        DrawWindowSize();
        ImGui::EndTabItem();
      }
      if (ImGui::BeginTabItem(Tid("Online").c_str(), nullptr, TabFlags("Online"))) {
        if (!select_tab_) tab_ = "Online";
        for (const Option& o : OnlineOptions()) DrawOption(o);
        ImGui::EndTabItem();
      }
      if (ImGui::BeginTabItem(Tid("Acerca de").c_str(), nullptr, TabFlags("Acerca de"))) {
        if (!select_tab_) tab_ = "Acerca de";
        DrawAbout();
        ImGui::EndTabItem();
      }
      // mk9-lauta (2026-11-16): the ways to support the project, and the networks for whoever cannot donate
      if (ImGui::BeginTabItem(Tid("Apóyame").c_str(), nullptr, TabFlags("Apóyame"))) {
        if (!select_tab_) tab_ = "Apóyame";
        brand::SupportPanel(ImGui::GetContentRegionAvail().x, UiScale());
        ImGui::EndTabItem();
      }
      ImGui::EndTabBar();
      first_tabs_ = false;
      select_tab_ = false;
    }
    ImGui::Separator();
    if (!note_.empty()) ImGui::TextColored(note_ok_ ? ImVec4(0.45f, 0.9f, 0.5f, 1) : ImVec4(1, 0.45f, 0.4f, 1), "%s",
                                           note_.c_str());
    ImGui::TextColored(ImVec4(0.65f, 0.65f, 0.7f, 1), "%s",
                       T("Los cambios se aplican al momento y quedan guardados en mk9.toml."));
    ImGui::End();
  }

 private:
  // mk9-lauta (2026-11-15): tests (mk9_options_test_cycle_s): F2's choices one after another, each through Set as its
  // click goes: every choice of every graphics and display option (not the API nor the language, which only write
  // mk9.toml for the next start), then each quality preset, round after round. The Linux players' "it crashes when I
  // change a setting" is this, unattended.
  void TestCycle() {
    const double every = REXCVAR_GET(mk9_options_test_cycle_s);
    if (!(every > 0.0)) return;
    const auto now = std::chrono::steady_clock::now();
    const auto step = std::chrono::milliseconds(int64_t(every * 1000.0));
    if (test_next_ == std::chrono::steady_clock::time_point{}) test_next_ = now + step;
    if (now < test_next_) return;
    test_next_ = now + step;
    std::vector<const Option*> options;
    for (const Option& o : GraphicsOptions()) options.push_back(&o);
    for (const Option& o : DisplayOptions()) {
      if (std::string_view(o.key) != "renut_gpu_backend" && std::string_view(o.key) != "user_language") {
        options.push_back(&o);
      }
    }
    const size_t presets = Presets().size();
    const size_t total = options.size() + presets;
    for (size_t tries = 0; tries < total; ++tries) {
      const size_t at = test_option_ % total;
      if (at >= options.size()) {  // a preset's button
        const Preset& p = Presets()[at - options.size()];
        ++test_option_;
        test_choice_ = 0;
        REXLOG_INFO("[options] test {}: the preset \"{}\"", ++test_steps_, p.label);
        Apply(p);
        return;
      }
      const Option& o = *options[at];
      if (!rex::cvar::GetFlagInfo(o.key) || test_choice_ >= o.choices.size()) {
        ++test_option_;
        test_choice_ = 0;
        continue;
      }
      const Choice& c = o.choices[test_choice_++];
      if (test_choice_ >= o.choices.size()) {
        ++test_option_;
        test_choice_ = 0;
      }
      REXLOG_INFO("[options] test {}: {} = {}", ++test_steps_, o.key, c.value);
      Set({{o.key, c.value}}, o.label);
      return;
    }
  }

  // The monitor the game opens on (the SDK's monitor: 1-based in SDL's order, 0 the system's default).
  void DrawMonitor() {
    if (!rex::cvar::GetFlagInfo("monitor")) return;
    int count = 0;
    SDL_DisplayID* displays = SDL_GetDisplays(&count);
    std::vector<std::pair<std::string, std::string>> choices = {{"0", T("Automático (el principal)")}};
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
    DrawChoices("monitor", T("Monitor"), choices,
                T("En qué monitor se abre el juego (en pantalla completa, ese). Se aplica al volver a abrir el juego."),
                {{"monitor", ""}});
  }

  // The window's size at start (the SDK's window_width / window_height, 0 = 1280x720); dragging its border works too.
  void DrawWindowSize() {
    if (!rex::cvar::GetFlagInfo("window_width")) return;
    const std::vector<std::pair<std::string, std::string>> sizes = {
        {"0x0", T("Automático (1280x720)")}, {"1280x720", "1280x720"}, {"1600x900", "1600x900"},
        {"1920x1080", "1920x1080"},          {"2560x1440", "2560x1440"},
    };
    const std::string current = Current("window_width") + "x" + Current("window_height");
    ImGui::PushID("window_size");
    ImGui::AlignTextToFramePadding();
    ImGui::Text("%s", T("Tamaño de la ventana"));
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
          Set({{"window_width", value.substr(0, x)}, {"window_height", value.substr(x + 1)}}, T("Tamaño de la ventana"));
        }
      }
      ImGui::EndCombo();
    }
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.65f, 0.65f, 0.7f, 1));
    ImGui::TextWrapped("%s", T("El tamaño con el que abre en ventana (también se puede arrastrar el borde). Se aplica "
                               "al volver a abrir el juego."));
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

  // mk9-lauta: the version, the updater, the logs folder and the project's page; mk9-lauta (2026-11-11): the logo and
  // Lauta's networks.
  void DrawAbout() {
    const updater::Status st = updater::GetStatus();
    const float scale = UiScale();
    brand::Logo(std::min(ImGui::GetContentRegionAvail().x * 0.5f, 330.0f * scale));
    const std::string version = "MK9 Online by Lauta  v" + st.current;
    ImGui::PushFont(renut::gpu::overlay::StrongFont(), 0.0f);
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() +
                         std::max(0.0f, (ImGui::GetContentRegionAvail().x - ImGui::CalcTextSize(version.c_str()).x) * 0.5f));
    ImGui::TextColored(ImVec4(1.0f, 0.80f, 0.30f, 1.0f), "%s", version.c_str());
    ImGui::PopFont();
    const char* about = T("Mortal Kombat Komplete Edition para PC, con online con rollback. Hecho por y para la comunidad.");
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() +
                         std::max(0.0f, (ImGui::GetContentRegionAvail().x - ImGui::CalcTextSize(about).x) * 0.5f));
    ImGui::TextColored(ImVec4(0.65f, 0.65f, 0.7f, 1), "%s", about);
    // mk9-lauta (2026-11-16, issue #8): third-party code running in the game, always in sight
    if (const std::vector<std::string> plugins = plugins::Loaded(); !plugins.empty()) {
      std::string list;
      for (const std::string& name : plugins) list += (list.empty() ? "" : ", ") + name;
      ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.35f, 1),
                         T("Plugins de código cargados (mk9_plugins en mk9.toml): %s"), list.c_str());
    }
    ImGui::Separator();
    bool check = rex::cvar::GetFlagByName("mk9_update_check") != "false";
    if (ImGui::Checkbox(Tid("Buscar una versión nueva al iniciar").c_str(), &check)) {
      Set({{"mk9_update_check", check ? "true" : "false"}},
          T(check ? "Buscar versiones nuevas: sí" : "Buscar versiones nuevas: no"));
    }
    const bool busy = st.phase == updater::Phase::kChecking || st.phase == updater::Phase::kDownloading;
    if (busy) ImGui::BeginDisabled();
    if (ImGui::Button(Tid("Buscar ahora").c_str())) updater::CheckNow();
    if (busy) ImGui::EndDisabled();
    if (!st.message.empty()) {
      ImGui::SameLine();
      ImGui::TextUnformatted(st.message.c_str());
    }
    if (st.phase == updater::Phase::kAvailable) {
      if (ImGui::Button(Tid("Actualizar ahora").c_str())) updater::Download();
    } else if (st.phase == updater::Phase::kDownloading) {
      ImGui::ProgressBar(float(st.progress), ImVec2(-1, 0));
    } else if (st.phase == updater::Phase::kReady) {
      if (ImGui::Button(Tid("Reiniciar ahora").c_str())) updater::InstallAndRestart();
    }
    ImGui::Separator();
    ImGui::TextWrapped("%s", T("Si algo falla, la carpeta logs tiene lo que pasó: mandá el último mk9_*.log (y el "
                               "crash_*.dmp de la misma hora si el juego se cerró de golpe)."));
    if (ImGui::Button(Tid("Abrir la carpeta de logs").c_str())) updater::Open(updater::LogsFolder());
    ImGui::SameLine();
    if (ImGui::Button(Tid("Reportar un problema").c_str())) updater::Open("https://github.com/machinebreak/mk9-lauta/issues");
    ImGui::SameLine();
    if (ImGui::Button(Tid("Página del proyecto").c_str())) updater::Open("https://github.com/machinebreak/mk9-lauta");
    ImGui::Separator();
    ImGui::PushFont(renut::gpu::overlay::StrongFont(), 0.0f);
    ImGui::Text("%s", T("Comunidad y redes"));
    ImGui::PopFont();
    brand::SocialButtons(ImGui::GetContentRegionAvail().x, 3, std::round(38.0f * scale));
    // mk9-lauta (2026-11-16): to the Apóyame tab (next frame: this one is inside the tab bar)
    ImGui::Spacing();
    if (brand::SocialButton(brand::Social::kHeart, ImVec2(ImGui::GetContentRegionAvail().x, std::round(38.0f * scale)),
                            true, T("Apoyar el proyecto"))) {
      g_open_support.store(true);
    }
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
        ImGui::Text(T("%u FPS (juego a %u)"), shown, game);
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
    // (mk9-lauta, 2026-11-16: "Apoyame" without the accent names Apóyame too: a command line's accent may not arrive)
    const std::string& wanted = REXCVAR_GET(mk9_options_tab);
    const bool named = wanted == tab || (wanted == "Apoyame" && std::string_view(tab) == "Apóyame");
    return first_tabs_ && named ? ImGuiTabItemFlags_SetSelected : 0;
  }

  void DrawPresets() {
    ImGui::AlignTextToFramePadding();
    ImGui::Text("%s", T("Calidad:"));
    for (const Preset& p : Presets()) {
      bool active = true;  // every option as the preset has it
      for (const auto& [key, value] : p.values) active &= Current(key) == value;
      ImGui::SameLine();
      if (active) ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.75f, 0.45f, 0.10f, 1.0f));
      if (ImGui::Button(Tid(p.label).c_str())) Apply(p);
      if (active) ImGui::PopStyleColor();
      if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", T(p.help));
    }
  }

  void DrawControls() {
    const ImVec4 grey(0.65f, 0.65f, 0.7f, 1);
    bool keyboard = rex::cvar::GetFlagByName("mnk_mode") == "true";
    if (ImGui::Checkbox(Tid("Jugar con el teclado").c_str(), &keyboard)) {
      Set({{"mnk_mode", keyboard ? "true" : "false"}}, T(keyboard ? "Teclado activado" : "Teclado desactivado"));
    }
    if (keyboard) {
      ImGui::SameLine(0, 30);
      ImGui::AlignTextToFramePadding();
      ImGui::Text("%s", T("El teclado maneja al"));
      ImGui::SameLine();
      const std::string player = rex::cvar::GetFlagByName("mk9_keyboard_player");
      ImGui::SetNextItemWidth(150);
      if (ImGui::BeginCombo("##keyboard_player", T(player == "2" ? "Jugador 2" : "Jugador 1"))) {
        if (ImGui::Selectable(Tid("Jugador 1").c_str(), player != "2") && player == "2")
          Set({{"mk9_keyboard_player", "1"}}, T("El teclado maneja al Jugador 1"));
        if (ImGui::Selectable(Tid("Jugador 2").c_str(), player == "2") && player != "2")
          Set({{"mk9_keyboard_player", "2"}}, T("El teclado maneja al Jugador 2"));
        ImGui::EndCombo();
      }
      ImGui::PushStyleColor(ImGuiCol_Text, grey);
      ImGui::TextWrapped("%s", T("Jugador 2: para un Versus en esta PC con el teclado y un mando (el mando es el "
                                 "Jugador 1). Online manejás a tu luchador con el teclado y con cualquier mando "
                                 "conectado."));
      ImGui::PopStyleColor();
      // mk9-lauta (2026-11-22, issue #18): two players on one keyboard, each with keys of their own
      bool second = rex::cvar::GetFlagByName("mnk_second_player") == "true";
      if (ImGui::Checkbox(Tid("Dos jugadores en el mismo teclado").c_str(), &second)) SetSecondPlayer(second);
      ImGui::Spacing();
      if (second) {
        ImGui::Text("%s", Tf("Jugador {}", controls::KeyboardPlayer()).c_str());
      }
      DrawKeys(0);
      if (second) {
        ImGui::Spacing();
        ImGui::Text("%s", Tf("Jugador {} (en el mismo teclado)", controls::SecondKeyboardPlayer()).c_str());
        DrawKeys(kSecondBase);
        ImGui::PushStyleColor(ImGuiCol_Text, grey);
        ImGui::TextWrapped("%s", T("Una tecla es de un solo jugador: si la elegís para uno, se la saco al otro. Algunos "
                                   "teclados no registran muchas teclas apretadas a la vez."));
        ImGui::PopStyleColor();
      }
    }
    ImGui::Separator();
    DrawOption(kButtonIconsOption);
    bool rumble = rex::cvar::GetFlagByName("input_rumble") == "true";
    if (ImGui::Checkbox(Tid("Vibración del mando").c_str(), &rumble)) {
      Set({{"input_rumble", rumble ? "true" : "false"}}, T(rumble ? "Vibración activada" : "Vibración desactivada"));
    }
    // mk9-lauta (2026-11-11, H2): the inputs on screen (mk9_input_display.h)
    bool inputs = rex::cvar::GetFlagByName("mk9_input_display") == "true";
    if (ImGui::Checkbox(Tid("Mostrar las entradas en la pelea (para practicar combos)").c_str(), &inputs)) {
      Set({{"mk9_input_display", inputs ? "true" : "false"}},
          T(inputs ? "Entradas en pantalla activadas" : "Entradas en pantalla desactivadas"));
    }
    // mk9-lauta (2026-11-17): Practice's frame data (mk9_frame_data.h)
    bool frames = rex::cvar::GetFlagByName("mk9_frame_data") == "true";
    if (ImGui::Checkbox(Tid("Mostrar la frame data en Práctica (arranque y ventaja de cada golpe)").c_str(), &frames)) {
      Set({{"mk9_frame_data", frames ? "true" : "false"}}, T(frames ? "Frame data activada" : "Frame data desactivada"));
    }
    // mk9-lauta (2026-11-18): Practice's recording of the opponent (mk9_practice_record.h)
    bool record = rex::cvar::GetFlagByName("mk9_practice_record") == "true";
    if (ImGui::Checkbox(Tid("Grabar al rival en Práctica (F5 grabar, F6 reproducir, F7 en bucle)").c_str(), &record)) {
      Set({{"mk9_practice_record", record ? "true" : "false"}},
          T(record ? "Grabación del rival activada" : "Grabación del rival desactivada"));
    }
    // mk9-lauta (2026-11-21): the PS Vita costumes (mk9_vita_costumes.h), at the next start
    if (rex::cvar::GetFlagInfo("mk9_vita_costumes")) {
      bool vita = Current("mk9_vita_costumes") != "false";
      if (ImGui::Checkbox(Tid("Trajes extra de la versión de PS Vita (con sus Babalities)").c_str(), &vita)) {
        Set({{"mk9_vita_costumes", vita ? "true" : "false"}},
            T(vita ? "Trajes de PS Vita activados" : "Trajes de PS Vita desactivados"));
      }
      // what this start has (the switch applies at the next one)
      if (rex::cvar::GetFlagByName("mk9_vita_costumes") != "false") {
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.65f, 0.65f, 0.7f, 1));
        if (vita_costumes::Installed()) {
          ImGui::TextWrapped("%s", Tf("{} trajes listos: aparecen en la selección de personaje (los Babalities "
                                      "también).",
                                      vita_costumes::Summary())
                                       .c_str());
        } else {
          ImGui::TextWrapped("%s", T("No se pudieron armar desde los archivos del juego: la carpeta logs dice por "
                                     "qué (F2 > Acerca de)."));
        }
        // the pack of the ones made of the Vita's own data, when its download at this start failed
        if (vita_costumes::PackMissing()) {
          ImGui::TextWrapped("%s", T("Faltan los trajes hechos con datos de la PS Vita (no se pudieron descargar): "
                                     "se descargan la próxima vez que abras el juego."));
        }
        ImGui::PopStyleColor();
      }
    }
    ImGui::Text("%s", T("Mandos conectados:"));
    bool any_pad = false;
    for (const controls::Device& d : controls::Devices()) {
      if (d.keyboard) continue;
      any_pad = true;
      if (d.player > 0) ImGui::BulletText(T("Jugador %d: %s"), d.player, d.name.c_str());
      else ImGui::BulletText(T("%s (sin jugador: ya hay cuatro)"), d.name.c_str());
    }
    ImGui::PushStyleColor(ImGuiCol_Text, grey);
    if (!any_pad) {
      ImGui::TextWrapped("%s", T("Ninguno. Los de Xbox, PlayStation y Switch Pro andan apenas se conectan; el primero "
                                 "que se conecta es el Jugador 1."));
    }
    ImGui::TextWrapped("%s", T("Qué golpe hace cada botón del mando se cambia en el menú del juego: OPCIONES > MANDO "
                               "(para las peleas online, acá abajo). El teclado lo sigue: cada tecla aprieta el botón del "
                               "mando que dice la tabla."));
    ImGui::PopStyleColor();
    DrawOnlineButtons();
  }

  // mk9-lauta (2026-11-16): the buttons of the online fights (online_buttons.h): a session's game always plays with the
  // game's own, so the player's go here and this machine's pad is turned into the game's before it is sent.
  void DrawOnlineButtons() {
    const ImVec4 grey(0.65f, 0.65f, 0.7f, 1);
    ImGui::Separator();
    ImGui::PushFont(renut::gpu::overlay::StrongFont(), 0.0f);
    ImGui::Text("%s", T("Botones en el online"));
    ImGui::PopFont();
    ImGui::PushStyleColor(ImGuiCol_Text, grey);
    ImGui::TextWrapped("%s", T("En las peleas online el juego usa siempre los botones de fábrica (cada partida online "
                               "arranca con un perfil nuevo para que las dos PC jueguen igual), aunque los hayas cambiado "
                               "en OPCIONES > MANDO. Elegí acá qué botón hace cada golpe en tus peleas online: tu rival no "
                               "se entera y no cambia nada para él."));
    ImGui::PopStyleColor();
    online_buttons::Layout layout = online_buttons::Current();
    bool changed = false;
    if (ImGui::BeginTable("online_buttons", 2, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH |
                                                   ImGuiTableFlags_SizingStretchProp)) {
      ImGui::TableSetupColumn(T("Golpe"), ImGuiTableColumnFlags_WidthStretch, 2.0f);
      ImGui::TableSetupColumn(T("Botón"), ImGuiTableColumnFlags_WidthStretch, 1.2f);
      ImGui::TableHeadersRow();
      for (int a = 0; a < online_buttons::kActionCount; ++a) {
        ImGui::PushID(a);
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted(T(online_buttons::ActionLabel(a)));
        ImGui::TableNextColumn();
        ImGui::SetNextItemWidth(-1);
        const online_buttons::Button current = layout[size_t(a)];
        if (ImGui::BeginCombo("##button", T(online_buttons::ButtonLabel(current)))) {
          for (int b = 0; b < online_buttons::kButtonCount; ++b) {
            const auto button = online_buttons::Button(b);
            if (ImGui::Selectable(Tid(online_buttons::ButtonLabel(button)).c_str(), button == current) &&
                button != current) {
              // the move that had it takes this one's, as OPCIONES > MANDO does (no button does two moves)
              for (auto& other : layout) {
                if (other == button) other = current;
              }
              layout[size_t(a)] = button;
              changed = true;
            }
          }
          ImGui::EndCombo();
        }
        ImGui::PopID();
      }
      ImGui::EndTable();
    }
    if (layout != online_buttons::kGameLayout && ImGui::Button(Tid("Volver a los botones de fábrica").c_str())) {
      layout = online_buttons::kGameLayout;
      changed = true;
    }
    if (changed) {
      online_buttons::SetCurrent(layout);
      const std::string text = online_buttons::Encode(layout);
      Set({{"mk9_net_buttons", text}},
          text.empty() ? std::string(T("Botones en el online: los de fábrica"))
                       : Tf("Botones en el online: {}", text));
    }
  }

  // mk9-lauta (2026-11-22, issue #18): the actions of the table at base 0 (the keyboard's) or kSecondBase (the second
  // keyboard player's); capturing_ is base + the action's index.
  static constexpr int kSecondBase = 100;
  static const std::vector<controls::Action>& TableActions(int base) {
    return base == kSecondBase ? controls::SecondPlayerActions() : controls::Actions();
  }

  // The second keyboard player on or off: on, the first player's keys lose the ones the second's use (the arrows)
  void SetSecondPlayer(bool on) {
    StopCapture();
    std::vector<std::pair<std::string, std::string>> values{{"mnk_second_player", on ? "true" : "false"}};
    if (on) {
      std::vector<std::string> theirs;
      for (const controls::Action& a : controls::SecondPlayerActions()) {
        theirs.push_back(rex::cvar::GetFlagByName(a.cvar));
      }
      for (const controls::Action& a : controls::Actions()) {
        const std::string bind = rex::cvar::GetFlagByName(a.cvar);
        const std::string without = WithoutKeysOf(bind, theirs);
        if (without != bind) values.emplace_back(a.cvar, without);
      }
    }
    Set(values, T(on ? "Dos jugadores en el teclado" : "Un jugador en el teclado"));
  }

  void DrawKeys(int base = 0) {
    const auto& actions = TableActions(base);
    if (ImGui::BeginTable(base ? "keys2" : "keys", 4, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH |
                                                       ImGuiTableFlags_SizingStretchProp)) {
      ImGui::TableSetupColumn(T("Acción"), ImGuiTableColumnFlags_WidthStretch, 2.3f);
      ImGui::TableSetupColumn(T("Mando"), ImGuiTableColumnFlags_WidthStretch, 1.0f);
      ImGui::TableSetupColumn(T("Teclas"), ImGuiTableColumnFlags_WidthStretch, 1.8f);
      ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed, 110.0f);
      ImGui::TableHeadersRow();
      for (int i = 0; i < int(actions.size()); ++i) {
        const controls::Action& a = actions[size_t(i)];
        const int id = base + i;
        ImGui::PushID(id);
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted(T(a.label));
        ImGui::TableNextColumn();
        ImGui::TextColored(ImVec4(0.65f, 0.65f, 0.7f, 1), "%s", T(a.button));
        ImGui::TableNextColumn();
        if (capturing_ == id) {
          ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.3f, 1), "%s", T("Apretá una tecla..."));
        } else {
          const std::string keys = controls::KeysLabel(rex::cvar::GetFlagByName(a.cvar));
          ImGui::TextUnformatted(keys.empty() ? T("(ninguna)") : keys.c_str());
        }
        ImGui::TableNextColumn();
        if (capturing_ == id) {
          if (ImGui::Button(Tid("Cancelar").c_str(), ImVec2(-1, 0))) StopCapture();
        } else if (ImGui::Button(Tid("Cambiar").c_str(), ImVec2(-1, 0))) {
          capturing_ = id;
          controls::CaptureNextKey(true);
        }
        ImGui::PopID();
      }
      ImGui::EndTable();
    }
    if (base == 0 && ImGui::Button(Tid("Volver a las teclas de fábrica").c_str())) {
      StopCapture();
      std::vector<std::pair<std::string, std::string>> values;
      // with a second keyboard player, both to their own, the first's without the second's
      const bool second = rex::cvar::GetFlagByName("mnk_second_player") == "true";
      std::vector<std::string> theirs;
      if (second) {
        for (const controls::Action& a : controls::SecondPlayerActions()) {
          const std::string bind = BuiltInDefault(a.cvar);
          values.emplace_back(a.cvar, bind);
          theirs.push_back(bind);
        }
      }
      for (const controls::Action& a : actions) values.emplace_back(a.cvar, WithoutKeysOf(BuiltInDefault(a.cvar), theirs));
      Set(values, T("Teclas de fábrica"));
    }
  }

  // The key pressed for the action waiting for one: it is that action's only key now, and no other action's.
  void TakeKey() {
    const int vk = controls::TakeCapturedKey();
    if (vk < 0) return;
    const int base = capturing_ >= kSecondBase ? kSecondBase : 0;
    const auto& actions = TableActions(base);
    const controls::Action& action = actions[size_t(capturing_ - base)];
    capturing_ = -1;
    const std::string name = controls::KeyName(vk);
    if (name.empty()) {
      note_ok_ = false;
      note_ = T("Esa tecla no se puede usar: elegí otra");
      return;
    }
    std::vector<std::pair<std::string, std::string>> values{{action.cvar, name}};
    std::string moved;
    // mk9-lauta (2026-11-22, issue #18): from the other keyboard player's actions too, when there is one
    const bool second = rex::cvar::GetFlagByName("mnk_second_player") == "true";
    for (const int table : {0, kSecondBase}) {
      if (table == kSecondBase && !second) continue;
      for (const controls::Action& other : TableActions(table)) {
        if (&other == &action) continue;
        const std::string bind = rex::cvar::GetFlagByName(other.cvar);
        const std::string without = WithoutKey(bind, vk);
        if (without != bind) {
          values.emplace_back(other.cvar, without);
          moved = table == base ? std::string(T(other.label))
                                : Tf("{} del otro jugador", T(other.label));
        }
      }
    }
    Set(values, std::string(T(action.label)) + ": " + controls::KeysLabel(name) +
                    (moved.empty() ? std::string() : Tf(" (se la saqué a {})", moved)));
  }

  void StopCapture() {
    if (capturing_ < 0) return;
    capturing_ = -1;
    controls::CaptureNextKey(false);
  }

  void Apply(const Preset& p) {
    std::vector<std::pair<std::string, std::string>> values;
    for (const auto& [key, value] : p.values) values.emplace_back(key, value);
    Set(values, Tf("Calidad {}", T(p.label)));
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
      if (current == c.value) shown = T(c.label);
    }
    ImGui::PushID(o.key);
    ImGui::AlignTextToFramePadding();
    ImGui::Text("%s", T(o.label));
    ImGui::SameLine(250);
    ImGui::SetNextItemWidth(-1);
    if (ImGui::BeginCombo("##value", shown)) {
      for (const Choice& c : o.choices) {
        if (ImGui::Selectable(Tid(c.label).c_str(), current == c.value) && current != c.value) {
          Set({{o.key, c.value}}, T(o.label));
        }
      }
      ImGui::EndCombo();
    }
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.65f, 0.65f, 0.7f, 1));
    ImGui::TextWrapped("%s", T(o.help));
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
      later |= key == "renut_gpu_adapter";
      later |= key == kButtonIconsOption.key;
      // mk9-lauta (2026-11-17): F1, F2 and the other windows change language at once (the game, at its next start)
      if (key == "user_language") i18n::FollowGame(value.c_str());
    }
    note_ok_ = SaveToConfig(values);
    note_ = !note_ok_ ? std::string(T("No se pudo guardar en mk9.toml"))
            : later   ? Tf("{}: guardado (se aplica al volver a abrir el juego)", what)
                      : Tf("{}: guardado", what);
    REXLOG_INFO("[options] {}{} ({})", what, later ? " (next start)" : "",
                note_ok_ ? "saved to mk9.toml" : "could not save mk9.toml");
  }

  bool visible_ = REXCVAR_GET(mk9_options_show), note_ok_ = true;
  // mk9_options_test_cycle_s: the next test step's time, option and choice
  std::chrono::steady_clock::time_point test_next_{};
  size_t test_option_ = 0, test_choice_ = 0, test_steps_ = 0;
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

void OpenSupport() { g_open_support.store(true); }

}  // namespace mk9::options_ui
