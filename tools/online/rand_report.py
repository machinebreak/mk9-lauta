#!/usr/bin/env python3
"""Reads mk9_rand_log files (every draw of the game's random number generator, mk9_capture.cpp) and names their host
stacks: the game's recompiled functions come out as sub_XXXXXXXX, so a stack is the guest call chain.

    python tools/online/rand_report.py <log> [--frames 1700-1710] [--depth 10]
    python tools/online/rand_report.py <log A> --other <log B> [--context 4] [--depth 14] [--from A0,B0]

The first form lists the draws of those frames. The second walks both machines' draws in order and stops at the first
one that differs (frame, source line or result): the draws around it are listed with their stacks, the first extra or
missing draw being the code that ran on one machine only. --from A0,B0 compares from those frames on (the same game
moment on both, e.g. an epoch's frame 0 after a session barrier), frames counted from them.
Log lines: "<frame> <thread> <file>:<line> <result> <stack offsets...>" (thread g = the game thread).
"""
import argparse
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from stacks_report import symbolizer  # noqa: E402

import subprocess  # noqa: E402

REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))


class Draw:
    __slots__ = ('frame', 'thread', 'source', 'result', 'stack')

    def __init__(self, line):
        parts = line.split()
        self.frame = int(parts[0])
        self.thread = parts[1]
        self.source = parts[2]
        self.result = parts[3]
        self.stack = [int(x, 16) for x in parts[4:] if x != 'ffffffffffffffff']

    def key(self):
        return self.frame, self.source, self.result


def load(path):
    out = []
    for line in open(path, encoding='utf-8', errors='replace'):
        if line.strip():
            try:
                out.append(Draw(line))
            except (ValueError, IndexError):
                pass
    return out


def names_for(exe, offsets):
    offsets = sorted(set(offsets))
    if not offsets:
        return {}
    with open(exe, 'rb') as f:
        head = f.read(4096)
    pe = int.from_bytes(head[0x3C:0x40], 'little')
    image_base = int.from_bytes(head[pe + 24 + 24:pe + 24 + 32], 'little')
    tool = symbolizer()
    names = {}
    for i in range(0, len(offsets), 2000):
        chunk = offsets[i:i + 2000]
        # a return address points after the call: look one byte back so the call's own line and function come out
        out = subprocess.run([tool, '--functions=linkage', '--no-inlines', '--output-style=GNU', '--obj', exe],
                             input='\n'.join(f'0x{image_base + o - 1:x}' for o in chunk), capture_output=True,
                             text=True).stdout.split('\n')
        for j, o in enumerate(chunk):
            name = out[2 * j] if 2 * j < len(out) else '?'
            names[o] = name.strip() or '?'
    return names


def show(draws, names, depth, mark=None):
    for d in draws:
        chain = []
        for o in d.stack:
            n = names.get(o, '?')
            if n.startswith('mk9::') or n.startswith('rex::') or 'CaptureDraw' in n or n in chain[-1:]:
                continue
            chain.append(n)
            if len(chain) >= depth:
                break
        flag = '>>' if mark is not None and d is mark else '  '
        print(f'{flag} {d.frame:6} {d.thread:4} {d.source:28} {d.result}  ' + ' < '.join(chain))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('log')
    ap.add_argument('--other')
    ap.add_argument('--frames')
    ap.add_argument('--context', type=int, default=4)
    ap.add_argument('--from', dest='start', help='A0,B0: the frames both logs are compared from')
    ap.add_argument('--depth', type=int, default=10)
    ap.add_argument('--exe', default=os.path.join(REPO, 'out', 'pc', 'mk9.exe'))
    a = ap.parse_args()
    draws = load(a.log)

    if not a.other:
        lo, hi = 0, 1 << 62
        if a.frames:
            lo, _, hi = a.frames.partition('-')
            lo, hi = int(lo), int(hi or lo)
        sel = [d for d in draws if lo <= d.frame <= hi]
        names = names_for(a.exe, [o for d in sel for o in d.stack])
        show(sel, names, a.depth)
        print(f'{len(sel)} draws')
        return

    other = load(a.other)
    if a.start:
        a0, b0 = (int(x) for x in a.start.split(','))
        draws = [d for d in draws if d.frame >= a0]
        other = [d for d in other if d.frame >= b0]
        for d in draws:
            d.frame -= a0
        for d in other:
            d.frame -= b0
    n = min(len(draws), len(other))
    i = next((k for k in range(n) if draws[k].key() != other[k].key()), None)
    print(f'{len(draws)} draws in {a.log}, {len(other)} in {a.other}')
    if i is None:
        print(f'the first {n} draws are the same on both')
        return
    print(f'the draws part at #{i}: frame {draws[i].frame} here, {other[i].frame} there')
    lo, hi = max(0, i - a.context), i + a.context + 1
    sel_a, sel_b = draws[lo:hi], other[lo:hi]
    names = names_for(a.exe, [o for d in sel_a + sel_b for o in d.stack])
    print(f'== {a.log}')
    show(sel_a, names, a.depth, draws[i])
    print(f'== {a.other}')
    show(sel_b, names, a.depth, other[i])


if __name__ == '__main__':
    main()
