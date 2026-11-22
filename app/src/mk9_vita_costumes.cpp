// mk9-lauta: the PS Vita version's extra costumes (mk9_vita_costumes.h).

#include "mk9_vita_costumes.h"

#if !defined(MK9_VITA_COSTUMES)
// The PC builds only (app/CMakeLists.txt: the patches and miniz are there).
namespace mk9::vita_costumes {
void Install(rex::filesystem::VirtualFileSystem*, const std::filesystem::path&) {}
bool Installed() { return false; }
uint32_t Signature() { return 0; }
std::string Summary() { return {}; }
void AtStart() {}
bool PackIn() { return false; }
bool PackMissing() { return false; }
}  // namespace mk9::vita_costumes
#else

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <fstream>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include <rex/cvar.h>
#include <rex/filesystem.h>
#include <rex/filesystem/devices/host_path_device.h>
#include <rex/filesystem/vfs.h>
#include <rex/logging.h>

#include <fmt/format.h>

#include "miniz.h"
#include "mk9_fetch_window.h"
#include "mk9_platform.h"
#include "mk9_updater.h"
#include "online/i18n.h"

REXCVAR_DEFINE_BOOL(mk9_vita_costumes, true, "MK9",
                    "The PS Vita version's extra costumes, with their Babalities: the MK2 retro ones of Scorpion, "
                    "Sub-Zero, Reptile, Ermac and Mileena and Skarlet's MK2 and MK3, made from the player's game files "
                    "at the first start, and the MK3 retro ones of Scorpion, Sub-Zero, Reptile, Ermac, Smoke, Noob and "
                    "Rain and Kenshi's and Skarlet's alternate ones, from the pack of the Vita's own meshes and "
                    "textures (downloaded once, about 15 MB) (mk9_vita_costumes.h)")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_STRING(mk9_vita_costumes_pack, "", "MK9",
                      "Tests: the pack of the costumes made of the PS Vita's own meshes and textures read from this "
                      "file (tools/costumes/make_mk3_pack.py) instead of the downloaded one")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

namespace mk9::vita_costumes {
namespace {

// The patches and the INI lines (tools/costumes/make_bundle.py: no game data, the bytes that are new)
const unsigned char kVitaCostumes[] = {
#embed "../costumes/vita_costumes.bin"
};
constexpr size_t kVitaCostumesSize = sizeof(kVitaCostumes);

// The pack of the costumes made of the Vita's own meshes and textures (tools/costumes/make_mk3_pack.py): the same
// format, but its new bytes are the Vita's data, converted, so it is not built in: it is hosted apart, in a repository
// of its own (as the Kratos port is), and downloaded at the start (AtStart). A pack with other bytes is a release of its
// own there (v2, ...), named here by the game version that uses it. Tests: mk9_vita_costumes_pack, a local file.
constexpr const char* kPackUrl =
    "https://github.com/machinebreak/mk9-lauta-vita-costumes/releases/download/v1/vita_costumes_pack.bin";
constexpr const char* kPackSha = "e96f7e310f900f340f8149fdfa11979c4b1d279677e0c20848f286f7e870d64d";
constexpr uint64_t kPackSize = 15309810;

namespace fs = std::filesystem;

constexpr std::string_view kGameMount = "\\Device\\Harddisk0\\Partition1";
constexpr uint32_t kPackageTag = 0x9E2A83C1;

std::atomic<bool> g_installed{false};
std::atomic<uint32_t> g_signature{0};
std::string g_summary;

uint16_t Be16(const uint8_t* p) { return uint16_t((p[0] << 8) | p[1]); }
uint32_t Be32(const uint8_t* p) { return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3]; }
uint64_t Be64(const uint8_t* p) { return (uint64_t(Be32(p)) << 32) | Be32(p + 4); }

// A reader over the bundle and the patches: every read checked against the end.
struct Reader {
  const uint8_t* p;
  const uint8_t* end;
  bool ok = true;
  bool Need(size_t n) {
    if (!ok || size_t(end - p) < n) ok = false;
    return ok;
  }
  uint16_t U16() { return Need(2) ? (p += 2, Be16(p - 2)) : 0; }
  uint32_t U32() { return Need(4) ? (p += 4, Be32(p - 4)) : 0; }
  uint64_t U64() { return Need(8) ? (p += 8, Be64(p - 8)) : 0; }
  std::string Str(size_t n) {
    if (!Need(n)) return {};
    std::string s(reinterpret_cast<const char*>(p), n);
    p += n;
    return s;
  }
  const uint8_t* Bytes(size_t n) { return Need(n) ? (p += n, p - n) : nullptr; }
};

// ---------------------------------------------------------------------------------------------------------------------
// LZO1X, the game's package compression: lzo1x_decompress_safe's state machine (every read and write checked).

bool Lzo1x(const uint8_t* src, size_t src_len, uint8_t* dst, size_t dst_len) {
  const uint8_t* ip = src;
  const uint8_t* const ip_end = src + src_len;
  uint8_t* op = dst;
  uint8_t* const op_end = dst + dst_len;
  size_t t = 0;
  auto have = [&](size_t n) { return size_t(ip_end - ip) >= n; };
  auto room = [&](size_t n) { return size_t(op_end - op) >= n; };
  auto literals = [&](size_t n) {
    if (!have(n) || !room(n)) return false;
    std::memcpy(op, ip, n);
    op += n;
    ip += n;
    return true;
  };
  auto run = [&](size_t base, size_t& out) {  // a length's 0 bytes (255 each), then its last byte
    size_t n = 0;
    while (true) {
      if (!have(1)) return false;
      if (*ip != 0) break;
      n += 255;
      ++ip;
    }
    out = n + base + *ip++;
    return true;
  };
  auto copy = [&](size_t back, size_t n) {  // from 'back' bytes before (may overlap the bytes it writes)
    if (back == 0 || back > size_t(op - dst) || !room(n)) return false;
    const uint8_t* from = op - back;
    for (size_t i = 0; i < n; ++i) *op++ = from[i];
    return true;
  };
  enum { kLoop, kFirstLiteralRun, kMatch, kMatchDone, kMatchNext } state = kLoop;
  if (!have(1)) return false;
  if (*ip > 17) {
    t = size_t(*ip++) - 17;
    if (t < 4) {
      state = kMatchNext;
    } else {
      if (!literals(t)) return false;
      state = kFirstLiteralRun;
    }
  }
  while (true) {
    switch (state) {
      case kLoop:
        if (!have(1)) return false;
        t = *ip++;
        if (t >= 16) {
          state = kMatch;
          break;
        }
        if (t == 0 && !run(15, t)) return false;
        if (!literals(t + 3)) return false;
        state = kFirstLiteralRun;
        break;
      case kFirstLiteralRun: {
        if (!have(2)) return false;
        t = *ip++;
        if (t >= 16) {
          state = kMatch;
          break;
        }
        const size_t back = 1 + 0x0800 + (t >> 2) + (size_t(*ip++) << 2);
        if (!copy(back, 3)) return false;
        state = kMatchDone;
        break;
      }
      case kMatch: {
        if (t >= 64) {
          if (!have(1)) return false;
          const size_t back = 1 + ((t >> 2) & 7) + (size_t(*ip++) << 3);
          if (!copy(back, (t >> 5) + 1)) return false;
        } else if (t >= 32) {
          t &= 31;
          if (t == 0 && !run(31, t)) return false;
          if (!have(2)) return false;
          const size_t back = 1 + (ip[0] >> 2) + (size_t(ip[1]) << 6);
          ip += 2;
          if (!copy(back, t + 2)) return false;
        } else if (t >= 16) {
          const size_t far = (t & 8) << 11;
          t &= 7;
          if (t == 0 && !run(7, t)) return false;
          if (!have(2)) return false;
          const size_t back = far + (ip[0] >> 2) + (size_t(ip[1]) << 6);
          ip += 2;
          if (back == 0) return op == op_end;  // the end of the stream: all of the block written
          if (!copy(back + 0x4000, t + 2)) return false;
        } else {
          if (!have(1)) return false;
          const size_t back = 1 + (t >> 2) + (size_t(*ip++) << 2);
          if (!copy(back, 2)) return false;
        }
        state = kMatchDone;
        break;
      }
      case kMatchDone:
        t = ip[-2] & 3;
        state = t == 0 ? kLoop : kMatchNext;
        break;
      case kMatchNext:
        if (!literals(t) || !have(1)) return false;
        t = *ip++;
        state = kMatch;
        break;
    }
  }
}

// A package's decompressed image, the coordinates the patches copy from: the file's bytes up to its first chunk's
// uncompressed offset (the summary), then each LZO chunk decompressed at its offset.
bool PackageImage(const std::string& raw, std::vector<uint8_t>& img, std::string& why) {
  const auto* d = reinterpret_cast<const uint8_t*>(raw.data());
  const size_t n = raw.size();
  if (n < 64 || Be32(d) != kPackageTag) return why = "no es un paquete del juego", false;
  const int32_t folder = int32_t(Be32(d + 20));
  if (folder < 0 || folder > 256) return why = "encabezado raro", false;
  const size_t flags_at = 24 + size_t(folder);
  size_t table = 0, count = 0;
  for (size_t p = flags_at + 32; p + 24 <= std::min<size_t>(n, 0x400); ++p) {  // the compression flags (2), the count
    if (Be32(d + p) != 2) continue;
    const uint32_t c = Be32(d + p + 4);
    if (c == 0 || c >= 4096) continue;
    const uint32_t us = Be32(d + p + 12), co = Be32(d + p + 16);
    if (us == 0 || us > 0x200000 || co == 0 || co + 4 > n || Be32(d + co) != kPackageTag) continue;
    table = p + 8;
    count = c;
    break;
  }
  if (!table || table + 16 * count > n) return why = "sin tabla de bloques", false;
  size_t size = 0;
  for (size_t i = 0; i < count; ++i) {
    size = std::max<size_t>(size, size_t(Be32(d + table + 16 * i)) + Be32(d + table + 16 * i + 4));
  }
  const size_t first = Be32(d + table);
  if (first > n || first > size || size > (1u << 30)) return why = "tabla de bloques rara", false;
  img.assign(size, 0);
  std::memcpy(img.data(), d, first);
  for (size_t i = 0; i < count; ++i) {
    const size_t uo = Be32(d + table + 16 * i), us = Be32(d + table + 16 * i + 4), co = Be32(d + table + 16 * i + 8);
    if (co + 16 > n || Be32(d + co) != kPackageTag) return why = "bloque roto", false;
    const size_t block = Be32(d + co + 4), usize = Be32(d + co + 12);
    if (block == 0 || usize < us || uo + us > size) return why = "bloque roto", false;
    const size_t blocks = (usize + block - 1) / block;
    size_t p = co + 16 + 8 * blocks, out = uo;
    if (p > n) return why = "bloque roto", false;
    std::vector<uint8_t> tail;
    for (size_t b = 0; b < blocks; ++b) {
      const size_t bc = Be32(d + co + 16 + 8 * b), bu = Be32(d + co + 16 + 8 * b + 4);
      if (p + bc > n) return why = "bloque cortado", false;
      if (out + bu <= uo + us) {
        if (!Lzo1x(d + p, bc, img.data() + out, bu)) return why = "LZO roto", false;
      } else {  // the chunk's last block past its size (never in the game's): through a buffer
        tail.assign(bu, 0);
        if (!Lzo1x(d + p, bc, tail.data(), bu)) return why = "LZO roto", false;
        std::memcpy(img.data() + out, tail.data(), std::min(bu, uo + us - out));
      }
      p += bc;
      out += bu;
    }
  }
  return true;
}

// ---------------------------------------------------------------------------------------------------------------------
// The bundle: 'MK9V' u32 version; u32 costumes; per costume: u16+key, u32 DLC id, u16 INI edits (u8 kind: 0 a line
// added to the top of a section if it is not there, 1 a key set in a section made before [IniVersion] if missing;
// u16+section, u16+line), u16 patches (u32+the patch). A patch (tools/costumes/makepatch.py): 'MK9C' u32 1; u16
// sources (u16+path, u64 size, 32-byte SHA-256); u16+target path, u64 size, 32-byte SHA-256; u32+zlib of the
// operations (1 COPY varint source, offset, length; 2 DATA varint length, bytes; 0 END).

struct Source {
  std::string path;
  uint64_t size = 0;
};

struct Patch {
  std::vector<Source> sources;
  std::string target;
  uint64_t size = 0;
  uint8_t sha[32] = {};
  const uint8_t* ops = nullptr;
  size_t ops_size = 0;
};

struct IniEdit {
  uint8_t kind = 0;
  std::string section, line;
};

struct Costume {
  std::string key;
  uint32_t id = 0;
  std::vector<IniEdit> ini;
  std::vector<Patch> patches;
  bool from_pack = false;  // the downloaded pack's (made of the Vita's own data)
};

bool ParsePatch(const uint8_t* data, size_t size, Patch& patch) {
  Reader r{data, data + size};
  if (r.Str(4) != "MK9C" || r.U32() != 1) return false;
  const uint16_t sources = r.U16();
  for (uint16_t i = 0; i < sources && r.ok; ++i) {
    Source s;
    s.path = r.Str(r.U16());
    s.size = r.U64();
    r.Bytes(32);  // the source's SHA-256: the target's own covers it (a source of other bytes gives other bytes)
    patch.sources.push_back(std::move(s));
  }
  patch.target = r.Str(r.U16());
  patch.size = r.U64();
  if (const uint8_t* sha = r.Bytes(32)) std::memcpy(patch.sha, sha, 32);
  patch.ops_size = r.U32();
  patch.ops = r.Bytes(patch.ops_size);
  return r.ok && patch.ops && !patch.sources.empty();
}

bool ParseBundle(const uint8_t* data, size_t size, std::vector<Costume>& out) {
  Reader r{data, data + size};
  if (r.Str(4) != "MK9V" || r.U32() != 1) return false;
  const uint32_t count = r.U32();
  for (uint32_t i = 0; i < count && r.ok; ++i) {
    Costume c;
    c.key = r.Str(r.U16());
    c.id = r.U32();
    const uint16_t edits = r.U16();
    for (uint16_t k = 0; k < edits && r.ok; ++k) {
      IniEdit e;
      if (const uint8_t* kind = r.Bytes(1)) e.kind = *kind;
      e.section = r.Str(r.U16());
      e.line = r.Str(r.U16());
      c.ini.push_back(std::move(e));
    }
    const uint16_t patches = r.U16();
    for (uint16_t k = 0; k < patches && r.ok; ++k) {
      const uint32_t size = r.U32();
      const uint8_t* data = r.Bytes(size);
      Patch p;
      if (!data || !ParsePatch(data, size, p)) return false;
      c.patches.push_back(std::move(p));
    }
    out.push_back(std::move(c));
  }
  return r.ok;
}

// ---------------------------------------------------------------------------------------------------------------------
// Files

std::string Lower(std::string text) {
  for (char& c : text) c = char(std::tolower(static_cast<unsigned char>(c)));
  return text;
}

// A game file by its path ('/' separated) as it is on the disk: Linux's file systems tell "Asset" from "asset".
fs::path OnDisk(const fs::path& root, const std::string& relative) {
  fs::path at = root;
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
      if (Lower(rex::path_to_utf8(entry.path().filename())) == Lower(part)) {
        found = entry.path();
        break;
      }
    }
    at = found.empty() ? at / part : found;
  }
  return at;
}

bool ReadAll(const fs::path& file, std::string& out) {
  std::ifstream in(file, std::ios::binary);
  if (!in) return false;
  out.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
  return true;
}

// A file written beside its place, then renamed over it: a half-written one never takes the place of a whole one.
bool WriteNew(const fs::path& file, std::string_view data) {
  std::error_code ec;
  fs::create_directories(file.parent_path(), ec);
  const fs::path part = fs::path(file).concat(".part");
  {
    std::ofstream out(part, std::ios::binary | std::ios::trunc);
    if (!out) return false;
    out.write(data.data(), std::streamsize(data.size()));
    if (!out) return false;
  }
  fs::rename(part, file, ec);
  if (ec) {
    fs::remove(file, ec);
    fs::rename(part, file, ec);
  }
  return !ec;
}

std::string Hex(const uint8_t* data, size_t size) {
  static const char kDigits[] = "0123456789abcdef";
  std::string out;
  for (size_t i = 0; i < size; ++i) {
    out += kDigits[data[i] >> 4];
    out += kDigits[data[i] & 15];
  }
  return out;
}

// Beside each package made, the SHA-256 it was checked against: a release whose patch makes other bytes (a costume
// fixed) makes it again, even at the same size.
fs::path Mark(const fs::path& file) { return fs::path(file).concat(".sha256"); }

uint64_t ReadVarint(const uint8_t*& p, const uint8_t* end, bool& ok) {
  uint64_t v = 0;
  for (int shift = 0; shift < 64; shift += 7) {
    if (p >= end) return ok = false, 0;
    const uint8_t b = *p++;
    v |= uint64_t(b & 0x7F) << shift;
    if (!(b & 0x80)) return v;
  }
  return ok = false, 0;
}

// One package: its sources' images (decompressed once for all the costumes that use them), the patch's operations,
// the file checked against its SHA-256 and written.
bool Make(const Patch& patch, const fs::path& game_root, std::map<std::string, std::vector<uint8_t>>& images,
          const fs::path& file, std::string& why) {
  std::vector<const std::vector<uint8_t>*> sources;
  for (const Source& s : patch.sources) {
    auto it = images.find(s.path);
    if (it == images.end()) {
      const fs::path path = OnDisk(game_root, s.path);
      std::error_code ec;
      if (fs::file_size(path, ec) != s.size || ec) return why = s.path + " no es el del juego de Xbox 360", false;
      std::string raw;
      std::vector<uint8_t> img;
      if (!ReadAll(path, raw) || !PackageImage(raw, img, why)) return why = s.path + ": " + why, false;
      it = images.emplace(s.path, std::move(img)).first;
    }
    sources.push_back(&it->second);
  }
  // the operations
  std::vector<uint8_t> ops(4u << 20);
  mz_ulong ops_size = 0;
  for (int tries = 0; tries < 6; ++tries) {
    ops_size = mz_ulong(ops.size());
    const int rc = mz_uncompress(ops.data(), &ops_size, patch.ops, mz_ulong(patch.ops_size));
    if (rc == MZ_OK) break;
    if (rc != MZ_BUF_ERROR) return why = "parche roto", false;
    ops.resize(ops.size() * 4);
  }
  std::string out;
  out.reserve(size_t(patch.size));
  const uint8_t* p = ops.data();
  const uint8_t* const end = ops.data() + ops_size;
  bool ok = true;
  while (ok && p < end) {
    const uint8_t op = *p++;
    if (op == 0) break;
    if (op == 1) {
      const uint64_t source = ReadVarint(p, end, ok), offset = ReadVarint(p, end, ok), length = ReadVarint(p, end, ok);
      if (!ok || source >= sources.size() || offset + length > sources[source]->size()) return why = "parche roto", false;
      out.append(reinterpret_cast<const char*>(sources[source]->data() + offset), size_t(length));
    } else if (op == 2) {
      const uint64_t length = ReadVarint(p, end, ok);
      if (!ok || uint64_t(end - p) < length) return why = "parche roto", false;
      out.append(reinterpret_cast<const char*>(p), size_t(length));
      p += length;
    } else {
      return why = "parche roto", false;
    }
    if (out.size() > patch.size) return why = "parche roto", false;
  }
  if (!ok || out.size() != patch.size) return why = "parche roto", false;
  if (!WriteNew(file, out)) return why = "no se pudo escribir " + rex::path_to_utf8(file), false;
  uint8_t sha[32];
  if (!mk9::updater::FileSha256(file, sha) || std::memcmp(sha, patch.sha, 32) != 0) {
    std::error_code ec;
    fs::remove(file, ec);
    return why = "el traje armado no coincide (¿archivos del juego de otra versión?)", false;
  }
  WriteNew(Mark(file), Hex(patch.sha, 32));
  return true;
}

// Made at an earlier start by this release's patch (its size, and the SHA-256 it was checked against beside it).
bool Made(const fs::path& file, const Patch& patch) {
  std::error_code ec;
  if (fs::file_size(file, ec) != patch.size || ec) return false;
  std::string mark;
  return ReadAll(Mark(file), mark) && mark == Hex(patch.sha, 32);
}

// ---------------------------------------------------------------------------------------------------------------------
// Coalesced.ini: a 4-byte header, then (big-endian length, name) (length, body) per INI file. The costumes' lines go
// to the game's Xe-MK9Game.ini.

void PutBe32(std::string& d, uint32_t v) {
  d.push_back(char(v >> 24));
  d.push_back(char(v >> 16));
  d.push_back(char(v >> 8));
  d.push_back(char(v));
}

std::vector<std::string> Split(const std::string& text, const std::string& sep) {
  std::vector<std::string> out;
  size_t start = 0;
  while (true) {
    const size_t at = text.find(sep, start);
    if (at == std::string::npos) {
      out.push_back(text.substr(start));
      return out;
    }
    out.push_back(text.substr(start, at - start));
    start = at + sep.size();
  }
}

// The section's header line and the line after its last one (the next header), or npos.
std::pair<size_t, size_t> SectionSpan(const std::vector<std::string>& lines, const std::string& header) {
  for (size_t i = 0; i < lines.size(); ++i) {
    if (Lower(lines[i]) != Lower(header)) continue;
    size_t e = i + 1;
    while (e < lines.size() && (lines[e].empty() || lines[e][0] != '[')) ++e;
    return {i, e};
  }
  return {std::string::npos, std::string::npos};
}

bool EditIni(std::string& body, const std::vector<const Costume*>& costumes) {
  std::string end;
  if (!body.empty() && body.back() == '\0') {
    body.pop_back();
    end = std::string(1, '\0');
  }
  const std::string eol = body.find("\r\n") != std::string::npos ? "\r\n" : "\n";
  std::vector<std::string> lines = Split(body, eol);
  for (const Costume* c : costumes) {
    for (const IniEdit& edit : c->ini) {
      auto [h, e] = SectionSpan(lines, edit.section);
      if (edit.kind == 0) {
        if (h == std::string::npos) return false;
        if (std::find(lines.begin() + long(h) + 1, lines.begin() + long(e), edit.line) == lines.begin() + long(e)) {
          lines.insert(lines.begin() + long(h) + 1, edit.line);
        }
        continue;
      }
      if (h == std::string::npos) {  // a section of its own, before [IniVersion] (or at the end)
        size_t at = SectionSpan(lines, "[IniVersion]").first;
        if (at == std::string::npos) at = lines.size();
        lines.insert(lines.begin() + long(at), {edit.section, std::string()});
        std::tie(h, e) = SectionSpan(lines, edit.section);
      }
      const std::string key = Lower(edit.line.substr(0, edit.line.find('=')));
      bool set = false;
      for (size_t i = h + 1; i < e; ++i) {
        if (Lower(lines[i].substr(0, lines[i].find('='))) == key) {
          lines[i] = edit.line;
          set = true;
          break;
        }
      }
      if (!set) {  // after the section's last line (its lines in the bundle's order)
        size_t at = e;
        while (at > h + 1 && lines[at - 1].empty()) --at;
        lines.insert(lines.begin() + long(at), edit.line);
      }
    }
  }
  body.clear();
  for (size_t i = 0; i < lines.size(); ++i) {
    if (i) body += eol;
    body += lines[i];
  }
  body += end;
  return true;
}

bool EditCoalesced(const std::string& data, const std::vector<const Costume*>& costumes, std::string& out) {
  struct Entry {
    std::string name, body;
  };
  std::vector<Entry> entries;
  const auto* d = reinterpret_cast<const uint8_t*>(data.data());
  size_t o = 4;
  while (o < data.size()) {
    if (o + 4 > data.size()) return false;
    const size_t n = Be32(d + o);
    if (o + 8 + n > data.size()) return false;
    Entry e;
    e.name = data.substr(o + 4, n);
    o += 4 + n;
    const size_t m = Be32(d + o);
    if (o + 4 + m > data.size()) return false;
    e.body = data.substr(o + 4, m);
    o += 4 + m;
    entries.push_back(std::move(e));
  }
  int edited = 0;
  for (Entry& e : entries) {
    std::string name = e.name;
    while (!name.empty() && name.back() == '\0') name.pop_back();
    name = Lower(name);
    if (name.size() < 14 || name.compare(name.size() - 14, 14, "xe-mk9game.ini") != 0) continue;
    if (!EditIni(e.body, costumes)) return false;
    ++edited;
  }
  if (edited != 1) return false;
  out = data.substr(0, 4);
  for (const Entry& e : entries) {
    PutBe32(out, uint32_t(e.name.size()));
    out += e.name;
    PutBe32(out, uint32_t(e.body.size()));
    out += e.body;
  }
  return true;
}

// The TOC: Coalesced.ini's line with its new size, a line per costume package ("<size> 0 0 ..\Asset\<name> 0"; the
// size set when the game's TOC lists it already).
std::string EditToc(const std::string& toc, uint64_t coalesced, const std::vector<std::pair<std::string, uint64_t>>& add) {
  const std::string eol = toc.find("\r\n") != std::string::npos ? "\r\n" : "\n";
  std::vector<std::string> lines = Split(toc, eol);
  while (!lines.empty() && lines.back().empty()) lines.pop_back();
  // a file's line ("<size> 0 0 <path> 0") given its new size; false when the TOC does not list it
  auto set_size = [&](const std::string& path, uint64_t size) {
    bool found = false;
    for (std::string& line : lines) {
      const size_t space = line.find(' ');
      const std::string low = Lower(line);
      const size_t at = low.find(Lower(path));
      if (space == std::string::npos || at == std::string::npos || at == 0 || low[at - 1] != ' ' ||
          low.compare(at + path.size(), 1, " ") != 0) {
        continue;
      }
      line = std::to_string(size) + line.substr(space);
      found = true;
    }
    return found;
  };
  set_size("..\\Config\\Coalesced.ini", coalesced);
  for (const auto& [name, size] : add) {
    if (!set_size("..\\Asset\\" + name, size)) lines.push_back(std::to_string(size) + " 0 0 ..\\Asset\\" + name + " 0");
  }
  std::string out;
  for (const std::string& line : lines) out += line + eol;
  return out;
}

uint32_t Fnv(std::string_view text, uint32_t h = 2166136261u) {
  for (unsigned char c : text) h = (h ^ c) * 16777619u;
  return h;
}

std::string g_pack;  // the pack's bytes (its patches point into them)
// The start's download of it (AtStart): thousandths, and whether it goes on (the window's bar)
std::atomic<int> g_pack_got{0};
std::atomic<bool> g_pack_downloading{false};
bool g_pack_checked = false;         // PackPath() is the published pack: checked at this start (or just downloaded)
bool g_pack_missing = false;         // wanted at this start, but not here (its download failed: F2 says so)
std::atomic<bool> g_pack_in{false};  // its costumes are in this game (the rooms say so)

fs::path PackPath() { return mk9::platform::ExeDir() / "costumes" / "vita_costumes_pack.bin"; }

// The published pack: its size, then its SHA-256.
bool PackGood(const fs::path& file) {
  std::error_code ec;
  uint8_t digest[32];
  return *kPackSha && fs::file_size(file, ec) == kPackSize && !ec && updater::FileSha256(file, digest) &&
         Hex(digest, 32) == kPackSha;
}

// The pack's costumes after the built-in ones: the downloaded pack (when it is the published one) or the test's file.
void LoadPack(std::vector<Costume>& costumes) {
  const std::string test = REXCVAR_GET(mk9_vita_costumes_pack);
  const fs::path file = test.empty() ? PackPath() : rex::to_path(test);
  std::error_code ec;
  if (!fs::is_regular_file(file, ec)) return;
  if (test.empty() && !g_pack_checked && !PackGood(file)) {
    REXLOG_WARN("[costumes] {} is not the published pack (size or SHA-256): deleted", rex::path_to_utf8(file));
    fs::remove(file, ec);
    return;
  }
  std::vector<Costume> more;
  if (!ReadAll(file, g_pack) || !ParseBundle(reinterpret_cast<const uint8_t*>(g_pack.data()), g_pack.size(), more)) {
    REXLOG_ERROR("[costumes] the pack {} is damaged: its costumes left out", rex::path_to_utf8(file));
    return;
  }
  REXLOG_INFO("[costumes] the pack {}: {} costumes", rex::path_to_utf8(file), more.size());
  for (Costume& c : more) {
    c.from_pack = true;
    costumes.push_back(std::move(c));
  }
}

// Downloaded at the start when it is published and not here; not in an online session's game (started again joined
// to a room, watching, or a replay: its content was what the room compared) nor in a test (no lobby, a scripted pad,
// an online test, a test's pack).
bool PackWanted() {
  if (!*kPackUrl || !REXCVAR_GET(mk9_vita_costumes) || rex::cvar::GetFlagByName("mk9_lobby_url") == "off") return false;
  for (const char* name : {"mk9_net_room", "mk9_net_role", "mk9_net_replay", "mk9_input_script",
                           "mk9_input_script_file", "mk9_online_test", "mk9_vita_costumes_pack"}) {
    if (!rex::cvar::GetFlagByName(name).empty()) return false;
  }
  return true;
}

// The download, on its own thread while the window shows it: on from the .partial a stopped one left, and in the
// pack's place only when it is the published one.
void DownloadPack() {
  const fs::path file = PackPath();
  fs::path partial = file;
  partial += ".partial";
  std::error_code ec;
  fs::create_directories(file.parent_path(), ec);
  uint64_t got = fs::exists(partial, ec) ? fs::file_size(partial, ec) : 0;
  if (ec || got >= kPackSize) got = 0;  // nothing usable there: from the start
  std::ofstream out(partial, std::ios::binary | (got ? std::ios::app : std::ios::trunc));
  if (!out) {
    REXLOG_ERROR("[costumes] could not write {}", rex::path_to_utf8(partial));
    return;
  }
  REXLOG_INFO("[costumes] downloading the PS Vita costumes' pack ({} MB) from {}{}", kPackSize >> 20, kPackUrl,
              got ? fmt::format(", on from {} KB", got >> 10) : std::string());
  g_pack_got = int(got * 1000 / kPackSize);  // the bar from where it was
  std::string error;
  const bool ok = updater::DownloadTo(
      kPackUrl,
      [&](const char* data, size_t n) {
        out.write(data, std::streamsize(n));
        if (!out) return false;
        got += n;
        g_pack_got = int(std::min<uint64_t>(1000, got * 1000 / kPackSize));
        return true;
      },
      &error, got);
  out.close();
  if (!ok) {
    REXLOG_WARN("[costumes] the pack's download stopped ({}): it goes on at the next start", error);
    return;
  }
  if (!PackGood(partial)) {
    REXLOG_ERROR("[costumes] the download is not the published pack (size or SHA-256): discarded");
    fs::remove(partial, ec);
    return;
  }
  fs::rename(partial, file, ec);
  if (ec) {
    REXLOG_ERROR("[costumes] the pack could not take its place ({}): it is downloaded again at the next start",
                 ec.message());
    fs::remove(partial, ec);
    return;
  }
  g_pack_checked = true;
  REXLOG_INFO("[costumes] the PS Vita costumes' pack downloaded: its costumes are in from this start");
}

}  // namespace

void AtStart() {
  if (!PackWanted()) return;
  const fs::path file = PackPath();
  std::error_code ec;
  if (fs::exists(file, ec)) {
    if (PackGood(file)) {
      g_pack_checked = true;
      return;
    }
    REXLOG_WARN("[costumes] {} is not the published pack (size or SHA-256): downloaded again", rex::path_to_utf8(file));
    fs::remove(file, ec);
  }
  g_pack_downloading = true;
  std::thread download([] {
    DownloadPack();
    g_pack_downloading = false;
  });
  const bool stayed = fetch_window::Run(
      {"MK9 Online by Lauta - PS Vita", i18n::T("Descargando los trajes de PS Vita"),
       i18n::T("Los trajes extra de la versión de PS Vita se suman al juego (una sola vez, unos 15 MB). Cuando "
               "termine, el juego arranca con ellos."),
       i18n::T("Si cerrás esta ventana se cierra el juego: la descarga sigue la próxima vez que lo abras."), kPackSize},
      [](float& progress) {
        progress = float(g_pack_got.load(std::memory_order_relaxed)) / 1000.0f;
        return g_pack_downloading.load(std::memory_order_relaxed);
      });
  if (!stayed) {
    REXLOG_INFO("[costumes] the start's window was closed while the pack downloaded: the game closes (the download "
                "goes on at the next start)");
    fetch_window::CloseGame();
  }
  download.join();  // ended (the window closed by itself), or no window could open: its end waited for here
  g_pack_missing = !g_pack_checked;
  REXLOG_INFO("[costumes] the start's window: {}", g_pack_checked ? "the pack downloaded" : "no pack this time");
}

void Install(rex::filesystem::VirtualFileSystem* vfs, const std::filesystem::path& game_root) {
  if (!vfs || game_root.empty() || !REXCVAR_GET(mk9_vita_costumes)) return;
  const auto start = std::chrono::steady_clock::now();
  std::vector<Costume> costumes;
  if (!ParseBundle(kVitaCostumes, kVitaCostumesSize, costumes)) {
    REXLOG_ERROR("[costumes] the built-in costume patches are damaged: none");
    return;
  }
  LoadPack(costumes);
  const fs::path folder = mk9::platform::ExeDir() / "cache" / "costumes";
  // the packages to make (the first start, or a release with other patches) and how many of them each source package
  // of the game is for: its decompressed image (tens of MB) goes after its last one
  std::vector<std::vector<bool>> todo(costumes.size());
  std::map<std::string, int> uses;
  for (size_t i = 0; i < costumes.size(); ++i) {
    for (const Patch& p : costumes[i].patches) {
      todo[i].push_back(!Made(folder / rex::to_path(p.target), p));
      if (todo[i].back()) {
        for (const Source& s : p.sources) ++uses[s.path];
      }
    }
  }
  std::map<std::string, std::vector<uint8_t>> images;
  auto done_with = [&](const Patch& p) {
    for (const Source& s : p.sources) {
      if (--uses[s.path] <= 0) images.erase(s.path);
    }
  };
  std::vector<const Costume*> ready;
  std::vector<std::pair<std::string, uint64_t>> files;
  int made = 0;
  for (size_t i = 0; i < costumes.size(); ++i) {
    const Costume& c = costumes[i];
    bool ok = true;
    std::vector<std::pair<std::string, uint64_t>> mine;
    for (size_t k = 0; k < c.patches.size(); ++k) {
      const Patch& p = c.patches[k];
      if (todo[i][k]) {
        if (!ok) {  // the costume is left out already
          done_with(p);
          continue;
        }
        std::string why;
        const bool built = Make(p, game_root, images, folder / rex::to_path(p.target), why);
        done_with(p);
        if (!built) {
          REXLOG_WARN("[costumes] {}: {} not made ({}): left out", c.key, p.target, why);
          ok = false;
          continue;
        }
        ++made;
      }
      mine.emplace_back(p.target.substr(p.target.rfind('/') + 1), p.size);
    }
    if (!ok) continue;
    ready.push_back(&c);
    files.insert(files.end(), mine.begin(), mine.end());
  }
  images.clear();
  if (ready.empty()) return;
  // the registration: the game's own Coalesced.ini and TOC (as they are now: the disc's, or Kratos') with the lines
  std::string coalesced, toc, coalesced_out;
  if (!ReadAll(OnDisk(game_root, "Config/Coalesced.ini"), coalesced) || !ReadAll(OnDisk(game_root, "Xbox360TOC.txt"), toc) ||
      !EditCoalesced(coalesced, ready, coalesced_out)) {
    REXLOG_WARN("[costumes] the game's Coalesced.ini or TOC could not be read or edited: no costumes");
    return;
  }
  const std::string toc_out = EditToc(toc, coalesced_out.size(), files);
  std::string have;
  for (const auto& [path, data] : {std::pair<fs::path, const std::string*>{folder / "Config" / "Coalesced.ini", &coalesced_out},
                                   {folder / "Xbox360TOC.txt", &toc_out}}) {
    if ((!ReadAll(path, have) || have != *data) && !WriteNew(path, *data)) {
      REXLOG_WARN("[costumes] {} could not be written: no costumes", rex::path_to_utf8(path));
      return;
    }
  }
  auto device = std::make_unique<rex::filesystem::HostPathDevice>(std::string(kGameMount), folder, true);
  device->SetOverlay(true);
  if (!device->Initialize() || !vfs->RegisterDevice(std::move(device))) {
    REXLOG_WARN("[costumes] {} could not be mounted: no costumes", rex::path_to_utf8(folder));
    return;
  }
  // the set and each package's bytes (a release that fixes a costume says so to the other games)
  uint32_t signature = Fnv("mk9-lauta vita costumes 1");
  std::string keys;
  for (const Costume* c : ready) {
    signature = Fnv(c->key, signature);
    for (const Patch& p : c->patches) signature = Fnv(std::string_view(reinterpret_cast<const char*>(p.sha), 32), signature);
    keys += (keys.empty() ? "" : ", ") + c->key;
  }
  g_signature = signature ? signature : 1;
  g_summary = std::to_string(ready.size());
  g_pack_in = std::any_of(ready.begin(), ready.end(), [](const Costume* c) { return c->from_pack; });
  g_installed = true;
  REXLOG_INFO("[costumes] {} PS Vita costumes ({}; {} packages made now) in {}, over the game's files, in {:.1f} s",
              ready.size(), keys, made, rex::path_to_utf8(folder),
              std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count());
}

bool Installed() { return g_installed.load(std::memory_order_relaxed); }

uint32_t Signature() { return g_signature.load(std::memory_order_relaxed); }

std::string Summary() { return g_summary; }

bool PackIn() { return g_pack_in.load(std::memory_order_relaxed); }

bool PackMissing() { return g_pack_missing; }

}  // namespace mk9::vita_costumes

#endif  // MK9_VITA_COSTUMES
