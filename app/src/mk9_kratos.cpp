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

}  // namespace

void AtStart(const std::filesystem::path& game_root) {
  if (game_root.empty()) return;
  g_game_root = game_root;
  RemoveIfAsked(game_root);
  if (const std::string test = REXCVAR_GET(mk9_kratos_install); !test.empty()) {
    REXLOG_INFO("[kratos] installing {} (mk9_kratos_install)", test);
    InstallNow(rex::to_path(test));
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

bool Partial() { return g_packages.load(std::memory_order_relaxed) && !Installed(); }

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

#if defined(_WIN32)

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <shobjidl.h>

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

std::mutex g_status_mutex;
InstallStatus g_status;

void SetStep(const std::string& step, float progress) {
  std::lock_guard<std::mutex> lock(g_status_mutex);
  g_status.step = step;
  g_status.progress = progress;
}

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
  temp += L".kratos_tmp";
  {
    std::ofstream out(temp, std::ios::binary | std::ios::trunc);
    if (!out) return false;
    out.write(data.data(), std::streamsize(data.size()));
    if (!out.good()) return false;
  }
  return MoveFileExW(temp.c_str(), file.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
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
  SetStep("Kratos instalado: reiniciá el juego para jugar con él.", 1.0f);
  return true;
}

bool InstallNow(const fs::path& from) {
  {
    std::lock_guard<std::mutex> lock(g_status_mutex);
    g_status = InstallStatus{};
    g_status.busy = true;
  }
  std::string why;
  const bool ok = InstallSteps(from, why);
  if (ok) {
    REXLOG_INFO("[kratos] installed from {}", rex::path_to_utf8(from));
  } else {
    REXLOG_ERROR("[kratos] the install from {} failed: {}", rex::path_to_utf8(from), why);
  }
  std::lock_guard<std::mutex> lock(g_status_mutex);
  g_status.busy = false;
  g_status.done = ok;
  g_status.failed = !ok;
  if (!ok) g_status.step = "No se pudo instalar: " + why + ".";
  return ok;
}

fs::path RemoveMarker(const fs::path& game_root) { return game_root / "kratos_backup" / "remove_at_start"; }

}  // namespace

void InstallFrom(const fs::path& zip_or_folder) {
  {
    std::lock_guard<std::mutex> lock(g_status_mutex);
    if (g_status.busy) return;
    g_status.busy = true;
  }
  std::thread([zip_or_folder] { InstallNow(zip_or_folder); }).detach();
}

void ChooseAndInstall() {
  {
    std::lock_guard<std::mutex> lock(g_status_mutex);
    if (g_status.busy) return;
  }
  const HWND owner = GetForegroundWindow();
  std::thread([owner] {
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    std::wstring path;
    IFileOpenDialog* dialog = nullptr;
    if (SUCCEEDED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dialog)))) {
      const COMDLG_FILTERSPEC types[] = {{L"Kratos-MK9-360-Share.zip", L"*.zip"}, {L"*.*", L"*.*"}};
      dialog->SetFileTypes(2, types);
      dialog->SetTitle(L"Elegí el zip del port de Kratos (Kratos-MK9-360-Share.zip)");
      DWORD options = 0;
      if (SUCCEEDED(dialog->GetOptions(&options))) dialog->SetOptions(options | FOS_FORCEFILESYSTEM | FOS_FILEMUSTEXIST);
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
    if (!path.empty()) InstallNow(fs::path(path));
  }).detach();
}

InstallStatus Status() {
  std::lock_guard<std::mutex> lock(g_status_mutex);
  return g_status;
}

void RemoveAtNextStart() {
  if (g_game_root.empty()) return;
  std::error_code ec;
  fs::create_directories(RemoveMarker(g_game_root).parent_path(), ec);
  std::ofstream(RemoveMarker(g_game_root)) << "remove\n";
  REXLOG_INFO("[kratos] removal asked for: at the next start");
}

bool RemovePending() {
  std::error_code ec;
  return !g_game_root.empty() && fs::exists(RemoveMarker(g_game_root), ec);
}

void RemoveIfAsked(const fs::path& game_root) {
  std::error_code ec;
  if (!fs::exists(RemoveMarker(game_root), ec)) return;
  const fs::path backup = game_root / "kratos_backup";
  // the disc's two back (only the right ones), then his files
  if (ShaOf(backup / "Coalesced.ini") == kDiscCoalescedSha && ShaOf(backup / "Xbox360TOC.txt") == kDiscTocSha) {
    std::string coal, toc;
    if (ReadAll(backup / "Coalesced.ini", coal) && ReadAll(backup / "Xbox360TOC.txt", toc)) {
      WriteNew(game_root / "Config" / "Coalesced.ini", coal);
      WriteNew(game_root / "Xbox360TOC.txt", toc);
    }
  } else {
    REXLOG_ERROR("[kratos] removal: the kept Coalesced.ini / Xbox360TOC.txt are not the disc's; left as they are");
  }
  for (const char* name : kPackages) fs::remove(game_root / "Asset" / name, ec);
  fs::remove_all(PackageFolder(game_root), ec);
  fs::remove_all(backup, ec);
  REXLOG_INFO("[kratos] removed from {}", rex::path_to_utf8(game_root));
}

}  // namespace mk9::kratos

#else  // not Windows: no installer

namespace mk9::kratos {
namespace {
bool InstallNow(const std::filesystem::path&) { return false; }
}  // namespace
void InstallFrom(const std::filesystem::path&) {}
void ChooseAndInstall() {}
InstallStatus Status() { return {}; }
void RemoveAtNextStart() {}
bool RemovePending() { return false; }
void RemoveIfAsked(const std::filesystem::path&) {}
}  // namespace mk9::kratos

#endif
