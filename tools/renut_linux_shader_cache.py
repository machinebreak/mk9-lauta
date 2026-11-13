#!/usr/bin/env python3
"""Writes the shader cache of Renut's renderer for the native Linux build from the Windows build's.

mk9-lauta (2026-11-13). Renut's XenosRecomp translates the game's 19,094 shaders with DXC, and the repository carries
DXC for Windows only: the Windows build (out/pc) makes renut_gpu/shader_cache.cpp, with the DXIL (Direct3D 12) and
the SPIR-V (Vulkan) caches embedded with #embed. Linux runs Vulkan only: this copy keeps the DXIL symbols with an empty
array (the renderer references them; Windows alone reads them) and the SPIR-V next to it, so the Linux build embeds
the very shaders the Windows build has. Output: out/renut_linux/shader_cache_linux.cpp and its .spirv.zst
(app/cmake/mk9_renut_gpu.cmake, MK9_RENUT_SHADER_CACHE_CPP). The cache is game data: never committed or distributed
apart from the executable.

Usage: python tools/renut_linux_shader_cache.py [--pc out/pc] [--out out/renut_linux]
"""
import os
import re
import shutil
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def arg(name, default):
    return sys.argv[sys.argv.index(name) + 1] if name in sys.argv else default


def main():
    pc = arg('--pc', os.path.join(ROOT, 'out', 'pc'))
    out_dir = arg('--out', os.path.join(ROOT, 'out', 'renut_linux'))
    src = os.path.join(pc, 'renut_gpu', 'shader_cache.cpp')
    spirv = src + '.spirv.zst'
    if not os.path.isfile(src) or not os.path.isfile(spirv):
        sys.exit(f'renut_linux_shader_cache: no {src} (and its .spirv.zst): build the Windows tree first')
    text = open(src, encoding='utf-8', errors='strict').read()
    m = re.search(r'(const uint8_t g_compressedDxilCache\[\]\s*=\s*\{)(.*?)(\};)', text, re.S)
    if not m:
        sys.exit('renut_linux_shader_cache: no g_compressedDxilCache in the Windows shader cache')
    text = text[:m.start(2)] + '\n0\n' + text[m.end(2):]
    text = re.sub(r'(const size_t g_dxilCacheCompressedSize\s*=\s*)\d+', r'\g<1>0', text)
    text = re.sub(r'(const size_t g_dxilCacheDecompressedSize\s*=\s*)\d+', r'\g<1>0', text)
    if '#embed "shader_cache.cpp.spirv.zst"' not in text:
        sys.exit('renut_linux_shader_cache: the SPIR-V cache is not embedded from shader_cache.cpp.spirv.zst')
    text = text.replace('#embed "shader_cache.cpp.spirv.zst"', '#embed "shader_cache_linux.cpp.spirv.zst"')

    os.makedirs(out_dir, exist_ok=True)
    blob = os.path.join(out_dir, 'shader_cache_linux.cpp.spirv.zst')
    shutil.copyfile(spirv, blob + '.tmp')
    os.replace(blob + '.tmp', blob)
    out = os.path.join(out_dir, 'shader_cache_linux.cpp')
    with open(out + '.tmp', 'w', encoding='utf-8', newline='\n') as f:
        f.write(text)
    os.replace(out + '.tmp', out)
    # Renut's own shaders (present, overlay, resolves): their SPIR-V headers, which the Windows build made with DXC
    host_src = os.path.join(pc, 'renut_gpu', 'host_shaders')
    host_out = os.path.join(out_dir, 'host_shaders')
    os.makedirs(host_out, exist_ok=True)
    host = 0
    for name in sorted(os.listdir(host_src)) if os.path.isdir(host_src) else []:
        if name.endswith('.spirv.h'):
            shutil.copyfile(os.path.join(host_src, name), os.path.join(host_out, name))
            host += 1
    if not host:
        sys.exit(f'renut_linux_shader_cache: no host shaders in {host_src}')
    print(f'renut_linux_shader_cache: {host} host shaders -> {host_out}')
    # its header (shader_cache.h: the entry layout) beside it, as the Windows build has it
    header = os.path.join(pc, 'renut_gpu', 'shader_cache.h')
    if os.path.isfile(header):
        shutil.copyfile(header, os.path.join(out_dir, 'shader_cache.h'))
    print(f'renut_linux_shader_cache: {os.path.getsize(src) >> 20} MB of source -> {out} '
          f'+ {os.path.getsize(blob) >> 10} KB of SPIR-V')


if __name__ == '__main__':
    main()
