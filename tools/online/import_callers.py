#!/usr/bin/env python3
"""Which recompiled game functions call a kernel/XAM import, from the generated code.

    python tools/online/import_callers.py XamInputGetCapabilities XamUserGetSigninState [...]

Prints, per import, each calling function (sub_XXXXXXXX) and how many call sites it has. The generated sources call
an import as __imp__<Name>(ctx, base).
"""
import collections
import glob
import os
import re
import sys

REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
SOURCES = os.path.join(REPO, 'app', 'generated', 'default', 'mk9_recomp.*.cpp')


def main():
    names = sys.argv[1:]
    if not names:
        sys.exit(__doc__)
    found = {n: collections.Counter() for n in names}
    calls = {n: re.compile(r'__imp__' + re.escape(n) + r'\(ctx') for n in names}
    for path in sorted(glob.glob(SOURCES)):
        func = None
        with open(path, encoding='utf-8', errors='replace') as f:
            for line in f:
                if line.startswith('DEFINE_REX_FUNC('):
                    func = line[len('DEFINE_REX_FUNC('):line.index(')')]
                    continue
                if '__imp__X' not in line and '__imp__K' not in line and '__imp__N' not in line:
                    continue
                for n, rx in calls.items():
                    if rx.search(line):
                        found[n][func] += 1
    for n in names:
        print(f'== {n}: {sum(found[n].values())} call sites in {len(found[n])} functions')
        for func, count in sorted(found[n].items()):
            print(f'  {func} x{count}')


if __name__ == '__main__':
    main()
