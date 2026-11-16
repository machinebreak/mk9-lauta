"""D3D11 feasibility probe (docs/renderer/d3d11.md): turn dumped pixel shader HLSL (DXC, SM 6.0, bindless) into SM 5.0
HLSL and compile it with FXC. Measures how many compile and how long it takes.

    python tools/d3d11/fxc_probe.py out/pc/renut_gpu/early_alpha_hlsl <out dir> <count> [seed]

2026-11-16: 1000 of 1000 random pixel shaders compiled (ps_5_0), 0.10 s each on average, 0.94 s the slowest. The
texture slots are literal here and the descriptor flags (resolution scale, stacked textures) are dropped: enough to
know FXC takes the code, not yet what the game draws.
"""
import os
import random
import re
import subprocess
import sys
import time

FXC = r'C:\Program Files (x86)\Windows Kits\10\bin\10.0.26100.0\x64\fxc.exe'

PRELUDE = ('#define RENUT_D3D11 1\n#define select(c, a, b) ((c) ? (a) : (b))\n'
           '#define and(a, b) ((a) && (b))\n#define or(a, b) ((a) || (b))\n')

DECLS = [
    (r'ByteAddressBuffer g_VertexFetchData : register\(t0, space5\);',
     'ByteAddressBuffer g_VertexFetchData : register(t120);'),
    (r'RWByteAddressBuffer g_MemExport : register\(u0, space6\);',
     'RWByteAddressBuffer g_MemExport : register(u7);'),
    (r'Texture2D<float4> g_Texture2DDescriptorHeap\[\] : register\(t0, space0\);',
     'Texture2D<float4> g_Texture2DDescriptorHeap[32] : register(t0);'),
    (r'Texture3D<float4> g_Texture3DDescriptorHeap\[\] : register\(t0, space1\);',
     'Texture3D<float4> g_Texture3DDescriptorHeap[32] : register(t32);'),
    (r'TextureCube<float4> g_TextureCubeDescriptorHeap\[\] : register\(t0, space2\);',
     'TextureCube<float4> g_TextureCubeDescriptorHeap[32] : register(t64);'),
    (r'SamplerState g_SamplerDescriptorHeap\[\] : register\(s0, space3\);',
     'SamplerState g_SamplerDescriptorHeap[16] : register(s0);'),
    (r'Texture2DArray<float4> g_Texture2DArrayDescriptorHeap\[\] : register\(t0, space4\);',
     'Texture2DArray<float4> g_Texture2DArrayDescriptorHeap[16] : register(t96);'),
]
SWIZ = {'x': 0, 'y': 1, 'z': 2, 'w': 3}
FIELD = re.compile(r'^\s*uint (\w+)_(Texture2D|Texture3D|TextureCube|Sampler)DescriptorIndex : packoffset\(c(\d+)\.([xyzw])\);\s*$')
BASE = {'Texture2D': 0, 'Texture3D': 8, 'TextureCube': 16, 'Sampler': 24}


def transform(text):
    # FXC's preprocessor takes no function-like macro with an empty parameter list: object-like ones instead
    empty = set(re.findall(r'#define (\w+)\(\)', text))
    for name in empty:
        text = text.replace('#define ' + name + '()', '#define ' + name)
        text = re.sub(r'\b' + name + r'\(\)', name, text)
    for pattern, repl in DECLS:
        text = re.sub(pattern, repl, text)
    # FXC keywords (the old effects syntax) used as names
    for word in ('texture', 'sampler', 'pass', 'technique', 'string'):
        text = re.sub(r'\b' + word + r'\b', word + '_', text)
    text = re.sub(r'register\((b\d+), space\d+\)', r'register(\1)', text)
    # no 64-bit integer literals in SM 5.0 (the operands they meet are 32-bit uints)
    text = re.sub(r'\b(\d+)ull\b', r'\1u', text)
    text = re.sub(r'\b(\d+)ll\b', r'\1', text)
    out = []
    for line in text.split('\n'):
        m = FIELD.match(line)
        if m:
            name, kind, c, sw = m.group(1), m.group(2), int(m.group(3)), SWIZ[m.group(4)]
            slot = (c - BASE[kind]) * 4 + sw
            if kind == 'Sampler':
                slot &= 15
            out.append(f'#define {name}_{kind}DescriptorIndex {slot}u')
            continue
        out.append(line)
    return PRELUDE + '\n'.join(out)


def main():
    src, dst, count = sys.argv[1], sys.argv[2], int(sys.argv[3])
    seed = int(sys.argv[4]) if len(sys.argv) > 4 else 1
    os.makedirs(dst, exist_ok=True)
    files = sorted(f for f in os.listdir(src) if f.endswith('.hlsl'))
    random.Random(seed).shuffle(files)
    ok, fail, times, errors = 0, 0, [], {}
    for f in files[:count]:
        text = open(os.path.join(src, f), encoding='utf-8', errors='replace').read()
        out_path = os.path.join(dst, f)
        open(out_path, 'w', encoding='utf-8').write(transform(text))
        t0 = time.time()
        r = subprocess.run([FXC, '/nologo', '/D', 'RENUT_RECOMP=1', '/T', 'ps_5_0', '/E', 'main', '/Fo', out_path + '.dxbc', out_path],
                           capture_output=True, text=True)
        dt = time.time() - t0
        times.append(dt)
        if r.returncode == 0:
            ok += 1
        else:
            fail += 1
            msg = [l for l in (r.stdout + r.stderr).split('\n') if 'error' in l]
            first = msg[0] if msg else (r.stdout + r.stderr)[:200]
            key = re.sub(r'^.*?\.hlsl\(\d+,\d+(-\d+)?\): ', '', first)[:150]
            errors.setdefault(key, []).append(f)
    print(f'{ok} compiled, {fail} failed of {ok + fail}; FXC time mean {sum(times) / len(times):.2f} s, '
          f'max {max(times):.2f} s')
    for key, fs in sorted(errors.items(), key=lambda kv: -len(kv[1]))[:12]:
        print(f'  {len(fs)}x {key}   (e.g. {fs[0]})')


if __name__ == '__main__':
    main()
