// MK9 online: the countries' flags and this player's country (flags.h).

#include "online/flags.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <winhttp.h>

#include <algorithm>
#include <atomic>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>

#include "imgui.h"
#include "imgui_internal.h"  // RegisterUserTexture

#include <rex/cvar.h>
#include <rex/logging.h>

#define STB_IMAGE_STATIC
#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#define STBI_NO_STDIO
#include "../../../sdk/thirdparty/stb/stb_image.h"

REXCVAR_DEFINE_STRING(mk9_online_country, "", "MK9 Online",
                      "Your country in the online screens (two letters: AR, ES, MX...; empty = found by itself: by "
                      "your connection, else Windows' region)");

namespace mk9::flags {
namespace {

constexpr int kCellW = 40, kCellH = 30, kColumns = 16;

const char* const kCodes[] = {
#include "online/flags_codes.inc"
};
constexpr int kCount = int(sizeof(kCodes) / sizeof(kCodes[0]));

const unsigned char kAtlasPng[] = {
#embed "flags_atlas.png"
};

int IndexOf(const std::string& code) {
  if (code.size() != 2) return -1;
  const char lower[3] = {char(std::tolower(static_cast<unsigned char>(code[0]))),
                         char(std::tolower(static_cast<unsigned char>(code[1]))), 0};
  const auto it = std::lower_bound(kCodes, kCodes + kCount, lower,
                                   [](const char* a, const char* b) { return std::strcmp(a, b) < 0; });
  return it != kCodes + kCount && std::strcmp(*it, lower) == 0 ? int(it - kCodes) : -1;
}

// The atlas as a texture of the current ImGui context (the overlay's), made at its first use there.
ImTextureData* Atlas() {
  static ImTextureData* atlas = [] () -> ImTextureData* {
    int w = 0, h = 0, channels = 0;
    unsigned char* rgba = stbi_load_from_memory(kAtlasPng, int(sizeof(kAtlasPng)), &w, &h, &channels, 4);
    if (!rgba) {
      REXLOG_WARN("[flags] the flag atlas did not decode");
      return nullptr;
    }
    auto* tex = IM_NEW(ImTextureData)();
    tex->Create(ImTextureFormat_RGBA32, w, h);
    std::memcpy(tex->GetPixels(), rgba, size_t(w) * size_t(h) * 4);
    stbi_image_free(rgba);
    tex->UseColors = true;
    tex->SetStatus(ImTextureStatus_WantCreate);
    ImGui::RegisterUserTexture(tex);
    return tex;
  }();
  return atlas;
}

void Cell(int index, ImVec2* uv0, ImVec2* uv1) {
  const ImTextureData* atlas = Atlas();
  const float w = float(atlas->Width), h = float(atlas->Height);
  const float x = float((index % kColumns) * kCellW), y = float((index / kColumns) * kCellH);
  *uv0 = ImVec2(x / w, y / h);
  *uv1 = ImVec2((x + kCellW) / w, (y + kCellH) / h);
}

// --- this player's country ---

std::mutex g_mutex;
std::string g_mine;
std::atomic<bool> g_started{false};

std::string Upper(std::string s) {
  for (char& c : s) c = char(std::toupper(static_cast<unsigned char>(c)));
  return s;
}

// Cloudflare's trace answers the address's country ("loc=AR"); "" when it cannot be reached.
std::string ByConnection() {
  std::string out;
  HINTERNET session = WinHttpOpen(L"mk9-lauta", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME,
                                  WINHTTP_NO_PROXY_BYPASS, 0);
  if (!session) return out;
  WinHttpSetTimeouts(session, 4000, 4000, 4000, 4000);
  if (HINTERNET connect = WinHttpConnect(session, L"www.cloudflare.com", INTERNET_DEFAULT_HTTPS_PORT, 0)) {
    if (HINTERNET request = WinHttpOpenRequest(connect, L"GET", L"/cdn-cgi/trace", nullptr, WINHTTP_NO_REFERER,
                                               WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE)) {
      if (WinHttpSendRequest(request, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) &&
          WinHttpReceiveResponse(request, nullptr)) {
        std::string body;
        DWORD available = 0;
        while (WinHttpQueryDataAvailable(request, &available) && available && body.size() < 4096) {
          std::string chunk(available, '\0');
          DWORD read = 0;
          if (!WinHttpReadData(request, chunk.data(), available, &read) || !read) break;
          body.append(chunk.data(), read);
        }
        const size_t at = body.find("\nloc=");
        if (at != std::string::npos && at + 7 <= body.size()) out = Upper(body.substr(at + 5, 2));
      }
      WinHttpCloseHandle(request);
    }
    WinHttpCloseHandle(connect);
  }
  WinHttpCloseHandle(session);
  return out;
}

// Windows' region (Settings > Time and language > Region).
std::string ByRegion() {
  const GEOID geo = GetUserGeoID(GEOCLASS_NATION);
  if (geo == GEOID_NOT_AVAILABLE) return {};
  wchar_t iso2[8] = {};
  if (!GetGeoInfoW(geo, GEO_ISO2, iso2, int(std::size(iso2)), 0)) return {};
  std::string out;
  for (int i = 0; i < 2 && iso2[i]; ++i) out += char(iso2[i]);
  return Upper(out);
}

}  // namespace

bool Draw(const std::string& code, float height) {
  const int index = IndexOf(code);
  if (index < 0 || !Atlas()) return false;
  ImVec2 uv0, uv1;
  Cell(index, &uv0, &uv1);
  ImGui::Image(Atlas()->GetTexRef(), ImVec2(height * kCellW / kCellH, height), uv0, uv1);
  if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", Name(code).c_str());
  return true;
}

bool DrawAt(ImDrawList* list, const std::string& code, const ImVec2& pos, float height) {
  const int index = IndexOf(code);
  if (index < 0 || !list || !Atlas()) return false;
  ImVec2 uv0, uv1;
  Cell(index, &uv0, &uv1);
  list->AddImage(Atlas()->GetTexRef(), pos, ImVec2(pos.x + height * kCellW / kCellH, pos.y + height), uv0, uv1);
  return true;
}

bool Known(const std::string& code) { return IndexOf(code) >= 0; }

std::string Name(const std::string& code) {
  // Windows' own name of the country, in its language (Windows 10 and later)
  using GetGeoInfoEx_t = int(WINAPI*)(PWSTR, GEOTYPE, PWSTR, int);
  static const auto get = reinterpret_cast<GetGeoInfoEx_t>(
      reinterpret_cast<void*>(GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "GetGeoInfoEx")));
  if (get && code.size() == 2) {
    wchar_t location[3] = {wchar_t(std::toupper(static_cast<unsigned char>(code[0]))),
                           wchar_t(std::toupper(static_cast<unsigned char>(code[1]))), 0};
    wchar_t name[128] = {};
    if (get(location, GEO_FRIENDLYNAME, name, int(std::size(name))) > 0) {
      const int n = WideCharToMultiByte(CP_UTF8, 0, name, -1, nullptr, 0, nullptr, nullptr);
      std::string out(size_t(std::max(n, 1)) - 1, '\0');
      WideCharToMultiByte(CP_UTF8, 0, name, -1, out.data(), n, nullptr, nullptr);
      if (!out.empty()) return out;
    }
  }
  return Upper(code);
}

std::string Mine() {
  std::lock_guard<std::mutex> lock(g_mutex);
  return g_mine;
}

void Detect() {
  if (g_started.exchange(true)) return;
  const std::string chosen = Upper(REXCVAR_GET(mk9_online_country));
  if (Known(chosen)) {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_mine = chosen;
    return;
  }
  std::thread([] {
    std::string cc = ByConnection();
    const char* how = "connection";
    if (!Known(cc) || cc == "XX" || cc == "T1") {
      cc = ByRegion();
      how = "Windows' region";
    }
    if (!Known(cc)) {
      REXLOG_INFO("[flags] this player's country could not be found");
      return;
    }
    REXLOG_INFO("[flags] this player's country: {} (by its {})", cc, how);
    std::lock_guard<std::mutex> lock(g_mutex);
    g_mine = cc;
  }).detach();
}

}  // namespace mk9::flags
