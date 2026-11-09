// mk9-lauta (2026-11-09): what this PC is, for its defaults (mk9_hardware.h).

#include "mk9_hardware.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <dxgi1_4.h>

#include <algorithm>
#include <cctype>
#include <thread>
#include <utility>
#include <vector>

#include <rex/cvar.h>
#include <rex/logging.h>

#include "options_ui.h"

#pragma comment(lib, "dxgi.lib")

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

}  // namespace

Info Detect() {
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
  const Info info = Detect();
  REXLOG_INFO("[hardware] {} (vendor {:04X}, {} MB of its own), {} MB of RAM, {} threads", info.gpu, info.vendor,
              info.vram >> 20, info.ram >> 20, info.threads);
  std::vector<std::pair<std::string, std::string>> values;
  // a PC with 6 GB or less: the renderer's memory policy (unless the player set it)
  if (info.ram && info.ram <= 6 * kGB && !rex::cvar::HasNonDefaultValue("banjo_renut_memory_policy")) {
    rex::cvar::SetFlagByName("banjo_renut_memory_policy", "true");
    rex::cvar::SetFlagByName("banjo_renut_stream_cache_mb", "64");
    REXLOG_INFO("[hardware] {} MB of RAM: the renderer saves memory (banjo_renut_memory_policy)", info.ram >> 20);
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
  mk9::options_ui::SaveToConfig(values);
  REXLOG_INFO("[hardware] first start: quality preset \"{}\" for this PC (F2 changes it)", preset);
}

}  // namespace mk9::hardware
