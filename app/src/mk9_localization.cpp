// mk9-nx online (2026-11-09): the game's own texts where the PC version is not the Xbox 360's (mk9_localization.h).

#include "mk9_localization.h"

#include "mk9_platform.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#if defined(_WIN32)
#include <windows.h>
#endif

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <memory>
#include <fstream>
#include <iterator>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#include <rex/cvar.h>
#include <rex/filesystem/devices/host_path_device.h>
#include <rex/filesystem/vfs.h>
#include <rex/logging.h>

REXCVAR_DEFINE_BOOL(mk9_pc_texts, true, "MK9",
                    "The main menu's Xbox LIVE entry and its message point to this version's online (F1), in the "
                    "game's languages (mk9_localization.h)")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

namespace mk9::localization {
namespace {

// The game's file system mounts game_root here (rex/system/runtime.cpp SetupVfs).
constexpr std::string_view kGameMount = "\\Device\\Harddisk0\\Partition1";

struct Texts {
  const char* extension;  // Coalesced.<extension>
  const char* entry;      // the menu entry
  const char* about;      // its description
  const char* message;    // what choosing it says
};

// UTF-8 here; the game's files are Windows-1252.
const Texts kTexts[] = {
    {"spa", "ONLINE (F1)", "Apretá F1 para jugar online: salas, partida rápida y chat.",
     "El online de esta versión se abre con F1: salas, partida rápida y chat global."},
    {"int", "ONLINE (F1)", "Press F1 to play online: rooms, quick match and chat.",
     "This version's online opens with F1: rooms, quick match and global chat."},
    {"eng", "ONLINE (F1)", "Press F1 to play online: rooms, quick match and chat.",
     "This version's online opens with F1: rooms, quick match and global chat."},
    {"por", "ONLINE (F1)", "Aperte F1 para jogar online: salas, partida rápida e chat.",
     "O online desta versão abre com F1: salas, partida rápida e chat global."},
    {"fra", "EN LIGNE (F1)", "Appuyez sur F1 pour jouer en ligne : salons, partie rapide et chat.",
     "Le jeu en ligne de cette version s'ouvre avec F1 : salons, partie rapide et chat."},
    {"ger", "ONLINE (F1)", "Drücke F1, um online zu spielen: Räume, schnelles Spiel und Chat.",
     "Das Online-Spiel dieser Version öffnet sich mit F1: Räume, schnelles Spiel und Chat."},
    {"ita", "ONLINE (F1)", "Premi F1 per giocare online: stanze, partita rapida e chat.",
     "L'online di questa versione si apre con F1: stanze, partita rapida e chat."},
};

#if defined(_WIN32)
std::string Cp1252(std::string_view utf8) {
  const int wide_size = MultiByteToWideChar(CP_UTF8, 0, utf8.data(), int(utf8.size()), nullptr, 0);
  std::wstring wide(size_t(std::max(wide_size, 0)), L'\0');
  MultiByteToWideChar(CP_UTF8, 0, utf8.data(), int(utf8.size()), wide.data(), wide_size);
  const int size = WideCharToMultiByte(1252, 0, wide.data(), int(wide.size()), nullptr, 0, nullptr, nullptr);
  std::string out(size_t(std::max(size, 0)), '\0');
  WideCharToMultiByte(1252, 0, wide.data(), int(wide.size()), out.data(), size, nullptr, nullptr);
  return out;
}
#else
// mk9-lauta (2026-11-13): Linux: UTF-8 to Windows-1252 by its table (what WideCharToMultiByte(1252) gives: Latin-1 as
// it is, the 27 characters of 0x80-0x9F, '?' for the rest)
std::string Cp1252(std::string_view utf8) {
  static const std::pair<char32_t, uint8_t> kExtra[] = {
      {0x20AC, 0x80}, {0x201A, 0x82}, {0x0192, 0x83}, {0x201E, 0x84}, {0x2026, 0x85}, {0x2020, 0x86}, {0x2021, 0x87},
      {0x02C6, 0x88}, {0x2030, 0x89}, {0x0160, 0x8A}, {0x2039, 0x8B}, {0x0152, 0x8C}, {0x017D, 0x8E}, {0x2018, 0x91},
      {0x2019, 0x92}, {0x201C, 0x93}, {0x201D, 0x94}, {0x2022, 0x95}, {0x2013, 0x96}, {0x2014, 0x97}, {0x02DC, 0x98},
      {0x2122, 0x99}, {0x0161, 0x9A}, {0x203A, 0x9B}, {0x0153, 0x9C}, {0x017E, 0x9E}, {0x0178, 0x9F}};
  std::string out;
  for (size_t i = 0; i < utf8.size();) {
    const uint8_t b = uint8_t(utf8[i]);
    char32_t c = b;
    size_t n = 1;
    if (b >= 0xF0 && i + 3 < utf8.size()) {
      c = char32_t(b & 0x07) << 18 | char32_t(utf8[i + 1] & 0x3F) << 12 | char32_t(utf8[i + 2] & 0x3F) << 6 |
          char32_t(utf8[i + 3] & 0x3F);
      n = 4;
    } else if (b >= 0xE0 && i + 2 < utf8.size()) {
      c = char32_t(b & 0x0F) << 12 | char32_t(utf8[i + 1] & 0x3F) << 6 | char32_t(utf8[i + 2] & 0x3F);
      n = 3;
    } else if (b >= 0xC0 && i + 1 < utf8.size()) {
      c = char32_t(b & 0x1F) << 6 | char32_t(utf8[i + 1] & 0x3F);
      n = 2;
    }
    i += n;
    if (c < 0x80 || (c >= 0xA0 && c <= 0xFF)) {
      out += char(c);
      continue;
    }
    char mapped = '?';
    for (const auto& [code, byte] : kExtra) {
      if (code == c) mapped = char(byte);
    }
    out += mapped;
  }
  return out;
}
#endif

uint32_t Get32(const std::string& data, size_t at) {
  return uint32_t(uint8_t(data[at])) << 24 | uint32_t(uint8_t(data[at + 1])) << 16 |
         uint32_t(uint8_t(data[at + 2])) << 8 | uint32_t(uint8_t(data[at + 3]));
}

void Put32(std::string& out, uint32_t v) {
  out.push_back(char(v >> 24));
  out.push_back(char(v >> 16));
  out.push_back(char(v >> 8));
  out.push_back(char(v));
}

// In one ini text: the keys' values in [section] replaced, every other byte as it was. Returns how many.
int ReplaceKeys(std::string& text, std::string_view section,
                const std::vector<std::pair<std::string, std::string>>& values) {
  const std::string header = "[" + std::string(section) + "]";
  std::string out;
  out.reserve(text.size() + 256);
  bool inside = false;
  int replaced = 0;
  size_t at = 0;
  while (at < text.size()) {
    size_t end = text.find('\n', at);
    const bool newline = end != std::string::npos;
    if (!newline) end = text.size();
    size_t line_end = end;
    if (line_end > at && text[line_end - 1] == '\r') --line_end;
    const std::string_view line(text.data() + at, line_end - at);
    std::string kept(line);
    if (!line.empty() && line.front() == '[') {
      inside = line == header;
    } else if (inside) {
      for (const auto& [key, value] : values) {
        if (line.size() > key.size() && line.compare(0, key.size(), key) == 0 && line[key.size()] == '=') {
          kept = key + "=" + value;
          ++replaced;
          break;
        }
      }
    }
    out += kept;
    out.append(text, line_end, end - line_end);  // a carriage return
    if (newline) out.push_back('\n');
    at = end + 1;
  }
  text = std::move(out);
  return replaced;
}

// A Coalesced file (FArchive of strings: u32 count, then for each an i32 length with its null, ANSI when positive)
// with the texts of this version; the same bytes when there is nothing to change. Wide strings stay as they are.
std::string Rewrite(const std::string& data, const Texts& texts, int& replaced) {
  replaced = 0;
  if (data.size() < 4) return data;
  std::string out = data.substr(0, 4);
  const uint32_t count = Get32(data, 0);
  size_t at = 4;
  std::string last_name;
  for (uint32_t i = 0; i < count; ++i) {
    if (at + 4 > data.size()) return data;
    const int32_t length = int32_t(Get32(data, at));
    const size_t bytes = length >= 0 ? size_t(length) : size_t(-int64_t(length)) * 2;
    if (at + 4 + bytes > data.size()) return data;
    std::string value = data.substr(at + 4, bytes);
    at += 4 + bytes;
    // strings alternate: a file's name, then its contents
    if (i % 2 == 0 || length <= 0) {
      if (i % 2 == 0) last_name = value;
      Put32(out, uint32_t(length));
      out += value;
      continue;
    }
    std::string text = value.substr(0, value.size() - 1);  // without its null
    if (last_name.find("MK9Game.") != std::string::npos) {
      replaced += ReplaceKeys(text, "MainMenu",
                              {{"XboxLIVE", Cp1252(texts.entry)},
                               {"XboxLIVEDesc", Cp1252(texts.about)},
                               {"XboxLIVE2Player", Cp1252(texts.entry)},
                               {"XboxLIVE2PlayerDesc", Cp1252(texts.about)}});
    } else if (last_name.find("MKOnline.") != std::string::npos) {
      replaced += ReplaceKeys(text, "General", {{"MultiplayerPrivilegeError_Message", Cp1252(texts.message)}});
    }
    Put32(out, uint32_t(text.size() + 1));
    out += text;
    out.push_back('\0');
  }
  out += data.substr(at);
  return out;
}

std::string ReadAll(const std::filesystem::path& path) {
  std::ifstream in(path, std::ios::binary);
  return in ? std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()) : std::string();
}

}  // namespace

void Install(rex::filesystem::VirtualFileSystem* vfs, const std::filesystem::path& game_root) {
  if (!vfs || game_root.empty() || !REXCVAR_GET(mk9_pc_texts)) return;
  std::error_code ec;
  const std::filesystem::path source = game_root / "Localization";
  if (!std::filesystem::is_directory(source, ec)) return;
  const std::filesystem::path folder = mk9::platform::ExeDir() / "cache" / "localization";
  std::filesystem::create_directories(folder, ec);
  int files = 0, texts = 0;
  for (const auto& entry : std::filesystem::directory_iterator(source, ec)) {
    if (!entry.is_regular_file(ec)) continue;
    const std::string data = ReadAll(entry.path());
    std::string written = data;
    std::string extension = entry.path().extension().string();
    if (!extension.empty()) extension.erase(0, 1);
    for (char& c : extension) c = char(std::tolower(static_cast<unsigned char>(c)));
    for (const Texts& t : kTexts) {
      if (extension != t.extension) continue;
      int replaced = 0;
      written = Rewrite(data, t, replaced);
      texts += replaced;
    }
    const std::filesystem::path target = folder / entry.path().filename();
    if (ReadAll(target) != written) {
      std::ofstream out(target, std::ios::binary | std::ios::trunc);
      out.write(written.data(), std::streamsize(written.size()));
    }
    ++files;
  }
  const std::string mount = std::string(kGameMount) + "\\Localization";
  auto device = std::make_unique<rex::filesystem::HostPathDevice>(mount, folder, true);
  if (!device->Initialize() || !vfs->RegisterDevice(std::move(device))) {
    REXLOG_WARN("[texts] the PC texts could not be mounted ({}): the game's own", folder.string());
    return;
  }
  REXLOG_INFO("[texts] {} localization files, {} texts for the PC (the Xbox LIVE entry: F1), mounted over {}",
              files, texts, mount);
}

}  // namespace mk9::localization
