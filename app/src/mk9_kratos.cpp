// mk9-lauta: Kratos (mk9_kratos.h, kratos.toml).

#include "mk9_kratos.h"

#include <atomic>
#include <string>
#include <vector>

#include <rex/cvar.h>
#include <rex/filesystem.h>
#include <rex/logging.h>
#include <rex/ppc/context.h>

// Tests: the port's zip (or folder) installed at the start, before the game (the same steps as F2 > Kratos).
REXCVAR_DEFINE_STRING(mk9_kratos_install, "", "MK9 Online",
                      "Tests: the Kratos port's release zip (or its folder) installed into the game files at the start")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_BOOL(mk9_kratos_auto, true, "MK9 Online",
                    "Kratos comes by himself: the port's zip downloaded in the background and installed at the next "
                    "start (false: never; tests)")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

namespace mk9::kratos {
namespace {

std::filesystem::path g_game_root;
std::atomic<bool> g_installed{false};
std::atomic<bool> g_packages{false};  // his packages are in Asset/ (the file check's question)
std::atomic<uint64_t> g_toc_size{0};  // Xbox360TOC.txt with his packages listed, from their sizes here

// The port's release v1.0: his packages, and the banks made from Kenshi's (the same sizes as those)
const char* const kPackages[] = {"CHAR_Kratos.xxx",     "CHAR_Kratos_B.xxx",       "Kratos_CharIntro.xxx",
                                 "Kratos_Fatality1.xxx", "Kratos_Fatality2.xxx",    "Kratos_Ladder.xxx",
                                 "Kratos_Victory.xxx",   "Kratos_ui.xxx",           "Kratos_ui_n.xxx",
                                 "NPC_Kratos_Baby.xxx",  "NPC_Kratos_B_Baby.xxx",   "ui_e_Kratos.xxx",
                                 "SND_SFX_Kratos.xxx",   "SND_VO_Kratos.xxx",       "SND_VO_Announcer_Kratos.xxx"};
constexpr size_t kTocPackages = 12;          // the first 12: the ones the port lists in Xbox360TOC.txt
constexpr uint64_t kCoalescedSize = 164903;  // Config/Coalesced.ini with his four edits (the disc's: 164744)
constexpr uint64_t kDiscTocSize = 56027;     // Xbox360TOC.txt on the disc; the port adds a line per package

std::atomic<uint64_t> g_checks_skipped{0};
std::atomic<uint64_t> g_null_calls{0};

bool InstallNow(const std::filesystem::path& from);
void InstallDownloaded();
void FetchAtStart();

// This game is an online session's (started again joined to a room, watching, or a replay): its content was what the
// room compared, so nothing is installed nor downloaded in it.
bool SessionGame() {
  for (const char* name : {"mk9_net_room", "mk9_net_role", "mk9_net_replay"}) {
    if (!rex::cvar::GetFlagByName(name).empty()) return true;
  }
  return false;
}

}  // namespace

void AtStart(const std::filesystem::path& game_root) {
  if (game_root.empty()) return;
  g_game_root = game_root;
  if (const std::string test = REXCVAR_GET(mk9_kratos_install); !test.empty()) {
    REXLOG_INFO("[kratos] installing {} (mk9_kratos_install)", test);
    InstallNow(rex::to_path(test));
  } else if (!SessionGame()) {
    FetchAtStart();       // he is not here and not downloaded: downloaded now, with a window (the player may skip it)
    InstallDownloaded();  // the zip, when he is not here yet
  }
  std::error_code ec;
  std::vector<std::string> missing;
  int packages = 0;
  uint64_t toc = kDiscTocSize;
  for (size_t i = 0; i < std::size(kPackages); ++i) {
    const uint64_t size = std::filesystem::file_size(game_root / "Asset" / kPackages[i], ec);
    if (ec) {
      missing.push_back(std::string("Asset/") + kPackages[i]);
      continue;
    }
    ++packages;
    // the port's line for it: "<size> 0 0 ..\Asset\<name> 0" and CR LF (kratos_install.py toc_patch)
    if (i < kTocPackages) {
      toc += std::to_string(size).size() + std::string_view(" 0 0 ..\\Asset\\").size() +
             std::string_view(kPackages[i]).size() + std::string_view(" 0\r\n").size();
    }
  }
  if (packages == 0) return;  // the game as on the disc: nothing to say
  g_packages = true;
  if (packages == int(std::size(kPackages))) g_toc_size = toc;
  const uint64_t coalesced = std::filesystem::file_size(game_root / "Config" / "Coalesced.ini", ec);
  if (ec || coalesced != kCoalescedSize) missing.push_back("Config/Coalesced.ini con Kratos registrado");
  // the DLC: dlc_root when given (tests), else the game files' dlc folder
  std::string dlc_text = rex::cvar::GetFlagByName("dlc_root");
  const std::filesystem::path dlc = dlc_text.empty() ? game_root / "dlc" : rex::to_path(dlc_text);
  const std::filesystem::path package = dlc / "0000000000000000" / "575207FD" / "00000002" / "KRATOS";
  if (!std::filesystem::is_regular_file(package / "Kratos__Xe-MK9Game.ini", ec)) {
    missing.push_back(rex::path_to_utf8(package) + " (el paquete DLC)");
  }
  if (!missing.empty()) {
    REXLOG_WARN("[kratos] {} of his {} packages are in the game files, but not all he needs: off", packages,
                std::size(kPackages));
    for (const std::string& m : missing) REXLOG_WARN("[kratos]   missing: {}", m);
    return;
  }
  if (dlc_text.empty()) rex::cvar::SetFlagByName("dlc_root", rex::path_to_utf8(dlc));
  g_installed = true;
  REXLOG_INFO("[kratos] installed: his packages, the Coalesced.ini that registers him, his DLC in {}; the port's two "
              "code changes on",
              rex::path_to_utf8(package));
}

bool Installed() { return g_installed.load(std::memory_order_relaxed); }


bool PatchedSize(std::string_view path, uint64_t size) {
  if (!g_packages.load(std::memory_order_relaxed)) return false;
  return (path == "Config/Coalesced.ini" && size == kCoalescedSize) ||
         (path == "Xbox360TOC.txt" && size == g_toc_size.load(std::memory_order_relaxed));
}

// The first time each change makes a difference, in the log (a test sees they ran).
void NoteSkippedCheck() {
  if (g_checks_skipped.fetch_add(1, std::memory_order_relaxed) == 0) {
    REXLOG_INFO("[kratos] 0x829AFDA8: the check the port removes was going to run; skipped, as there");
  }
}

void NoteNullCall() {
  if (g_null_calls.fetch_add(1, std::memory_order_relaxed) == 0) {
    REXLOG_INFO("[kratos] 0x82A3B86C: the call the port guards came with no object; skipped, as there");
  }
}

}  // namespace mk9::kratos

// 0x829AFDA8 (kratos.toml): before its beq, which the port makes unconditional (cr6.eq: the game branches anyway).
bool Mk9KratosSkipCheck(PPCCRRegister& cr6) {
  if (!mk9::kratos::Installed()) return false;
  if (!cr6.eq) mk9::kratos::NoteSkippedCheck();
  return true;
}

// 0x82A3B86C (kratos.toml): before its bl 0x8278e900; true with no object (r3 == 0) goes past the call.
bool Mk9KratosNullGuard(PPCRegister& r3) {
  if (r3.u32 != 0 || !mk9::kratos::Installed()) return false;
  mk9::kratos::NoteNullCall();
  return true;
}

// ---------------------------------------------------------------------------------------------------------------------
// F2 > Kratos: the installer (tools/kratos/kratos_install.py's steps, each checked) and the removal.

#if defined(_WIN32) || (defined(__linux__) && !defined(__SWITCH__))

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <commctrl.h>
#include <shobjidl.h>
#else
// mk9-lauta (2026-11-13): Linux: the same steps; a file replaced by rename after an fsync, the download's window a small
// one of the game's (mk9_small_window.h)
#include <sys/resource.h>
#include <sys/syscall.h>
#include <unistd.h>

#include "imgui.h"
#include "mk9_platform.h"
#include "mk9_small_window.h"
#endif

#include <cstdio>
#include <cstring>
#include <fstream>
#include <mutex>
#include <queue>
#include <algorithm>
#include <regex>
#include <unordered_map>
#include <span>
#include <thread>

#include <rex/filesystem/devices/stfs_container_device.h>
#include <rex/filesystem/entry.h>
#include <rex/filesystem/file.h>
#include <rex/string/utf8.h>

#include <fmt/format.h>

#include "mk9_updater.h"

namespace mk9::kratos {
namespace {

namespace fs = std::filesystem;

// The disc's files the install starts from (app/mk9_game_files.txt) and what the port's steps make of them.
constexpr const char* kDiscCoalescedSha = "e0fe89cc556f8a55ad30dc91124783bd0a1ce4593efafeb56966392adc719bee";
constexpr const char* kKratosCoalescedSha = "ad90a73c7d4823dc2b0b14f7c8d31b78602e922f638ce8f02a8241e45d5ec21a";
constexpr const char* kDiscTocSha = "001f0375d0275fea072518331d1c309dc5949290ff51e4874db8cfc6eee15d5c";
struct Bank {
  const char* from;
  const char* from_sha;
  const char* to;
  const char* to_sha;
};
const Bank kBanks[] = {
    {"SND_SFX_Kenshi.xxx", "d59c107ad10a98a13a04204b7c5823dce984cd1954d4c5437f31924fc3157c3f", "SND_SFX_Kratos.xxx",
     "bf5dc48e03c45df70f5d800acde78fd416d14dde44d67dfb4654d270e4cb5ea2"},
    {"SND_VO_Kenshi.xxx", "09db1f75424c0d817aa793ecca6041ff6915a61add4a81d66dcbff4e9e087ae7", "SND_VO_Kratos.xxx",
     "3b1da41a7417f9d7c9bcd9acba78ca473a4bf383a77169fc4571355aad0b37cb"},
    {"SND_VO_Announcer_Kenshi.xxx", "28e8bbecae05b94137366e469be0457a31529cd339856bec6efa0a2a412dedd7",
     "SND_VO_Announcer_Kratos.xxx", "3a2159d65250cc86cfca167509db0061c9b5b88d13876ecdae95dc20fc7dd682"},
};

struct Edit {
  const char* entry;
  const char* section;
  std::vector<std::string> insert_top, remove;
};
const Edit kEdits[] = {
    {"Xe-MK9Engine.ini",
     "[Engine.HDCacheControlPrefixes]",
     {"CHAR_Kratos=false", "Kratos_=false", "NPC_Kratos=false", "ui_e_Kratos=false", "SND_SFX_Kratos=false",
      "SND_VO_Kratos=false", "SND_VO_Announcer_Kratos=false"},
     {}},
    {"Xe-MK9Game.ini", "[Player]", {}, {"IsOnDiscDLC=kratos", "IsOnDiscDLC=kratos_b"}},
    {"Xe-MK9Game.ini", "[Player.DownloadContentID]", {"kratos=1001", "kratos_b=1002"}, {}},
    {"Xe-MK9Game.ini", "[Player.DownloadContentUnlocked]", {"kratos=TRUE", "kratos_b=TRUE"}, {}},
};

void SetStep(const std::string& step, float) { REXLOG_INFO("[kratos] {}", step); }

std::string Hex(const uint8_t digest[32]) {
  static const char* const kDigits = "0123456789abcdef";
  std::string out;
  for (int i = 0; i < 32; ++i) {
    out += kDigits[digest[i] >> 4];
    out += kDigits[digest[i] & 15];
  }
  return out;
}

std::string ShaOf(const fs::path& file) {
  uint8_t digest[32];
  return updater::FileSha256(file, digest) ? Hex(digest) : std::string();
}

bool ReadAll(const fs::path& file, std::string& out) {
  std::ifstream in(file, std::ios::binary);
  if (!in) return false;
  out.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
  return !in.bad();
}

// A new file over the old one: written beside it, then renamed over it (a game_root of hard links to another keeps
// that one as it was; a half-written file never takes the place of a whole one).
bool WriteNew(const fs::path& file, std::string_view data) {
  std::error_code ec;
  fs::create_directories(file.parent_path(), ec);
  fs::path temp = file;
#if defined(_WIN32)
  temp += L".kratos_tmp";
#else
  temp += ".kratos_tmp";
#endif
  {
    std::ofstream out(temp, std::ios::binary | std::ios::trunc);
    if (!out) return false;
    out.write(data.data(), std::streamsize(data.size()));
    if (!out.good()) return false;
  }
#if defined(_WIN32)
  return MoveFileExW(temp.c_str(), file.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
#else
  // on the disk before it takes the old one's place (Windows: MOVEFILE_WRITE_THROUGH); rename replaces in one step
  if (std::FILE* written = platform::OpenFile(temp, "rb+")) {
    fsync(fileno(written));
    std::fclose(written);
  }
  std::error_code rename_ec;
  fs::rename(temp, file, rename_ec);
  return !rename_ec;
#endif
}

bool CopyNew(const fs::path& from, const fs::path& to) {
  std::string data;
  return ReadAll(from, data) && WriteNew(to, data);
}

std::string Lower(std::string text) {
  for (char& c : text) c = char(std::tolower(static_cast<unsigned char>(c)));
  return text;
}

// The release's file whose path ends with 'tail' ('/' separated, any case), or "" when it has none.
fs::path Find(const fs::path& root, const std::string& tail) {
  const std::string want = Lower(tail);
  std::error_code ec;
  for (fs::recursive_directory_iterator it(root, ec), end; it != end; it.increment(ec)) {
    if (ec) break;
    if (!it->is_regular_file(ec)) continue;
    std::string path = Lower(rex::path_to_utf8(it->path()));
    std::replace(path.begin(), path.end(), '\\', '/');
    if (path.size() >= want.size() && path.compare(path.size() - want.size(), want.size(), want) == 0 &&
        (path.size() == want.size() || path[path.size() - want.size() - 1] == '/')) {
      return it->path();
    }
  }
  return {};
}

uint32_t Be32(const std::string& d, size_t o) {
  return uint32_t(uint8_t(d[o])) << 24 | uint32_t(uint8_t(d[o + 1])) << 16 | uint32_t(uint8_t(d[o + 2])) << 8 |
         uint32_t(uint8_t(d[o + 3]));
}

void PutBe32(std::string& d, uint32_t v) {
  d += char(v >> 24);
  d += char(v >> 16);
  d += char(v >> 8);
  d += char(v);
}

std::vector<std::string> Split(const std::string& text, const std::string& sep) {
  std::vector<std::string> out;
  size_t at = 0;
  for (;;) {
    const size_t next = text.find(sep, at);
    if (next == std::string::npos) {
      out.push_back(text.substr(at));
      return out;
    }
    out.push_back(text.substr(at, next - at));
    at = next + sep.size();
  }
}

// Coalesced.ini: a 4-byte header, then (big-endian length, name) (length, body) per INI file; the port's four edits.
bool PatchCoalesced(const std::string& data, std::string& out, std::string& why) {
  struct Entry {
    std::string name, body;
  };
  std::vector<Entry> entries;
  size_t o = 4;
  while (o < data.size()) {
    if (o + 4 > data.size()) return why = "Coalesced.ini cortado", false;
    const size_t n = Be32(data, o);
    if (o + 4 + n + 4 > data.size()) return why = "Coalesced.ini cortado", false;
    Entry e;
    e.name = data.substr(o + 4, n);
    o += 4 + n;
    const size_t m = Be32(data, o);
    if (o + 4 + m > data.size()) return why = "Coalesced.ini cortado", false;
    e.body = data.substr(o + 4, m);
    o += 4 + m;
    entries.push_back(std::move(e));
  }
  for (const Edit& edit : kEdits) {
    const std::string tail = "\\" + Lower(edit.entry);
    Entry* hit = nullptr;
    int hits = 0;
    for (Entry& e : entries) {
      std::string name = e.name;
      while (!name.empty() && name.back() == '\0') name.pop_back();
      name = Lower(name);
      std::replace(name.begin(), name.end(), '/', '\\');
      if (name.size() >= tail.size() && name.compare(name.size() - tail.size(), tail.size(), tail) == 0) {
        hit = &e;
        ++hits;
      }
    }
    if (hits != 1) return why = std::string("Coalesced.ini sin ") + edit.entry, false;
    std::string body = hit->body, end;
    if (!body.empty() && body.back() == '\0') {
      body.pop_back();
      end = std::string(1, '\0');
    }
    const std::string eol = body.find("\r\n") != std::string::npos ? "\r\n" : "\n";
    std::vector<std::string> lines = Split(body, eol);
    const auto h = std::find(lines.begin(), lines.end(), std::string(edit.section));
    if (h == lines.end()) return why = std::string("Coalesced.ini sin ") + edit.section, false;
    const size_t hi = size_t(h - lines.begin());
    size_t e = hi + 1;
    while (e < lines.size() && (lines[e].empty() || lines[e][0] != '[')) ++e;
    if (!edit.insert_top.empty()) {
      std::vector<std::string> add;
      for (const std::string& line : edit.insert_top) {
        if (std::find(lines.begin() + long(hi) + 1, lines.begin() + long(e), line) == lines.begin() + long(e)) {
          add.push_back(line);
        }
      }
      lines.insert(lines.begin() + long(hi) + 1, add.begin(), add.end());
    } else {
      std::vector<std::string> kept(lines.begin(), lines.begin() + long(hi) + 1);
      for (size_t i = hi + 1; i < e; ++i) {
        if (std::find(edit.remove.begin(), edit.remove.end(), lines[i]) == edit.remove.end()) kept.push_back(lines[i]);
      }
      kept.insert(kept.end(), lines.begin() + long(e), lines.end());
      lines = std::move(kept);
    }
    std::string joined;
    for (size_t i = 0; i < lines.size(); ++i) {
      if (i) joined += eol;
      joined += lines[i];
    }
    hit->body = joined + end;
  }
  out = data.substr(0, 4);
  for (const Entry& e : entries) {
    PutBe32(out, uint32_t(e.name.size()));
    out += e.name;
    PutBe32(out, uint32_t(e.body.size()));
    out += e.body;
  }
  return true;
}

// Xbox360TOC.txt: "<size> <n> <n> <path> <n>" per line; Coalesced.ini's new size, and a line per package.
bool PatchToc(const std::string& data, uint64_t coalesced_size, const std::vector<uint64_t>& sizes, std::string& out,
              std::string& why) {
  struct Row {
    std::string text, eol;
  };
  std::vector<std::string> parts = Split(data, "\n");
  const std::string last = parts.back();
  parts.pop_back();
  std::vector<Row> rows;
  int crlf = 0, lf = 0;
  for (std::string& p : parts) {
    if (!p.empty() && p.back() == '\r') {
      p.pop_back();
      rows.push_back({p, "\r\n"});
      ++crlf;
    } else {
      rows.push_back({p, "\n"});
      ++lf;
    }
  }
  if (!last.empty()) rows.push_back({last, ""});
  const std::string eol = crlf > lf ? "\r\n" : "\n";
  static const std::regex kLine(R"(^(\d+)( \d+ \d+ )(.+?)( \d+)$)");
  std::unordered_map<std::string, size_t> index;
  for (size_t i = 0; i < rows.size(); ++i) {
    std::smatch m;
    if (std::regex_match(rows[i].text, m, kLine)) index.emplace(Lower(m[3].str()), i);
  }
  auto set_size = [&](size_t i, uint64_t size) {
    std::smatch m;
    std::regex_match(rows[i].text, m, kLine);
    rows[i].text = std::to_string(size) + m[2].str() + m[3].str() + m[4].str();
  };
  const auto coalesced = index.find(Lower("..\\Config\\Coalesced.ini"));
  if (coalesced == index.end()) return why = "Xbox360TOC.txt sin Coalesced.ini", false;
  set_size(coalesced->second, coalesced_size);
  for (size_t i = 0; i < kTocPackages; ++i) {
    const std::string path = std::string("..\\Asset\\") + kPackages[i];
    if (const auto it = index.find(Lower(path)); it != index.end()) {
      set_size(it->second, sizes[i]);
    } else {
      if (!rows.empty() && rows.back().eol.empty()) rows.back().eol = eol;
      rows.push_back({std::to_string(sizes[i]) + " 0 0 " + path + " 0", eol});
    }
  }
  out.clear();
  for (const Row& r : rows) out += r.text + r.eol;
  return true;
}

// The DLC package (an STFS "LIVE" file) as the content manager takes DLC: its files in a folder.
bool ExtractDlc(const fs::path& package, const fs::path& to, std::string& why) {
  auto device = std::make_unique<rex::filesystem::StfsContainerDevice>("", package);
  if (!device->Initialize()) return why = "el paquete DLC no se pudo leer", false;
  auto* root = device->ResolvePath("");
  if (!root) return why = "el paquete DLC está vacío", false;
  std::error_code ec;
  fs::remove_all(to, ec);
  fs::create_directories(to, ec);
  std::queue<rex::filesystem::Entry*> queue;
  queue.push(root);
  int files = 0;
  std::vector<uint8_t> buffer(4 << 20);
  while (!queue.empty()) {
    auto* entry = queue.front();
    queue.pop();
    for (auto& child : entry->children()) queue.push(child.get());
    if (entry == root) continue;
    const fs::path dest = to / rex::to_path(rex::string::utf8_fix_path_separators(entry->path()));
    if (entry->attributes() & rex::filesystem::kFileAttributeDirectory) {
      fs::create_directories(dest, ec);
      continue;
    }
    rex::filesystem::File* in = nullptr;
    if (entry->Open(rex::filesystem::FileAccess::kFileReadData, &in) != 0 || !in) {  // 0: X_STATUS_SUCCESS
      return why = "no se pudo leer " + entry->path() + " del paquete DLC", false;
    }
    std::string data;
    data.reserve(entry->size());
    size_t offset = 0;
    while (offset < entry->size()) {
      size_t read = 0;
      const size_t want = std::min(buffer.size(), entry->size() - offset);
      in->ReadSync(std::span<uint8_t>(buffer.data(), want), offset, &read);
      if (read == 0) break;
      data.append(reinterpret_cast<const char*>(buffer.data()), read);
      offset += read;
    }
    in->Destroy();
    if (data.size() != entry->size() || !WriteNew(dest, data)) {
      return why = "no se pudo copiar " + entry->path() + " del paquete DLC", false;
    }
    ++files;
  }
  if (!fs::is_regular_file(to / "Kratos__Xe-MK9Game.ini", ec)) return why = "el paquete DLC no es el de Kratos", false;
  REXLOG_INFO("[kratos] DLC: {} files in {}", files, rex::path_to_utf8(to));
  return true;
}

fs::path PackageFolder(const fs::path& game_root) {
  const std::string dlc_text = rex::cvar::GetFlagByName("dlc_root");
  const fs::path dlc = dlc_text.empty() ? game_root / "dlc" : rex::to_path(dlc_text);
  return dlc / "0000000000000000" / "575207FD" / "00000002" / "KRATOS";
}

bool InstallSteps(const fs::path& from, std::string& why) {
  const fs::path game = g_game_root;
  if (game.empty()) return why = "no se encontró la carpeta del juego", false;
  std::error_code ec;
  // 1. the release: its zip unpacked beside the game files (the folder as it is)
  fs::path release = from;
  const fs::path unpacked = game.parent_path() / "kratos_unpacked";
  if (fs::is_regular_file(from, ec)) {
    SetStep("Descomprimiendo el zip de Kratos…", 0.02f);
    if (!updater::UnpackZip(from, unpacked)) return why = "no se pudo descomprimir el zip", false;
    release = unpacked;
  }
  struct Cleanup {
    fs::path p;
    ~Cleanup() {
      std::error_code e;
      if (!p.empty()) fs::remove_all(p, e);
    }
  } cleanup{release == unpacked ? unpacked : fs::path()};
  std::vector<fs::path> assets;
  for (size_t i = 0; i < kTocPackages; ++i) {
    const fs::path f = Find(release, std::string("Game_Hdd1_MK9/Asset/") + kPackages[i]);
    if (f.empty()) return why = std::string("el paquete no trae ") + kPackages[i] + " (¿es Kratos-MK9-360-Share?)", false;
    assets.push_back(f);
  }
  const fs::path dlc_package = Find(release, "Content_DLC/KRATOS");
  if (dlc_package.empty()) return why = "el paquete no trae Content_DLC/KRATOS", false;
  // 2. the disc's Coalesced.ini and TOC, checked, and kept (the removal puts them back)
  SetStep("Revisando los archivos del juego…", 0.08f);
  const fs::path coal_path = game / "Config" / "Coalesced.ini", toc_path = game / "Xbox360TOC.txt";
  const fs::path backup = game / "kratos_backup";
  std::string coal, toc;
  if (!ReadAll(coal_path, coal) || !ReadAll(toc_path, toc)) return why = "no se pudo leer Coalesced.ini o la TOC", false;
  const std::string coal_sha = ShaOf(coal_path), toc_sha = ShaOf(toc_path);
  if (coal_sha == kKratosCoalescedSha || toc_sha != kDiscTocSha) {
    // installed before (all or part): the disc's two from the backup
    if (ShaOf(backup / "Coalesced.ini") != kDiscCoalescedSha || ShaOf(backup / "Xbox360TOC.txt") != kDiscTocSha ||
        !ReadAll(backup / "Coalesced.ini", coal) || !ReadAll(backup / "Xbox360TOC.txt", toc)) {
      return why = "Coalesced.ini o Xbox360TOC.txt no son los del disco y no hay copia de ellos", false;
    }
  } else if (coal_sha != kDiscCoalescedSha) {
    return why = "Config/Coalesced.ini no es el del disco (¿otro mod?)", false;
  } else if (!WriteNew(backup / "Coalesced.ini", coal) || !WriteNew(backup / "Xbox360TOC.txt", toc)) {
    return why = "no se pudo guardar la copia de Coalesced.ini y la TOC", false;
  }
  // 3. his packages
  std::vector<uint64_t> sizes;
  for (size_t i = 0; i < assets.size(); ++i) {
    SetStep(std::string("Copiando ") + kPackages[i] + "…", 0.1f + 0.5f * float(i) / float(assets.size()));
    sizes.push_back(fs::file_size(assets[i], ec));
    if (ec || !CopyNew(assets[i], game / "Asset" / kPackages[i])) {
      return why = std::string("no se pudo copiar ") + kPackages[i], false;
    }
  }
  // 4. his sound banks, from Kenshi's (the names inside changed, the same lengths)
  SetStep("Armando los sonidos de Kratos…", 0.62f);
  for (const Bank& b : kBanks) {
    std::string data;
    if (ShaOf(game / "Asset" / b.from) != b.from_sha || !ReadAll(game / "Asset" / b.from, data)) {
      return why = std::string(b.from) + " no es el del disco", false;
    }
    for (const auto& [a, z] : {std::pair<std::string, std::string>{"Kenshi", "Kratos"}, {"kens", "krat"}}) {
      for (size_t at = data.find(a); at != std::string::npos; at = data.find(a, at + z.size())) data.replace(at, a.size(), z);
    }
    if (!WriteNew(game / "Asset" / b.to, data) || ShaOf(game / "Asset" / b.to) != b.to_sha) {
      return why = std::string(b.to) + " no salió como el del port", false;
    }
  }
  // 5. Coalesced.ini and the TOC
  SetStep("Registrando a Kratos (Coalesced.ini)…", 0.66f);
  std::string coal_new, toc_new;
  if (!PatchCoalesced(coal, coal_new, why)) return false;
  if (!WriteNew(coal_path, coal_new) || ShaOf(coal_path) != kKratosCoalescedSha) {
    return why = "Coalesced.ini no salió como el del port", false;
  }
  if (!PatchToc(toc, coal_new.size(), sizes, toc_new, why)) return false;
  if (!WriteNew(toc_path, toc_new)) return why = "no se pudo escribir Xbox360TOC.txt", false;
  // 6. the DLC package
  SetStep("Instalando el paquete DLC…", 0.72f);
  if (!ExtractDlc(dlc_package, PackageFolder(game), why)) return false;
  SetStep("Kratos instalado.", 1.0f);
  return true;
}

bool InstallNow(const fs::path& from) {
  std::string why;
  const bool ok = InstallSteps(from, why);
  if (ok) {
    REXLOG_INFO("[kratos] installed from {}", rex::path_to_utf8(from));
  } else {
    REXLOG_ERROR("[kratos] the install from {} failed: {}", rex::path_to_utf8(from), why);
  }
  return ok;
}

// The port's release (eLGatoRDV/mk9-kratos-xbox360 v1.0), as GitHub lists it.
constexpr const char* kZipUrl =
    "https://github.com/eLGatoRDV/mk9-kratos-xbox360/releases/download/v1.0/Kratos-MK9-360-Share.zip";
constexpr const char* kZipSha = "47bb63c6e886a7cc7f8a8a1be6a01497eaac4d7ca598d870e525b7f3274b8ef9";
constexpr uint64_t kZipSize = 95943617;

fs::path ZipPath() {
  return rex::filesystem::GetExecutableFolder() / "kratos" / "Kratos-MK9-360-Share.zip";
}

// Whole and the port's (its size, then its SHA-256).
bool ZipGood(const fs::path& zip) {
  std::error_code ec;
  return fs::file_size(zip, ec) == kZipSize && !ec && ShaOf(zip) == kZipSha;
}

// He is in these game files already (installed at an earlier start, or a game root that had him): his packages, his
// DLC package and the Coalesced.ini that registers him.
bool InGameRoot() {
  if (g_game_root.empty()) return false;
  std::error_code ec;
  for (const char* name : kPackages) {
    if (!fs::is_regular_file(g_game_root / "Asset" / name, ec)) return false;
  }
  return fs::is_regular_file(PackageFolder(g_game_root) / "Kratos__Xe-MK9Game.ini", ec) &&
         ShaOf(g_game_root / "Config" / "Coalesced.ini") == kKratosCoalescedSha;
}

void InstallDownloaded() {
  std::error_code ec;
  const fs::path zip = ZipPath();
  if (!fs::exists(zip, ec)) return;
  // already here: the zip is not needed any more
  if (!InGameRoot()) {
    if (!ZipGood(zip)) {
      REXLOG_WARN("[kratos] {} is not the port's release (size or SHA-256): deleted, downloaded again",
                  rex::path_to_utf8(zip));
    } else if (!InstallNow(zip)) {
      return;  // kept: the log says why (the next start tries again, with no download)
    }
  }
  fs::remove(zip, ec);
}

std::mutex g_fetch_mutex;
Fetch g_fetch;

void FetchWorker() {
  const fs::path zip = ZipPath();
  fs::path partial = zip;
#if defined(_WIN32)
  partial += L".partial";
#else
  partial += ".partial";
#endif
  std::error_code ec;
  fs::create_directories(zip.parent_path(), ec);
  uint64_t got = fs::exists(partial, ec) ? fs::file_size(partial, ec) : 0;
  if (ec || got >= kZipSize) got = 0;  // nothing usable there: from the start
#if defined(_WIN32)
  FILE* out = _wfopen(partial.c_str(), got ? L"ab" : L"wb");
#else
  FILE* out = platform::OpenFile(partial, got ? "ab" : "wb");
#endif
  if (!out) {
    REXLOG_ERROR("[kratos] could not write {}", rex::path_to_utf8(partial));
    std::lock_guard<std::mutex> lock(g_fetch_mutex);
    g_fetch.downloading = false;
    return;
  }
  REXLOG_INFO("[kratos] downloading the port's release ({} MB) from {}{}", kZipSize >> 20, kZipUrl,
              got ? fmt::format(", on from {} MB", got >> 20) : std::string());
  std::string error;
  const bool ok = updater::DownloadTo(
      kZipUrl,
      [&](const char* data, size_t n) {
        if (std::fwrite(data, 1, n, out) != n) return false;
        got += n;
        std::lock_guard<std::mutex> lock(g_fetch_mutex);
        g_fetch.progress = float(double(got) / double(kZipSize));
        return true;
      },
      &error, got);
  std::fclose(out);
  bool ready = false;
  if (!ok) {
    REXLOG_WARN("[kratos] the download stopped ({}): it goes on at the next start", error);
  } else if (!ZipGood(partial)) {
    REXLOG_ERROR("[kratos] the download is not the port's release (size or SHA-256): discarded");
  } else {
#if defined(_WIN32)
    ready = MoveFileExW(partial.c_str(), zip.c_str(), MOVEFILE_REPLACE_EXISTING) != 0;
#else
    std::error_code rename_ec;
    fs::rename(partial, zip, rename_ec);
    ready = !rename_ec;
#endif
    if (ready) REXLOG_INFO("[kratos] downloaded: he comes at the next start");
  }
  if (!ready && ok) fs::remove(partial, ec);  // a whole download that is not the port's: never kept
  std::lock_guard<std::mutex> lock(g_fetch_mutex);
  g_fetch.downloading = false;
  g_fetch.ready = ready;
}

// The download may start: he is not here, no zip waits, not a session's game, not a test (no lobby, a scripted pad,
// an online test), mk9_kratos_auto.
bool FetchWanted() {
  if (Installed() || g_game_root.empty() || SessionGame() || !REXCVAR_GET(mk9_kratos_auto)) return false;
  if (rex::cvar::GetFlagByName("mk9_lobby_url") == "off" || !rex::cvar::GetFlagByName("mk9_input_script").empty() ||
      !rex::cvar::GetFlagByName("mk9_input_script_file").empty() ||
      !rex::cvar::GetFlagByName("mk9_online_test").empty()) {
    return false;
  }
  std::error_code ec;
  if (fs::exists(ZipPath(), ec)) return false;  // downloaded already: installed at the start (or its failure logged)
  // in the game files already: the zip is deleted once he is in, and AtStart asks this before it counts his files
  // (mk9-lauta 0.5.1-0.6.0 downloaded him again at every start)
  return !InGameRoot();
}

bool StartFetchLocked() {
  if (g_fetch.downloading || g_fetch.ready) return false;
  g_fetch.downloading = true;
  std::thread([] {
#if defined(_WIN32)
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
#else
    setpriority(PRIO_PROCESS, id_t(syscall(SYS_gettid)), 5);  // this thread's nice (Linux's are per thread)
#endif
    FetchWorker();
  }).detach();
  return true;
}

// The window before the game: the download's bar, and a button to play without him meanwhile (the download goes on).
#if defined(_WIN32)
constexpr int kIdSkip = 101;
constexpr UINT_PTR kTimer = 1;
struct FetchWindow {
  HWND hwnd = nullptr, bar = nullptr, status = nullptr;
  HFONT font = nullptr, title_font = nullptr;
  UINT dpi = 96;
  bool skipped = false;
};
FetchWindow* g_window = nullptr;

LRESULT CALLBACK FetchWindowProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
  FetchWindow& w = *g_window;
  switch (msg) {
    case WM_TIMER: {
      const Fetch f = FetchStatus();
      if (!f.downloading) {
        DestroyWindow(hwnd);
        return 0;
      }
      SendMessageW(w.bar, PBM_SETPOS, WPARAM(std::clamp(f.progress, 0.0f, 1.0f) * 1000.0f), 0);
      const std::wstring text = std::to_wstring(int(f.progress * float(kZipSize) / 1000000.0f)) + L" de " +
                                std::to_wstring(int((kZipSize + 500000) / 1000000)) + L" MB";
      SetWindowTextW(w.status, text.c_str());
      return 0;
    }
    case WM_COMMAND:
      if (LOWORD(wparam) == kIdSkip) {
        w.skipped = true;
        DestroyWindow(hwnd);
      }
      return 0;
    case WM_CLOSE:
      w.skipped = true;
      DestroyWindow(hwnd);
      return 0;
    case WM_CTLCOLORSTATIC:
      SetBkMode(reinterpret_cast<HDC>(wparam), TRANSPARENT);
      return reinterpret_cast<LRESULT>(GetSysColorBrush(COLOR_WINDOW));
    case WM_DESTROY:
      KillTimer(hwnd, kTimer);
      PostQuitMessage(0);
      return 0;
    default:
      break;
  }
  return DefWindowProcW(hwnd, msg, wparam, lparam);
}

void RunFetchWindow(FetchWindow& w) {
  SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
  INITCOMMONCONTROLSEX controls{sizeof(controls), ICC_PROGRESS_CLASS | ICC_STANDARD_CLASSES};
  InitCommonControlsEx(&controls);
  HINSTANCE instance = GetModuleHandleW(nullptr);
  WNDCLASSEXW wc{sizeof(wc)};
  wc.lpfnWndProc = FetchWindowProc;
  wc.hInstance = instance;
  wc.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512));  // IDC_ARROW
  wc.hbrBackground = GetSysColorBrush(COLOR_WINDOW);
  wc.hIcon = LoadIconW(instance, MAKEINTRESOURCEW(1));
  wc.lpszClassName = L"mk9LautaKratos";
  RegisterClassExW(&wc);
  w.dpi = GetDpiForSystem();
  auto scale = [&](int v) { return MulDiv(v, int(w.dpi), 96); };
  const DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
  RECT area{0, 0, scale(560), scale(230)};
  AdjustWindowRectExForDpi(&area, style, FALSE, 0, w.dpi);
  const int ww = area.right - area.left, wh = area.bottom - area.top;
  w.hwnd = CreateWindowExW(0, wc.lpszClassName, L"MK9 Online by Lauta - Kratos", style,
                           (GetSystemMetrics(SM_CXSCREEN) - ww) / 2, (GetSystemMetrics(SM_CYSCREEN) - wh) / 2, ww, wh,
                           nullptr, nullptr, instance, nullptr);
  NONCLIENTMETRICSW metrics{sizeof(metrics)};
  SystemParametersInfoForDpi(SPI_GETNONCLIENTMETRICS, sizeof(metrics), &metrics, 0, w.dpi);
  metrics.lfMessageFont.lfHeight = -MulDiv(10, int(w.dpi), 72);
  w.font = CreateFontIndirectW(&metrics.lfMessageFont);
  LOGFONTW title = metrics.lfMessageFont;
  title.lfHeight = -MulDiv(15, int(w.dpi), 72);
  title.lfWeight = FW_BOLD;
  w.title_font = CreateFontIndirectW(&title);
  auto make = [&](const wchar_t* cls, const wchar_t* text, DWORD st, int x, int y, int cw, int ch, int id) {
    HWND control = CreateWindowExW(0, cls, text, WS_CHILD | WS_VISIBLE | st, scale(x), scale(y), scale(cw), scale(ch),
                                   w.hwnd, reinterpret_cast<HMENU>(intptr_t(id)), instance, nullptr);
    SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(w.font), TRUE);
    return control;
  };
  HWND heading = make(L"STATIC", L"Descargando a Kratos", 0, 24, 16, 512, 30, 0);
  SendMessageW(heading, WM_SETFONT, reinterpret_cast<WPARAM>(w.title_font), TRUE);
  make(L"STATIC",
       L"Kratos se suma al juego (una sola vez, unos 96 MB). Cuando termine, el juego arranca con él. Si querés, "
       L"podés jugar ya: se sigue descargando y aparece la próxima vez que abras el juego.",
       0, 24, 52, 512, 56, 0);
  w.bar = make(PROGRESS_CLASSW, L"", 0, 24, 118, 512, 20, 0);
  SendMessageW(w.bar, PBM_SETRANGE32, 0, 1000);
  w.status = make(L"STATIC", L"", SS_LEFT, 24, 146, 300, 22, 0);
  make(L"BUTTON", L"Jugar sin Kratos por ahora", BS_PUSHBUTTON, 316, 176, 220, 34, kIdSkip);
  ShowWindow(w.hwnd, SW_SHOW);
  SetForegroundWindow(w.hwnd);
  SetTimer(w.hwnd, kTimer, 150, nullptr);
  MSG msg;
  while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
    if (!IsDialogMessageW(w.hwnd, &msg)) {
      TranslateMessage(&msg);
      DispatchMessageW(&msg);
    }
  }
  DeleteObject(w.font);
  DeleteObject(w.title_font);
}
#else
// mk9-lauta (2026-11-13): Linux: a small window of the game's (mk9_small_window.h), the same words, bar and button.
struct FetchWindow {
  bool skipped = false;
};
FetchWindow* g_window = nullptr;

void RunFetchWindow(FetchWindow& w) {
  small_window::Run(
      "MK9 Online by Lauta - Kratos", 560, 230,
      [&w](const small_window::Look& look) {
        const Fetch f = FetchStatus();
        if (!f.downloading) return false;
        const float s = look.scale;
        ImGui::PushFont(look.title, 28.0f * s);
        ImGui::TextUnformatted("Descargando a Kratos");
        ImGui::PopFont();
        ImGui::TextWrapped(
            "Kratos se suma al juego (una sola vez, unos 96 MB). Cuando termine, el juego arranca con él. Si querés, "
            "podés jugar ya: se sigue descargando y aparece la próxima vez que abras el juego.");
        ImGui::Spacing();
        ImGui::ProgressBar(std::clamp(f.progress, 0.0f, 1.0f), ImVec2(-1.0f, 20.0f * s), "");
        ImGui::Text("%d de %d MB", int(f.progress * float(kZipSize) / 1000000.0f), int((kZipSize + 500000) / 1000000));
        ImGui::SameLine(ImGui::GetWindowWidth() - 240.0f * s - ImGui::GetStyle().WindowPadding.x);
        if (ImGui::Button("Jugar sin Kratos por ahora", ImVec2(240.0f * s, 34.0f * s))) {
          w.skipped = true;
          return false;
        }
        return true;
      },
      [&w] {
        w.skipped = true;
        return true;
      });
}
#endif

void FetchAtStart() {
  if (!FetchWanted()) return;
  {
    std::lock_guard<std::mutex> lock(g_fetch_mutex);
    if (!StartFetchLocked()) return;
  }
  FetchWindow window;
  g_window = &window;
#if defined(_WIN32)
  std::thread ui([&window] { RunFetchWindow(window); });  // a message loop of its own
  ui.join();
#else
  RunFetchWindow(window);  // SDL's window: on the main thread (this one), as SDL asks
#endif
  g_window = nullptr;
  REXLOG_INFO("[kratos] the start's window: {}", window.skipped ? "the player went on without him (the download goes on)"
                                                                : "the download ended");
}

}  // namespace

void StartFetch() {
  if (!FetchWanted()) return;
  std::lock_guard<std::mutex> lock(g_fetch_mutex);
  StartFetchLocked();
}

Fetch FetchStatus() {
  std::lock_guard<std::mutex> lock(g_fetch_mutex);
  return g_fetch;
}

}  // namespace mk9::kratos

#else  // the Switch: no installer

namespace mk9::kratos {
namespace {
bool InstallNow(const std::filesystem::path&) { return false; }
void InstallDownloaded() {}
}  // namespace
void StartFetch() {}
Fetch FetchStatus() { return {}; }
}  // namespace mk9::kratos

#endif
