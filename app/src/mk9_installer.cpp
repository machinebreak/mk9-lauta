// mk9-lauta (2026-11-09): the first start's installer (mk9_installer.h).

#include "mk9_installer.h"

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <bcrypt.h>
#include <commctrl.h>
#include <shobjidl.h>
#else
// mk9-lauta (2026-11-13): Linux: the disc read by pread, SHA-256 by mk9_sha256.h, the window a small one of the game's
// (mk9_small_window.h: SDL's, drawn with Dear ImGui), SDL's file dialogs
#include <fcntl.h>
#include <unistd.h>

#include <cerrno>
#include <ctime>
#include <optional>

#include <SDL3/SDL.h>

#include "imgui.h"
#include "mk9_platform.h"
#include "mk9_sha256.h"
#include "mk9_small_window.h"
#endif

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>
#include <mutex>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <vector>

#include <rex/logging.h>
#include <toml++/toml.hpp>

#if defined(MK9_BUILTIN_CONFIG)
#include "mk9_builtin_config.h"  // kMk9GameFiles: app/mk9_game_files.txt
#endif

#if defined(_WIN32)
#pragma comment(lib, "bcrypt.lib")
#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "ole32.lib")
// Windows' own look for the installer's buttons and bar (common controls 6)
#pragma comment(linker, "\"/manifestdependency:type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")
#endif

namespace mk9::installer {
namespace {

namespace fs = std::filesystem;

// --- The game's files: path, size, SHA-256 (app/mk9_game_files.txt) ------------------------------------------------

struct GameFile {
  std::string path;  // '/' separated, as in the list
  uint64_t size = 0;
  std::array<uint8_t, 32> sha256{};
};

std::string Lower(std::string_view text) {
  std::string out(text);
  for (char& c : out) c = char(std::tolower(static_cast<unsigned char>(c)));
  return out;
}

std::vector<GameFile> GameFiles() {
  std::vector<GameFile> files;
#if defined(MK9_BUILTIN_CONFIG)
  std::string_view text = kMk9GameFiles;
  while (!text.empty()) {
    const size_t end = text.find('\n');
    std::string_view line = text.substr(0, end);
    text = end == std::string_view::npos ? std::string_view() : text.substr(end + 1);
    if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
    if (line.empty() || line[0] == '#') continue;
    const size_t a = line.find(' ');
    const size_t b = a == std::string_view::npos ? a : line.find(' ', a + 1);
    if (b == std::string_view::npos || b - a - 1 != 64) continue;
    GameFile f;
    f.size = std::strtoull(std::string(line.substr(0, a)).c_str(), nullptr, 10);
    for (size_t i = 0; i < 32; ++i) {
      f.sha256[i] = uint8_t(std::stoul(std::string(line.substr(a + 1 + 2 * i, 2)), nullptr, 16));
    }
    f.path = std::string(line.substr(b + 1));
    files.push_back(std::move(f));
  }
#endif
  return files;
}

#if defined(_WIN32)
// --- SHA-256 (Windows' CNG) -----------------------------------------------------------------------------------------

class Sha256 {
 public:
  Sha256() {
    if (BCryptOpenAlgorithmProvider(&alg_, BCRYPT_SHA256_ALGORITHM, nullptr, 0) == 0) {
      BCryptCreateHash(alg_, &hash_, nullptr, 0, nullptr, 0, 0);
    }
  }
  ~Sha256() {
    if (hash_) BCryptDestroyHash(hash_);
    if (alg_) BCryptCloseAlgorithmProvider(alg_, 0);
  }
  void Update(const void* data, size_t size) {
    if (hash_) BCryptHashData(hash_, static_cast<PUCHAR>(const_cast<void*>(data)), ULONG(size), 0);
  }
  std::array<uint8_t, 32> Finish() {
    std::array<uint8_t, 32> out{};
    if (hash_) BCryptFinishHash(hash_, out.data(), ULONG(out.size()), 0);
    return out;
  }

 private:
  BCRYPT_ALG_HANDLE alg_ = nullptr;
  BCRYPT_HASH_HANDLE hash_ = nullptr;
};
#else
// --- SHA-256 (Linux: mk9_sha256.h, the system's libcrypto, else picohash) -------------------------------------------

class Sha256 {
 public:
  Sha256() : hash_(mk9_sha256_begin()) {}
  ~Sha256() {
    if (hash_) {
      unsigned char unused[32];
      mk9_sha256_end(hash_, unused);
    }
  }
  Sha256(const Sha256&) = delete;
  Sha256& operator=(const Sha256&) = delete;
  void Update(const void* data, size_t size) { mk9_sha256_add(hash_, data, size); }
  std::array<uint8_t, 32> Finish() {
    std::array<uint8_t, 32> out{};
    if (hash_) mk9_sha256_end(hash_, out.data());
    hash_ = nullptr;
    return out;
  }

 private:
  mk9_sha256* hash_ = nullptr;
};
#endif

// --- An Xbox 360 disc image (XDVDFS: the game partition's directory trees) -----------------------------------------

class Disc {
 public:
  struct Entry {
    uint64_t offset = 0;
    uint64_t size = 0;
  };

  ~Disc() {
#if defined(_WIN32)
    if (file_ != INVALID_HANDLE_VALUE) CloseHandle(file_);
#else
    if (fd_ >= 0) close(fd_);
#endif
  }

  bool Open(const fs::path& path, std::string& error) {
#if defined(_WIN32)
    file_ = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                        FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (file_ == INVALID_HANDLE_VALUE) {
#else
    fd_ = open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd_ >= 0) posix_fadvise(fd_, 0, 0, POSIX_FADV_SEQUENTIAL);
    if (fd_ < 0) {
#endif
      error = "No se pudo abrir el archivo.";
      return false;
    }
    // where the game partition starts: a plain image, or a whole disc of each Xbox 360 disc generation
    static const uint64_t kPartitions[] = {0, 0x2080000, 0xFD90000, 0x18300000};
    for (uint64_t partition : kPartitions) {
      uint8_t sector[2048];
      if (!Read(partition + 32 * 2048, sector, sizeof(sector))) continue;
      if (std::memcmp(sector, "MICROSOFT*XBOX*MEDIA", 20) != 0) continue;
      const uint32_t root_sector = Get32(sector + 20), root_size = Get32(sector + 24);
      std::set<uint64_t> seen;
      Walk(partition, root_sector, root_size, "", 0, seen);
      if (!files_.empty()) return true;
    }
    error = "No es una imagen de un disco de Xbox 360.";
    return false;
  }

  const Entry* Find(std::string_view path) const {
    auto it = files_.find(Lower(path));
    return it == files_.end() ? nullptr : &it->second;
  }

  bool Read(uint64_t offset, void* out, size_t size) {
#if !defined(_WIN32)
    auto* bytes = static_cast<uint8_t*>(out);
    while (size > 0) {
      const ssize_t got = pread(fd_, bytes, std::min<size_t>(size, 1 << 24), off_t(offset));
      if (got < 0 && errno == EINTR) continue;
      if (got <= 0) return false;
      bytes += got;
      size -= size_t(got);
      offset += uint64_t(got);
    }
    return true;
#else
    LARGE_INTEGER at;
    at.QuadPart = LONGLONG(offset);
    if (!SetFilePointerEx(file_, at, nullptr, FILE_BEGIN)) return false;
    auto* bytes = static_cast<uint8_t*>(out);
    while (size > 0) {
      DWORD got = 0;
      const DWORD want = DWORD(std::min<size_t>(size, 1 << 24));
      if (!ReadFile(file_, bytes, want, &got, nullptr) || got == 0) return false;
      bytes += got;
      size -= got;
    }
    return true;
#endif
  }

 private:
  static uint32_t Get32(const uint8_t* p) { return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24; }
  static uint16_t Get16(const uint8_t* p) { return uint16_t(p[0] | p[1] << 8); }

  // One directory's table: a binary tree of entries (left and right children as offsets in 4-byte words).
  void Walk(uint64_t partition, uint32_t sector, uint32_t size, const std::string& prefix, int depth,
            std::set<uint64_t>& seen) {
    if (size == 0 || size > (64u << 20) || depth > 32) return;
    const uint64_t at = partition + uint64_t(sector) * 2048;
    if (!seen.insert(at).second) return;
    std::vector<uint8_t> table(size);
    if (!Read(at, table.data(), size)) return;
    std::vector<uint32_t> stack{0};
    std::set<uint32_t> visited;
    while (!stack.empty()) {
      const uint32_t off = stack.back();
      stack.pop_back();
      if (off + 14 > size || !visited.insert(off).second) continue;
      const uint8_t* e = table.data() + off;
      const uint16_t left = Get16(e), right = Get16(e + 2);
      if (left == 0xFFFF && right == 0xFFFF) continue;  // padding
      const uint32_t start = Get32(e + 4), length = Get32(e + 8);
      const uint8_t attributes = e[12], name_length = e[13];
      if (off + 14 + name_length > size) continue;
      const std::string name(reinterpret_cast<const char*>(e + 14), name_length);
      if (left) stack.push_back(uint32_t(left) * 4);
      if (right) stack.push_back(uint32_t(right) * 4);
      if (name.empty()) continue;
      const std::string path = prefix.empty() ? name : prefix + "/" + name;
      if (attributes & 0x10) {
        Walk(partition, start, length, path, depth + 1, seen);
      } else {
        files_[Lower(path)] = Entry{partition + uint64_t(start) * 2048, length};
      }
    }
  }

#if defined(_WIN32)
  HANDLE file_ = INVALID_HANDLE_VALUE;
#else
  int fd_ = -1;
#endif
  std::map<std::string, Entry> files_;
};

// --- The work: from an image or a folder ----------------------------------------------------------------------------

struct Job {
  std::atomic<bool> running{false}, done{false}, ok{false}, cancel{false};
  std::atomic<uint64_t> bytes_done{0}, bytes_total{0};
  std::mutex mutex;
  std::string status;  // the file being written, or the error
  fs::path result;
};

void SetStatus(Job& job, std::string text) {
  std::lock_guard lock(job.mutex);
  job.status = std::move(text);
}

#if defined(_WIN32)
std::string Utf8(const std::wstring& text) {
  if (text.empty()) return {};
  const int size = WideCharToMultiByte(CP_UTF8, 0, text.data(), int(text.size()), nullptr, 0, nullptr, nullptr);
  std::string out(size_t(std::max(size, 0)), '\0');
  WideCharToMultiByte(CP_UTF8, 0, text.data(), int(text.size()), out.data(), size, nullptr, nullptr);
  return out;
}

std::wstring Wide(std::string_view text) {
  if (text.empty()) return {};
  const int size = MultiByteToWideChar(CP_UTF8, 0, text.data(), int(text.size()), nullptr, 0);
  std::wstring out(size_t(std::max(size, 0)), L'\0');
  MultiByteToWideChar(CP_UTF8, 0, text.data(), int(text.size()), out.data(), size);
  return out;
}
#else
// paths are UTF-8 bytes on Linux: the list's as they are
std::string Wide(std::string_view text) { return std::string(text); }

// A game file in a folder the player chose, as it is on the disk: Linux's file systems tell "dlc" from "DLC" (the
// game's own, the SDK's, does not; nor Windows'): each name the list's, else the one there that differs only in case.
fs::path OnDisk(const fs::path& folder, const std::string& relative) {
  fs::path at = folder;
  size_t start = 0;
  while (start < relative.size()) {
    const size_t slash = relative.find('/', start);
    const std::string part = relative.substr(start, slash == std::string::npos ? std::string::npos : slash - start);
    start = slash == std::string::npos ? relative.size() : slash + 1;
    if (part.empty()) continue;
    std::error_code ec;
    if (fs::exists(at / part, ec)) {
      at /= part;
      continue;
    }
    fs::path found;
    for (const auto& entry : fs::directory_iterator(at, ec)) {
      if (Lower(entry.path().filename().string()) == Lower(part)) {
        found = entry.path();
        break;
      }
    }
    at = found.empty() ? at / part : found;
  }
  return at;
}
#endif

const char* kWrongVersion =
    "no es la versión que usa mk9-lauta (Mortal Kombat Komplete Edition de Xbox 360, la europea: En, Fr, De, Es, "
    "It, Pt).";

bool InstallFromDisc(const fs::path& image, const fs::path& exe_folder, Job& job) {
  SetStatus(job, "Leyendo la imagen...");
  Disc disc;
  std::string error;
  if (!disc.Open(image, error)) {
    SetStatus(job, error);
    return false;
  }
  const std::vector<GameFile> files = GameFiles();
  if (files.empty()) {
    SetStatus(job, "Esta copia del juego no trae la lista de archivos.");
    return false;
  }
  // the right game: default.xex first, then every file the game uses
  uint64_t total = 0;
  for (const GameFile& f : files) {
    const Disc::Entry* e = disc.Find(f.path);
    if (!e) {
      SetStatus(job, "A la imagen le falta " + f.path + ": " + kWrongVersion);
      return false;
    }
    if (e->size != f.size) {
      SetStatus(job, "En la imagen, " + f.path + " tiene otro tamaño: " + kWrongVersion);
      return false;
    }
    total += f.size;
  }
  job.bytes_total = total;
#if defined(_WIN32)
  ULARGE_INTEGER free_bytes{};
  if (GetDiskFreeSpaceExW(exe_folder.c_str(), &free_bytes, nullptr, nullptr) &&
      free_bytes.QuadPart < total + (256ull << 20)) {
    SetStatus(job, "No hay lugar: hacen falta " + std::to_string((total >> 30) + 1) + " GB libres en " +
                       Utf8(exe_folder.root_name().wstring()) + ".");
    return false;
  }
#else
  std::error_code space_ec;
  const fs::space_info space = fs::space(exe_folder, space_ec);
  if (!space_ec && space.available < total + (256ull << 20)) {
    SetStatus(job, "No hay lugar: hacen falta " + std::to_string((total >> 30) + 1) + " GB libres en " +
                       exe_folder.string() + ".");
    return false;
  }
#endif
  std::error_code ec;
  const fs::path partial = exe_folder / "game_root.partial";
  fs::remove_all(partial, ec);  // a cut install of ours
  fs::create_directories(partial, ec);
  if (ec) {
#if defined(_WIN32)
    SetStatus(job, "No se puede escribir en " + Utf8(exe_folder.wstring()) +
                       ": poné la carpeta del juego en otro lugar (por ejemplo Documentos).");
#else
    SetStatus(job, "No se puede escribir en " + exe_folder.string() +
                       ": poné la carpeta del juego en otro lugar (por ejemplo en tu carpeta personal).");
#endif
    return false;
  }
  // default.xex first: a wrong version stops before 7 GB are copied
  std::vector<const GameFile*> order;
  for (const GameFile& f : files) order.push_back(&f);
  std::stable_partition(order.begin(), order.end(), [](const GameFile* f) { return Lower(f->path) == "default.xex"; });
  std::vector<uint8_t> buffer(4 << 20);
  for (const GameFile* f : order) {
    if (job.cancel) {
      SetStatus(job, "Cancelado.");
      fs::remove_all(partial, ec);
      return false;
    }
    SetStatus(job, f->path);
    const Disc::Entry* e = disc.Find(f->path);
    const fs::path target = partial / fs::path(Wide(f->path));
    fs::create_directories(target.parent_path(), ec);
#if defined(_WIN32)
    HANDLE out = CreateFileW(target.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (out == INVALID_HANDLE_VALUE) {
#else
    std::FILE* out = platform::OpenFile(target, "wb");
    if (!out) {
#endif
      SetStatus(job, "No se pudo escribir " + f->path + ".");
      fs::remove_all(partial, ec);
      return false;
    }
    Sha256 sha;
    uint64_t left = f->size, at = e->offset;
    bool fine = true;
    while (left > 0 && fine) {
      const size_t chunk = size_t(std::min<uint64_t>(left, buffer.size()));
      if (!disc.Read(at, buffer.data(), chunk)) {
        SetStatus(job, "No se pudo leer " + f->path + " de la imagen (¿está dañada?).");
        fine = false;
        break;
      }
      sha.Update(buffer.data(), chunk);
#if defined(_WIN32)
      DWORD wrote = 0;
      if (!WriteFile(out, buffer.data(), DWORD(chunk), &wrote, nullptr) || wrote != chunk) {
#else
      if (std::fwrite(buffer.data(), 1, chunk, out) != chunk) {
#endif
        SetStatus(job, "No se pudo escribir " + f->path + " (¿se llenó el disco?).");
        fine = false;
        break;
      }
      left -= chunk;
      at += chunk;
      job.bytes_done += chunk;
      if (job.cancel) break;
    }
#if defined(_WIN32)
    CloseHandle(out);
#else
    if (std::fclose(out) != 0 && fine) {  // what was still buffered is written at the close: a full disk shows here
      SetStatus(job, "No se pudo escribir " + f->path + " (¿se llenó el disco?).");
      fine = false;
    }
#endif
    if (fine && !job.cancel && sha.Finish() != f->sha256) {
      SetStatus(job, f->path + " no coincide con el original: " +
                         (Lower(f->path) == "default.xex" ? std::string(kWrongVersion)
                                                          : std::string("la imagen está dañada.")));
      fine = false;
    }
    if (!fine || job.cancel) {
      if (job.cancel) SetStatus(job, "Cancelado.");
      fs::remove_all(partial, ec);
      return false;
    }
  }
  // the finished folder takes game_root's place (an old one is kept aside, never deleted)
  const fs::path root = exe_folder / "game_root";
  if (fs::exists(root, ec)) {
#if defined(_WIN32)
    fs::rename(root, exe_folder / ("game_root.old-" + std::to_string(GetTickCount64())), ec);
#else
    fs::rename(root, exe_folder / ("game_root.old-" + std::to_string(std::time(nullptr))), ec);
#endif
  }
  fs::rename(partial, root, ec);
  if (ec) {
    SetStatus(job, "No se pudo renombrar game_root.partial a game_root.");
    return false;
  }
  job.result = root;
  return true;
}

// The player's mk9.toml: game_data_root set to this folder (the line replaced, or added).
void KeepGameRoot(const fs::path& exe_folder, const fs::path& root) {
  const fs::path config = exe_folder / "mk9.toml";
  std::vector<std::string> lines;
  {
    std::ifstream in(config, std::ios::binary);
    for (std::string line; std::getline(in, line);) {
      if (!line.empty() && line.back() == '\r') line.pop_back();
      lines.push_back(line);
    }
  }
  std::string value;
#if defined(_WIN32)
  for (char c : Utf8(root.wstring())) {
#else
  for (char c : root.string()) {
#endif
    if (c == '\\' || c == '"') value.push_back('\\');
    value.push_back(c);
  }
  const std::string entry = "game_data_root = \"" + value + "\"";
  bool replaced = false;
  for (std::string& line : lines) {
    if (line.rfind("game_data_root", 0) == 0) {
      line = entry;
      replaced = true;
    }
  }
  if (!replaced) lines.push_back(entry);
  std::ofstream out(config, std::ios::binary | std::ios::trunc);
  for (const std::string& line : lines) out << line << "\r\n";
}

bool InstallFromFolder(const fs::path& folder, const fs::path& exe_folder, Job& job) {
  SetStatus(job, "Revisando la carpeta...");
  const std::vector<GameFile> files = GameFiles();
  std::error_code ec;
  std::vector<std::string> problems;
  uint64_t total = 0;
  for (const GameFile& f : files) total += f.size;
  job.bytes_total = total;
  for (const GameFile& f : files) {
#if defined(_WIN32)
    const fs::path path = folder / fs::path(Wide(f.path));
#else
    const fs::path path = OnDisk(folder, f.path);
#endif
    const uint64_t size = fs::file_size(path, ec);
    if (ec) {
      problems.push_back(f.path + ": falta");
    } else if (size != f.size) {
      problems.push_back(f.path + ": otro tamaño");
    } else if (Lower(f.path) == "default.xex") {
      // the right version: default.xex's hash
      std::ifstream in(path, std::ios::binary);
      Sha256 sha;
      std::vector<char> buffer(1 << 20);
      while (in) {
        in.read(buffer.data(), std::streamsize(buffer.size()));
        if (in.gcount() > 0) sha.Update(buffer.data(), size_t(in.gcount()));
      }
      if (sha.Finish() != f.sha256) {
        SetStatus(job, std::string("El default.xex de esa carpeta ") + kWrongVersion);
        return false;
      }
    }
    job.bytes_done += f.size;
    if (job.cancel) {
      SetStatus(job, "Cancelado.");
      return false;
    }
  }
  if (!problems.empty()) {
    std::string text = std::to_string(problems.size()) + " archivo(s) del juego faltan o están incompletos en esa carpeta (" +
                       problems.front() + (problems.size() > 1 ? ", ..." : "") +
                       "). Elegí la carpeta donde está default.xex, o la ISO.";
    SetStatus(job, text);
    return false;
  }
  KeepGameRoot(exe_folder, folder);
  job.result = folder;
  return true;
}

// --- The window ---------------------------------------------------------------------------------------------------

#if defined(_WIN32)
enum : int { kIdIso = 101, kIdFolder = 102, kIdQuit = 103, kTimer = 1 };

struct Window {
  HWND hwnd = nullptr, title = nullptr, body = nullptr, iso = nullptr, folder = nullptr, quit = nullptr,
       bar = nullptr, status = nullptr;
  HFONT font = nullptr, title_font = nullptr;
  UINT dpi = 96;
  fs::path exe_folder;
  Job job;
  std::thread worker;
  fs::path chosen;
  ULONGLONG closing_at = 0;
  bool auto_started = false;
};
Window* g_window = nullptr;

int Scale(int value) { return MulDiv(value, int(g_window->dpi), 96); }

void SetText(HWND control, const std::string& text) { SetWindowTextW(control, Wide(text).c_str()); }

void Start(bool image, const fs::path& path) {
  Window& w = *g_window;
  if (w.job.running) return;
  if (w.worker.joinable()) w.worker.join();
  w.job.done = false;
  w.job.ok = false;
  w.job.cancel = false;
  w.job.bytes_done = 0;
  w.job.bytes_total = 0;
  w.job.running = true;
  EnableWindow(w.iso, FALSE);
  EnableWindow(w.folder, FALSE);
  SetText(w.quit, "Cancelar");
  ShowWindow(w.bar, SW_SHOW);
  SendMessageW(w.bar, PBM_SETPOS, 0, 0);
  REXLOG_INFO("[installer] from {}", Utf8(path.wstring()));
  w.worker = std::thread([image, path] {
    Window& w = *g_window;
    const bool ok = image ? InstallFromDisc(path, w.exe_folder, w.job) : InstallFromFolder(path, w.exe_folder, w.job);
    w.job.ok = ok;
    w.job.running = false;
    w.job.done = true;
  });
}

fs::path Pick(HWND owner, bool folder) {
  fs::path result;
  IFileOpenDialog* dialog = nullptr;
  if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dialog)))) return result;
  DWORD options = 0;
  dialog->GetOptions(&options);
  dialog->SetOptions(options | FOS_FORCEFILESYSTEM | (folder ? FOS_PICKFOLDERS : 0));
  if (folder) {
    dialog->SetTitle(L"Elegí la carpeta del juego (la que tiene default.xex)");
  } else {
    static const COMDLG_FILTERSPEC kTypes[] = {{L"Imagen de disco de Xbox 360 (*.iso)", L"*.iso"},
                                               {L"Todos los archivos", L"*.*"}};
    dialog->SetFileTypes(2, kTypes);
    dialog->SetTitle(L"Elegí la ISO del juego");
  }
  if (SUCCEEDED(dialog->Show(owner))) {
    IShellItem* item = nullptr;
    if (SUCCEEDED(dialog->GetResult(&item))) {
      PWSTR path = nullptr;
      if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path))) {
        result = path;
        CoTaskMemFree(path);
      }
      item->Release();
    }
  }
  dialog->Release();
  return result;
}

LRESULT CALLBACK WindowProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
  Window& w = *g_window;
  switch (msg) {
    case WM_COMMAND:
      switch (LOWORD(wparam)) {
        case kIdIso:
          if (fs::path p = Pick(hwnd, false); !p.empty()) Start(true, p);
          return 0;
        case kIdFolder:
          if (fs::path p = Pick(hwnd, true); !p.empty()) Start(false, p);
          return 0;
        case kIdQuit:
          if (w.job.running) {
            w.job.cancel = true;
          } else {
            DestroyWindow(hwnd);
          }
          return 0;
        default:
          break;
      }
      break;
    case WM_TIMER: {
      if (!w.auto_started) {
        w.auto_started = true;
        char test[MAX_PATH] = {};
        if (GetEnvironmentVariableA("MK9_TEST_INSTALL", test, sizeof(test))) {
          const fs::path path(Wide(test));
          std::error_code ec;
          Start(!fs::is_directory(path, ec), path);
        }
      }
      if (w.closing_at && GetTickCount64() >= w.closing_at) {
        DestroyWindow(hwnd);
        return 0;
      }
      const uint64_t total = w.job.bytes_total, done = w.job.bytes_done;
      std::string status;
      {
        std::lock_guard lock(w.job.mutex);
        status = w.job.status;
      }
      if (w.job.running) {
        const int permille = total ? int(done * 1000 / total) : 0;
        SendMessageW(w.bar, PBM_SETPOS, WPARAM(permille), 0);
        SetText(w.status, total ? status + "  (" + std::to_string(permille / 10) + " %, " +
                                      std::to_string(done >> 20) + " de " + std::to_string(total >> 20) + " MB)"
                                : status);
      } else if (w.job.done) {
        w.job.done = false;
        if (w.worker.joinable()) w.worker.join();
        if (w.job.ok) {
          SendMessageW(w.bar, PBM_SETPOS, 1000, 0);
          SetText(w.status, "¡Listo! Arrancando el juego...");
          REXLOG_INFO("[installer] done: {}", Utf8(w.job.result.wstring()));
          w.chosen = w.job.result;
          w.closing_at = GetTickCount64() + 900;
        } else {
          SetText(w.status, status);
          REXLOG_WARN("[installer] {}", status);
          EnableWindow(w.iso, TRUE);
          EnableWindow(w.folder, TRUE);
          SetText(w.quit, "Salir");
        }
      }
      return 0;
    }
    case WM_CTLCOLORSTATIC: {
      HDC dc = reinterpret_cast<HDC>(wparam);
      SetBkMode(dc, TRANSPARENT);
      if (reinterpret_cast<HWND>(lparam) == w.title) SetTextColor(dc, RGB(150, 20, 20));
      return reinterpret_cast<LRESULT>(GetSysColorBrush(COLOR_WINDOW));
    }
    case WM_CLOSE:
      if (w.job.running) {
        w.job.cancel = true;
        return 0;
      }
      DestroyWindow(hwnd);
      return 0;
    case WM_DESTROY:
      KillTimer(hwnd, kTimer);
      PostQuitMessage(0);
      return 0;
    default:
      break;
  }
  return DefWindowProcW(hwnd, msg, wparam, lparam);
}

void RunWindow() {
  Window& w = *g_window;
  SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
  CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
  INITCOMMONCONTROLSEX controls{sizeof(controls), ICC_PROGRESS_CLASS | ICC_STANDARD_CLASSES};
  InitCommonControlsEx(&controls);
  HINSTANCE instance = GetModuleHandleW(nullptr);
  WNDCLASSEXW wc{sizeof(wc)};
  wc.lpfnWndProc = WindowProc;
  wc.hInstance = instance;
  wc.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512));  // IDC_ARROW (this build has no UNICODE)
  wc.hbrBackground = GetSysColorBrush(COLOR_WINDOW);
  wc.hIcon = LoadIconW(instance, MAKEINTRESOURCEW(1));
  wc.lpszClassName = L"mk9LautaInstaller";
  RegisterClassExW(&wc);
  w.dpi = GetDpiForSystem();
  const int width = Scale(620), height = Scale(330);
  RECT area{0, 0, width, height};
  AdjustWindowRectExForDpi(&area, WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX, FALSE, 0, w.dpi);
  const int ww = area.right - area.left, wh = area.bottom - area.top;
  w.hwnd = CreateWindowExW(0, wc.lpszClassName, L"mk9-lauta - Instalación",
                           WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
                           (GetSystemMetrics(SM_CXSCREEN) - ww) / 2, (GetSystemMetrics(SM_CYSCREEN) - wh) / 2, ww, wh,
                           nullptr, nullptr, instance, nullptr);
  NONCLIENTMETRICSW metrics{sizeof(metrics)};
  SystemParametersInfoForDpi(SPI_GETNONCLIENTMETRICS, sizeof(metrics), &metrics, 0, w.dpi);
  metrics.lfMessageFont.lfHeight = -MulDiv(10, int(w.dpi), 72);
  w.font = CreateFontIndirectW(&metrics.lfMessageFont);
  LOGFONTW title = metrics.lfMessageFont;
  title.lfHeight = -MulDiv(16, int(w.dpi), 72);
  title.lfWeight = FW_BOLD;
  w.title_font = CreateFontIndirectW(&title);
  auto make = [&](const wchar_t* cls, const std::string& text, DWORD style, int x, int y, int cw, int ch, int id) {
    HWND control = CreateWindowExW(0, cls, Wide(text).c_str(), WS_CHILD | WS_VISIBLE | style, Scale(x), Scale(y),
                                   Scale(cw), Scale(ch), w.hwnd, reinterpret_cast<HMENU>(intptr_t(id)), instance,
                                   nullptr);
    SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(w.font), TRUE);
    return control;
  };
  w.title = make(L"STATIC", "mk9-lauta", 0, 24, 18, 572, 34, 0);
  SendMessageW(w.title, WM_SETFONT, reinterpret_cast<WPARAM>(w.title_font), TRUE);
  w.body = make(L"STATIC",
                "Para jugar hace falta tu copia de Mortal Kombat Komplete Edition de Xbox 360. Elegí la imagen .iso "
                "del disco, o la carpeta donde ya la tenés extraída (la que tiene default.xex). Se revisa que sea la "
                "versión correcta y se prepara una sola vez: desde la ISO se copian ~7 GB al lado del juego.",
                0, 24, 60, 572, 92, 0);
  w.iso = make(L"BUTTON", "Elegir la ISO...", BS_PUSHBUTTON, 24, 164, 200, 36, kIdIso);
  w.folder = make(L"BUTTON", "Elegir una carpeta...", BS_PUSHBUTTON, 236, 164, 200, 36, kIdFolder);
  w.quit = make(L"BUTTON", "Salir", BS_PUSHBUTTON, 476, 164, 120, 36, kIdQuit);
  w.bar = make(PROGRESS_CLASSW, "", 0, 24, 222, 572, 22, 0);
  SendMessageW(w.bar, PBM_SETRANGE32, 0, 1000);
  ShowWindow(w.bar, SW_HIDE);
  w.status = make(L"STATIC", "", SS_LEFT, 24, 252, 572, 60, 0);
  ShowWindow(w.hwnd, SW_SHOW);
  SetForegroundWindow(w.hwnd);
  SetTimer(w.hwnd, kTimer, 100, nullptr);
  MSG msg;
  while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
    if (!IsDialogMessageW(w.hwnd, &msg)) {
      TranslateMessage(&msg);
      DispatchMessageW(&msg);
    }
  }
  if (w.worker.joinable()) {
    w.job.cancel = true;
    w.worker.join();
  }
  DeleteObject(w.font);
  DeleteObject(w.title_font);
  CoUninitialize();
}
#else  // Linux

// mk9-lauta (2026-11-13): Linux: a small window of the game's (mk9_small_window.h: SDL's, drawn with Dear ImGui and the
// screens' Barlow fonts); the same words, buttons, bar and steps as Windows' window. The ISO or the folder may also be
// dropped on the window: a desktop with neither the XDG portal nor zenity has no file dialog.

struct Window {
  fs::path exe_folder;
  Job job;
  std::thread worker;
  fs::path chosen;
  uint64_t closing_at = 0;  // SDL_GetTicks' ms
  bool auto_started = false;
  bool quit = false;
  bool show_bar = false;
  float progress = 0.0f;
  std::string status;  // the status line
  std::atomic<bool> dialog_open{false};
  std::mutex picked_mutex;
  std::optional<std::pair<bool, fs::path>> picked;  // a dialog's answer (an image?, its path), for the loop
  bool dialog_failed = false;                       // picked_mutex
};
Window* g_window = nullptr;

void Start(bool image, const fs::path& path) {
  Window& w = *g_window;
  if (w.job.running) return;
  if (w.worker.joinable()) w.worker.join();
  w.job.done = false;
  w.job.ok = false;
  w.job.cancel = false;
  w.job.bytes_done = 0;
  w.job.bytes_total = 0;
  w.job.running = true;
  w.show_bar = true;
  w.progress = 0.0f;
  REXLOG_INFO("[installer] from {}", path.string());
  w.worker = std::thread([image, path] {
    Window& w = *g_window;
    const bool ok = image ? InstallFromDisc(path, w.exe_folder, w.job) : InstallFromFolder(path, w.exe_folder, w.job);
    w.job.ok = ok;
    w.job.running = false;
    w.job.done = true;
  });
}

// SDL's dialog's answer (on whatever thread SDL gives it: zenity's is a thread of its own).
void SDLCALL Picked(void* image, const char* const* files, int) {
  Window& w = *g_window;
  std::lock_guard lock(w.picked_mutex);
  w.dialog_open = false;
  if (!files) {
    REXLOG_WARN("[installer] the file dialog did not open: {}", SDL_GetError());
    w.dialog_failed = true;
    return;
  }
  if (files[0]) w.picked = std::make_pair(image != nullptr, fs::path(files[0]));
}

void Pick(bool folder) {
  Window& w = *g_window;
  if (w.dialog_open.exchange(true)) return;
  static const SDL_DialogFileFilter kTypes[] = {{"Imagen de disco de Xbox 360 (*.iso)", "iso;ISO"},
                                                {"Todos los archivos", "*"}};
  const SDL_PropertiesID props = SDL_CreateProperties();
  SDL_SetPointerProperty(props, SDL_PROP_FILE_DIALOG_WINDOW_POINTER, small_window::Current());
  if (folder) {
    SDL_SetStringProperty(props, SDL_PROP_FILE_DIALOG_TITLE_STRING,
                          "Elegí la carpeta del juego (la que tiene default.xex)");
  } else {
    SDL_SetStringProperty(props, SDL_PROP_FILE_DIALOG_TITLE_STRING, "Elegí la ISO del juego");
    SDL_SetPointerProperty(props, SDL_PROP_FILE_DIALOG_FILTERS_POINTER, const_cast<SDL_DialogFileFilter*>(kTypes));
    SDL_SetNumberProperty(props, SDL_PROP_FILE_DIALOG_NFILTERS_NUMBER, 2);
  }
  SDL_ShowFileDialogWithProperties(folder ? SDL_FILEDIALOG_OPENFOLDER : SDL_FILEDIALOG_OPENFILE, Picked,
                                   folder ? nullptr : reinterpret_cast<void*>(1), props);
  SDL_DestroyProperties(props);
}

// What Windows' timer does (every frame here), then the window's contents. False: the window closes.
bool Frame(const small_window::Look& look) {
  Window& w = *g_window;
  if (!w.auto_started) {
    w.auto_started = true;
    if (const char* test = std::getenv("MK9_TEST_INSTALL"); test && *test) {
      const fs::path path(test);
      std::error_code ec;
      Start(!fs::is_directory(path, ec), path);
    }
  }
  if (w.quit || (w.closing_at && SDL_GetTicks() >= w.closing_at)) return false;
  {
    std::lock_guard lock(w.picked_mutex);
    if (w.picked && !w.job.running && !w.closing_at) Start(w.picked->first, w.picked->second);
    w.picked.reset();
    if (w.dialog_failed) {
      w.dialog_failed = false;
      w.status = "No se pudo abrir el selector de archivos de tu escritorio: arrastrá la ISO o la carpeta a esta "
                 "ventana.";
    }
  }
  const uint64_t total = w.job.bytes_total, done = w.job.bytes_done;
  std::string status;
  {
    std::lock_guard lock(w.job.mutex);
    status = w.job.status;
  }
  if (w.job.running) {
    const int permille = total ? int(done * 1000 / total) : 0;
    w.progress = float(permille) / 1000.0f;
    w.status = total ? status + "  (" + std::to_string(permille / 10) + " %, " + std::to_string(done >> 20) + " de " +
                           std::to_string(total >> 20) + " MB)"
                     : status;
  } else if (w.job.done) {
    w.job.done = false;
    if (w.worker.joinable()) w.worker.join();
    if (w.job.ok) {
      w.progress = 1.0f;
      w.status = "¡Listo! Arrancando el juego...";
      REXLOG_INFO("[installer] done: {}", w.job.result.string());
      w.chosen = w.job.result;
      w.closing_at = SDL_GetTicks() + 900;
    } else {
      w.status = status;
      REXLOG_WARN("[installer] {}", status);
    }
  }

  const float s = look.scale;
  ImGui::PushFont(look.title, 34.0f * s);
  ImGui::TextColored(ImVec4(0.95f, 0.72f, 0.25f, 1.0f), "mk9-lauta");
  ImGui::PopFont();
  ImGui::Spacing();
  ImGui::TextWrapped(
      "Para jugar hace falta tu copia de Mortal Kombat Komplete Edition de Xbox 360. Elegí la imagen .iso del disco, "
      "o la carpeta donde ya la tenés extraída (la que tiene default.xex). Se revisa que sea la versión correcta y se "
      "prepara una sola vez: desde la ISO se copian ~7 GB al lado del juego. También podés arrastrar la ISO o la "
      "carpeta a esta ventana.");
  ImGui::Spacing();
  ImGui::Spacing();
  const bool busy = w.job.running || w.closing_at;
  const ImVec2 button(200.0f * s, 36.0f * s);
  ImGui::BeginDisabled(busy);
  if (ImGui::Button("Elegir la ISO...", button)) Pick(false);
  ImGui::SameLine();
  if (ImGui::Button("Elegir una carpeta...", button)) Pick(true);
  ImGui::EndDisabled();
  ImGui::SameLine(ImGui::GetWindowWidth() - 120.0f * s - ImGui::GetStyle().WindowPadding.x);
  ImGui::BeginDisabled(w.closing_at != 0);
  if (ImGui::Button(w.job.running ? "Cancelar" : "Salir", ImVec2(120.0f * s, 36.0f * s))) {
    if (w.job.running) {
      w.job.cancel = true;
    } else {
      w.quit = true;
    }
  }
  ImGui::EndDisabled();
  ImGui::Spacing();
  if (w.show_bar) ImGui::ProgressBar(w.progress, ImVec2(-1.0f, 22.0f * s), "");
  ImGui::TextWrapped("%s", w.status.c_str());
  return true;
}

void RunWindow() {
  Window& w = *g_window;
  const bool opened = small_window::Run(
      "mk9-lauta - Instalación", 620, 330, Frame,
      [&w] {
        // the X: a job running is cancelled (the window stays, as on Windows); else it closes
        if (w.job.running) {
          w.job.cancel = true;
          return false;
        }
        return !w.closing_at;
      },
      [&w](const fs::path& path) {
        if (w.job.running || w.closing_at) return;
        std::error_code ec;
        Start(!fs::is_directory(path, ec), path);
      });
  if (!opened) {
    std::fprintf(stderr,
                 "mk9: no se encontraron los archivos del juego y no se puede abrir la ventana del instalador (%s).\n"
                 "     Abrí el juego desde tu escritorio, o poné la carpeta game_root al lado del programa.\n",
                 SDL_GetError());
  }
  if (w.worker.joinable()) {
    w.job.cancel = true;
    w.worker.join();
  }
}
#endif

}  // namespace

fs::path ConfiguredRoot(const fs::path& config_path) {
  std::error_code ec;
  if (config_path.empty() || !fs::is_regular_file(config_path, ec)) return {};
  try {
    const toml::table config = toml::parse_file(config_path.string());
    if (const auto value = config["game_data_root"].value<std::string>(); value && !value->empty()) {
      return fs::path(Wide(*value));
    }
  } catch (const std::exception&) {
    // a config the SDK will complain about itself
  }
  return {};
}

fs::path Run(const fs::path& exe_folder) {
  Window window;
  window.exe_folder = exe_folder;
  g_window = &window;
  REXLOG_INFO("[installer] no game files found: the installer opens");
#if defined(_WIN32)
  std::thread ui(RunWindow);  // its own thread: a COM apartment and a message loop of its own
  ui.join();
#else
  REXLOG_INFO("[installer] SHA-256 by {}", mk9_sha256_engine());
  RunWindow();  // SDL's window: on the main thread (this one), as SDL asks
#endif
  g_window = nullptr;
  return window.chosen;
}

}  // namespace mk9::installer
