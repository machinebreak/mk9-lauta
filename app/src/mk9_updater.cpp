// mk9-lauta: the in-game updater (mk9_updater.h).

#include "mk9_updater.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <bcrypt.h>
#include <shellapi.h>
#include <winhttp.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <cwctype>
#include <filesystem>
#include <fstream>
#include <functional>
#include <mutex>
#include <string>
#include <system_error>
#include <thread>
#include <vector>

#include <nlohmann/json.hpp>

#include "imgui.h"

#include <rex/cvar.h>
#include <rex/logging.h>

#include "online/netplay.h"
#include "renut_engine/gpu/overlay/overlay_ui.h"

#pragma comment(lib, "winhttp.lib")
#pragma comment(lib, "bcrypt.lib")
#pragma comment(lib, "shell32.lib")

REXCVAR_DEFINE_BOOL(mk9_update_check, true, "MK9",
                    "At start, ask GitHub whether there is a newer release of mk9-lauta (the banner over the game)");
REXCVAR_DEFINE_STRING(mk9_update_repo, "machinebreak/mk9-lauta", "MK9",
                      "The GitHub repository the updater asks for releases (owner/name)");
REXCVAR_DEFINE_STRING(mk9_update_api, "", "MK9",
                      "Tests: the address of the newest release's description (GitHub's JSON) instead of GitHub's");
REXCVAR_DEFINE_BOOL(mk9_update_test_auto, false, "MK9",
                    "Tests: a newer release is downloaded, installed and started by itself (the new program is started "
                    "without this option)");

#ifndef MK9_LAUTA_VERSION
#define MK9_LAUTA_VERSION "0.0.0"
#endif

namespace mk9::updater {
namespace {

namespace fs = std::filesystem;
using json = nlohmann::json;

// The release key's public half (P-256: X then Y). Releases are signed with its private half, which never leaves the
// maintainer's machine (tools/release/sign_release.py).
constexpr uint8_t kPublicKey[64] = {
    0x5d, 0x79, 0xd3, 0x43, 0x76, 0x8c, 0x5b, 0xd7, 0x41, 0x94, 0x01, 0x68, 0x3b, 0xaf, 0xe1, 0xc2,
    0x02, 0x69, 0xb4, 0x35, 0x24, 0x7e, 0x3b, 0x60, 0x98, 0x90, 0xdc, 0x97, 0xf8, 0x53, 0x03, 0xa9,
    0x98, 0xf8, 0x7f, 0x75, 0x37, 0xfa, 0x8c, 0x7f, 0x64, 0x9d, 0x89, 0x17, 0x78, 0xb6, 0x82, 0x30,
    0x10, 0x91, 0xf1, 0xf9, 0x68, 0x4d, 0x5e, 0x43, 0x19, 0x38, 0x8d, 0xbb, 0x72, 0xa2, 0xd0, 0x85,
};

// What the release has besides the program, and the player's own: never replaced.
bool Kept(const fs::path& relative) {
  if (relative.empty()) return false;
  const std::wstring first = relative.begin()->wstring();
  std::wstring lower = first;
  std::transform(lower.begin(), lower.end(), lower.begin(), ::towlower);
  return lower == L"mk9.toml" || lower == L"game_root" || lower == L"logs" || lower == L"cache" ||
         lower == L"update" || lower == L"captures" || lower.rfind(L"online_session", 0) == 0;
}

struct State {
  std::mutex mutex;
  Status status;
  std::string zip_url, sig_url, zip_name;
  uint64_t zip_size = 0;
  bool busy = false;         // the worker runs
  bool dismissed = false;    // "Más tarde": no banner until the next start
  bool auto_install = false; // mk9_update_test_auto
  bool show_notes = false;
};
State g;

fs::path ExeFolder() {
  wchar_t path[MAX_PATH] = {};
  GetModuleFileNameW(nullptr, path, MAX_PATH);
  return fs::path(path).parent_path();
}

std::string Utf8(const std::wstring& w) {
  if (w.empty()) return {};
  const int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), int(w.size()), nullptr, 0, nullptr, nullptr);
  std::string s(size_t(n), '\0');
  WideCharToMultiByte(CP_UTF8, 0, w.data(), int(w.size()), s.data(), n, nullptr, nullptr);
  return s;
}

std::wstring Wide(const std::string& s) {
  if (s.empty()) return {};
  const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), nullptr, 0);
  std::wstring w(size_t(n), L'\0');
  MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), w.data(), n);
  return w;
}

void SetStatus(Phase phase, const std::string& message, double progress = 0) {
  std::lock_guard lock(g.mutex);
  g.status.phase = phase;
  g.status.message = message;
  g.status.progress = progress;
}

// "v1.2.3" or "1.2.3": a comparable number (each part up to 999).
uint64_t VersionNumber(std::string v) {
  if (!v.empty() && (v[0] == 'v' || v[0] == 'V')) v.erase(0, 1);
  unsigned a = 0, b = 0, c = 0;
  if (std::sscanf(v.c_str(), "%u.%u.%u", &a, &b, &c) < 1) return 0;
  return uint64_t(a) * 1000000 + uint64_t(b) * 1000 + c;
}

// GET over HTTPS, the body to 'sink' as it comes (false from it stops); total: the Content-Length when there is one.
bool HttpGet(const std::string& url, const std::function<bool(const char*, size_t)>& sink, uint64_t* total,
             std::string* error) {
  const std::wstring wurl = Wide(url);
  URL_COMPONENTS parts{sizeof(parts)};
  wchar_t host[256] = {}, path[2048] = {};
  parts.lpszHostName = host;
  parts.dwHostNameLength = 256;
  parts.lpszUrlPath = path;
  parts.dwUrlPathLength = 2048;
  wchar_t extra[2048] = {};
  parts.lpszExtraInfo = extra;
  parts.dwExtraInfoLength = 2048;
  if (!WinHttpCrackUrl(wurl.c_str(), 0, 0, &parts)) {
    *error = "dirección inválida";
    return false;
  }
  const std::wstring agent = L"mk9-lauta/" + Wide(MK9_LAUTA_VERSION);
  HINTERNET session = WinHttpOpen(agent.c_str(), WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME,
                                  WINHTTP_NO_PROXY_BYPASS, 0);
  if (!session) {
    *error = "sin conexión";
    return false;
  }
  WinHttpSetTimeouts(session, 10000, 10000, 15000, 30000);
  bool ok = false;
  if (HINTERNET connect = WinHttpConnect(session, host, parts.nPort, 0)) {
    const std::wstring object = std::wstring(path) + extra;
    if (HINTERNET request = WinHttpOpenRequest(connect, L"GET", object.c_str(), nullptr, WINHTTP_NO_REFERER,
                                               WINHTTP_DEFAULT_ACCEPT_TYPES,
                                               parts.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0)) {
      const wchar_t* headers = L"Accept: application/vnd.github+json, application/octet-stream\r\n";
      if (WinHttpSendRequest(request, headers, DWORD(-1), WINHTTP_NO_REQUEST_DATA, 0, 0, 0) &&
          WinHttpReceiveResponse(request, nullptr)) {
        DWORD code = 0, size = sizeof(code);
        WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX,
                            &code, &size, WINHTTP_NO_HEADER_INDEX);
        if (code != 200) {
          *error = "respuesta " + std::to_string(code);
        } else {
          if (total) {
            wchar_t length[32] = {};
            DWORD bytes = sizeof(length);
            *total = WinHttpQueryHeaders(request, WINHTTP_QUERY_CONTENT_LENGTH, WINHTTP_HEADER_NAME_BY_INDEX, length,
                                         &bytes, WINHTTP_NO_HEADER_INDEX)
                         ? std::wcstoull(length, nullptr, 10)
                         : 0;
          }
          ok = true;
          std::vector<char> buffer(1 << 16);
          for (;;) {
            DWORD available = 0;
            if (!WinHttpQueryDataAvailable(request, &available)) {
              ok = false;
              *error = "la descarga se cortó";
              break;
            }
            if (!available) break;
            DWORD read = 0;
            if (!WinHttpReadData(request, buffer.data(), std::min<DWORD>(available, DWORD(buffer.size())), &read) ||
                !read) {
              ok = false;
              *error = "la descarga se cortó";
              break;
            }
            if (!sink(buffer.data(), read)) {
              ok = false;
              if (error->empty()) *error = "cancelada";
              break;
            }
          }
        }
      } else {
        *error = "sin conexión con GitHub";
      }
      WinHttpCloseHandle(request);
    }
    WinHttpCloseHandle(connect);
  }
  WinHttpCloseHandle(session);
  return ok;
}

// SHA-256 of a file.
bool Sha256(const fs::path& file, uint8_t out[32]) {
  BCRYPT_ALG_HANDLE alg = nullptr;
  if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0) != 0) return false;
  BCRYPT_HASH_HANDLE hash = nullptr;
  bool ok = BCryptCreateHash(alg, &hash, nullptr, 0, nullptr, 0, 0) == 0;
  std::ifstream in(file, std::ios::binary);
  std::vector<char> buffer(1 << 20);
  while (ok && in) {
    in.read(buffer.data(), std::streamsize(buffer.size()));
    const std::streamsize n = in.gcount();
    if (n > 0) ok = BCryptHashData(hash, reinterpret_cast<PUCHAR>(buffer.data()), ULONG(n), 0) == 0;
  }
  if (ok) ok = BCryptFinishHash(hash, out, 32, 0) == 0;
  if (hash) BCryptDestroyHash(hash);
  BCryptCloseAlgorithmProvider(alg, 0);
  return ok;
}

// The release key's signature (r and s, 64 bytes) of a SHA-256.
bool SignatureValid(const uint8_t digest[32], const std::vector<uint8_t>& signature) {
  if (signature.size() != 64) return false;
  BCRYPT_ALG_HANDLE alg = nullptr;
  if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_ECDSA_P256_ALGORITHM, nullptr, 0) != 0) return false;
  struct {
    BCRYPT_ECCKEY_BLOB header;
    uint8_t xy[64];
  } blob{};
  blob.header.dwMagic = BCRYPT_ECDSA_PUBLIC_P256_MAGIC;
  blob.header.cbKey = 32;
  std::memcpy(blob.xy, kPublicKey, 64);
  BCRYPT_KEY_HANDLE key = nullptr;
  bool ok = BCryptImportKeyPair(alg, nullptr, BCRYPT_ECCPUBLIC_BLOB, &key, reinterpret_cast<PUCHAR>(&blob),
                                sizeof(blob), 0) == 0;
  if (ok) {
    ok = BCryptVerifySignature(key, nullptr, const_cast<PUCHAR>(digest), 32,
                               const_cast<PUCHAR>(signature.data()), 64, 0) == 0;
    BCryptDestroyKey(key);
  }
  BCryptCloseAlgorithmProvider(alg, 0);
  return ok;
}

std::vector<uint8_t> FromHex(const std::string& text) {
  std::vector<uint8_t> out;
  int high = -1;
  for (char c : text) {
    int v = -1;
    if (c >= '0' && c <= '9') v = c - '0';
    else if (c >= 'a' && c <= 'f') v = c - 'a' + 10;
    else if (c >= 'A' && c <= 'F') v = c - 'A' + 10;
    else continue;
    if (high < 0) {
      high = v;
    } else {
      out.push_back(uint8_t(high << 4 | v));
      high = -1;
    }
  }
  return out;
}

// Runs a program without a window and waits for it (exit code, or -1).
int RunHidden(std::wstring command_line) {
  STARTUPINFOW si{sizeof(si)};
  PROCESS_INFORMATION pi{};
  if (!CreateProcessW(nullptr, command_line.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si,
                      &pi)) {
    return -1;
  }
  WaitForSingleObject(pi.hProcess, 10 * 60 * 1000);
  DWORD code = DWORD(-1);
  GetExitCodeProcess(pi.hProcess, &code);
  CloseHandle(pi.hThread);
  CloseHandle(pi.hProcess);
  return int(code);
}

// The zip unpacked into 'to': Windows' tar (10 1803 and later), else PowerShell.
bool Unpack(const fs::path& zip, const fs::path& to) {
  std::error_code ec;
  fs::remove_all(to, ec);
  fs::create_directories(to, ec);
  wchar_t system[MAX_PATH] = {};
  GetSystemDirectoryW(system, MAX_PATH);
  const fs::path tar = fs::path(system) / L"tar.exe";
  if (fs::exists(tar) &&
      RunHidden(L"\"" + tar.wstring() + L"\" -xf \"" + zip.wstring() + L"\" -C \"" + to.wstring() + L"\"") == 0) {
    return true;
  }
  fs::remove_all(to, ec);
  fs::create_directories(to, ec);
  return RunHidden(L"powershell -NoProfile -NonInteractive -Command \"Expand-Archive -LiteralPath '" + zip.wstring() +
                   L"' -DestinationPath '" + to.wstring() + L"' -Force\"") == 0;
}

fs::path UpdateFolder() { return ExeFolder() / L"update"; }

// The staged release that is ready to install, if any ("" if none): update\ready.txt holds its version.
std::string StagedVersion() {
  std::ifstream in(UpdateFolder() / L"ready.txt");
  std::string version;
  std::getline(in, version);
  return version;
}

void Check() {
  SetStatus(Phase::kChecking, "Buscando una versión nueva...");
  std::string body, error;
  const std::string api = !REXCVAR_GET(mk9_update_api).empty()
                              ? REXCVAR_GET(mk9_update_api)
                              : "https://api.github.com/repos/" + REXCVAR_GET(mk9_update_repo) + "/releases/latest";
  if (!HttpGet(api, [&](const char* d, size_t n) { body.append(d, n); return body.size() < (4u << 20); }, nullptr,
               &error)) {
    REXLOG_INFO("[update] the check failed: {}", error);
    SetStatus(Phase::kFailed, "No se pudo buscar una versión nueva (" + error + ").");
    return;
  }
  const json release = json::parse(body, nullptr, false);
  if (!release.is_object() || !release.contains("tag_name")) {
    SetStatus(Phase::kFailed, "GitHub no dio la última versión.");
    return;
  }
  const std::string tag = release.value("tag_name", "");
  std::string zip_url, sig_url, zip_name;
  uint64_t zip_size = 0;
  for (const json& asset : release.value("assets", json::array())) {
    const std::string name = asset.value("name", "");
    const std::string url = asset.value("browser_download_url", "");
    if (name.size() > 4 && name.rfind("-windows-x64.zip") == name.size() - 16) {
      zip_url = url;
      zip_name = name;
      zip_size = asset.value("size", uint64_t(0));
    } else if (name.size() > 4 && name.rfind("-windows-x64.zip.sig") == name.size() - 20) {
      sig_url = url;
    }
  }
  const std::string latest = tag.size() > 1 && (tag[0] == 'v' || tag[0] == 'V') ? tag.substr(1) : tag;
  const bool newer = VersionNumber(latest) > VersionNumber(MK9_LAUTA_VERSION);
  {
    std::lock_guard lock(g.mutex);
    g.status.latest = latest;
    g.status.page = release.value("html_url", "");
    g.status.notes = release.value("body", "");
    g.zip_url = zip_url;
    g.sig_url = sig_url;
    g.zip_name = zip_name;
    g.zip_size = zip_size;
  }
  REXLOG_INFO("[update] newest release {} ({}), this program {}", latest, newer ? "newer" : "not newer",
              MK9_LAUTA_VERSION);
  if (!newer) {
    SetStatus(Phase::kUpToDate, "Tenés la última versión.");
  } else if (StagedVersion() == latest) {
    SetStatus(Phase::kReady, "La v" + latest + " ya está bajada: se instala al reiniciar.", 1);
  } else if (zip_url.empty() || sig_url.empty()) {
    SetStatus(Phase::kAvailable, "Hay una versión nueva, pero sin el archivo firmado: bajala a mano.");
  } else {
    SetStatus(Phase::kAvailable, "Hay una versión nueva: v" + latest + ".");
    if (REXCVAR_GET(mk9_update_test_auto)) {
      std::lock_guard lock(g.mutex);
      g.auto_install = true;
    }
  }
}

void DownloadWorker() {
  std::string zip_url, sig_url, zip_name, latest;
  uint64_t expected = 0;
  {
    std::lock_guard lock(g.mutex);
    zip_url = g.zip_url;
    sig_url = g.sig_url;
    zip_name = g.zip_name;
    latest = g.status.latest;
    expected = g.zip_size;
  }
  const fs::path folder = UpdateFolder();
  std::error_code ec;
  fs::create_directories(folder, ec);
  fs::remove(folder / L"ready.txt", ec);
  const fs::path zip = folder / Wide(zip_name);
  const fs::path partial = fs::path(zip).concat(L".partial");
  std::string error;
  // the signature first (small)
  std::string sig_text;
  if (!HttpGet(sig_url, [&](const char* d, size_t n) { sig_text.append(d, n); return sig_text.size() < 4096; },
               nullptr, &error)) {
    SetStatus(Phase::kFailed, "No se pudo bajar la firma (" + error + ").");
    return;
  }
  {
    std::ofstream out(partial, std::ios::binary | std::ios::trunc);
    uint64_t got = 0, total = 0;
    bool ok = out && HttpGet(
                         zip_url,
                         [&](const char* d, size_t n) {
                           out.write(d, std::streamsize(n));
                           got += n;
                           const uint64_t all = total ? total : expected;
                           if (all) {
                             char text[96];
                             std::snprintf(text, sizeof(text), "Bajando la v%s... %.0f MB de %.0f MB", latest.c_str(),
                                           got / 1048576.0, all / 1048576.0);
                             SetStatus(Phase::kDownloading, text, std::min(1.0, double(got) / double(all)));
                           }
                           return bool(out);
                         },
                         &total, &error);
    out.close();
    if (!ok) {
      fs::remove(partial, ec);
      SetStatus(Phase::kFailed, "No se pudo bajar la v" + latest + " (" + error + ").");
      return;
    }
  }
  SetStatus(Phase::kDownloading, "Verificando la firma...", 1);
  uint8_t digest[32] = {};
  if (!Sha256(partial, digest) || !SignatureValid(digest, FromHex(sig_text))) {
    fs::remove(partial, ec);
    REXLOG_ERROR("[update] {}: the signature does not match: not installed", zip_name);
    SetStatus(Phase::kFailed, "La descarga no tiene una firma válida: no se instala.");
    return;
  }
  fs::remove(zip, ec);
  fs::rename(partial, zip, ec);
  SetStatus(Phase::kDownloading, "Preparando los archivos...", 1);
  if (!Unpack(zip, folder / L"staging")) {
    SetStatus(Phase::kFailed, "No se pudo descomprimir la v" + latest + ".");
    return;
  }
  fs::remove(zip, ec);
  std::ofstream(folder / L"ready.txt") << latest << "\n";
  REXLOG_INFO("[update] v{} downloaded, signature checked, unpacked: installs at the restart", latest);
  SetStatus(Phase::kReady, "La v" + latest + " está lista. Se instala al reiniciar.", 1);
  bool now = false;
  {
    std::lock_guard lock(g.mutex);
    now = g.auto_install;
  }
  if (now) InstallAndRestart();
}

template <typename Work>
void RunWorker(Work work) {
  {
    std::lock_guard lock(g.mutex);
    if (g.busy) return;
    g.busy = true;
  }
  std::thread([work] {
    work();
    std::lock_guard lock(g.mutex);
    g.busy = false;
  }).detach();
}

// The files of an update installed before (kept as .old while the old program ran) go; a staged release this program
// already is goes too.
void CleanUp() {
  const fs::path exe = ExeFolder();
  std::error_code ec;
  for (fs::recursive_directory_iterator it(exe, fs::directory_options::skip_permission_denied, ec), end;
       it != end; it.increment(ec)) {
    if (ec) break;
    if (it->is_directory(ec) && Kept(fs::relative(it->path(), exe, ec))) {
      it.disable_recursion_pending();
      continue;
    }
    if (it->is_regular_file(ec) && it->path().extension() == L".old") fs::remove(it->path(), ec);
  }
  const std::string staged = StagedVersion();
  if (!staged.empty() && VersionNumber(staged) <= VersionNumber(MK9_LAUTA_VERSION)) {
    fs::remove_all(UpdateFolder(), ec);
    REXLOG_INFO("[update] now on v{}: the update's files are gone", MK9_LAUTA_VERSION);
  }
}

// The banner over the game: a newer release, its download, its restart.
class Banner : public renut::gpu::overlay::Dialog {
 public:
  void Draw() override {
    Status s;
    bool dismissed, signed_zip;
    {
      std::lock_guard lock(g.mutex);
      s = g.status;
      dismissed = g.dismissed;
      signed_zip = !g.zip_url.empty() && !g.sig_url.empty();
    }
    const bool show = !dismissed && !mk9::netplay::Active() &&
                      (s.phase == Phase::kAvailable || s.phase == Phase::kDownloading || s.phase == Phase::kReady ||
                       (s.phase == Phase::kFailed && attempted_));
    if (!show) return;
    const ImGuiIO& io = ImGui::GetIO();
    ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f, 12), ImGuiCond_Always, ImVec2(0.5f, 0));
    ImGui::SetNextWindowSizeConstraints(ImVec2(360, 0), ImVec2(std::min(760.0f, io.DisplaySize.x - 24), 600));
    const ImGuiWindowFlags flags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                                   ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings |
                                   ImGuiWindowFlags_NoFocusOnAppearing;
    if (!ImGui::Begin("##update", nullptr, flags)) {
      ImGui::End();
      return;
    }
    ImGui::TextColored(ImVec4(1.0f, 0.82f, 0.35f, 1), "mk9-lauta v%s", s.latest.c_str());
    ImGui::SameLine();
    ImGui::TextDisabled("(tenés la v%s)", MK9_LAUTA_VERSION);
    ImGui::TextUnformatted(s.message.c_str());
    if (s.phase == Phase::kDownloading) {
      ImGui::ProgressBar(float(s.progress), ImVec2(-1, 0));
    }
    if (s.phase == Phase::kAvailable && signed_zip) {
      if (ImGui::Button("Actualizar ahora")) {
        attempted_ = true;
        Download();
      }
      ImGui::SameLine();
    }
    if (s.phase == Phase::kReady) {
      ImGui::TextDisabled("Tus partidas, opciones y archivos del juego no se tocan.");
      if (ImGui::Button("Reiniciar ahora")) InstallAndRestart();
      ImGui::SameLine();
    }
    if (s.phase == Phase::kFailed && !s.page.empty()) {
      if (ImGui::Button("Bajarla de GitHub")) Open(s.page);
      ImGui::SameLine();
    }
    if (s.phase != Phase::kDownloading) {
      if (ImGui::Button("Más tarde")) {
        std::lock_guard lock(g.mutex);
        g.dismissed = true;
      }
      ImGui::SameLine();
    }
    if (!s.notes.empty() && ImGui::Button(notes_ ? "Ocultar novedades" : "Novedades")) notes_ = !notes_;
    if (notes_ && !s.notes.empty()) {
      ImGui::BeginChild("##notes", ImVec2(std::min(720.0f, io.DisplaySize.x - 48), 260), true);
      ImGui::PushTextWrapPos(0);
      ImGui::TextUnformatted(s.notes.c_str());
      ImGui::PopTextWrapPos();
      ImGui::EndChild();
    }
    ImGui::End();
  }

 private:
  bool attempted_ = false;
  bool notes_ = false;
};

}  // namespace

void Install() {
  static std::once_flag once;
  std::call_once(once, [] {
    {
      std::lock_guard lock(g.mutex);
      g.status.current = MK9_LAUTA_VERSION;
    }
    CleanUp();
    static Banner banner;
    renut::gpu::overlay::AddDialog(&banner);
    if (REXCVAR_GET(mk9_update_check) && !mk9::netplay::Active()) {
      RunWorker([] {
        Check();
        bool now = false;
        {
          std::lock_guard lock(g.mutex);
          now = g.auto_install;
        }
        if (now) DownloadWorker();
      });
    }
  });
}

Status GetStatus() {
  std::lock_guard lock(g.mutex);
  return g.status;
}

void CheckNow() { RunWorker(&Check); }

void Download() {
  {
    std::lock_guard lock(g.mutex);
    if (g.zip_url.empty() || g.sig_url.empty()) return;
  }
  SetStatus(Phase::kDownloading, "Empezando la descarga...", 0);
  RunWorker(&DownloadWorker);
}

// The program in this folder (a new version just installed, or this one) started as this one was, and this one
// closed; returns only if it could not start.
static void StartAgain() {
  const fs::path exe_folder = ExeFolder();
  STARTUPINFOW si{sizeof(si)};
  PROCESS_INFORMATION pi{};
  const fs::path exe = exe_folder / L"mk9.exe";
  std::wstring command = L"\"" + exe.wstring() + L"\"";
  int argc = 0;
  if (wchar_t** argv = CommandLineToArgvW(GetCommandLineW(), &argc)) {
    for (int i = 1; i < argc; ++i) {
      const std::wstring arg = argv[i];
      if (arg.rfind(L"--mk9_update_test", 0) == 0) continue;
      command += L" \"" + arg + L"\"";
    }
    LocalFree(argv);
  }
  if (CreateProcessW(exe.c_str(), command.data(), nullptr, nullptr, FALSE, 0, nullptr, exe_folder.c_str(), &si,
                     &pi)) {
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    ExitProcess(0);
  }
}

void InstallAndRestart() {
  const fs::path exe_folder = ExeFolder();
  const fs::path staging = UpdateFolder() / L"staging";
  std::error_code ec;
  if (!fs::exists(staging, ec)) {
    SetStatus(Phase::kFailed, "No está la actualización bajada.");
    return;
  }
  // every file of the release, over the program's; the replaced ones as .old (a running program's file can be
  // renamed, not overwritten)
  std::vector<std::pair<fs::path, fs::path>> moved;  // (installed file, its .old or empty)
  bool ok = true;
  for (fs::recursive_directory_iterator it(staging, ec), end; ok && it != end; it.increment(ec)) {
    if (ec) {
      ok = false;
      break;
    }
    if (!it->is_regular_file(ec)) continue;
    const fs::path relative = fs::relative(it->path(), staging, ec);
    if (Kept(relative)) continue;
    const fs::path target = exe_folder / relative;
    fs::create_directories(target.parent_path(), ec);
    fs::path old;
    if (fs::exists(target, ec)) {
      old = fs::path(target).concat(L".old");
      fs::remove(old, ec);
      fs::rename(target, old, ec);
      if (ec) {
        ok = false;
        break;
      }
    }
    fs::rename(it->path(), target, ec);
    if (ec) {
      fs::copy_file(it->path(), target, fs::copy_options::overwrite_existing, ec);
    }
    if (ec) {
      if (!old.empty()) fs::rename(old, target, ec);
      ok = false;
      break;
    }
    moved.emplace_back(target, old);
  }
  if (!ok) {
    // back as it was
    for (auto it = moved.rbegin(); it != moved.rend(); ++it) {
      fs::remove(it->first, ec);
      if (!it->second.empty()) fs::rename(it->second, it->first, ec);
    }
    REXLOG_ERROR("[update] installing failed: the program's files are back as they were");
    SetStatus(Phase::kFailed, "No se pudo instalar (¿la carpeta del juego es de solo lectura?). Quedó como estaba.");
    return;
  }
  REXLOG_INFO("[update] {} files installed: starting the new version", moved.size());
  fs::remove_all(UpdateFolder(), ec);
  StartAgain();
  SetStatus(Phase::kFailed, "La versión nueva quedó instalada: cerrá y abrí el juego.");
}

void Restart() {
  REXLOG_INFO("[update] the game starts again");
  StartAgain();
  REXLOG_ERROR("[update] the game could not start again");
}

void Open(const std::string& what) {
  ShellExecuteW(nullptr, L"open", Wide(what).c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

std::string LogsFolder() { return Utf8((ExeFolder() / L"logs").wstring()); }

}  // namespace mk9::updater
