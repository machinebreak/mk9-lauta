// MK9 online: the players' profile pictures (avatars.h).

#include "online/avatars.h"

#include "imgui_internal.h"  // ImGui::RegisterUserTexture

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <objbase.h>
#include <propidl.h>
#include <shobjidl.h>
#include <wincodec.h>
#include <winhttp.h>
#include <wrl/client.h>

#include <algorithm>
#include <atomic>
#include <cfloat>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <deque>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include <fmt/format.h>
#include <rex/cvar.h>
#include <rex/logging.h>

#include "online/discord.h"
#include "online/i18n.h"
#include "renut_engine/gpu/overlay/overlay_ui.h"

#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "windowscodecs.lib")
#pragma comment(lib, "winhttp.lib")

REXCVAR_DEFINE_STRING(mk9_avatar_file, "online_avatar.jpg", "MK9 Online",
                      "The player's own profile picture's file beside the executable (tests: two games on one PC, each "
                      "its own)");
REXCVAR_DEFINE_BOOL(mk9_avatars_others, true, "MK9 Online",
                    "Show other players' profile pictures (the rival's in the room and the fight, the profiles'); a "
                    "silenced player's never show");

namespace mk9::avatars {
namespace {

using i18n::T;
using Microsoft::WRL::ComPtr;
using Clock = std::chrono::steady_clock;

constexpr size_t kKeep = 300;    // others' pictures kept in avatars\ (the newest)
constexpr size_t kTextures = 24;  // textures at once

// --- files ---

std::filesystem::path ExeDir() {
  wchar_t exe[MAX_PATH] = {};
  GetModuleFileNameW(nullptr, exe, MAX_PATH);
  return std::filesystem::path(exe).parent_path();
}
std::filesystem::path MinePath() {
  const std::string file = REXCVAR_GET(mk9_avatar_file);
  const bool plain = !file.empty() && file.find_first_of("/\\:") == std::string::npos;  // a name, not a path
  return ExeDir() / std::filesystem::u8path(plain ? file : std::string("online_avatar.jpg"));
}
std::filesystem::path CacheDir() { return ExeDir() / L"avatars"; }
std::filesystem::path CachePath(const std::string& hash) {
  return CacheDir() / (std::wstring(hash.begin(), hash.end()) + L".jpg");
}

std::string ReadFile(const std::filesystem::path& path) {
  std::ifstream in(path, std::ios::binary);
  if (!in) return {};
  std::string data((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  return data.size() <= kMaxBytes ? data : std::string();
}

bool WriteFile(const std::filesystem::path& path, const std::string& data) {
  std::error_code ec;
  std::filesystem::create_directories(path.parent_path(), ec);
  const std::filesystem::path part = path.wstring() + L".part";
  {
    std::ofstream out(part, std::ios::binary | std::ios::trunc);
    if (!out) return false;
    out.write(data.data(), std::streamsize(data.size()));
    if (!out) return false;
  }
  std::filesystem::rename(part, path, ec);
  return !ec;
}

std::wstring Wide(const std::string& text) {
  const int n = MultiByteToWideChar(CP_UTF8, 0, text.c_str(), int(text.size()), nullptr, 0);
  std::wstring out(size_t(std::max(n, 0)), L'\0');
  if (n > 0) MultiByteToWideChar(CP_UTF8, 0, text.c_str(), int(text.size()), out.data(), n);
  return out;
}

// --- the state the threads share ---

std::mutex g_mutex;
bool g_mine_loaded = false;
std::string g_mine_hash, g_mine_data;
std::string g_note;
std::atomic<bool> g_busy{false};
std::atomic<uint32_t> g_stored{0};  // bumped at each picture kept (a texture that had none tries again)
std::unordered_map<std::string, Clock::time_point> g_asked;

struct Decoded {
  std::string hash;
  uint32_t width = 0, height = 0;
  std::vector<uint8_t> rgba;  // empty: it could not be read
};
std::deque<Decoded> g_decoded;  // the pictures' thread to the overlay's

void LoadMineLocked() {
  if (g_mine_loaded) return;
  g_mine_loaded = true;
  g_mine_data = ReadFile(MinePath());
  g_mine_hash = g_mine_data.empty() ? std::string() : HashOf(g_mine_data);
}

void Finish(const std::string& note) {
  {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_note = note;
  }
  g_busy.store(false);
}

// The pictures' thread: one, with COM (multithreaded) and a WIC factory of its own; the jobs in order.
class Worker {
 public:
  using Job = std::function<void(IWICImagingFactory*)>;
  void Post(Job job) {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      if (!started_) {
        started_ = true;
        std::thread([this] { Run(); }).detach();
      }
      jobs_.push_back(std::move(job));
    }
    cv_.notify_one();
  }

 private:
  void Run() {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    ComPtr<IWICImagingFactory> factory;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory)))) {
      REXLOG_WARN("[avatars] Windows' imaging (WIC) is not there: no profile pictures");
    }
    for (;;) {
      Job job;
      {
        std::unique_lock<std::mutex> lock(mutex_);
        cv_.wait(lock, [this] { return !jobs_.empty(); });
        job = std::move(jobs_.front());
        jobs_.pop_front();
      }
      job(factory.Get());
    }
  }
  std::mutex mutex_;
  std::condition_variable cv_;
  std::deque<Job> jobs_;
  bool started_ = false;
};

Worker& Pictures() {
  static Worker worker;
  return worker;
}

// --- WIC (the pictures' thread) ---

// A picture's pixels, RGBA. strict (another player's): a JPEG of exactly kSize x kSize.
bool Pixels(IWICImagingFactory* wic, const std::string& data, bool strict, std::vector<uint8_t>* rgba, UINT* w,
            UINT* h) {
  if (!wic || data.empty()) return false;
  ComPtr<IWICStream> stream;
  if (FAILED(wic->CreateStream(&stream)) ||
      FAILED(stream->InitializeFromMemory(reinterpret_cast<BYTE*>(const_cast<char*>(data.data())), DWORD(data.size())))) {
    return false;
  }
  ComPtr<IWICBitmapDecoder> decoder;
  if (FAILED(wic->CreateDecoderFromStream(stream.Get(), nullptr, WICDecodeMetadataCacheOnDemand, &decoder))) return false;
  GUID format{};
  if (strict && (FAILED(decoder->GetContainerFormat(&format)) || format != GUID_ContainerFormatJpeg)) return false;
  ComPtr<IWICBitmapFrameDecode> frame;
  if (FAILED(decoder->GetFrame(0, &frame))) return false;
  UINT fw = 0, fh = 0;
  if (FAILED(frame->GetSize(&fw, &fh)) || !fw || !fh || fw > 1024 || fh > 1024) return false;
  if (strict && (fw != UINT(kSize) || fh != UINT(kSize))) return false;
  ComPtr<IWICFormatConverter> converter;
  if (FAILED(wic->CreateFormatConverter(&converter)) ||
      FAILED(converter->Initialize(frame.Get(), GUID_WICPixelFormat32bppRGBA, WICBitmapDitherTypeNone, nullptr, 0.0,
                                   WICBitmapPaletteTypeCustom))) {
    return false;
  }
  rgba->assign(size_t(fw) * fh * 4, 0);
  if (FAILED(converter->CopyPixels(nullptr, fw * 4, UINT(rgba->size()), rgba->data()))) return false;
  *w = fw;
  *h = fh;
  return true;
}

// How the camera held it (EXIF's orientation, 1-8; 1 for none).
UINT OrientationOf(IWICBitmapFrameDecode* frame) {
  ComPtr<IWICMetadataQueryReader> reader;
  if (FAILED(frame->GetMetadataQueryReader(&reader))) return 1;
  UINT out = 1;
  for (const wchar_t* query : {L"/app1/ifd/{ushort=274}", L"/ifd/{ushort=274}"}) {
    PROPVARIANT value;
    PropVariantInit(&value);
    if (SUCCEEDED(reader->GetMetadataByName(query, &value))) {
      if (value.vt == VT_UI2) out = value.uiVal;
      PropVariantClear(&value);
      break;
    }
  }
  return out >= 1 && out <= 8 ? out : 1;
}

std::string EncodeJpeg(IWICImagingFactory* wic, const std::vector<uint8_t>& bgr, float quality) {
  ComPtr<IStream> stream;
  if (FAILED(CreateStreamOnHGlobal(nullptr, TRUE, &stream))) return {};
  ComPtr<IWICBitmapEncoder> encoder;
  if (FAILED(wic->CreateEncoder(GUID_ContainerFormatJpeg, nullptr, &encoder)) ||
      FAILED(encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache))) {
    return {};
  }
  ComPtr<IWICBitmapFrameEncode> frame;
  ComPtr<IPropertyBag2> options;
  if (FAILED(encoder->CreateNewFrame(&frame, &options))) return {};
  PROPBAG2 option{};
  option.pstrName = const_cast<LPOLESTR>(L"ImageQuality");
  VARIANT value{};
  value.vt = VT_R4;
  value.fltVal = quality;
  options->Write(1, &option, &value);
  WICPixelFormatGUID format = GUID_WICPixelFormat24bppBGR;
  if (FAILED(frame->Initialize(options.Get())) || FAILED(frame->SetSize(kSize, kSize)) ||
      FAILED(frame->SetPixelFormat(&format)) || format != GUID_WICPixelFormat24bppBGR ||
      FAILED(frame->WritePixels(kSize, kSize * 3, UINT(bgr.size()), const_cast<BYTE*>(bgr.data()))) ||
      FAILED(frame->Commit()) || FAILED(encoder->Commit())) {
    return {};
  }
  STATSTG stat{};
  if (FAILED(stream->Stat(&stat, STATFLAG_NONAME))) return {};
  std::string out(size_t(stat.cbSize.QuadPart), '\0');
  LARGE_INTEGER zero{};
  ULONG read = 0;
  if (FAILED(stream->Seek(zero, STREAM_SEEK_SET, nullptr)) || FAILED(stream->Read(out.data(), ULONG(out.size()), &read))) {
    return {};
  }
  out.resize(read);
  return out;
}

ComPtr<IWICBitmapScaler> Scaler(IWICImagingFactory* wic, IWICBitmapSource* source, UINT w, UINT h,
                                WICBitmapInterpolationMode mode) {
  ComPtr<IWICBitmapScaler> scaler;
  if (FAILED(wic->CreateBitmapScaler(&scaler)) || FAILED(scaler->Initialize(source, w, h, mode))) return nullptr;
  return scaler;
}

// Any picture as a profile's: the square from its middle, turned as the camera held it, kSize px, over the screens'
// dark where it is transparent, a JPEG of kMaxBytes at most. "" when it cannot be made.
std::string MakeAvatar(IWICImagingFactory* wic, IWICBitmapSource* source, UINT orientation) {
  ComPtr<IWICFormatConverter> premultiplied;
  if (FAILED(wic->CreateFormatConverter(&premultiplied)) ||
      FAILED(premultiplied->Initialize(source, GUID_WICPixelFormat32bppPRGBA, WICBitmapDitherTypeNone, nullptr, 0.0,
                                       WICBitmapPaletteTypeCustom))) {
    return {};
  }
  UINT w = 0, h = 0;
  if (FAILED(premultiplied->GetSize(&w, &h)) || !w || !h) return {};
  // a big photo: its shorter side down to twice the picture's first, then held in memory
  ComPtr<IWICBitmapSource> current = premultiplied;
  if (const double k = double(kSize * 2) / double(std::min(w, h)); k < 1.0) {
    ComPtr<IWICBitmapScaler> smaller = Scaler(wic, current.Get(), std::max(1u, UINT(w * k + 0.5)),
                                              std::max(1u, UINT(h * k + 0.5)), WICBitmapInterpolationModeFant);
    if (!smaller) return {};
    current = smaller;
  }
  ComPtr<IWICBitmap> held;
  if (FAILED(wic->CreateBitmapFromSource(current.Get(), WICBitmapCacheOnLoad, &held))) return {};
  current = held;
  WICBitmapTransformOptions turn = WICBitmapTransformRotate0;
  switch (orientation) {
    case 2: turn = WICBitmapTransformFlipHorizontal; break;
    case 3: turn = WICBitmapTransformRotate180; break;
    case 4: turn = WICBitmapTransformFlipVertical; break;
    case 5: turn = WICBitmapTransformOptions(WICBitmapTransformRotate270 | WICBitmapTransformFlipHorizontal); break;
    case 6: turn = WICBitmapTransformRotate90; break;
    case 7: turn = WICBitmapTransformOptions(WICBitmapTransformRotate90 | WICBitmapTransformFlipHorizontal); break;
    case 8: turn = WICBitmapTransformRotate270; break;
    default: break;
  }
  if (turn != WICBitmapTransformRotate0) {
    ComPtr<IWICBitmapFlipRotator> rotator;
    ComPtr<IWICBitmap> turned;
    if (SUCCEEDED(wic->CreateBitmapFlipRotator(&rotator)) && SUCCEEDED(rotator->Initialize(current.Get(), turn)) &&
        SUCCEEDED(wic->CreateBitmapFromSource(rotator.Get(), WICBitmapCacheOnLoad, &turned))) {
      current = turned;
    }
  }
  if (FAILED(current->GetSize(&w, &h)) || !w || !h) return {};
  const UINT side = std::min(w, h);
  const WICRect square{INT((w - side) / 2), INT((h - side) / 2), INT(side), INT(side)};
  ComPtr<IWICBitmapClipper> clipper;
  if (FAILED(wic->CreateBitmapClipper(&clipper)) || FAILED(clipper->Initialize(current.Get(), &square))) return {};
  ComPtr<IWICBitmapScaler> scaler =
      Scaler(wic, clipper.Get(), kSize, kSize, WICBitmapInterpolationMode(4));  // HighQualityCubic: Windows 10 on
  if (!scaler) scaler = Scaler(wic, clipper.Get(), kSize, kSize, WICBitmapInterpolationModeFant);
  if (!scaler) return {};
  std::vector<uint8_t> rgba(size_t(kSize) * kSize * 4);
  if (FAILED(scaler->CopyPixels(nullptr, kSize * 4, UINT(rgba.size()), rgba.data()))) return {};
  // over the screens' dark (premultiplied: color + dark x (1 - alpha)), as the JPEG's BGR
  constexpr int kBackR = 24, kBackG = 18, kBackB = 20;
  std::vector<uint8_t> bgr(size_t(kSize) * kSize * 3);
  for (size_t i = 0; i < size_t(kSize) * kSize; ++i) {
    const int a = rgba[i * 4 + 3];
    bgr[i * 3 + 0] = uint8_t(std::min(255, rgba[i * 4 + 2] + kBackB * (255 - a) / 255));
    bgr[i * 3 + 1] = uint8_t(std::min(255, rgba[i * 4 + 1] + kBackG * (255 - a) / 255));
    bgr[i * 3 + 2] = uint8_t(std::min(255, rgba[i * 4 + 0] + kBackR * (255 - a) / 255));
  }
  for (float quality = 0.88f; quality > 0.35f; quality -= 0.12f) {
    std::string jpeg = EncodeJpeg(wic, bgr, quality);
    if (jpeg.empty()) return {};
    if (jpeg.size() <= kMaxBytes) return jpeg;
  }
  return {};
}

void SetMine(const std::string& jpeg, const char* how) {
  if (jpeg.empty() || !WriteFile(MinePath(), jpeg)) {
    Finish(T("No se pudo guardar la foto."));
    return;
  }
  const std::string hash = HashOf(jpeg);
  WriteFile(CachePath(hash), jpeg);
  {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_mine_loaded = true;
    g_mine_hash = hash;
    g_mine_data = jpeg;
  }
  g_stored.fetch_add(1);
  REXLOG_INFO("[avatars] the profile picture is now {} ({}, {} bytes)", hash, how, jpeg.size());
  Finish(T("Foto de perfil guardada."));
}

void FromFile(IWICImagingFactory* wic, const std::wstring& path) {
  ComPtr<IWICBitmapDecoder> decoder;
  ComPtr<IWICBitmapFrameDecode> frame;
  if (!wic ||
      FAILED(wic->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnDemand,
                                            &decoder)) ||
      FAILED(decoder->GetFrame(0, &frame))) {
    REXLOG_WARN("[avatars] a picture chosen did not open");
    Finish(T("No se pudo abrir esa imagen: probá con una JPG o PNG."));
    return;
  }
  SetMine(MakeAvatar(wic, frame.Get(), OrientationOf(frame.Get())), "a file");
}

// A file of Discord's servers (https://cdn.discordapp.com<path>); "" on failure. 4 MB at most.
std::string Download(const std::wstring& path) {
  std::string body;
  HINTERNET session = WinHttpOpen(L"mk9-lauta", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME,
                                  WINHTTP_NO_PROXY_BYPASS, 0);
  if (!session) return body;
  WinHttpSetTimeouts(session, 5000, 5000, 8000, 8000);
  if (HINTERNET connect = WinHttpConnect(session, L"cdn.discordapp.com", INTERNET_DEFAULT_HTTPS_PORT, 0)) {
    if (HINTERNET request = WinHttpOpenRequest(connect, L"GET", path.c_str(), nullptr, WINHTTP_NO_REFERER,
                                               WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE)) {
      DWORD status = 0, size = sizeof(status);
      if (WinHttpSendRequest(request, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) &&
          WinHttpReceiveResponse(request, nullptr) &&
          WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, nullptr, &status, &size,
                              nullptr) &&
          status == 200) {
        DWORD available = 0;
        while (WinHttpQueryDataAvailable(request, &available) && available && body.size() < (4u << 20)) {
          std::string chunk(available, '\0');
          DWORD read = 0;
          if (!WinHttpReadData(request, chunk.data(), available, &read) || !read) break;
          body.append(chunk.data(), read);
        }
      }
      WinHttpCloseHandle(request);
    }
    WinHttpCloseHandle(connect);
  }
  WinHttpCloseHandle(session);
  return body.size() < (4u << 20) ? body : std::string();
}

bool Digits(const std::string& s) {
  return !s.empty() && s.size() <= 24 && std::all_of(s.begin(), s.end(), [](char c) { return c >= '0' && c <= '9'; });
}
// Discord's picture names: hex digits, "a_" before an animated one's.
bool AvatarId(const std::string& s) {
  return !s.empty() && s.size() <= 40 && std::all_of(s.begin(), s.end(), [](char c) {
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
  });
}

void FromDiscord(IWICImagingFactory* wic) {
  std::string id, avatar;
  if (!discord::User(&id, &avatar, 3000)) {
    Finish(T("Abrí Discord en esta PC y probá de nuevo."));
    return;
  }
  if (avatar.empty()) {
    Finish(T("Tu cuenta de Discord no tiene foto."));
    return;
  }
  if (!Digits(id) || !AvatarId(avatar)) {
    Finish(T("No se pudo bajar tu foto de Discord."));
    return;
  }
  const std::string png = Download(L"/avatars/" + std::wstring(id.begin(), id.end()) + L"/" +
                                   std::wstring(avatar.begin(), avatar.end()) + L".png?size=256");
  ComPtr<IWICStream> stream;
  ComPtr<IWICBitmapDecoder> decoder;
  ComPtr<IWICBitmapFrameDecode> frame;
  if (png.empty() || !wic || FAILED(wic->CreateStream(&stream)) ||
      FAILED(stream->InitializeFromMemory(reinterpret_cast<BYTE*>(const_cast<char*>(png.data())), DWORD(png.size()))) ||
      FAILED(wic->CreateDecoderFromStream(stream.Get(), nullptr, WICDecodeMetadataCacheOnDemand, &decoder)) ||
      FAILED(decoder->GetFrame(0, &frame))) {
    Finish(T("No se pudo bajar tu foto de Discord."));
    return;
  }
  SetMine(MakeAvatar(wic, frame.Get(), 1), "Discord's");
}

// The newest kKeep of the others' pictures stay (the player's own always).
void Prune(const std::string& keep) {
  std::error_code ec;
  std::vector<std::pair<std::filesystem::file_time_type, std::filesystem::path>> files;
  for (const auto& entry : std::filesystem::directory_iterator(CacheDir(), ec)) {
    if (entry.path().extension() != L".jpg") continue;
    files.emplace_back(entry.last_write_time(ec), entry.path());
  }
  if (files.size() <= kKeep) return;
  std::sort(files.begin(), files.end());
  for (size_t i = 0; i + kKeep < files.size(); ++i) {
    if (files[i].second.stem().string() != keep) std::filesystem::remove(files[i].second, ec);
  }
}

void PushDecoded(Decoded decoded) {
  std::lock_guard<std::mutex> lock(g_mutex);
  g_decoded.push_back(std::move(decoded));
}

// --- textures (the overlay's thread) ---

struct Texture {
  enum State { kLoading, kReady, kMissing } state = kLoading;
  ImTextureData* data = nullptr;
  int used = 0;          // the ImGui frame it was last drawn at
  uint32_t stored = 0;   // kMissing: g_stored then (another picture kept since: try again)
};
std::unordered_map<std::string, Texture> g_textures;
std::vector<ImTextureData*> g_dying;  // asked to go: deleted once the overlay let them go

void Decode(const std::string& hash) {
  Pictures().Post([hash](IWICImagingFactory* wic) {
    Decoded decoded;
    decoded.hash = hash;
    std::string data = ReadFile(CachePath(hash));
    if (data.empty() && hash == MineHash()) data = MineData();
    std::vector<uint8_t> rgba;
    UINT w = 0, h = 0;
    if (HashOf(data) == hash && Pixels(wic, data, true, &rgba, &w, &h)) {
      decoded.width = w;
      decoded.height = h;
      decoded.rgba = std::move(rgba);
    }
    PushDecoded(std::move(decoded));
  });
}

ImTextureData* NewTexture(const Decoded& d) {
  auto* texture = IM_NEW(ImTextureData)();
  texture->Create(ImTextureFormat_RGBA32, int(d.width), int(d.height));
  std::memcpy(texture->GetPixels(), d.rgba.data(), d.rgba.size());
  texture->UseColors = true;
  texture->SetStatus(ImTextureStatus_WantCreate);
  ImGui::RegisterUserTexture(texture);
  return texture;
}

// The decoded pictures into textures, the least used past kTextures let go, the ones let go deleted.
void Pump() {
  std::deque<Decoded> decoded;
  {
    std::lock_guard<std::mutex> lock(g_mutex);
    decoded.swap(g_decoded);
  }
  const int frame = ImGui::GetFrameCount();
  for (Decoded& d : decoded) {
    auto it = g_textures.find(d.hash);
    if (it == g_textures.end() || it->second.state == Texture::kReady) continue;
    if (d.rgba.empty()) {
      it->second.state = Texture::kMissing;
      it->second.stored = g_stored.load();
      continue;
    }
    it->second.data = NewTexture(d);
    it->second.state = Texture::kReady;
    it->second.used = frame;
  }
  for (auto it = g_dying.begin(); it != g_dying.end();) {
    if ((*it)->Status == ImTextureStatus_Destroyed) {
      ImGui::UnregisterUserTexture(*it);
      IM_DELETE(*it);
      it = g_dying.erase(it);
    } else {
      ++it;
    }
  }
  size_t ready = 0;
  for (const auto& [hash, t] : g_textures) ready += t.state == Texture::kReady;
  while (ready > kTextures) {
    auto oldest = g_textures.end();
    for (auto it = g_textures.begin(); it != g_textures.end(); ++it) {
      if (it->second.state != Texture::kReady || it->second.used >= frame - 1) continue;
      if (oldest == g_textures.end() || it->second.used < oldest->second.used) oldest = it;
    }
    if (oldest == g_textures.end()) break;
    oldest->second.data->SetStatus(ImTextureStatus_WantDestroy);
    g_dying.push_back(oldest->second.data);
    g_textures.erase(oldest);
    --ready;
  }
}

ImTextureData* TextureOf(const std::string& hash) {
  Pump();
  if (hash.empty()) return nullptr;
  auto it = g_textures.find(hash);
  if (it == g_textures.end()) {
    g_textures.emplace(hash, Texture{});
    Decode(hash);
    return nullptr;
  }
  Texture& t = it->second;
  t.used = ImGui::GetFrameCount();
  if (t.state == Texture::kMissing && t.stored != g_stored.load()) {
    t.state = Texture::kLoading;
    Decode(hash);
  }
  return t.state == Texture::kReady ? t.data : nullptr;
}

uint32_t Fnv(const std::string& s) {
  uint32_t h = 2166136261u;
  for (unsigned char c : s) {
    h ^= c;
    h *= 16777619u;
  }
  return h;
}

}  // namespace

std::string MineHash() {
  std::lock_guard<std::mutex> lock(g_mutex);
  LoadMineLocked();
  return g_mine_hash;
}

std::string MineData() {
  std::lock_guard<std::mutex> lock(g_mutex);
  LoadMineLocked();
  return g_mine_data;
}

void ChooseFile() {
  if (g_busy.exchange(true)) return;
  {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_note.clear();
  }
  const HWND owner = GetForegroundWindow();
  const std::wstring title = Wide(T("Elegí tu foto de perfil"));
  const std::wstring kind = Wide(T("Imágenes"));
  std::thread([owner, title, kind] {
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    std::wstring path;
    IFileOpenDialog* dialog = nullptr;
    if (SUCCEEDED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dialog)))) {
      const COMDLG_FILTERSPEC types[] = {
          {kind.c_str(), L"*.jpg;*.jpeg;*.jfif;*.png;*.bmp;*.gif;*.webp;*.tif;*.tiff;*.heic"}, {L"*.*", L"*.*"}};
      dialog->SetFileTypes(2, types);
      dialog->SetTitle(title.c_str());
      DWORD options = 0;
      if (SUCCEEDED(dialog->GetOptions(&options))) {
        dialog->SetOptions(options | FOS_FORCEFILESYSTEM | FOS_FILEMUSTEXIST);
      }
      if (SUCCEEDED(dialog->Show(owner))) {
        IShellItem* item = nullptr;
        if (SUCCEEDED(dialog->GetResult(&item)) && item) {
          PWSTR chosen = nullptr;
          if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &chosen)) && chosen) {
            path = chosen;
            CoTaskMemFree(chosen);
          }
          item->Release();
        }
      }
      dialog->Release();
    }
    CoUninitialize();
    if (path.empty()) {
      g_busy.store(false);
      return;
    }
    Pictures().Post([path](IWICImagingFactory* wic) { FromFile(wic, path); });
  }).detach();
}

void UseDiscord() {
  if (g_busy.exchange(true)) return;
  {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_note.clear();
  }
  Pictures().Post([](IWICImagingFactory* wic) { FromDiscord(wic); });
}

void Remove() {
  std::error_code ec;
  std::filesystem::remove(MinePath(), ec);
  {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_mine_loaded = true;
    g_mine_hash.clear();
    g_mine_data.clear();
    g_note = T("Sin foto de perfil.");
  }
  REXLOG_INFO("[avatars] the profile picture removed");
}

bool Busy() { return g_busy.load(); }

std::string Note() {
  std::lock_guard<std::mutex> lock(g_mutex);
  return g_note;
}

bool Have(const std::string& hash) {
  if (CleanHash(hash).empty()) return false;
  if (hash == MineHash()) return true;
  std::error_code ec;
  return std::filesystem::exists(CachePath(hash), ec);
}

std::string Read(const std::string& hash) {
  if (CleanHash(hash).empty()) return {};
  if (hash == MineHash()) return MineData();
  std::string data = ReadFile(CachePath(hash));
  return HashOf(data) == hash ? data : std::string();
}

void Store(const std::string& hash_in, const std::string& jpeg) {
  const std::string hash = CleanHash(hash_in);
  if (hash.empty() || jpeg.empty() || jpeg.size() > kMaxBytes || HashOf(jpeg) != hash || Have(hash)) return;
  Pictures().Post([hash, jpeg](IWICImagingFactory* wic) {
    std::vector<uint8_t> rgba;
    UINT w = 0, h = 0;
    if (!Pixels(wic, jpeg, true, &rgba, &w, &h)) {
      REXLOG_WARN("[avatars] a picture from another game is not a {}x{} JPEG: dropped", kSize, kSize);
      return;
    }
    if (!WriteFile(CachePath(hash), jpeg)) return;
    Prune(MineHash());
    g_stored.fetch_add(1);
    Decoded decoded{hash, w, h, std::move(rgba)};  // its texture, if one waits for it, from these pixels
    PushDecoded(std::move(decoded));
    REXLOG_INFO("[avatars] kept another player's picture {} ({} bytes)", hash, jpeg.size());
  });
}

bool Wanted(const std::string& hash_in) {
  const std::string hash = CleanHash(hash_in);
  if (hash.empty() || Have(hash)) return false;
  std::lock_guard<std::mutex> lock(g_mutex);
  const auto now = Clock::now();
  auto& at = g_asked[hash];
  if (at != Clock::time_point{} && now - at < std::chrono::seconds(20)) return false;
  at = now;
  return true;
}

bool ShowOthers() { return REXCVAR_GET(mk9_avatars_others); }

bool Draw(ImDrawList* list, const std::string& hash, const ImVec2& pos, float size, float rounding, ImU32 border,
          float border_size) {
  if (!list || hash.empty()) return false;
  ImTextureData* texture = TextureOf(hash);
  if (!texture) return false;
  const ImVec2 end(pos.x + size, pos.y + size);
  list->AddImageRounded(texture->GetTexRef(), pos, end, ImVec2(0, 0), ImVec2(1, 1), IM_COL32_WHITE, rounding);
  if (border_size > 0.0f && (border >> IM_COL32_A_SHIFT)) list->AddRect(pos, end, border, rounding, 0, border_size);
  return true;
}

void Item(const std::string& hash, const std::string& name, float size) {
  const ImVec2 at = ImGui::GetCursorScreenPos();
  ImDrawList* list = ImGui::GetWindowDrawList();
  const float rounding = size * 0.18f;
  const ImU32 border = IM_COL32(255, 196, 64, 150);
  const float border_size = std::max(1.0f, size / 64.0f);
  if (!Draw(list, hash, at, size, rounding, border, border_size)) {
    // the name's first letter on a color of its own (a hue from the name)
    const uint32_t h = Fnv(name);
    ImVec4 color;
    ImGui::ColorConvertHSVtoRGB(float(h % 360) / 360.0f, 0.55f, 0.55f, color.x, color.y, color.z);
    color.w = 1.0f;
    const ImVec2 end(at.x + size, at.y + size);
    list->AddRectFilled(at, end, ImGui::ColorConvertFloat4ToU32(color), rounding);
    list->AddRect(at, end, border, rounding, 0, border_size);
    std::string letter;
    if (!name.empty()) {
      const unsigned char c = static_cast<unsigned char>(name[0]);
      const size_t n = c < 0x80 ? 1 : (c >> 5) == 0x6 ? 2 : (c >> 4) == 0xE ? 3 : (c >> 3) == 0x1E ? 4 : 1;
      letter = name.substr(0, std::min(n, name.size()));
      if (letter.size() == 1 && letter[0] >= 'a' && letter[0] <= 'z') letter[0] = char(letter[0] - 'a' + 'A');
    }
    ImFont* font = renut::gpu::overlay::TitleFont();
    if (!font) font = ImGui::GetFont();
    const float font_size = size * 0.58f;
    const ImVec2 extent = font->CalcTextSizeA(font_size, FLT_MAX, 0.0f, letter.c_str());
    list->AddText(font, font_size, ImVec2(at.x + (size - extent.x) * 0.5f, at.y + (size - extent.y) * 0.5f),
                  IM_COL32(255, 255, 255, 235), letter.c_str());
  }
  ImGui::Dummy(ImVec2(size, size));
}

std::string Base64(const std::string& bytes) {
  static const char kAlphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  std::string out;
  out.reserve((bytes.size() + 2) / 3 * 4);
  size_t i = 0;
  for (; i + 2 < bytes.size(); i += 3) {
    const uint32_t v = uint32_t(uint8_t(bytes[i])) << 16 | uint32_t(uint8_t(bytes[i + 1])) << 8 | uint8_t(bytes[i + 2]);
    out += kAlphabet[v >> 18];
    out += kAlphabet[(v >> 12) & 63];
    out += kAlphabet[(v >> 6) & 63];
    out += kAlphabet[v & 63];
  }
  if (i < bytes.size()) {
    uint32_t v = uint32_t(uint8_t(bytes[i])) << 16;
    if (i + 1 < bytes.size()) v |= uint32_t(uint8_t(bytes[i + 1])) << 8;
    out += kAlphabet[v >> 18];
    out += kAlphabet[(v >> 12) & 63];
    out += i + 1 < bytes.size() ? kAlphabet[(v >> 6) & 63] : '=';
    out += '=';
  }
  return out;
}

std::string Unbase64(const std::string& text) {
  auto value = [](char c) -> int {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
  };
  if (text.size() % 4 != 0 || text.size() > (kMaxBytes + 2) / 3 * 4) return {};
  std::string out;
  out.reserve(text.size() / 4 * 3);
  for (size_t i = 0; i < text.size(); i += 4) {
    int v[4];
    for (int k = 0; k < 4; ++k) {
      const char c = text[i + k];
      v[k] = c == '=' && i + 4 == text.size() && k >= 2 ? 0 : value(c);
      if (v[k] < 0) return {};
    }
    const uint32_t n = uint32_t(v[0]) << 18 | uint32_t(v[1]) << 12 | uint32_t(v[2]) << 6 | uint32_t(v[3]);
    out += char(n >> 16);
    if (text[i + 2] != '=') out += char((n >> 8) & 0xFF);
    if (text[i + 3] != '=') out += char(n & 0xFF);
  }
  return out;
}

std::string HashOf(const std::string& bytes) {
  if (bytes.empty()) return {};
  uint64_t h = 1469598103934665603ull;
  for (unsigned char c : bytes) {
    h ^= c;
    h *= 1099511628211ull;
  }
  return fmt::format("{:016x}", h);
}

std::string CleanHash(const std::string& hash) {
  if (hash.size() != 16) return {};
  for (char c : hash) {
    if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return {};
  }
  return hash;
}

}  // namespace mk9::avatars
