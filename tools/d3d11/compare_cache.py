"""Compare a small shader cache's DXIL and SPIR-V with the full cache's, shader by shader (by hash).

    python compare_cache.py <subset cache.cpp> <full shader_cache.cpp>
"""
import re
import sys

try:
    import zstandard
except ImportError:
    sys.exit('pip install zstandard')

ENTRY = re.compile(r'\{ 0x([0-9A-Fa-f]+), (\d+), (\d+), (\d+), (\d+),')


def load(cpp):
    text = open(cpp, encoding='utf-8', errors='replace').read()
    entries = {}
    for m in ENTRY.finditer(text):
        h = int(m.group(1), 16)
        entries[h] = tuple(int(m.group(i)) for i in range(2, 6))
    blobs = {}
    for kind in ('dxil', 'spirv'):
        data = open(cpp + f'.{kind}.zst', 'rb').read()
        blobs[kind] = zstandard.ZstdDecompressor().decompressobj().decompress(data)
    return entries, blobs


def main():
    sub_entries, sub_blobs = load(sys.argv[1])
    full_entries, full_blobs = load(sys.argv[2])
    same = {'dxil': 0, 'spirv': 0}
    diff = {'dxil': [], 'spirv': []}
    missing = 0
    for h, (do, ds, so, ss) in sub_entries.items():
        if h not in full_entries:
            missing += 1
            continue
        fdo, fds, fso, fss = full_entries[h]
        for kind, (o, s, fo, fs) in (('dxil', (do, ds, fdo, fds)), ('spirv', (so, ss, fso, fss))):
            a = sub_blobs[kind][o:o + s]
            b = full_blobs[kind][fo:fo + fs]
            if a == b:
                same[kind] += 1
            else:
                diff[kind].append(f'{h:016X} ({s} vs {fs} bytes)')
    print(f'{len(sub_entries)} shaders, {missing} not in the full cache')
    for kind in ('dxil', 'spirv'):
        print(f'{kind}: {same[kind]} identical, {len(diff[kind])} different', diff[kind][:8])


if __name__ == '__main__':
    main()
