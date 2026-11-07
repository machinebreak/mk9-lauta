#!/usr/bin/env python3
"""Converts the game's snapshots (binary PPM, P6, from mk9_snapshot_s) to PNG, next to each file.

    python tools/online/ppm2png.py out/pc/captures/*.ppm      (or a folder: every .ppm in it)

Pure Python (zlib), so it runs without Pillow. Optional --scale N keeps one pixel in N per axis (smaller files).
"""
import os
import struct
import sys
import zlib


def read_ppm(path):
    data = open(path, 'rb').read()
    fields, pos = [], 0
    while len(fields) < 4:
        while data[pos:pos + 1].isspace():
            pos += 1
        if data[pos:pos + 1] == b'#':
            pos = data.index(b'\n', pos) + 1
            continue
        end = pos
        while not data[end:end + 1].isspace():
            end += 1
        fields.append(data[pos:end])
        pos = end
    if fields[0] != b'P6' or int(fields[3]) != 255:
        raise ValueError(f'{path}: not an 8-bit binary PPM')
    width, height = int(fields[1]), int(fields[2])
    pixels = data[pos + 1:pos + 1 + width * height * 3]
    return width, height, pixels


def write_png(path, width, height, pixels, scale=1):
    if scale > 1:
        rows = []
        for y in range(0, height, scale):
            row = pixels[y * width * 3:(y + 1) * width * 3]
            rows.append(b''.join(row[x * 3:x * 3 + 3] for x in range(0, width, scale)))
        width, height = len(range(0, width, scale)), len(rows)
        raw = b''.join(b'\0' + r for r in rows)
    else:
        stride = width * 3
        raw = b''.join(b'\0' + pixels[y * stride:(y + 1) * stride] for y in range(height))

    def chunk(kind, body):
        return struct.pack('>I', len(body)) + kind + body + struct.pack('>I', zlib.crc32(kind + body) & 0xFFFFFFFF)

    with open(path, 'wb') as f:
        f.write(b'\x89PNG\r\n\x1a\n')
        f.write(chunk(b'IHDR', struct.pack('>IIBBBBB', width, height, 8, 2, 0, 0, 0)))
        f.write(chunk(b'IDAT', zlib.compress(raw, 6)))
        f.write(chunk(b'IEND', b''))


def main():
    args = sys.argv[1:]
    scale = 1
    if '--scale' in args:
        i = args.index('--scale')
        scale = int(args[i + 1])
        del args[i:i + 2]
    paths = []
    for a in args:
        if os.path.isdir(a):
            paths += [os.path.join(a, n) for n in sorted(os.listdir(a)) if n.lower().endswith('.ppm')]
        else:
            paths.append(a)
    for p in paths:
        w, h, px = read_ppm(p)
        out = os.path.splitext(p)[0] + '.png'
        write_png(out, w, h, px, scale)
        print(f'{out} ({w}x{h})')


if __name__ == '__main__':
    main()
