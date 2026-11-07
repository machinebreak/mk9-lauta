#!/usr/bin/env python3
"""Lists the strings a function's code points at (ASCII or UTF-16BE, as built with lis/addi and friends), to tell what
a function is: log texts, names, file paths.

    python tools/online/func_strings.py <function address> [...]

Uses tools/online/ppc_xref.py's constant propagation over the function (from .pdata or the recompiler's list).
"""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from ppc_xref import BASE, DEFAULT_IMAGE, Image, scan  # noqa: E402


def text_at(img, addr):
    off = addr - BASE
    d = img.data
    if not 0 <= off < len(d):
        return None
    end = d.find(b'\0', off)
    raw = d[off:end]
    if len(raw) >= 4 and all(32 <= c < 127 for c in raw):
        return raw.decode('ascii')
    # UTF-16BE
    chars = []
    while off + 1 < len(d) and d[off] == 0 and 32 <= d[off + 1] < 127:
        chars.append(chr(d[off + 1]))
        off += 2
    if len(chars) >= 4 and d[off:off + 2] == b'\0\0':
        return 'W"' + ''.join(chars) + '"'
    return None


def main():
    img = Image(DEFAULT_IMAGE)
    lo = min(img.sections[s][0] for s in ('.rdata',) if s in img.sections)
    hi = max(img.sections[s][1] for s in ('.rdata', '.data') if s in img.sections)
    for arg in sys.argv[1:]:
        start = int(arg, 16)
        f = img.func_of(start)
        if not f:
            print(f'{start:08X}: no function')
            continue
        saved = img.funcs
        img.funcs = [f]
        seen = set()
        print(f'== sub_{f[0]:08X} ({f[1] - f[0]} bytes)')
        for a, _, v, how in scan(img, [lo], hi - lo):
            t = text_at(img, v)
            if t and v not in seen:
                seen.add(v)
                print(f'  {a:08X} {v:08X} {t[:100]}')
        img.funcs = saved


if __name__ == '__main__':
    main()
