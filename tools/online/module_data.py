#!/usr/bin/env python3
"""A statically linked library's place in MK9's Xbox 360 image: its code (the functions that use its own strings) and
the image data (.data, its zero-filled tail included) only its code builds addresses into.

    python tools/online/module_data.py --strings "..\\src\\fmod" [--strings more] [--gap 0x400] [--image path]

--strings: text that only the library's code refers to (its source file names, its error texts); every string in the
image that contains it counts. The library's code is the span from the first to the last function that refers to one
of them (a static library's objects are linked one after the other). Its data: every address in .data that code in
that span builds (ppc_xref.py's constant propagation), split into what only the library builds and what other code
builds too (shared with the rest of the game), grouped into ranges whose words are at most --gap bytes apart.
Output: the code span, then "own" and "shared" ranges, one per line: start-end (bytes, how many addresses).
"""
import argparse
import bisect
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import ppc_xref  # noqa: E402


def strings_with(img, needle):
    lo, hi = img.sections['.rdata']
    data = img.data[lo - ppc_xref.BASE:hi - ppc_xref.BASE]
    found = []
    start = 0
    raw = needle.encode('ascii')
    while (i := data.find(raw, start)) >= 0:
        # back to the string's first byte
        s = i
        while s > 0 and 32 <= data[s - 1] < 127:
            s -= 1
        found.append(lo + s)
        start = i + 1
    return sorted(set(found))


def group(addresses, gap):
    out = []
    for a in sorted(addresses):
        if out and a - out[-1][1] <= gap:
            out[-1][1] = a + 4
            out[-1][2] += 1
        else:
            out.append([a, a + 4, 1])
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--strings', action='append', required=True)
    ap.add_argument('--gap', type=lambda s: int(s, 0), default=0x400)
    ap.add_argument('--image', default=ppc_xref.DEFAULT_IMAGE)
    args = ap.parse_args()
    img = ppc_xref.Image(args.image)

    targets = sorted({s for needle in args.strings for s in strings_with(img, needle)})
    if not targets:
        sys.exit('no string has that text')
    users = sorted({func for _, func, _, _ in ppc_xref.scan(img, targets, 1)})
    code_lo = users[0]
    code_hi = img.func_of(users[-1])[1]
    print(f'code {code_lo:08X}-{code_hi:08X} ({len(users)} functions use the strings, {len(targets)} strings)')

    data_lo, data_hi = img.sections['.data']
    inside, outside = set(), set()
    for _, func, value, _ in ppc_xref.scan(img, [data_lo], data_hi - data_lo):
        (inside if code_lo <= func < code_hi else outside).add(value & ~3)
    own = inside - outside
    shared = inside & outside
    for name, addresses in (('own', own), ('shared', shared)):
        ranges = group(addresses, args.gap)
        total = sum(r[1] - r[0] for r in ranges)
        print(f'{name}: {len(addresses)} addresses in {len(ranges)} ranges, {total} bytes')
        for start, end, count in ranges:
            print(f'  {start:08X}-{end:08X} ({end - start} bytes, {count})')


if __name__ == '__main__':
    main()
