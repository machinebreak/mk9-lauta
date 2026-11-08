// mk9-nx (2026-11-08): the game's options for the players (options_ui.h).

#include "options_ui.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include "imgui.h"

#include <rex/cvar.h>
#include <rex/logging.h>

#include "renut_engine/gpu/overlay/overlay_ui.h"

REXCVAR_DEFINE_BOOL(mk9_options_show, false, "MK9", "The options window (F2) opens at start");
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
      {"renut_vsync", "Sincronización vertical",
       {{"true", "Activada (sin cortes de imagen)"}, {"false", "Desactivada"}},
       "Activada, la imagen no se corta. Desactivada puede bajar un poco la latencia en monitores de más de 60 Hz."},
      {"sa2_renut_msaa", "Antialiasing (MSAA)",
       {{"0", "Apagado (el del juego)"}, {"2", "2x"}, {"4", "4x"}, {"8", "8x"}},
       "Bordes más suaves; cada paso cuesta más placa de video. Se aplica al volver a abrir el juego."},
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
      {"Baja", "Para PCs con poca placa de video: 480p, sombras y efectos como en la consola.",
       {{"renut_resolution", "480p"}, {"renut_shadow_resolution", "1x"}, {"renut_ambient_occlusion", "Half"},
        {"renut_anisotropic_filtering", "Game"}, {"renut_post_process_precision", "Game"}}},
      {"Consola", "Como en la Xbox 360: 720p.",
       {{"renut_resolution", "720p"}, {"renut_shadow_resolution", "Resolution"}, {"renut_ambient_occlusion", "Half"},
        {"renut_anisotropic_filtering", "Game"}, {"renut_post_process_precision", "Game"}}},
      {"Alta", "1080p, sombras nítidas, texturas filtradas.",
       {{"renut_resolution", "1080p"}, {"renut_shadow_resolution", "2x"}, {"renut_ambient_occlusion", "Full"},
        {"renut_anisotropic_filtering", "8x"}, {"renut_post_process_precision", "High"}}},
      {"Ultra", "1440p y todo al máximo: para placas de video potentes.",
       {{"renut_resolution", "1440p"}, {"renut_shadow_resolution", "4x"}, {"renut_ambient_occlusion", "Full"},
        {"renut_anisotropic_filtering", "16x"}, {"renut_post_process_precision", "High"}}},
  };
  return presets;
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
bool SaveValues(const std::vector<std::pair<std::string, std::string>>& values) {
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

class OptionsWindow : public renut::gpu::overlay::Dialog {
 public:
  void Draw() override {
    if (static bool tested = false; !tested) {
      tested = true;
      for (const Preset& p : Presets()) {
        if (REXCVAR_GET(mk9_options_test_preset) == p.label) Apply(p);
      }
    }
    if (ImGui::IsKeyPressed(ImGuiKey_F2, false)) {
      visible_ = !visible_;
      if (visible_) ImGui::SetNextWindowFocus();  // over the online screens if they are open too
    }
    if (!visible_) return;
    const ImGuiIO& io = ImGui::GetIO();
    ImGui::SetNextWindowSize(ImVec2(640, 600), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f), ImGuiCond_FirstUseEver,
                            ImVec2(0.5f, 0.5f));
    if (!ImGui::Begin("Opciones  (F2 para mostrar u ocultar)", &visible_, ImGuiWindowFlags_NoCollapse)) {
      ImGui::End();
      return;
    }
    if (ImGui::BeginTabBar("options_tabs")) {
      if (ImGui::BeginTabItem("Gráficos")) {
        DrawPresets();
        ImGui::Separator();
        for (const Option& o : GraphicsOptions()) DrawOption(o);
        ImGui::EndTabItem();
      }
      if (ImGui::BeginTabItem("Online")) {
        for (const Option& o : OnlineOptions()) DrawOption(o);
        ImGui::EndTabItem();
      }
      ImGui::EndTabBar();
    }
    ImGui::Separator();
    if (!note_.empty()) ImGui::TextColored(note_ok_ ? ImVec4(0.45f, 0.9f, 0.5f, 1) : ImVec4(1, 0.45f, 0.4f, 1), "%s",
                                           note_.c_str());
    ImGui::TextColored(ImVec4(0.65f, 0.65f, 0.7f, 1), "Los cambios se aplican al momento y quedan guardados en mk9.toml.");
    ImGui::End();
  }

 private:
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
    }
    note_ok_ = SaveValues(values);
    note_ = !note_ok_ ? "No se pudo guardar en mk9.toml"
            : later   ? what + ": guardado (se aplica al volver a abrir el juego)"
                      : what + ": guardado";
    REXLOG_INFO("[options] {}{} ({})", what, later ? " (next start)" : "",
                note_ok_ ? "saved to mk9.toml" : "could not save mk9.toml");
  }

  bool visible_ = REXCVAR_GET(mk9_options_show), note_ok_ = true;
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
