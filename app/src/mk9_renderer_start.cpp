// mk9-lauta (2026-11-11): the renderer's start, watched (mk9_renderer_start.h).

#include "mk9_renderer_start.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

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
#include "options_ui.h"
#include "renut_engine/gpu/backend/backend.h"
#include "renut_engine/gpu/overlay/overlay_ui.h"

REXCVAR_DECLARE(std::string, renut_gpu_backend);

// A laptop with two GPUs runs the game on the dedicated one when the executable exports these (NVIDIA Optimus, AMD
// PowerXpress): the drivers read the .exe's exports, not a DLL's (the SDK's rexgpu DLL has them, to no effect).
extern "C" {
__declspec(dllexport) DWORD NvOptimusEnablement = 0x00000001;
__declspec(dllexport) int AmdPowerXpressRequestHighPerformance = 1;
}

namespace mk9::renderer_start {
namespace {

std::mutex g_mutex;
std::string g_api;     // this start's API (DeviceReady)
int g_failed = 0;      // failed starts in a row before this one, with g_api
std::string g_notice;  // an API swapped at this start: shown once the overlay runs
std::atomic<bool> g_presented{false};
std::atomic<bool> g_lost{false};

std::filesystem::path MarkerPath() {
  wchar_t exe[MAX_PATH] = {};
  GetModuleFileNameW(nullptr, exe, MAX_PATH);
  return std::filesystem::path(exe).parent_path() / L"logs" / L"renderer_start.txt";
}

void Write(const std::string& api, int failed, const char* state) {
  std::error_code ec;
  std::filesystem::create_directories(MarkerPath().parent_path(), ec);
  std::ofstream out(MarkerPath(), std::ios::trunc);
  out << api << ' ' << failed << ' ' << state << '\n';
}

const char* Name(const std::string& api) { return api == "vulkan" ? "Vulkan" : "DirectX 12"; }

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
      ImGui::TextColored(ImVec4(1.0f, 0.82f, 0.35f, 1), "API gráfica");
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

void BeforeDevice() {
  const std::string current = REXCVAR_GET(renut_gpu_backend) == "vulkan" ? "vulkan" : "d3d12";
  std::string api, state;
  int failed = 0;
  {
    std::ifstream in(MarkerPath());
    in >> api >> failed >> state;
  }
  failed = api == current && (state == "pending" || state == "lost") ? failed + 1 : 0;
  std::lock_guard lock(g_mutex);
  if (failed >= 2) {
    const std::string other = current == "vulkan" ? "d3d12" : "vulkan";
    REXLOG_WARN("[renderer] {} did not get an image on screen in the last {} starts (logs/renderer_start.txt): {} "
                "from now on (renut_gpu_backend, saved in mk9.toml)",
                Name(current), failed, Name(other));
    rex::cvar::SetFlagByName("renut_gpu_backend", other);
    renut::gpu::ForceBackend(other == "vulkan" ? renut::gpu::Backend::Vulkan : renut::gpu::Backend::D3D12);
    options_ui::SaveToConfig({{"renut_gpu_backend", other}});
    g_notice = fmt::format("El juego no llegó a mostrar imagen en los últimos {} arranques con {}: ahora usa {}. Si "
                           "querés volver, está en F2 > Pantalla e idioma > API gráfica.",
                           failed, Name(current), Name(other));
    failed = 0;
  }
  g_failed = failed;
}

void DeviceReady(const char* api) {
  std::lock_guard lock(g_mutex);
  g_api = api;
  Write(g_api, g_failed, "pending");
}

void FramesDone() {
  if (g_presented.load(std::memory_order_relaxed) || g_presented.exchange(true)) return;
  std::lock_guard lock(g_mutex);
  if (!g_api.empty()) Write(g_api, 0, "ok");
}

void DeviceLost() {
  if (g_lost.exchange(true)) return;
  std::string api;
  int failed;
  {
    std::lock_guard lock(g_mutex);
    api = g_api;
    failed = g_presented.load() ? 0 : g_failed;
    if (!api.empty()) Write(api, failed, "lost");
  }
  const std::string other = api == "vulkan" ? "DirectX 12" : "Vulkan";
  REXLOG_ERROR("[renderer] the {} device was lost: told the player; a second time in a row swaps to {} at the next "
               "start",
               Name(api), other);
  rex::ShowSimpleMessageBox(
      rex::SimpleMessageBoxType::Error,
      fmt::format("La placa de video dejó de responder (se perdió el dispositivo de {}): el juego ya no puede dibujar.\n\n"
                  "Cerralo y volvé a abrirlo. Si vuelve a pasar, el próximo arranque prueba con {}. También ayuda "
                  "actualizar el driver de la placa de video.\n\nEl detalle quedó en la carpeta logs.",
                  Name(api), other)
);
}

void Install() {
  static bool installed = false;
  if (installed) return;
  installed = true;
  static Notice notice;
  renut::gpu::overlay::AddDialog(&notice);
}

}  // namespace mk9::renderer_start
