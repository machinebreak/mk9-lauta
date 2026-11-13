// mk9-lauta (2026-11-09): what this PC is, for its defaults (mk9_hardware.h).

#include "mk9_hardware.h"

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <d3d12.h>
#include <dxgi1_4.h>
#else
#include <dlfcn.h>
#include <sys/sysinfo.h>

#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>

#include <fmt/format.h>
#endif

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

#if defined(_WIN32)
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "d3d12.lib")
#else
REXCVAR_DECLARE(std::string, renut_gpu_adapter);  // the renderer's (renut_engine/gpu/backend/backend.cpp)
#endif

REXCVAR_DEFINE_STRING(mk9_preset_detected, "", "MK9",
                      "The quality preset the first start chose for this PC (mk9_hardware.h); empty: not chosen yet, "
                      "the next start chooses it");

namespace mk9::hardware {
namespace {

#if defined(_WIN32)
std::string Utf8(const wchar_t* text) {
  const int size = WideCharToMultiByte(CP_UTF8, 0, text, -1, nullptr, 0, nullptr, nullptr);
  std::string out(size_t(std::max(size, 1)) - 1, '\0');
  if (size > 1) WideCharToMultiByte(CP_UTF8, 0, text, -1, out.data(), size, nullptr, nullptr);
  return out;
}
#endif

std::string Lower(std::string text) {
  for (char& c : text) c = char(std::tolower(static_cast<unsigned char>(c)));
  return text;
}

bool Has(const std::string& text, const char* what) { return text.find(what) != std::string::npos; }

constexpr uint64_t kGB = 1ull << 30;

#if defined(_WIN32)
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
#else
// mk9-lauta (2026-11-13): Linux: the PC's memory by sysinfo; the GPU by Vulkan (its loader, libvulkan.so.1, opened for
// a moment as Windows' build opens vulkan-1.dll), the one the renderer takes: plume's choice (renut_gpu_adapter's name,
// else the best kind: discrete, integrated, virtual, the CPU's; the first of it). A CPU's (llvmpipe, lavapipe) counts as
// none, as DXGI's Basic Render Driver on Windows. An integrated GPU's memory is the PC's (Vulkan gives a share of the
// RAM as its own): none of its own, as the first start's screen says it on Windows (first_run_ui.cpp).
void QueryVulkan(bool details, Info& info) {
  void* library = dlopen("libvulkan.so.1", RTLD_NOW | RTLD_LOCAL);
  if (!library) library = dlopen("libvulkan.so", RTLD_NOW | RTLD_LOCAL);
  if (!library) return;
  const auto get = reinterpret_cast<PFN_vkGetInstanceProcAddr>(dlsym(library, "vkGetInstanceProcAddr"));
  const auto create = get ? reinterpret_cast<PFN_vkCreateInstance>(get(VK_NULL_HANDLE, "vkCreateInstance")) : nullptr;
  VkApplicationInfo app{};
  app.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
  app.pApplicationName = "mk9-lauta";
  app.apiVersion = VK_API_VERSION_1_1;
  VkInstanceCreateInfo create_info{};
  create_info.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
  create_info.pApplicationInfo = &app;
  VkInstance instance = VK_NULL_HANDLE;
  if (!create || create(&create_info, nullptr, &instance) != VK_SUCCESS || !instance) {
    dlclose(library);
    return;
  }
  const auto destroy = reinterpret_cast<PFN_vkDestroyInstance>(get(instance, "vkDestroyInstance"));
  const auto enumerate =
      reinterpret_cast<PFN_vkEnumeratePhysicalDevices>(get(instance, "vkEnumeratePhysicalDevices"));
  const auto properties =
      reinterpret_cast<PFN_vkGetPhysicalDeviceProperties>(get(instance, "vkGetPhysicalDeviceProperties"));
  const auto properties2 =
      reinterpret_cast<PFN_vkGetPhysicalDeviceProperties2>(get(instance, "vkGetPhysicalDeviceProperties2"));
  const auto memory =
      reinterpret_cast<PFN_vkGetPhysicalDeviceMemoryProperties>(get(instance, "vkGetPhysicalDeviceMemoryProperties"));
  uint32_t count = 0;
  std::vector<VkPhysicalDevice> devices;
  if (enumerate && properties && memory && enumerate(instance, &count, nullptr) >= VK_SUCCESS && count) {
    devices.resize(count);
    if (enumerate(instance, &count, devices.data()) < VK_SUCCESS) count = 0;
    devices.resize(count);
  }
  const std::string wanted = REXCVAR_GET(renut_gpu_adapter);
  constexpr uint32_t kScore[] = {0, 3, 4, 2, 1};  // other, integrated, discrete, virtual, CPU: plume's
  uint32_t best_score = 0;
  VkPhysicalDevice best = VK_NULL_HANDLE;
  VkPhysicalDeviceProperties chosen{};
  for (VkPhysicalDevice device : devices) {
    VkPhysicalDeviceProperties p{};
    properties(device, &p);
    if (uint32_t(p.deviceType) > 4) continue;
    const bool named = !wanted.empty() && wanted == p.deviceName;
    if (kScore[p.deviceType] > best_score || named) {
      best = device;
      chosen = p;
      best_score = kScore[p.deviceType];
      if (named) break;
    }
  }
  if (best != VK_NULL_HANDLE && chosen.deviceType != VK_PHYSICAL_DEVICE_TYPE_CPU) {
    info.gpu = chosen.deviceName;
    info.vendor = chosen.vendorID;
    info.uma = chosen.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU;
    info.driver = chosen.driverVersion;
    info.vulkan_api = chosen.apiVersion;
    if (!info.uma) {
      VkPhysicalDeviceMemoryProperties m{};
      memory(best, &m);
      for (uint32_t i = 0; i < m.memoryHeapCount && i < VK_MAX_MEMORY_HEAPS; ++i) {
        if (m.memoryHeaps[i].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) info.vram = std::max(info.vram, m.memoryHeaps[i].size);
      }
    }
    // the driver's own name for itself (Vulkan 1.2 on the device)
    if (details && properties2 && chosen.apiVersion >= VK_API_VERSION_1_2) {
      VkPhysicalDeviceDriverProperties driver{};
      driver.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES;
      VkPhysicalDeviceProperties2 p2{};
      p2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
      p2.pNext = &driver;
      properties2(best, &p2);
      info.vulkan_driver = std::string(driver.driverName);
      if (driver.driverInfo[0]) info.vulkan_driver += std::string(", ") + driver.driverInfo;
    }
  }
  if (destroy) destroy(instance, nullptr);
  dlclose(library);
}
#endif

}  // namespace

Info Detect(bool d3d12) {
  Info info;
#if !defined(_WIN32)
  struct sysinfo memory {};
  if (sysinfo(&memory) == 0) info.ram = uint64_t(memory.totalram) * (memory.mem_unit ? memory.mem_unit : 1);
  info.threads = std::thread::hardware_concurrency();
  QueryVulkan(d3d12, info);
  return info;
#else
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
#endif
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
#if !defined(_WIN32)
  // Linux: Vulkan, the renderer's only graphics API there (nothing to choose)
  if (!info.gpu.empty()) {
    const uint32_t d = uint32_t(info.driver);
    // the driver's version: NVIDIA's in its own fields (10.8.8.6 bits), the others' as Vulkan's (VK_MAKE_VERSION)
    const std::string version = info.vendor == 0x10DE
                                    ? fmt::format("{}.{}.{}", d >> 22, (d >> 14) & 0xFF, (d >> 6) & 0xFF)
                                    : fmt::format("{}.{}.{}", d >> 22, (d >> 12) & 0x3FF, d & 0xFFF);
    REXLOG_INFO("[hardware] Vulkan {}.{}.{} there, {} memory, driver {}{}", VK_API_VERSION_MAJOR(info.vulkan_api),
                VK_API_VERSION_MINOR(info.vulkan_api), VK_API_VERSION_PATCH(info.vulkan_api),
                info.uma ? "unified" : "its own", version,
                info.vulkan_driver.empty() ? std::string() : " (" + info.vulkan_driver + ")");
  } else {
    REXLOG_WARN("[hardware] no Vulkan GPU on this PC (its driver missing?): the game needs one");
  }
#else
  if (info.d3d12) {
    REXLOG_INFO("[hardware] Direct3D 12 there: resource binding tier {}, resource heap tier {}, shader model {}.{}, {} "
                "memory, driver {}.{}.{}.{}",
                info.binding_tier, info.heap_tier, info.shader_model >> 4, info.shader_model & 0xF,
                info.uma ? "unified" : "its own", info.driver >> 48, (info.driver >> 32) & 0xFFFF,
                (info.driver >> 16) & 0xFFFF, info.driver & 0xFFFF);
  } else {
    REXLOG_WARN("[hardware] no Direct3D 12 device on it");
  }
#endif
  std::vector<std::pair<std::string, std::string>> values;
  // a PC with 6 GB or less: the renderer's memory policy (unless the player set it)
  if (info.ram && info.ram <= 6 * kGB && !rex::cvar::HasNonDefaultValue("banjo_renut_memory_policy")) {
    rex::cvar::SetFlagByName("banjo_renut_memory_policy", "true");
    rex::cvar::SetFlagByName("banjo_renut_stream_cache_mb", "64");
    REXLOG_INFO("[hardware] {} MB of RAM: the renderer saves memory (banjo_renut_memory_policy)", info.ram >> 20);
  }
  // mk9-lauta (2026-11-11): the graphics API, when the player has not chosen one (each start: a later release may
  // decide otherwise, and a choice in F2 is kept)
#if defined(_WIN32)
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
#endif
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
