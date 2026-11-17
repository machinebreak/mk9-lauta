// mk9-nx (2026-10-24): MK9's cutscenes (Movies\*.wmv: WMV3 Main 1280x720 30 fps with B frames, WMA2) are played by the
// XDK's XMV player (XMedia2, 0x82CE7xxx), whose video codec is Microsoft's embedded WMV9 decoder (wmvdec.c).
// Recompiled, that decoder ran 23-35 fps on a whole core of a V1 at 1785 MHz, so the picture fell behind the sound
// (~4 s by the end of a Story cutscene). The Switch's FFmpeg decodes the same file at 124 fps on one core.
//
// The codec is a plug-in of the player. A factory (sub_82D25910) fills a table of entry points, each called with
// r3 = the table:
//   [0] Init 82D255C0 (table, media type, ?, data callback, its context, player, &handle): the media type's +8 points
//       to the format (+8 width, +12 height, +20 FOURCC, +48 u16 private data size, +52 its address: WMV3's 4-byte
//       sequence header). The original allocates its 4240-byte state from the player (82CF94C0(player, 25, size,
//       &p)), keeps it at table+32 and ends by registering the codec's message handler 82D25580 with it
//       (82CF9018(player, handler, state, &state->4232)), returning that handle in *r9. The player then sends the
//       handler 0x80000 (the output format: 82D25470 writes width, height and frame bytes at state+32/+36/+108 and
//       answers 0x90000) before it asks for any frame. The replacement does the same with that original handler.
//   [1] Close 82D25280 (table): the original also zeroes table words 0-5 and 8.
//   [2] Decode 82D25330 (table, &bytes ready, ?): decodes one frame; *bytes = width * height * 3 / 2 when one is ready.
//   [3] GetOutput 82D25390 (table, buffer, capacity, &size, &flags, &time u64, &one): the frame as planar Y, U, V
//       (I420, no padding), its time in ms; *one = 1. 0x80500006 when the buffer is too small.
//   [4] Reset 82D25228 (table).
// The data callback (XMV's sub_82CFA1E0) is called (context, &ptr, &size, &u32, &time u64, &has_time, &frame size,
// &u32): each call gives the next piece of the compressed stream (up to 512 bytes), the first piece of a frame with
// has_time and the time in ms; it returns 0x00500001 for the piece that ends a frame, a negative HRESULT on error.
// Traced on PC (mk9_movie_trace) from the original's calls; the first frame of WBIntroLogo.wmv came out of both
// decoders with the same Y and U bytes.

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <deque>
#include <filesystem>
#include <memory>
#include <mutex>
#include <queue>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include <fmt/format.h>

#include <rex/cvar.h>
#include <rex/filesystem.h>
#include <rex/filesystem/entry.h>
#include <rex/filesystem/vfs.h>
#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/memory/mapped_memory.h>
#include <rex/ppc.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xmemory.h>

#include "mk9_movie.h"
#include "online/netplay.h"

extern "C" {
#include "libavcodec/avcodec.h"
}

REXCVAR_DEFINE_BOOL(mk9_movie_ffmpeg, true, "MK9",
                    "Cutscenes: the WMV video decoded by the host's FFmpeg instead of the game's own decoder")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_BOOL(mk9_skip_cutscenes, true, "MK9",
                    "The cutscenes (Movies\\*.wmv: Story Mode's) skipped with START or A, the first time too (F2 > "
                    "Pantalla e idioma); never in an online session");
REXCVAR_DEFINE_BOOL(mk9_movie_hq, true, "MK9",
                    "Cutscenes: a movie's picture from Movies\\<name>.bik when that file is there (the PC version's "
                    "videos, e.g. the HQ videos mods' PS3 masters): Bink 1, scaled to the game's 1280x720");
REXCVAR_DEFINE_STRING(mk9_movie_hq_dir, "", "MK9",
                      "A folder with the cutscenes' .bik files, looked in before the game's Movies folder (empty: "
                      "only the game's Movies folder)");
REXCVAR_DEFINE_INT32(mk9_movie_trace, 0, "MK9",
                     "Log this many calls of the cutscene codec's interface (XMV's WMV decoder), each kind")
    .range(0, 100000);

namespace {

constexpr uint32_t kBufferSmall = 0x80500006u;  // GetOutput: the buffer is too small (the original's code)
constexpr uint32_t kFail = 0x80004005u;         // E_FAIL

uint8_t* Raw(uint8_t* base, uint32_t a) { return base + a + (a >= 0xE0000000u ? 0x1000u : 0u); }
uint32_t Ld32(uint8_t* base, uint32_t a) {
  uint32_t v;
  std::memcpy(&v, Raw(base, a), 4);
  return __builtin_bswap32(v);
}
uint64_t Ld64(uint8_t* base, uint32_t a) {
  uint64_t v;
  std::memcpy(&v, Raw(base, a), 8);
  return __builtin_bswap64(v);
}
// Every write of guest memory here marks its store cards (rex/system/xmemory.h), as the guest's stores would: the
// decoded frames become textures Renut proves by them.
void Mark(uint8_t* base, uint32_t a, size_t n) { rex::memory::MarkStoreCardsAt(base, a, uint32_t(n)); }
void St32(uint8_t* base, uint32_t a, uint32_t v) {
  v = __builtin_bswap32(v);
  std::memcpy(Raw(base, a), &v, 4);
  Mark(base, a, 4);
}
void St64(uint8_t* base, uint32_t a, uint64_t v) {
  v = __builtin_bswap64(v);
  std::memcpy(Raw(base, a), &v, 8);
  Mark(base, a, 8);
}

bool Trace(std::atomic<int>& n) {
  const int limit = REXCVAR_GET(mk9_movie_trace);
  return limit > 0 && n.fetch_add(1, std::memory_order_relaxed) < limit;
}

struct Frame {
  std::vector<uint8_t> yuv;  // I420, width x height, no padding
  int64_t time_ms = 0;
};

// ---------------------------------------------------------------------------------------------------------------------
// mk9-lauta (2026-11-17, issue #11): a cutscene's picture from <name>.bik, the PC version's video of the same movie
// (the game's Movies folder beside its .wmv, or mk9_movie_hq_dir): the "HQ videos" mods give the PC version the PS3
// masters (1080p, 60 a second, far less compressed than the 360's 720p WMV). The movie still plays as the game's (its
// WMV decoded as before: the player's timing and its sound), but each picture shown is the Bink's at the same time
// from the movie's first picture, scaled to the WMV's size (the game's movie texture: 1280x720; bilinear). Bink 1 (the
// PC version's; FFmpeg's decoder, built into the SDK's FFmpeg for this), not Bink 2. The file is mapped; each frame
// begins with its sound (one packet per track: skipped), then the picture. The movie's name: the last Movies\*.wmv the
// game opened (NoteOpened), a moment before the codec starts.
//
// The Bink container (FFmpeg's libavformat/bink.c, little-endian): "BIK" and the revision, the file's size - 8, the
// frames, the largest frame, ?, width, height, fps numerator, denominator, the video flags (the decoder's 4 bytes of
// extradata), the sound tracks (then for revision k 4 bytes more; per track 4 bytes of max size, then 2 + 2 of rate and
// flags, then 4 of id); then each frame's offset (bit 0: a key frame; the first is one), the last ending at the file's
// end.

uint32_t Le32(const uint8_t* p) {
  return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24;
}

struct BinkVideo {
  std::unique_ptr<rex::memory::MappedMemory> map;
  const uint8_t* file = nullptr;
  size_t size = 0;
  std::string where;
  uint32_t tag = 0, width = 0, height = 0, fps_num = 0, fps_den = 0, frames = 0, tracks = 0;
  std::vector<uint32_t> pos;  // each frame's offset, then the file's end
  std::vector<uint8_t> key;   // each frame: a key frame (decoding can start there)
  AVCodecContext* av = nullptr;
  AVFrame* frame = nullptr;
  AVPacket* packet = nullptr;
  int64_t decoded = -1;   // the frame whose picture `frame` holds
  int64_t first_ms = -1;  // the movie's first picture's time: the Bink's frame 0
  bool picture = false, full_range = false;
  uint8_t to_studio[2][256] = {};  // full range (revision k) to the WMV's studio range: luma, chroma
  uint64_t fills = 0, decodes = 0, failures = 0, us = 0, us_decoding = 0;

  ~BinkVideo() {
    av_packet_free(&packet);
    av_frame_free(&frame);
    avcodec_free_context(&av);
  }
};

// The .bik of the movie `name` (empty: none, or not one this reads: `why`).
std::unique_ptr<rex::memory::MappedMemory> FindBink(const std::string& name, std::string& where) {
  const std::string dir = REXCVAR_GET(mk9_movie_hq_dir);
  if (!dir.empty()) {
    std::error_code ec;
    const std::filesystem::path folder = rex::to_path(dir);
    std::string lower = name + ".bik";
    std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) { return char(std::tolower(c)); });
    for (const auto& entry : std::filesystem::directory_iterator(folder, ec)) {  // any case (Linux)
      std::string file = rex::path_to_utf8(entry.path().filename());
      std::transform(file.begin(), file.end(), file.begin(), [](unsigned char c) { return char(std::tolower(c)); });
      if (file != lower) continue;
      if (auto map = rex::memory::MappedMemory::Open(entry.path(), rex::memory::MappedMemory::Mode::kRead)) {
        where = rex::path_to_utf8(entry.path());
        return map;
      }
    }
  }
  auto* fs = REX_KERNEL_FS();
  const std::string guest = "game:\\Movies\\" + name + ".bik";
  if (rex::filesystem::Entry* entry = fs ? fs->ResolvePath(guest) : nullptr) {
    if (auto map = entry->OpenMapped(rex::memory::MappedMemory::Mode::kRead)) {
      where = guest;
      return map;
    }
  }
  return nullptr;
}

std::unique_ptr<BinkVideo> OpenBink(std::unique_ptr<rex::memory::MappedMemory> map, std::string& why) {
  auto b = std::make_unique<BinkVideo>();
  b->file = static_cast<const uint8_t*>(map->data());
  b->size = map->size();
  b->map = std::move(map);
  const uint8_t* p = b->file;
  if (!p || b->size < 48) {
    why = "too small";
    return nullptr;
  }
  b->tag = Le32(p);
  const uint32_t kind = b->tag & 0xFFFFFFu;
  if (kind == (uint32_t('K') | uint32_t('B') << 8 | uint32_t('2') << 16)) {
    why = "Bink 2 (only Bink 1 is read)";
    return nullptr;
  }
  if (kind != (uint32_t('B') | uint32_t('I') << 8 | uint32_t('K') << 16)) {
    why = "not a Bink file";
    return nullptr;
  }
  const uint64_t file_size = uint64_t(Le32(p + 4)) + 8;
  b->frames = Le32(p + 8);
  b->width = Le32(p + 20);
  b->height = Le32(p + 24);
  b->fps_num = Le32(p + 28);
  b->fps_den = Le32(p + 32);
  b->tracks = Le32(p + 40);
  const char revision = char(b->tag >> 24);
  if (!b->frames || b->frames > 1000000 || !b->fps_num || !b->fps_den || !b->width || !b->height ||
      b->width > 4096 || b->height > 4096 || b->tracks > 256) {
    why = "a header out of range";
    return nullptr;
  }
  if (file_size > b->size) {
    why = fmt::format("cut short ({} of its {} bytes)", b->size, file_size);
    return nullptr;
  }
  size_t at = 44 + (revision == 'k' ? 4 : 0) + size_t(b->tracks) * 12;
  if (at + size_t(b->frames) * 4 > b->size) {
    why = "cut short (its frame index)";
    return nullptr;
  }
  b->pos.resize(size_t(b->frames) + 1);
  b->key.resize(b->frames);
  for (uint32_t i = 0; i < b->frames; ++i, at += 4) {
    const uint32_t v = Le32(p + at);
    b->pos[i] = v & ~1u;
    b->key[i] = i == 0 || (v & 1u);
  }
  b->pos[b->frames] = uint32_t(file_size);
  for (uint32_t i = 0; i < b->frames; ++i) {
    if (b->pos[i + 1] <= b->pos[i] || b->pos[i] < at) {
      why = fmt::format("a bad frame index (frame {})", i);
      return nullptr;
    }
  }
  const AVCodec* codec = avcodec_find_decoder(AV_CODEC_ID_BINKVIDEO);
  if (!codec) {
    why = "this build has no Bink decoder";
    return nullptr;
  }
  b->av = avcodec_alloc_context3(codec);
  b->av->codec_tag = b->tag;  // the decoder takes the revision from its top byte
  b->av->width = int(b->width);
  b->av->height = int(b->height);
  b->av->thread_count = 1;
  b->av->extradata = static_cast<uint8_t*>(av_mallocz(4 + AV_INPUT_BUFFER_PADDING_SIZE));
  std::memcpy(b->av->extradata, p + 36, 4);  // the video flags
  b->av->extradata_size = 4;
  if (avcodec_open2(b->av, codec, nullptr) < 0) {
    why = "FFmpeg could not open it";
    return nullptr;
  }
  b->frame = av_frame_alloc();
  b->packet = av_packet_alloc();
  b->full_range = revision == 'k';
  for (int v = 0; v < 256; ++v) {
    b->to_studio[0][v] = uint8_t(16 + (v * 219 + 127) / 255);
    b->to_studio[1][v] = uint8_t(128 + ((v - 128) * 224 + (v >= 128 ? 127 : -127)) / 255);
  }
  return b;
}

// Frame i's picture into b.frame (its sound packets first: skipped). The decoder takes one frame after another.
bool DecodeBink(BinkVideo& b, int64_t i) {
  b.decoded = i;
  const uint8_t* f = b.file + b.pos[size_t(i)];
  size_t left = b.pos[size_t(i) + 1] - b.pos[size_t(i)];
  for (uint32_t t = 0; t < b.tracks; ++t) {
    if (left < 4) return false;
    const uint32_t n = Le32(f);
    if (n > left - 4) return false;
    f += 4 + size_t(n);
    left -= 4 + size_t(n);
  }
  if (!left || av_new_packet(b.packet, int(left)) < 0) return false;  // (padded, as FFmpeg's decoders read)
  std::memcpy(b.packet->data, f, left);
  b.packet->flags = b.key[size_t(i)] ? AV_PKT_FLAG_KEY : 0;
  const auto started = std::chrono::steady_clock::now();
  const int sent = avcodec_send_packet(b.av, b.packet);
  av_packet_unref(b.packet);
  b.decodes++;
  b.picture = sent >= 0 && avcodec_receive_frame(b.av, b.frame) == 0;
  b.us_decoding += uint64_t(
      std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - started).count());
  return b.picture;
}

// One plane, bilinear (or copied when the sizes match), through `lut` when given.
void ScalePlane(const uint8_t* src, int stride, int sw, int sh, uint8_t* dst, int dw, int dh, const uint8_t* lut) {
  if (sw == dw && sh == dh) {
    for (int y = 0; y < dh; ++y) {
      const uint8_t* s = src + size_t(y) * size_t(stride);
      uint8_t* d = dst + size_t(y) * size_t(dw);
      if (lut) {
        for (int x = 0; x < dw; ++x) d[x] = lut[s[x]];
      } else {
        std::memcpy(d, s, size_t(dw));
      }
    }
    return;
  }
  const size_t n = static_cast<size_t>(dw);
  std::vector<int> x0(n), x1(n), fx(n);
  for (int x = 0; x < dw; ++x) {
    const double sx = std::max(0.0, (x + 0.5) * sw / dw - 0.5);
    const int i = std::min(int(sx), sw - 1);
    x0[size_t(x)] = i;
    x1[size_t(x)] = std::min(i + 1, sw - 1);
    fx[size_t(x)] = std::clamp(int((sx - i) * 256.0 + 0.5), 0, 256);
  }
  for (int y = 0; y < dh; ++y) {
    const double sy = std::max(0.0, (y + 0.5) * sh / dh - 0.5);
    const int i = std::min(int(sy), sh - 1);
    const int fy = std::clamp(int((sy - i) * 256.0 + 0.5), 0, 256);
    const uint8_t* r0 = src + size_t(i) * size_t(stride);
    const uint8_t* r1 = src + size_t(std::min(i + 1, sh - 1)) * size_t(stride);
    uint8_t* d = dst + size_t(y) * size_t(dw);
    for (int x = 0; x < dw; ++x) {
      const int a = fx[size_t(x)];
      const int top = r0[x0[size_t(x)]] * (256 - a) + r0[x1[size_t(x)]] * a;
      const int bottom = r1[x0[size_t(x)]] * (256 - a) + r1[x1[size_t(x)]] * a;
      const int v = (top * (256 - fy) + bottom * fy + 32768) >> 16;
      d[x] = lut ? lut[v] : uint8_t(v);
    }
  }
}

// The Bink's picture for the movie's time `time_ms` into `dst` (I420, w x h). False: none (the WMV's goes instead).
bool FillFromBink(BinkVideo& b, int64_t time_ms, uint8_t* dst, uint32_t w, uint32_t h) {
  const auto started = std::chrono::steady_clock::now();
  if (b.first_ms < 0) b.first_ms = time_ms;
  const int64_t den = int64_t(b.fps_den) * 1000;
  int64_t target = (std::max<int64_t>(0, time_ms - b.first_ms) * int64_t(b.fps_num) + den / 2) / den;
  target = std::min<int64_t>(target, int64_t(b.frames) - 1);
  if (target != b.decoded) {
    int64_t from = b.decoded + 1;
    // back, or far ahead: from the key frame at or before it
    constexpr int64_t kAheadAtMost = 90;
    if (target < from || target - from > kAheadAtMost) {
      int64_t k = target;
      while (k > 0 && !b.key[size_t(k)]) --k;
      if (target < from || k > from) {
        avcodec_flush_buffers(b.av);
        from = k;
      }
    }
    for (int64_t i = from; i <= target; ++i) {
      if (!DecodeBink(b, i)) b.failures++;
    }
  }
  if (!b.picture) return false;
  const AVFrame* f = b.frame;
  const int sw = f->width, sh = f->height;
  if (sw <= 0 || sh <= 0 || !f->data[0] || !f->data[1] || !f->data[2]) return false;
  const uint8_t* luma = b.full_range ? b.to_studio[0] : nullptr;
  const uint8_t* chroma = b.full_range ? b.to_studio[1] : nullptr;
  const int dw = int(w), dh = int(h);
  ScalePlane(f->data[0], f->linesize[0], sw, sh, dst, dw, dh, luma);
  uint8_t* u = dst + size_t(w) * h;
  uint8_t* v = u + size_t(w / 2) * (h / 2);
  ScalePlane(f->data[1], f->linesize[1], (sw + 1) / 2, (sh + 1) / 2, u, dw / 2, dh / 2, chroma);
  ScalePlane(f->data[2], f->linesize[2], (sw + 1) / 2, (sh + 1) / 2, v, dw / 2, dh / 2, chroma);
  b.fills++;
  b.us += uint64_t(
      std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - started).count());
  return true;
}

// The movie the game opened last (NoteOpened) and when.
std::mutex g_name_mutex;
std::string g_movie_name;
std::chrono::steady_clock::time_point g_movie_name_at{};

// The Bink for a codec starting now (nullptr: none; logged once a movie). Never in an online session: its movies (the
// boot logos) are skipped, and the player's timing stays the WMV's alone.
std::unique_ptr<BinkVideo> BinkForThisMovie(uint32_t table) {
  if (!REXCVAR_GET(mk9_movie_hq) || mk9::netplay::SessionGame()) return nullptr;
  std::string name;
  {
    std::lock_guard<std::mutex> lock(g_name_mutex);
    if (g_movie_name.empty() || std::chrono::steady_clock::now() - g_movie_name_at > std::chrono::seconds(30))
      return nullptr;
    name = g_movie_name;
  }
  std::string where;
  auto map = FindBink(name, where);
  if (!map) return nullptr;
  std::string why;
  auto b = OpenBink(std::move(map), why);
  if (!b) {
    REXLOG_WARN("[movie] {:08X}: {} not used: {}", table, where, why);
    return nullptr;
  }
  b->where = where;
  REXLOG_INFO("[movie] {:08X}: the picture from {} (Bink {}, {}x{}, {:.2f} a second, {} frames)", table, where,
              char(b->tag >> 24), b->width, b->height, double(b->fps_num) / b->fps_den, b->frames);
  return b;
}

struct Decoder {
  uint32_t player = 0, state = 0;  // the guest state block (the original's layout, for its message handler)
  AVCodecContext* av = nullptr;
  AVFrame* frame = nullptr;
  AVPacket* packet = nullptr;
  uint32_t width = 0, height = 0, callback = 0, callback_ctx = 0;
  std::vector<uint8_t> data;  // the frame being gathered from the callback's pieces
  std::deque<Frame> ready;
  bool eof = false;
  uint64_t frames_in = 0, frames_out = 0;
  // Output times. The data callback gives each compressed frame's time in decode order, and with B frames FFmpeg
  // hands that time on to the display-order frame (out: 133, 100, 200, 166 ms ...; the original decoder's are 100,
  // 133, 166, 200). B frames only reorder within the stream, so each decoded frame takes the earliest time not yet
  // given out. A compressed frame without a time gets the last known one + frames * 1000 / fps.
  std::priority_queue<int64_t, std::vector<int64_t>, std::greater<int64_t>> times;
  int64_t anchor_ms = 0, since_anchor = 0;
  uint32_t fps = 30;
  std::unique_ptr<BinkVideo> bink;  // mk9-lauta (2026-11-17, issue #11): the pictures from the PC version's .bik

  ~Decoder() {  // the guest state is released by Close
    av_packet_free(&packet);
    av_frame_free(&frame);
    avcodec_free_context(&av);
  }
  uint32_t FrameBytes() const { return width * height * 3 / 2; }
};

std::mutex g_mutex;
std::unordered_map<uint32_t, std::unique_ptr<Decoder>> g_decoders;  // by codec table (guest address)
std::atomic<uint32_t> g_playing{0};  // mk9-lauta (2026-11-10): g_decoders' size, for mk9::movie::Playing

Decoder* Find(uint32_t table) {
  std::lock_guard<std::mutex> lock(g_mutex);
  auto it = g_decoders.find(table);
  return it == g_decoders.end() ? nullptr : it->second.get();
}

// A guest function called from the host: arguments in r3-r10, out values in a scratch frame below the guest stack
// pointer (out = its +0x100; the guest ABI lets a callee use the area above its own frame only for its caller's
// saves). Returns r3.
constexpr uint32_t kOut = 0x100;
uint32_t CallGuest(PPCContext& ctx, uint8_t* base, uint32_t fn, std::initializer_list<uint32_t> args,
                   uint32_t* out_area = nullptr) {
  const uint64_t r1 = ctx.r1.u64, lr = ctx.lr;
  const uint32_t frame = (ctx.r1.u32 - 0x200) & ~0xFu;
  for (uint32_t i = 0; i < 0x40; i += 4) St32(base, frame + kOut + i, 0);
  if (out_area) *out_area = frame + kOut;
  ctx.r1.u64 = frame;
  PPCRegister* regs[] = {&ctx.r3, &ctx.r4, &ctx.r5, &ctx.r6, &ctx.r7, &ctx.r8, &ctx.r9, &ctx.r10};
  size_t i = 0;
  for (uint32_t a : args) regs[i++]->u64 = a == ~0u - 1 ? frame + kOut : a;  // ~0u - 1: the out area
  rex::runtime::ResolveIndirectFunction(fn)(ctx, base);
  const uint32_t result = ctx.r3.u32;
  ctx.r1.u64 = r1;
  ctx.lr = lr;
  return result;
}
constexpr uint32_t kOutArg = ~0u - 1;

constexpr uint32_t kPlayerAlloc = 0x82CF94C0, kPlayerFree = 0x82CF9518;        // (player, 25, size, &p) / (player, 25, &p)
constexpr uint32_t kPlayerRegister = 0x82CF9018, kPlayerUnregister = 0x82CF90B0;  // (player, fn, ctx, &h) / (player, h)
constexpr uint32_t kCodecHandler = 0x82D25580, kStateSize = 4240;

// The player's data callback, called from the host.
struct Piece {
  uint32_t result = 0, ptr = 0, size = 0, has_time = 0, whole = 0;
  uint64_t time_ms = 0;
};
Piece CallData(PPCContext& ctx, uint8_t* base, const Decoder& d) {
  const uint64_t r1 = ctx.r1.u64, lr = ctx.lr;
  const uint32_t frame = (ctx.r1.u32 - 0x200) & ~0xFu, out = frame + 0x100;
  for (uint32_t i = 0; i < 0x40; i += 4) St32(base, out + i, 0);
  ctx.r1.u64 = frame;
  ctx.r3.u64 = d.callback_ctx;
  ctx.r4.u64 = out + 0x00;   // ptr
  ctx.r5.u64 = out + 0x04;   // size
  ctx.r6.u64 = out + 0x08;   // u32
  ctx.r7.u64 = out + 0x10;   // time (u64)
  ctx.r8.u64 = out + 0x18;   // has time
  ctx.r9.u64 = out + 0x1C;   // the frame's whole size
  ctx.r10.u64 = out + 0x20;  // u32
  rex::runtime::ResolveIndirectFunction(d.callback)(ctx, base);
  Piece p;
  p.result = ctx.r3.u32;
  p.ptr = Ld32(base, out + 0x00);
  p.size = Ld32(base, out + 0x04);
  p.time_ms = Ld64(base, out + 0x10);
  p.has_time = Ld32(base, out + 0x18);
  p.whole = Ld32(base, out + 0x1C);
  ctx.r1.u64 = r1;
  ctx.lr = lr;
  return p;
}

void Collect(Decoder& d) {
  while (avcodec_receive_frame(d.av, d.frame) == 0) {
    Frame f;
    if (!d.times.empty()) {
      f.time_ms = d.times.top();
      d.times.pop();
    } else {
      f.time_ms = d.anchor_ms + d.since_anchor * 1000 / d.fps;  // not expected: a time goes in with every frame
    }
    f.yuv.resize(d.FrameBytes());
    uint8_t* dst = f.yuv.data();
    if (!d.bink || !FillFromBink(*d.bink, f.time_ms, dst, d.width, d.height)) {
      for (int plane = 0; plane < 3; ++plane) {
        const uint32_t w = plane ? d.width / 2 : d.width, h = plane ? d.height / 2 : d.height;
        for (uint32_t y = 0; y < h; ++y, dst += w)
          std::memcpy(dst, d.frame->data[plane] + size_t(y) * d.frame->linesize[plane], w);
      }
    }
    d.ready.push_back(std::move(f));
    d.frames_out++;
    av_frame_unref(d.frame);
  }
}

// One compressed frame from the callback into FFmpeg. False at the end of the data (or an error). A frame (an ASF
// media object) comes in pieces: its first one carries the time and the frame's whole size; 0x00500001 only ends an
// ASF payload, which can stop in the middle of a frame (WBIntroLogo's 185811-byte key frame took 16 of them).
bool FeedOne(PPCContext& ctx, uint8_t* base, Decoder& d) {
  d.data.clear();
  int64_t time_ms = AV_NOPTS_VALUE;
  uint32_t pieces = 0, whole = 0;
  for (;;) {
    const Piece p = CallData(ctx, base, d);
    if (static_cast<int32_t>(p.result) < 0) {
      static std::atomic<int> logged{0};
      if (logged.fetch_add(1) < 8)
        REXLOG_INFO("[movie] data callback {:08X} after {} frames in, {} out", p.result, d.frames_in, d.frames_out);
      return false;
    }
    if (pieces++ == 0) {
      whole = p.whole;
      if (p.has_time) time_ms = static_cast<int64_t>(p.time_ms);
    }
    if (p.size) {
      const size_t at = d.data.size();
      d.data.resize(at + p.size);
      std::memcpy(d.data.data() + at, Raw(base, p.ptr), p.size);
    }
    if (d.data.size() >= whole) break;
  }
  const size_t n = d.data.size();
  {
    static std::atomic<int> logged{0};
    if (logged.fetch_add(1) < REXCVAR_GET(mk9_movie_trace))
      REXLOG_INFO("[movie] frame {}: {} bytes in {} pieces (whole {}), time {}", d.frames_in, n, pieces, whole, time_ms);
  }
  d.data.resize(n + AV_INPUT_BUFFER_PADDING_SIZE, 0);
  av_packet_unref(d.packet);
  d.packet->data = d.data.data();
  d.packet->size = static_cast<int>(n);
  if (time_ms != AV_NOPTS_VALUE) {
    d.anchor_ms = time_ms;
    d.since_anchor = 0;
  }
  const int64_t frame_ms = d.anchor_ms + d.since_anchor++ * 1000 / d.fps;
  d.packet->pts = time_ms;
  d.packet->dts = AV_NOPTS_VALUE;
  if (const int err = avcodec_send_packet(d.av, d.packet); err < 0) {
    static std::atomic<int> logged{0};
    char text[64];
    av_strerror(err, text, sizeof(text));
    if (logged.fetch_add(1) < 8)
      REXLOG_WARN("[movie] FFmpeg refused frame {} ({} bytes, time {}): {} ({}); first bytes {:02X}{:02X}{:02X}{:02X}",
                  d.frames_in, n, time_ms, text, err, d.data[0], d.data[1], d.data[2], d.data[3]);
  } else {
    d.times.push(frame_ms);  // only for a frame FFmpeg took: one it refused yields no picture
  }
  d.frames_in++;
  Collect(d);
  return true;
}

}  // namespace

REX_EXTERN(__imp__sub_82D255C0);
REX_HOOK_RAW(sub_82D255C0) {  // Init
  static std::atomic<int> n{0};
  const uint32_t table = ctx.r3.u32, media = ctx.r4.u32, callback = ctx.r6.u32, callback_ctx = ctx.r7.u32;
  const uint32_t player = ctx.r8.u32, handle_out = ctx.r9.u32;
  const uint32_t format = Ld32(base, media + 8);
  const uint32_t width = Ld32(base, format + 8), height = Ld32(base, format + 12), fourcc = Ld32(base, format + 20);
  const uint32_t extra_n = Ld32(base, format + 48) >> 16, extra_p = Ld32(base, format + 52);
  const AVCodecID id = fourcc == 0x574D5633u ? AV_CODEC_ID_WMV3 : fourcc == 0x57564331u ? AV_CODEC_ID_VC1 : AV_CODEC_ID_NONE;
  const AVCodec* codec = id != AV_CODEC_ID_NONE ? avcodec_find_decoder(id) : nullptr;
  if (!REXCVAR_GET(mk9_movie_ffmpeg) || !codec || !width || !height || (width | height) & 1 || extra_n > 4096) {
    if (REXCVAR_GET(mk9_movie_ffmpeg))
      REXLOG_WARN("[movie] {:08X}: FOURCC {:08X} {}x{}: the game's own decoder", table, fourcc, width, height);
    __imp__sub_82D255C0(ctx, base);
    return;
  }
  auto d = std::make_unique<Decoder>();
  d->width = width;
  d->height = height;
  d->callback = callback;
  d->callback_ctx = callback_ctx;
  d->av = avcodec_alloc_context3(codec);
  d->av->width = static_cast<int>(width);
  d->av->height = static_cast<int>(height);
  d->av->thread_count = 1;
  d->av->pkt_timebase = AVRational{1, 1000};
  if (extra_n) {
    d->av->extradata = static_cast<uint8_t*>(av_mallocz(extra_n + AV_INPUT_BUFFER_PADDING_SIZE));
    std::memcpy(d->av->extradata, Raw(base, extra_p), extra_n);
    d->av->extradata_size = static_cast<int>(extra_n);
  }
  d->frame = av_frame_alloc();
  d->packet = av_packet_alloc();
  if (avcodec_open2(d->av, codec, nullptr) < 0) {
    REXLOG_WARN("[movie] {:08X}: FFmpeg could not open {}; the game's own decoder", table, codec->name);
    __imp__sub_82D255C0(ctx, base);
    return;
  }
  // The original's state block and message handler (see the header).
  uint32_t out = 0;
  if (static_cast<int32_t>(CallGuest(ctx, base, kPlayerAlloc, {player, 25, kStateSize, kOutArg}, &out)) < 0) {
    REXLOG_WARN("[movie] {:08X}: the player could not allocate the codec state; the game's own decoder", table);
    __imp__sub_82D255C0(ctx, base);
    return;
  }
  d->player = player;
  d->state = Ld32(base, out);
  std::memset(Raw(base, d->state), 0, kStateSize);
  Mark(base, d->state, kStateSize);
  St32(base, d->state + 0, callback);
  St32(base, d->state + 4, callback_ctx);
  St32(base, d->state + 4228, player);
  const uint32_t reg = CallGuest(ctx, base, kPlayerRegister, {player, kCodecHandler, d->state, d->state + 4232});
  if (static_cast<int32_t>(reg) < 0) {
    REXLOG_WARN("[movie] {:08X}: registering the codec's handler failed ({:08X})", table, reg);
    ctx.r3.u64 = reg;
    return;
  }
  if (handle_out) St32(base, handle_out, Ld32(base, d->state + 4232));
  St32(base, table + 32, d->state);
  REXLOG_INFO("[movie] {:08X}: {} {}x{} ({} bytes of private data) decoded by FFmpeg", table, codec->name, width, height,
              extra_n);
  d->bink = BinkForThisMovie(table);
  if (Trace(n))
    REXLOG_INFO("[movie] Init({:08X} media {:08X} cb {:08X} ctx {:08X} player {:08X}) state {:08X} handle {}", table,
                media, callback, callback_ctx, player, d->state, Ld32(base, d->state + 4232));
  {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_decoders[table] = std::move(d);
    g_playing.store(uint32_t(g_decoders.size()), std::memory_order_relaxed);
  }
  ctx.r3.u64 = 0;
}

REX_EXTERN(__imp__sub_82D25330);
REX_HOOK_RAW(sub_82D25330) {  // Decode
  static std::atomic<int> n{0};
  const uint32_t table = ctx.r3.u32, out = ctx.r4.u32;
  Decoder* d = Find(table);
  if (!d) {
    __imp__sub_82D25330(ctx, base);
    return;
  }
  while (d->ready.empty() && !d->eof) {
    if (!FeedOne(ctx, base, *d)) {
      d->eof = true;
      // Drain: the frames FFmpeg holds back for reordering. Not reached at the end of a movie with B frames: the
      // player calls Decode once per compressed frame and then Close, with no end-of-stream call, message or flag
      // (the callback's +8 output is 1 on every frame's last piece), so the last frame (33 ms) is never shown.
      avcodec_send_packet(d->av, nullptr);
      Collect(*d);
    }
  }
  const bool have = !d->ready.empty();
  if (out) St32(base, out, have ? d->FrameBytes() : 0);
  ctx.r3.u64 = have ? 0 : kFail;
  if (Trace(n))
    REXLOG_INFO("[movie] Decode({:08X}) = {:08X}, {} ready, in {} out {}", table, ctx.r3.u32, d->ready.size(),
                d->frames_in, d->frames_out);
}

REX_EXTERN(__imp__sub_82D25390);
REX_HOOK_RAW(sub_82D25390) {  // GetOutput
  static std::atomic<int> n{0};
  const uint32_t table = ctx.r3.u32, buffer = ctx.r4.u32, capacity = ctx.r5.u32;
  const uint32_t size_out = ctx.r6.u32, flags_out = ctx.r7.u32, time_out = ctx.r8.u32, one_out = ctx.r9.u32;
  Decoder* d = Find(table);
  if (!d) {
    __imp__sub_82D25390(ctx, base);
    return;
  }
  {
    // The size the player negotiated (written by the original handler) against the decoder's.
    static std::atomic<int> checked{0};
    const uint32_t w = Ld32(base, d->state + 32), h = Ld32(base, d->state + 36), bytes = Ld32(base, d->state + 108);
    if ((w != d->width || h != d->height || bytes != d->FrameBytes()) && checked.fetch_add(1) < 4)
      REXLOG_WARN("[movie] {:08X}: the player negotiated {}x{} ({} bytes), the stream is {}x{}", table, w, h, bytes,
                  d->width, d->height);
  }
  if (d->FrameBytes() > capacity) {
    ctx.r3.u64 = kBufferSmall;
    return;
  }
  if (d->ready.empty()) {
    if (size_out) St32(base, size_out, 0);
    if (flags_out) St32(base, flags_out, 0);
    ctx.r3.u64 = 0;
    return;
  }
  Frame f = std::move(d->ready.front());
  d->ready.pop_front();
  std::memcpy(Raw(base, buffer), f.yuv.data(), f.yuv.size());
  Mark(base, buffer, f.yuv.size());
  if (size_out) St32(base, size_out, static_cast<uint32_t>(f.yuv.size()));
  if (flags_out) St32(base, flags_out, 0);
  if (time_out) St64(base, time_out, static_cast<uint64_t>(f.time_ms));
  if (one_out) St32(base, one_out, 1);
  ctx.r3.u64 = 0;
  if (Trace(n)) REXLOG_INFO("[movie] GetOutput({:08X}) time {} ms, {} left", table, f.time_ms, d->ready.size());
}

REX_EXTERN(__imp__sub_82D25228);
REX_HOOK_RAW(sub_82D25228) {  // Reset
  const uint32_t table = ctx.r3.u32;
  Decoder* d = Find(table);
  if (!d) {
    __imp__sub_82D25228(ctx, base);
    return;
  }
  avcodec_flush_buffers(d->av);
  d->ready.clear();
  d->times = {};
  d->eof = false;
  REXLOG_INFO("[movie] {:08X}: reset after {} frames in, {} out", table, d->frames_in, d->frames_out);
  ctx.r3.u64 = 0;
}

REX_EXTERN(__imp__sub_82D25280);
REX_HOOK_RAW(sub_82D25280) {  // Close
  const uint32_t table = ctx.r3.u32;
  std::unique_ptr<Decoder> d;
  {
    std::lock_guard<std::mutex> lock(g_mutex);
    auto it = g_decoders.find(table);
    if (it != g_decoders.end()) {
      d = std::move(it->second);
      g_decoders.erase(it);
      g_playing.store(uint32_t(g_decoders.size()), std::memory_order_relaxed);
    }
  }
  if (!d) {
    __imp__sub_82D25280(ctx, base);
    return;
  }
  REXLOG_INFO("[movie] {:08X}: closed after {} frames in, {} out", table, d->frames_in, d->frames_out);
  if (d->bink) {
    const BinkVideo& b = *d->bink;
    REXLOG_INFO("[movie] {:08X}: {} pictures from the Bink ({} of its frames decoded, {} failed; {:.1f} ms a picture, "
                "{:.2f} ms a frame decoded)",
                table, b.fills, b.decodes, b.failures, b.fills ? double(b.us) / 1000.0 / double(b.fills) : 0.0,
                b.decodes ? double(b.us_decoding) / 1000.0 / double(b.decodes) : 0.0);
  }
  CallGuest(ctx, base, kPlayerUnregister, {d->player, Ld32(base, d->state + 4232)});
  St32(base, table + 32, d->state);
  CallGuest(ctx, base, kPlayerFree, {d->player, 25, table + 32});  // &p: table+32 holds the state's address
  for (uint32_t i = 0; i < 6; ++i) St32(base, table + 4 * i, 0);
  St32(base, table + 32, 0);
  ctx.r3.u64 = 0;
}

// mk9-lauta (2026-11-10): a movie decoded here is playing (the dynamic resolution and the draw mode's auto leave its
// windows alone: its frames come at its own 30 a second, sa2_renut_pacing.cpp).
// mk9-lauta (2026-11-17, issue #12): a cutscene skipped with START or A, the first time too. The movie object
// (GFullScreenMovie, 0x834246A8; its vtable 0x821605B0) has a tick (its [1], sub_829D02E0) that runs while its +20 is
// set; with the flag at 0x834246B4 a movie plays, and the tick ends it (sub_829CF4A0(this), the same call when the
// movie runs out) once its player says it is over, or when a controller presses the buttons of its +24 (a mask of the
// game's pad bits) -- 0 for every movie the game played on PC (the boot logos and Story Mode's alike; how the game
// lets a cutscene seen before be skipped was not looked into). Here, after the tick, START or A newly pressed on any of
// the game's controllers (0x83426F70, 12 of 1540 bytes, sub_829D87D8: +8 its kind, connected with 0xC9; +12 the
// buttons down, +24 the frame before's; A 0x20, START 0x100) ends the movie the way its own end does: Story Mode goes
// on as after a cutscene watched to the end. Not in the movie's first second (a press from the screen before), and
// never in an online session (its frames go alike on every machine). F2's option, read each time.
namespace {

constexpr uint32_t kMovieOn = 0x834246B4;      // a movie plays (the tick's flag)
constexpr uint32_t kMovieEnd = 0x829CF4A0;     // (this): the movie's end
constexpr uint32_t kPads = 0x83426F70, kPadSize = 1540, kPadCount = 12;
constexpr uint32_t kPadConnected = 0xC9, kPadA = 0x20, kPadStart = 0x100;
constexpr auto kSkipAfter = std::chrono::seconds(1);
constexpr auto kSkipIdle = std::chrono::milliseconds(250);  // no tick this long: the next one is another movie's

std::mutex g_skip_mutex;
bool g_skip_playing = false;
std::chrono::steady_clock::time_point g_skip_since{}, g_skip_tick{};

}  // namespace

extern "C" REX_FUNC(__imp__sub_829D02E0);
REX_HOOK_RAW(sub_829D02E0) {  // the movie object's tick
  const uint32_t self = ctx.r3.u32;
  __imp__sub_829D02E0(ctx, base);
  if (!REXCVAR_GET(mk9_skip_cutscenes) || mk9::netplay::SessionGame()) return;
  const bool playing = Ld32(base, self + 20) != 0 && Ld32(base, kMovieOn) != 0;
  std::lock_guard<std::mutex> lock(g_skip_mutex);
  const auto now = std::chrono::steady_clock::now();
  const bool idle = now - g_skip_tick > kSkipIdle;
  g_skip_tick = now;
  if (!playing) {
    g_skip_playing = false;
    return;
  }
  if (!g_skip_playing || idle) {
    g_skip_playing = true;
    g_skip_since = now;
    return;
  }
  if (now - g_skip_since < kSkipAfter) return;
  for (uint32_t i = 0; i < kPadCount; ++i) {
    const uint32_t pad = kPads + i * kPadSize;
    if ((Ld32(base, pad + 8) & kPadConnected) == 0) continue;
    const uint32_t pressed = Ld32(base, pad + 12) & ~Ld32(base, pad + 24) & (kPadStart | kPadA);
    if (!pressed) continue;
    REXLOG_INFO("[movie] the cutscene skipped ({} on controller {})", pressed & kPadStart ? "START" : "A", i);
    g_skip_playing = false;
    CallGuest(ctx, base, kMovieEnd, {self});
    return;
  }
}

namespace mk9::movie {

void NoteOpened(std::string_view guest_path) {
  // ...\<name>.wmv (any case): only the cutscenes are WMV
  if (guest_path.size() < 5) return;
  std::string tail(guest_path.substr(guest_path.size() - 4));
  std::transform(tail.begin(), tail.end(), tail.begin(), [](unsigned char c) { return char(std::tolower(c)); });
  if (tail != ".wmv") return;
  const size_t slash = guest_path.find_last_of("\\/");
  const std::string_view name =
      guest_path.substr(slash == std::string_view::npos ? 0 : slash + 1, std::string_view::npos);
  std::lock_guard<std::mutex> lock(g_name_mutex);
  g_movie_name.assign(name.substr(0, name.size() - 4));
  g_movie_name_at = std::chrono::steady_clock::now();
}
bool Playing() { return g_playing.load(std::memory_order_relaxed) != 0; }
}  // namespace mk9::movie
