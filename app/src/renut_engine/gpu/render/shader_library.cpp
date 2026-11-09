/**
 * @file    gpu/render/shader_library.cpp
 */
#include "renut_engine/gpu/render/shader_library.h"

#include <algorithm>
#include <atomic>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <unordered_set>
#include <vector>

#include <fmt/format.h>
#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif
#include <rex/cvar.h>
#include <smolv.h>
#include <xxhash.h>
#include <zstd.h>

#include "renut_engine/gpu/backend/backend.h"
#include "renut_engine/gpu/d3d/d3d.h"
#include "renut_engine/gpu/log.h"
#include "renut_engine/gpu/banjo_renut_tuning.h"
#if defined(PLUME_DEKO3D)
#include "renut_engine/gpu/banjo_renut_deko3d.h"  // banjo-nx (2026-10-17): the deko3d back end's GLSL package
#endif

REXCVAR_DEFINE_STRING(renut_shader_capture_dir, "shader_capture", "Nuts&Bolts/Graphics",
                      "Where shaders missing from the shader cache are written; the next build "
                      "recompiles them into the cache");

namespace renut::gpu {

namespace {

struct DecompressedCache {
  std::once_flag once;
  std::vector<uint8_t> bytes;
  const uint8_t *mapped = nullptr;  // mk9-lauta: the cache file's read-only view (MapCacheFile)
};

#if defined(_WIN32)
// mk9-lauta (2026-11-09): the decompressed cache as a file beside the executable (cache\shaders_<name>_<size>_<hash of
// the compressed cache>.bin), mapped read-only, instead of ~80 MB of the process's own memory: the view's pages are
// the file's, which Windows drops under memory pressure and reads again when needed (a PC with 4 GB) instead of writing
// them to the page file. Made once, written under another name and renamed when complete; nullptr when that fails (the
// cache then stays in memory, as before).
const uint8_t *MapCacheFile(const char *name, const uint8_t *compressed, size_t compressed_size,
                            size_t decompressed_size) {
  const uint64_t id = XXH3_64bits(compressed, compressed_size);
  const std::filesystem::path dir = banjo::renut_gpu::NextToExecutable("cache");
  const std::filesystem::path file =
      dir / fmt::format("shaders_{}_{}_{:016X}.bin", name, decompressed_size, id);
  std::error_code ec;
  if (std::filesystem::file_size(file, ec) != decompressed_size || ec) {
    std::filesystem::create_directories(dir, ec);
    std::vector<uint8_t> bytes(decompressed_size);
    const size_t n = ZSTD_decompress(bytes.data(), decompressed_size, compressed, compressed_size);
    if (ZSTD_isError(n) || n != decompressed_size)
      return nullptr;
    const std::filesystem::path partial = file.string() + ".partial";
    {
      std::ofstream out(partial, std::ios::binary | std::ios::trunc);
      out.write(reinterpret_cast<const char *>(bytes.data()), std::streamsize(bytes.size()));
      if (!out)
        return nullptr;
    }
    std::filesystem::rename(partial, file, ec);
    if (ec)
      return nullptr;
    // older builds' files of this cache go
    for (const auto &entry : std::filesystem::directory_iterator(dir, ec)) {
      const std::string other = entry.path().filename().string();
      if (other != file.filename().string() && other.rfind(fmt::format("shaders_{}_", name), 0) == 0)
        std::filesystem::remove(entry.path(), ec);
    }
  }
  HANDLE handle = CreateFileW(file.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                              FILE_FLAG_RANDOM_ACCESS, nullptr);
  if (handle == INVALID_HANDLE_VALUE)
    return nullptr;
  HANDLE mapping = CreateFileMappingW(handle, nullptr, PAGE_READONLY, 0, 0, nullptr);
  CloseHandle(handle);  // the mapping keeps the file
  if (!mapping)
    return nullptr;
  const void *view = MapViewOfFile(mapping, FILE_MAP_READ, 0, 0, 0);
  CloseHandle(mapping);  // the view keeps the mapping, for the process's life
  if (view)
    RGPU_INFO("{} shader cache: {} MB mapped from {}", name, decompressed_size >> 20, file.string());
  return static_cast<const uint8_t *>(view);
}
#endif

DecompressedCache g_spirv;
#if defined(_WIN32)
DecompressedCache g_dxil;
#endif
std::atomic<uint64_t> g_decompressed_bytes{0};  // ShaderCacheHostBytes

const uint8_t *Decompress(DecompressedCache &cache, const uint8_t *compressed,
                          size_t compressed_size, size_t decompressed_size, const char *name) {
  std::call_once(cache.once, [&] {
#if defined(_WIN32)
    cache.mapped = MapCacheFile(name, compressed, compressed_size, decompressed_size);
    if (cache.mapped)
      return;
#endif
    cache.bytes.resize(decompressed_size);
    const size_t n = ZSTD_decompress(cache.bytes.data(), decompressed_size, compressed,
                                     compressed_size);
    if (ZSTD_isError(n) || n != decompressed_size) {
      RGPU_ERROR("{} shader cache decompression failed", name);
      cache.bytes.clear();
      cache.bytes.shrink_to_fit();
    }
    g_decompressed_bytes.fetch_add(cache.bytes.capacity(), std::memory_order_relaxed);
  });
  if (cache.mapped)
    return cache.mapped;
  return cache.bytes.empty() ? nullptr : cache.bytes.data();
}

std::mutex g_capture_mutex;
std::unordered_set<uint64_t> g_captured;  // CaptureMissingShader

} // namespace

uint64_t HashShaderContainer(const uint8_t *container) {
  const auto *header = reinterpret_cast<const d3d::ShaderContainer *>(container);
  const size_t size = size_t(u32(header->virtualSize)) + u32(header->physicalSize);
  return XXH3_64bits(container, size);
}

const ShaderCacheEntry *FindShaderEntry(uint64_t hash) {
  if (g_shaderCacheLayout != kShaderCacheLayout) {
    static bool logged = false;
    if (!logged) {
      RGPU_ERROR("Shader cache layout {} does not match the renderer's {}; rebuild it",
                 g_shaderCacheLayout, kShaderCacheLayout);
      logged = true;
    }
    return nullptr;
  }
  const ShaderCacheEntry *begin = g_shaderCacheEntries;
  const ShaderCacheEntry *end = begin + g_shaderCacheEntryCount;
  const ShaderCacheEntry *it = std::lower_bound(
      begin, end, hash, [](const ShaderCacheEntry &e, uint64_t h) { return e.hash < h; });
  return (it != end && it->hash == hash) ? it : nullptr;
}

void ShaderBoolSpecMask(const ShaderCacheEntry &entry, uint32_t mask[8]) {
  std::fill(mask, mask + 8, 0u);
#if defined(PLUME_DEKO3D)
  // banjo-nx: the GLSL package lists them (the SPIR-V cache need not be unpacked for deko3d).
  if (IsDeko3D()) {
    banjo::renut_gpu::deko3d::BoolSpecMask(entry.hash, mask);
    return;
  }
#endif
  if (!IsVulkan() || entry.spirvSize == 0)
    return;
  const uint8_t *cache = Decompress(g_spirv, g_compressedSpirvCache, g_spirvCacheCompressedSize,
                                    g_spirvCacheDecompressedSize, "SPIR-V");
  if (!cache)
    return;
  const uint8_t *smol = cache + entry.spirvOffset;
  std::vector<uint8_t> spirv(smolv::GetDecodedBufferSize(smol, entry.spirvSize));
  if (spirv.size() < 20 || !smolv::Decode(smol, entry.spirvSize, spirv.data(), spirv.size()))
    return;
  // After the five header words, each instruction's first word is (word count << 16) | opcode. OpDecorate (71):
  // target, decoration, literals; SpecId is decoration 1.
  const auto *words = reinterpret_cast<const uint32_t *>(spirv.data());
  const size_t count = spirv.size() / 4;
  for (size_t i = 5; i < count;) {
    const uint32_t length = words[i] >> 16, opcode = words[i] & 0xFFFF;
    if (length == 0)
      break;
    if (opcode == 71 && length >= 4 && i + 3 < count && words[i + 2] == 1) {
      const uint32_t id = words[i + 3];
      if (id >= 1000 && id < 1256)
        mask[(id - 1000) / 32] |= 1u << ((id - 1000) % 32);
    }
    i += length;
  }
}

std::unique_ptr<plume::RenderShader> CreateCachedShader(plume::RenderDevice *device,
                                                        const ShaderCacheEntry &entry) {
  if (!device)
    return nullptr;

#if defined(PLUME_DEKO3D)
  // banjo-nx (2026-10-17): the deko3d back end's GLSL for uam, from the package in the NRO.
  if (IsDeko3D())
    return banjo::renut_gpu::deko3d::CreateGameShader(device, entry.hash);
#endif

  if (IsVulkan()) {
    const uint8_t *cache = Decompress(g_spirv, g_compressedSpirvCache, g_spirvCacheCompressedSize,
                                      g_spirvCacheDecompressedSize, "SPIR-V");
    if (!cache || entry.spirvSize == 0)
      return nullptr;
    const uint8_t *smol = cache + entry.spirvOffset;
    std::vector<uint8_t> spirv(smolv::GetDecodedBufferSize(smol, entry.spirvSize));
    if (spirv.empty() || !smolv::Decode(smol, entry.spirvSize, spirv.data(), spirv.size())) {
      RGPU_ERROR("SPIR-V decode failed for shader {:016X}", entry.hash);
      return nullptr;
    }
    return device->createShader(spirv.data(), spirv.size(), "main",
                                plume::RenderShaderFormat::SPIRV);
  }

#if defined(_WIN32)
  const uint8_t *cache = Decompress(g_dxil, g_compressedDxilCache, g_dxilCacheCompressedSize,
                                    g_dxilCacheDecompressedSize, "DXIL");
  if (!cache || entry.dxilSize == 0)
    return nullptr;
  return device->createShader(cache + entry.dxilOffset, entry.dxilSize, "main",
                              plume::RenderShaderFormat::DXIL);
#else
  return nullptr;
#endif
}

uint64_t ShaderCacheHostBytes() {
  return g_decompressed_bytes.load(std::memory_order_relaxed);
}

void CaptureMissingShader(const uint8_t *container, bool pixel_shader, uint64_t hash) {
  std::lock_guard lock(g_capture_mutex);
  if (!g_captured.insert(hash).second)
    return;

  const auto *header = reinterpret_cast<const d3d::ShaderContainer *>(container);
  const size_t size = size_t(u32(header->virtualSize)) + u32(header->physicalSize);
  // banjo-nx: next to the executable (on the Switch, the NRO's folder), not the working directory.
  const std::filesystem::path dir = banjo::renut_gpu::NextToExecutable(REXCVAR_GET(renut_shader_capture_dir));
  const std::filesystem::path path =
      dir / fmt::format("{}_{:016x}.bin", pixel_shader ? "ps" : "vs", hash);

  std::error_code ec;
  std::filesystem::create_directories(dir, ec);
  std::ofstream out(path, std::ios::binary);
  if (out)
    out.write(reinterpret_cast<const char *>(container), std::streamsize(size));
  if (!out) {
    RGPU_WARN("Shader {:016X} is not in the shader cache and could not be captured to {}", hash,
              path.string());
    return;
  }
  RGPU_WARN("Shader {:016X} is not in the shader cache: captured to {}, the next build adds it",
            hash, path.string());
}

} // namespace renut::gpu
