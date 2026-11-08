// MK9 online: FMOD's streaming reads, outside the game's memory.
//
// FMOD streams the music and the voices (announcer, intros, endings) from banks inside the game's packages. UE3's FMOD
// device opens each one as an archive (vtable 0x82072390: +4 the bulk data, +8 the position, +12 the package's name, an
// ANSI FString such as "..\Asset\SND_VO_CharIntro.xxx", +24 the size), whose read (sub_82604168) asks the engine's
// async IO system (GAsyncIOSystem, its queue in the game's memory) for each piece and waits for a counter on FMOD's own
// stack. A rollback that restores the game's memory while one of those requests is queued takes it away: FMOD's stream
// thread waits for good, its stream stays busy, and the music changing tracks later waits for it (FMOD's
// sub_82DE7740), the game frozen (two matches into a session with lag, at the CONTINUE screen).
//
// Here those reads are the host's: the package mapped from the game's files (the SDK's file system, game:\), at the
// bulk data's offset in it (sub_8305EC00) plus the position, which the host keeps for each archive (its seek,
// sub_82589650, writes it here too) and writes back where the game keeps it, as the original read does. Nothing of
// the game's memory is used but the archive's fields, which stay put while it is open.
//
//   mk9_sound_stream_host=<bool>   FMOD's streaming reads from the game's files on the host (true)

#include <algorithm>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>

#include <rex/cvar.h>
#include <rex/filesystem/entry.h>
#include <rex/filesystem/vfs.h>
#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/memory/mapped_memory.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xmemory.h>

REXCVAR_DEFINE_BOOL(mk9_sound_stream_host, true, "MK9 Online",
                    "FMOD's streaming reads (music, voices) from the game's files on the host instead of through the "
                    "engine's async IO, whose queue a rollback would take away (sound_stream.cpp)");

extern "C" REX_FUNC(__imp__sub_82604168);  // the streaming archive's read (r3 archive, r4 buffer, r5 size)
extern "C" REX_FUNC(__imp__sub_82589650);  // an archive's seek (r3 archive, r4 position): +8 = position
extern "C" REX_FUNC(__imp__sub_8305EC00);  // the bulk data's offset in its package (-1: none)

namespace mk9::sound_stream {
namespace {

constexpr uint32_t kStreamArchive = 0x82072390;

struct Stream {
  uint32_t bulk = 0, name = 0, size = 0;  // the archive's fields when it was seen first (another archive at the same
                                          // address later has others)
  bool opened = false, ok = false;
  const uint8_t* data = nullptr;  // the package's bytes (mapped)
  size_t data_size = 0;
  uint64_t offset = 0;  // the bulk data's in the package
  uint32_t pos = 0;
};

std::mutex g_mutex;
std::unordered_map<uint32_t, Stream> g_streams;                                    // by archive
std::map<std::string, std::unique_ptr<rex::memory::MappedMemory>> g_packages;      // by path, mapped once
uint64_t g_reads = 0, g_bytes = 0;

bool Enabled() {
  static const bool enabled = REXCVAR_GET(mk9_sound_stream_host);
  return enabled;
}

uint32_t Ld32(const uint8_t* base, uint32_t a) {
  uint32_t v;
  std::memcpy(&v, base + a, 4);
  return __builtin_bswap32(v);
}

// As a guest store: its page's store card marked (rex/system/xmemory.h; the cards are at the guest window's base).
void St32(uint8_t* base, uint32_t a, uint32_t v) {
  v = __builtin_bswap32(v);
  std::memcpy(base + a, &v, 4);
  rex::memory::MarkStoreCardsAt(base, a, 4);
}

// "..\Asset\X.xxx" (relative to the game's binaries): game:\Asset\X.xxx.
std::string PackagePath(const uint8_t* base, uint32_t archive) {
  const uint32_t text = Ld32(base, archive + 12), count = Ld32(base, archive + 16);
  std::string name;
  for (uint32_t i = 0; text && i < count && i < 512; ++i) {
    const char c = char(base[text + i]);
    if (!c) break;
    name += c;
  }
  while (name.rfind("..\\", 0) == 0) name.erase(0, 3);
  return "game:\\" + name;
}

const rex::memory::MappedMemory* MapPackage(const std::string& path) {
  auto it = g_packages.find(path);
  if (it != g_packages.end()) return it->second.get();
  std::unique_ptr<rex::memory::MappedMemory> map;
  auto* fs = REX_KERNEL_FS();
  if (rex::filesystem::Entry* entry = fs ? fs->ResolvePath(path) : nullptr) {
    map = entry->OpenMapped(rex::memory::MappedMemory::Mode::kRead);
  }
  if (!map) REXLOG_WARN("[sound] {} could not be mapped: FMOD streams it the game's way", path);
  return (g_packages[path] = std::move(map)).get();
}

// The archive's stream (g_mutex held): made anew when the archive at that address is another one.
Stream& Get(const uint8_t* base, uint32_t archive) {
  Stream& s = g_streams[archive];
  const uint32_t bulk = Ld32(base, archive + 4), name = Ld32(base, archive + 12), size = Ld32(base, archive + 24);
  if (s.bulk != bulk || s.name != name || s.size != size) {
    s = Stream{};
    s.bulk = bulk;
    s.name = name;
    s.size = size;
    s.pos = Ld32(base, archive + 8);
  }
  return s;
}

void Open(PPCContext& ctx, uint8_t* base, uint32_t archive, Stream& s) {
  s.opened = true;
  rex::CallFrame frame(ctx);
  frame.ctx.r3.u64 = s.bulk;
  __imp__sub_8305EC00(frame, base);
  const uint32_t offset = frame.ctx.r3.u32;
  const std::string path = PackagePath(base, archive);
  const rex::memory::MappedMemory* map = MapPackage(path);
  if (offset == 0xFFFFFFFFu || !map || uint64_t(offset) + s.size > map->size()) {
    REXLOG_WARN("[sound] FMOD's stream {} (offset {:08X}, {} bytes) left to the game's reads", path, offset, s.size);
    return;
  }
  s.data = static_cast<const uint8_t*>(map->data());
  s.data_size = map->size();
  s.offset = offset;
  s.ok = true;
  REXLOG_INFO("[sound] FMOD streams {} from the host ({} bytes at {:08X})", path, s.size, offset);
}

}  // namespace
}  // namespace mk9::sound_stream

using namespace mk9::sound_stream;

REX_HOOK_RAW(sub_82589650) {
  if (Enabled() && Ld32(base, ctx.r3.u32) == kStreamArchive) {
    std::lock_guard<std::mutex> lock(g_mutex);
    Get(base, ctx.r3.u32).pos = ctx.r4.u32;
  }
  __imp__sub_82589650(ctx, base);
}

REX_HOOK_RAW(sub_82604168) {
  const uint32_t archive = ctx.r3.u32, buffer = ctx.r4.u32;
  if (!Enabled() || Ld32(base, archive) != kStreamArchive) {
    __imp__sub_82604168(ctx, base);
    return;
  }
  std::unique_lock<std::mutex> lock(g_mutex);
  Stream& s = Get(base, archive);
  if (!s.opened) Open(ctx, base, archive, s);
  if (!s.ok) {
    lock.unlock();
    __imp__sub_82604168(ctx, base);
    return;
  }
  const uint32_t pos = std::min(s.pos, s.size);
  const uint32_t size = std::min(ctx.r5.u32, s.size - pos);
  if (size) {
    std::memcpy(rex::memory::GuestPtr(base, buffer), s.data + s.offset + pos, size);
    rex::memory::MarkStoreCardsAt(base, buffer, size);
  }
  s.pos = pos + size;
  St32(base, archive + 8, s.pos);
  ++g_reads;
  g_bytes += size;
  ctx.r3.u64 = size;
}
