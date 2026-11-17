// mk9-lauta (2026-11-11): the renderer's start, watched (mk9_renderer_start.h).

#include "mk9_renderer_start.h"

#include "mk9_platform.h"

#include <cstdlib>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#endif

#include <atomic>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>

#include <fmt/format.h>
#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/system.h>

#include "imgui.h"
#include "online/i18n.h"
#include "options_ui.h"
#include "renut_engine/gpu/backend/backend.h"
#include "renut_engine/gpu/overlay/overlay_ui.h"

REXCVAR_DECLARE(std::string, renut_gpu_backend);
REXCVAR_DEFINE_STRING(mk9_renderer_test_fail, "", "MK9",
                      "Tests: a start with this graphics API (d3d12, vulkan, d3d11) ends right before its device is "
                      "made, as a start a driver kills (mk9_renderer_start.h: the next start goes back to the API that "
                      "worked)");

// A laptop with two GPUs runs the game on the dedicated one when the executable exports these (NVIDIA Optimus, AMD
// PowerXpress): the drivers read the .exe's exports, not a DLL's (the SDK's rexgpu DLL has them, to no effect).
#if defined(_WIN32)
extern "C" {
__declspec(dllexport) DWORD NvOptimusEnablement = 0x00000001;
__declspec(dllexport) int AmdPowerXpressRequestHighPerformance = 1;
}
#endif

namespace mk9::renderer_start {
namespace {

std::mutex g_mutex;
std::string g_api;     // this start's API (DeviceReady)
int g_failed = 0;      // failed starts in a row before this one, with g_api
std::string g_ok_api;  // mk9-lauta (2026-11-11): the last API that got an image on screen ("" none yet)
std::string g_notice;  // an API swapped at this start: shown once the overlay runs
// mk9-lauta (2026-11-16): the APIs that failed to start on this PC (the marker's fifth field, comma separated): the
// watchdog does not go back to them (Direct3D 12 crashing in an old driver, then Vulkan without what it needs: Direct3D
// 11, not Direct3D 12 again).
std::string g_bad;
std::atomic<bool> g_presented{false};
std::atomic<bool> g_lost{false};

std::filesystem::path MarkerPath() {
  return mk9::platform::ExeDir() / "logs" / "renderer_start.txt";
}

// logs/renderer_start.txt: "<api> <failed starts in a row> <pending|ok|lost> [<the last API that showed an image>
// [<the APIs that failed to start, comma separated>]]" ("-" for an empty field before another).
void Write(const std::string& api, int failed, const char* state, const std::string& ok_api) {
  std::error_code ec;
  std::filesystem::create_directories(MarkerPath().parent_path(), ec);
  std::ofstream out(MarkerPath(), std::ios::trunc);
  out << api << ' ' << failed << ' ' << state;
  if (!ok_api.empty() || !g_bad.empty()) out << ' ' << (ok_api.empty() ? "-" : ok_api);
  if (!g_bad.empty()) out << ' ' << g_bad;
  out << '\n';
}

const char* Name(const std::string& api) {
  return api == "vulkan" ? "Vulkan" : api == "d3d11" ? "DirectX 11" : "DirectX 12";
}

// renut_gpu_backend's value as one of the three APIs.
std::string Canonical(const std::string& value) { return value == "vulkan" || value == "d3d11" ? value : "d3d12"; }

renut::gpu::Backend BackendOf(const std::string& api) {
  return api == "vulkan" ? renut::gpu::Backend::Vulkan
         : api == "d3d11" ? renut::gpu::Backend::D3D11
                          : renut::gpu::Backend::D3D12;
}

bool Listed(const std::string& list, const std::string& api) {
  return ("," + list + ",").find("," + api + ",") != std::string::npos;
}

void AddBad(const std::string& api) {
  if (!Listed(g_bad, api)) g_bad = g_bad.empty() ? api : g_bad + "," + api;
}

// The API after this one (Direct3D 12, Vulkan, Direct3D 11, in that order), skipping those that failed here; when all
// did, Direct3D 11 (Direct3D 12 from it).
std::string NextApi(const std::string& current) {
  static const char* const kOrder[] = {"d3d12", "vulkan", "d3d11"};
  int at = 0;
  for (int i = 0; i < 3; ++i)
    if (current == kOrder[i]) at = i;
  for (int step = 1; step < 3; ++step) {
    const std::string candidate = kOrder[(at + step) % 3];
    if (!Listed(g_bad, candidate)) return candidate;
  }
  return current == "d3d11" ? "d3d12" : "d3d11";
}

class Notice : public renut::gpu::overlay::Dialog {
 public:
  void Draw() override {
    std::string text;
    {
      std::lock_guard lock(g_mutex);
      text = g_notice;
    }
    if (text.empty()) return;
    const auto now = std::chrono::steady_clock::now();
    if (since_ == std::chrono::steady_clock::time_point{}) since_ = now;
    if (now - since_ > std::chrono::seconds(20)) return;
    const ImGuiIO& io = ImGui::GetIO();
    ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f, 12), ImGuiCond_Always, ImVec2(0.5f, 0));
    ImGui::SetNextWindowSizeConstraints(ImVec2(360, 0), ImVec2(std::min(760.0f, io.DisplaySize.x - 24), 400));
    const ImGuiWindowFlags flags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                                   ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings |
                                   ImGuiWindowFlags_NoFocusOnAppearing;
    if (ImGui::Begin("##renderer_start", nullptr, flags)) {
      ImGui::TextColored(ImVec4(1.0f, 0.82f, 0.35f, 1), "%s", mk9::i18n::T("API gráfica"));
      ImGui::PushTextWrapPos(std::min(740.0f, io.DisplaySize.x - 40));
      ImGui::TextUnformatted(text.c_str());
      ImGui::PopTextWrapPos();
    }
    ImGui::End();
  }

 private:
  std::chrono::steady_clock::time_point since_{};
};

}  // namespace

// mk9-lauta (2026-11-11): this start is "pending" from here, before the device is made, so a start that dies or hangs
// there (a driver's Vulkan, a layer of another program) counts too; it counted only past DeviceReady, and a player who
// chose Vulkan in F2 and whose PC could not start it was left with a game that never opened until mk9.toml went. An
// API chosen in F2 whose first start fails goes back at once to the one that last showed an image; any other, after
// two failed starts in a row.
void BeforeDevice() {
  std::string current = Canonical(REXCVAR_GET(renut_gpu_backend));
  std::string api, state, ok_api, bad;
  int failed = 0;
  {
    std::ifstream in(MarkerPath());
    in >> api >> failed >> state >> ok_api >> bad;  // (older markers have three or four fields)
  }
  if (ok_api == "-") ok_api.clear();
  if (bad == "-") bad.clear();
  if (ok_api.empty() && state == "ok") ok_api = api;
  const bool failed_before = api == current && (state == "pending" || state == "lost");
  failed = failed_before ? failed + 1 : 0;
  const bool chosen_failed = failed_before && !ok_api.empty() && ok_api != current;  // F2's new choice, failed once
  std::lock_guard lock(g_mutex);
  g_bad = bad;
#if !defined(_WIN32)
  // mk9-lauta (2026-11-13): Linux has Vulkan only: nothing to go back to (the marker still says how the starts went)
  if (false) {
#else
  if (failed >= 2 || chosen_failed) {
#endif
    AddBad(current);
    const std::string other = chosen_failed ? ok_api : NextApi(current);
    REXLOG_WARN("[renderer] {} did not get an image on screen in the last {} start(s) (logs/renderer_start.txt): {} "
                "from now on (renut_gpu_backend, saved in mk9.toml)",
                Name(current), failed, Name(other));
    rex::cvar::SetFlagByName("renut_gpu_backend", other);
    renut::gpu::ForceBackend(BackendOf(other));
    options_ui::SaveToConfig({{"renut_gpu_backend", other}});
    g_notice = chosen_failed
                   ? fmt::format(fmt::runtime(mk9::i18n::T(
                                     "El juego no pudo arrancar con {} en esta PC: volvió a {}, que sí anda. Si querés "
                                     "probar de nuevo, está en F2 > Pantalla e idioma > API gráfica.")),
                                 Name(current), Name(other))
                   : fmt::format(fmt::runtime(mk9::i18n::T(
                                     "El juego no llegó a mostrar imagen en los últimos {} arranques con {}: ahora usa "
                                     "{}. Si querés volver, está en F2 > Pantalla e idioma > API gráfica.")),
                                 failed, Name(current), Name(other));
    failed = 0;
    current = other;
  }
  g_failed = failed;
  g_ok_api = ok_api;
  Write(current, failed, "pending", ok_api);
  // tests (mk9_renderer_test_fail): a start of that API that dies here, as a driver's would
  if (const std::string fail = REXCVAR_GET(mk9_renderer_test_fail); !fail.empty() && fail == current) {
    REXLOG_WARN("[renderer] mk9_renderer_test_fail: this start of {} ends here", Name(current));
#if defined(_WIN32)
    TerminateProcess(GetCurrentProcess(), 3);
#else
    std::_Exit(3);
#endif
  }
}

void DeviceReady(const char* api) {
  std::lock_guard lock(g_mutex);
  g_api = api;
  Write(g_api, g_failed, "pending", g_ok_api);
}

void VulkanUnusable(const std::string& why, const char* api) {
  REXLOG_WARN("[renderer] Vulkan cannot draw this game on this PC ({}): {} from now on (renut_gpu_backend, saved in "
              "mk9.toml)",
              why, Name(api));
  rex::cvar::SetFlagByName("renut_gpu_backend", api);
  options_ui::SaveToConfig({{"renut_gpu_backend", api}});
  std::lock_guard lock(g_mutex);
  AddBad("vulkan");
  g_failed = 0;  // a start of another API: no failures of its own yet
  g_notice = fmt::format(
      fmt::runtime(mk9::i18n::T("El driver de tu placa de video no tiene lo que el juego necesita de Vulkan (Vulkan "
                                "1.2 con direcciones de buffer): ahora usa {}. Si actualizás el driver, Vulkan se puede "
                                "volver a elegir en F2 > Pantalla e idioma > API gráfica.")),
      Name(api));
}

const char* FallbackAfterVulkan() {
  std::lock_guard lock(g_mutex);
  return Listed(g_bad, "d3d12") ? "d3d11" : "d3d12";
}

void FramesDone() {
  if (g_presented.load(std::memory_order_relaxed) || g_presented.exchange(true)) return;
  std::lock_guard lock(g_mutex);
  if (!g_api.empty()) Write(g_api, 0, "ok", g_api);
}

void DeviceLost() {
  if (g_lost.exchange(true)) return;
  std::string api;
  int failed;
  {
    std::lock_guard lock(g_mutex);
    api = g_api;
    failed = g_presented.load() ? 0 : g_failed;
    if (!api.empty()) Write(api, failed, "lost", g_ok_api);
  }
#if defined(_WIN32)
  std::string other;
  {
    std::lock_guard lock(g_mutex);
    other = Name(NextApi(Canonical(api)));
  }
  REXLOG_ERROR("[renderer] the {} device was lost: told the player; a second time in a row swaps to {} at the next "
               "start",
               Name(api), other);
  rex::ShowSimpleMessageBox(
      rex::SimpleMessageBoxType::Error,
      fmt::format(fmt::runtime(mk9::i18n::T(
                      "La placa de video dejó de responder (se perdió el dispositivo de {}): el juego ya no puede "
                      "dibujar.\n\nCerralo y volvé a abrirlo. Si vuelve a pasar, el próximo arranque prueba con {}. "
                      "También ayuda actualizar el driver de la placa de video.\n\nEl detalle quedó en la carpeta "
                      "logs.")),
                  Name(api), other)
);
#else
  // mk9-lauta (2026-11-13): Linux has Vulkan only: no other API to try at the next start
  REXLOG_ERROR("[renderer] the Vulkan device was lost: told the player");
  rex::ShowSimpleMessageBox(
      rex::SimpleMessageBoxType::Error,
      mk9::i18n::T("La placa de video dejó de responder (se perdió el dispositivo de Vulkan): el juego ya no puede "
                   "dibujar.\n\nCerralo y volvé a abrirlo. Si vuelve a pasar, actualizá el driver de la placa de video "
                   "(Mesa en AMD e Intel, el de NVIDIA en NVIDIA).\n\nEl detalle quedó en la carpeta logs."));
#endif
}

void Install() {
  static bool installed = false;
  if (installed) return;
  installed = true;
  static Notice notice;
  renut::gpu::overlay::AddDialog(&notice);
}

}  // namespace mk9::renderer_start
