// mk9-lauta (2026-11-09): what this PC is, for its defaults (mk9_hardware.h).

#include "mk9_hardware.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <d3d12.h>
#include <dxgi1_4.h>

#include <algorithm>
#include <cctype>
#include <fstream>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <rex/cvar.h>
#include <rex/logging.h>

#include "options_ui.h"

#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "d3d12.lib")

REXCVAR_DEFINE_STRING(mk9_preset_detected, "", "MK9",
                      "The quality preset the first start chose for this PC (mk9_hardware.h); empty: not chosen yet, "
                      "the next start chooses it");

namespace mk9::hardware {
namespace {

std::string Utf8(const wchar_t* text) {
  const int size = WideCharToMultiByte(CP_UTF8, 0, text, -1, nullptr, 0, nullptr, nullptr);
  std::string out(size_t(std::max(size, 1)) - 1, '\0');
  if (size > 1) WideCharToMultiByte(CP_UTF8, 0, text, -1, out.data(), size, nullptr, nullptr);
  return out;
}

std::string Lower(std::string text) {
  for (char& c : text) c = char(std::tolower(static_cast<unsigned char>(c)));
  return text;
}

bool Has(const std::string& text, const char* what) { return text.find(what) != std::string::npos; }

constexpr uint64_t kGB = 1ull << 30;

// mk9-lauta (2026-11-11): the player chose the graphics API: a renut_gpu_backend line in mk9.toml (F2, or the renderer's
// start watchdog after two starts that failed) or on the command line (the tests).
bool ApiChosen() {
  if (std::wstring(GetCommandLineW()).find(L"renut_gpu_backend") != std::wstring::npos) return true;
  wchar_t env[16] = {};  // the SDK's cvars from the environment (tools/online/run_game.ps1: REX_RENUT_GPU_BACKEND)
  if (GetEnvironmentVariableW(L"REX_RENUT_GPU_BACKEND", env, 16) > 0) return true;
  wchar_t exe[MAX_PATH] = {};
  GetModuleFileNameW(nullptr, exe, MAX_PATH);
  std::wstring path(exe);
  path = path.substr(0, path.find_last_of(L"\\/") + 1) + L"mk9.toml";
  std::ifstream in(path);
  for (std::string line; std::getline(in, line);) {
    size_t i = line.find_first_not_of(" \t");
    if (i != std::string::npos && line.compare(i, 17, "renut_gpu_backend") == 0) return true;
  }
  return false;
}

// Where the renderer's Direct3D 12 path is weakest and Vulkan is the better default: a player's low-end PC showed broken
// characters and floors on Direct3D 12 and none on Vulkan (2026-11-11), and a strict Direct3D 12 (WARP) leaves whole
// draws out; the Vulkan path is the one this renderer is built and tested on most. Intel's integrated GPUs (not Arc),
// GPUs older than Direct3D 12's resource binding tier 3, and dedicated cards with less than 2 GB. The reason, or null.
const char* VulkanReason(const Info& info) {
  const std::string gpu = Lower(info.gpu);
  if (info.vendor == 0x8086 && !Has(gpu, "arc")) return "an Intel integrated GPU";
  if (info.d3d12 && info.binding_tier < 3) return "a GPU older than Direct3D 12's resource binding tier 3";
  if (!info.uma && info.vram && info.vram < 2 * kGB) return "a graphics card with less than 2 GB of its own";
  return nullptr;
}

bool VulkanPresent() {
  HMODULE vulkan = LoadLibraryW(L"vulkan-1.dll");
  if (!vulkan) return false;
  FreeLibrary(vulkan);
  return true;
}

// A Direct3D 12 device on the adapter for a moment: what it offers.
void QueryD3D12(IDXGIAdapter1* adapter, Info& info) {
  ID3D12Device* device = nullptr;
  if (FAILED(D3D12CreateDevice(adapter, D3D_FEATURE_LEVEL_11_0, __uuidof(ID3D12Device),
                               reinterpret_cast<void**>(&device))) || !device) {
    return;
  }
  info.d3d12 = true;
  D3D12_FEATURE_DATA_D3D12_OPTIONS options{};
  if (SUCCEEDED(device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS, &options, sizeof(options)))) {
    info.binding_tier = int(options.ResourceBindingTier);
    info.heap_tier = int(options.ResourceHeapTier);
  }
  for (D3D_SHADER_MODEL model : {D3D_SHADER_MODEL_6_7, D3D_SHADER_MODEL_6_6, D3D_SHADER_MODEL_6_5,
                                 D3D_SHADER_MODEL_6_4, D3D_SHADER_MODEL_6_3, D3D_SHADER_MODEL_6_2,
                                 D3D_SHADER_MODEL_6_1, D3D_SHADER_MODEL_6_0}) {
    D3D12_FEATURE_DATA_SHADER_MODEL shader{model};
    if (SUCCEEDED(device->CheckFeatureSupport(D3D12_FEATURE_SHADER_MODEL, &shader, sizeof(shader)))) {
      info.shader_model = int(shader.HighestShaderModel);
      break;
    }
  }
  D3D12_FEATURE_DATA_ARCHITECTURE architecture{};
  if (SUCCEEDED(device->CheckFeatureSupport(D3D12_FEATURE_ARCHITECTURE, &architecture, sizeof(architecture)))) {
    info.uma = architecture.UMA;
  }
  device->Release();
  LARGE_INTEGER version{};
  if (SUCCEEDED(adapter->CheckInterfaceSupport(__uuidof(IDXGIDevice), &version))) info.driver = uint64_t(version.QuadPart);
}

}  // namespace

Info Detect(bool d3d12) {
  Info info;
  MEMORYSTATUSEX memory{sizeof(memory)};
  if (GlobalMemoryStatusEx(&memory)) info.ram = memory.ullTotalPhys;
  info.threads = std::thread::hardware_concurrency();
  IDXGIFactory1* factory = nullptr;
  if (SUCCEEDED(CreateDXGIFactory1(__uuidof(IDXGIFactory1), reinterpret_cast<void**>(&factory)))) {
    IDXGIAdapter1* adapter = nullptr;
    for (UINT i = 0; factory->EnumAdapters1(i, &adapter) != DXGI_ERROR_NOT_FOUND; ++i) {
      DXGI_ADAPTER_DESC1 desc{};
      adapter->GetDesc1(&desc);
      adapter->Release();
      if (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) continue;  // Microsoft's Basic Render Driver
      info.gpu = Utf8(desc.Description);
      info.vendor = desc.VendorId;
      info.vram = desc.DedicatedVideoMemory;
      if (d3d12) {
        IDXGIAdapter1* again = nullptr;  // the one released above, asked for once more
        if (SUCCEEDED(factory->EnumAdapters1(i, &again)) && again) {
          QueryD3D12(again, info);
          again->Release();
        }
      }
      break;
    }
    factory->Release();
  }
  return info;
}

std::string ChoosePreset(const Info& info) {
  const std::string gpu = Lower(info.gpu);
  if (info.ram && info.ram <= 4 * kGB + kGB / 2) return "Muy baja";  // 4 GB PCs: the memory, before anything
  // integrated graphics, by the name (an APU's memory set aside by the firmware can be several GB: not a measure)
  if (info.vendor == 0x8086 && !Has(gpu, "arc")) {
    return Has(gpu, "iris") ? "Baja" : "Muy baja";  // UHD / HD Graphics: Celeron, Pentium and older Core laptops
  }
  if (info.vendor == 0x1002 && !Has(gpu, "rx ") && !Has(gpu, "pro w") && !Has(gpu, "radeon vii")) {
    // the RDNA 2/3 APUs (Radeon 660M to 890M) draw like the console; the Vega ones ("Radeon Graphics") less
    for (const char* rdna : {"660m", "680m", "740m", "760m", "780m", "880m", "890m", "8050s", "8060s"}) {
      if (Has(gpu, rdna)) return "Consola";
    }
    return "Baja";
  }
  // a graphics card of its own
  if (info.vram >= 6 * kGB) return "Alta";
  if (info.vram >= 2 * kGB) return "Consola";
  return "Baja";
}

void ApplyDefaults() {
  const bool chosen = !REXCVAR_GET(mk9_preset_detected).empty();
  const Info info = Detect(true);
  REXLOG_INFO("[hardware] {} (vendor {:04X}, {} MB of its own), {} MB of RAM, {} threads", info.gpu, info.vendor,
              info.vram >> 20, info.ram >> 20, info.threads);
  if (info.d3d12) {
    REXLOG_INFO("[hardware] Direct3D 12 there: resource binding tier {}, resource heap tier {}, shader model {}.{}, {} "
                "memory, driver {}.{}.{}.{}",
                info.binding_tier, info.heap_tier, info.shader_model >> 4, info.shader_model & 0xF,
                info.uma ? "unified" : "its own", info.driver >> 48, (info.driver >> 32) & 0xFFFF,
                (info.driver >> 16) & 0xFFFF, info.driver & 0xFFFF);
  } else {
    REXLOG_WARN("[hardware] no Direct3D 12 device on it");
  }
  std::vector<std::pair<std::string, std::string>> values;
  // a PC with 6 GB or less: the renderer's memory policy (unless the player set it)
  if (info.ram && info.ram <= 6 * kGB && !rex::cvar::HasNonDefaultValue("banjo_renut_memory_policy")) {
    rex::cvar::SetFlagByName("banjo_renut_memory_policy", "true");
    rex::cvar::SetFlagByName("banjo_renut_stream_cache_mb", "64");
    REXLOG_INFO("[hardware] {} MB of RAM: the renderer saves memory (banjo_renut_memory_policy)", info.ram >> 20);
  }
  // mk9-lauta (2026-11-11): the graphics API, when the player has not chosen one (each start: a later release may
  // decide otherwise, and a choice in F2 is kept)
  if (!ApiChosen()) {
    if (const char* reason = VulkanReason(info)) {
      if (VulkanPresent()) {
        rex::cvar::SetFlagByName("renut_gpu_backend", "vulkan");
        REXLOG_INFO("[hardware] Vulkan for this PC ({}): F2 > Pantalla e idioma > API gráfica changes it", reason);
      } else {
        REXLOG_WARN("[hardware] {}, where Vulkan would be better, but no Vulkan on this PC: Direct3D 12", reason);
      }
    }
  }
  if (chosen) return;
  const std::string preset = ChoosePreset(info);
  if (const auto* preset_values = mk9::options_ui::PresetValues(preset)) {
    for (const auto& [key, value] : *preset_values) {
      rex::cvar::SetFlagByName(key, value);
      values.emplace_back(key, value);
    }
  }
  values.emplace_back("mk9_preset_detected", preset);
  rex::cvar::SetFlagByName("mk9_preset_detected", preset);
  // mk9-lauta (2026-11-11, F3): the first start's steps show until finished or skipped (first_run_ui.h); the players
  // of earlier releases have mk9_preset_detected already and never see them
  values.emplace_back("mk9_first_run_done", "false");
  rex::cvar::SetFlagByName("mk9_first_run_done", "false");
  mk9::options_ui::SaveToConfig(values);
  REXLOG_INFO("[hardware] first start: quality preset \"{}\" for this PC (F2 changes it)", preset);
}

}  // namespace mk9::hardware
