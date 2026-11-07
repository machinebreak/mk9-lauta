#!/usr/bin/env python3
"""Compares two memory dumps of the same frame from two instances (or two runs) page by page and word by word.

    python tools/online/dumpdiff.py <dumpA> <dumpB> [--words 40] [--range lo-hi] [--summary]

Pages present in both dumps are compared; the output lists, per heap, how many pages differ, then the differing
big-endian words (address, A, B) inside --range (default: the image's .data/.bss 83380000-834D0000).
"""
import argparse
import collections
import os
import sys

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from memsearch import load  # noqa: E402


def pages(path):
    out = {}
    for start, buf in load(path):
        for off in range(0, len(buf) - len(buf) % 4096, 4096):
            out[start + off] = buf[off:off + 4096]
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('a')
    ap.add_argument('b')
    ap.add_argument('--words', type=int, default=40)
    ap.add_argument('--range', default='83380000-834D0000')
    a = ap.parse_args()
    lo, hi = (int(x, 16) for x in a.range.split('-'))
    pa, pb = pages(a.a), pages(a.b)
    common = sorted(set(pa) & set(pb))
    print(f'pages: A {len(pa)}, B {len(pb)}, both {len(common)}, only A {len(set(pa) - set(pb))}, '
          f'only B {len(set(pb) - set(pa))}')
    per_heap = collections.Counter()
    diff_pages = []
    for addr in common:
        if not np.array_equal(pa[addr], pb[addr]):
            per_heap[addr & 0xF0000000] += 1
            diff_pages.append(addr)
    for heap, n in sorted(per_heap.items()):
        print(f'  {heap:08X}: {n} pages differ')
    shown = 0
    for addr in diff_pages:
        if not (lo <= addr < hi):
            continue
        wa, wb = pa[addr].view('>u4'), pb[addr].view('>u4')
        for i in np.nonzero(wa != wb)[0]:
            if shown >= a.words:
                break
            print(f'  {addr + int(i) * 4:08X}  {int(wa[i]):08X}  {int(wb[i]):08X}')
            shown += 1
    print(f'{len(diff_pages)} of {len(common)} common pages differ')


if __name__ == '__main__':
    main()
