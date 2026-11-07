#!/usr/bin/env python3
"""Symbolizes a stack dump of the PC build (logs/pilas_N.txt, written by sa2_stack_dump_s) thread by thread.

    python tools/online/stacks_report.py out/pc/logs/pilas_1.txt [--exe out/pc/mk9.exe] [--depth 12]

Frames in the executable are named with llvm-symbolizer (LLVM's bin on PATH or ../_tools/llvm/bin): the game's
recompiled functions come out as sub_XXXXXXXX. Frames in other modules show as [module+offset].
"""
import argparse
import os
import shutil
import subprocess
import sys

REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))


def symbolizer():
    tool = shutil.which('llvm-symbolizer')
    if not tool:
        local = os.path.join(os.path.dirname(REPO), '_tools', 'llvm', 'bin', 'llvm-symbolizer.exe')
        tool = local if os.path.exists(local) else None
    if not tool:
        sys.exit('llvm-symbolizer not found')
    return tool


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('dump')
    ap.add_argument('--exe', default=os.path.join(REPO, 'out', 'pc', 'mk9.exe'))
    ap.add_argument('--depth', type=int, default=12)
    args = ap.parse_args()

    modules, threads, order = {}, {}, []
    for line in open(args.dump, encoding='utf-8', errors='replace'):
        line = line.rstrip('\n')
        if line.startswith('#modulo;'):
            _, idx, name, _base, _size, _path = line.split(';', 5)
            modules[int(idx)] = name
        elif line.startswith('#hilo;'):
            parts = line.split(';')
            tid, name = parts[1], parts[2]
            threads[tid] = {'name': name, 'frames': []}
            order.append(tid)
        elif line and not line.startswith('#'):
            tid, depth, mod, off = line.split(';')
            threads[tid]['frames'].append((int(mod), int(off, 16)))

    exe_offsets = sorted({off for t in threads.values() for mod, off in t['frames'] if mod == 0})
    names = {}
    if exe_offsets:
        with open(args.exe, 'rb') as f:
            head = f.read(4096)
        pe = int.from_bytes(head[0x3C:0x40], 'little')
        image_base = int.from_bytes(head[pe + 24 + 24:pe + 24 + 32], 'little')
        tool = symbolizer()
        for i in range(0, len(exe_offsets), 2000):
            chunk = exe_offsets[i:i + 2000]
            out = subprocess.run([tool, '--functions=linkage', '--no-inlines', '--output-style=GNU', '--obj', args.exe],
                                 input=chr(10).join(hex(image_base + o) for o in chunk), capture_output=True,
                                 text=True).stdout.splitlines()
            for j, off in enumerate(chunk):
                names[off] = out[2 * j].strip() if 2 * j < len(out) else '??'

    for tid in order:
        t = threads[tid]
        frames = []
        for mod, off in t['frames'][:args.depth]:
            if mod == 0:
                frames.append(names.get(off, f'mk9+{off:x}'))
            else:
                frames.append(f'[{modules.get(mod, mod)}]')
        game = [f for f in frames if f.startswith('sub_')]
        print(f'{tid} {t["name"] or "?"}: ' + ' < '.join(frames))
    return 0


if __name__ == '__main__':
    main()
