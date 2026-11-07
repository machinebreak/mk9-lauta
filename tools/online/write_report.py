#!/usr/bin/env python3
"""Names the writers the write tracer logged (app/src/online/write_trace.cpp: "[write_trace] page X written by host
thread T at PC; stack ..."), grouped by the function of the faulting instruction: the game's recompiled code
(sub_XXXXXXXX: its stores mark their cards, so those pages are fine) apart from host code (the SDK, Renut, the C
runtime: writers that mark no card, to fix).

    python tools/online/write_report.py out/pc/logs/<log> [--depth 6]
"""
import argparse
import collections
import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from rand_report import names_for  # noqa: E402

REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
LINE = re.compile(r'\[write_trace\] page ([0-9A-F]+) written by host thread (\d+) at ([0-9a-f]+); stack(.*)$')


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('log')
    ap.add_argument('--depth', type=int, default=6)
    ap.add_argument('--exe', default=os.path.join(REPO, 'out', 'pc', 'mk9.exe'))
    a = ap.parse_args()
    rows = []
    for line in open(a.log, encoding='utf-8', errors='replace'):
        m = LINE.search(line)
        if m:
            stack = [int(x, 16) for x in m.group(4).split() if x != 'ffffffffffffffff']
            rows.append((int(m.group(1), 16), int(m.group(2)), int(m.group(3), 16), stack))
    if not rows:
        sys.exit('no write_trace lines')
    # the faulting pc is the instruction itself (not a return address): look it up as is (+1 undone by names_for)
    names = names_for(a.exe, [pc + 1 for _, _, pc, _ in rows] + [o for _, _, _, s in rows for o in s])
    groups = collections.defaultdict(list)
    for page, thread, pc, stack in rows:
        chain = [names.get(o, '?') for o in stack]
        chain = [n for n in chain if not n.startswith('mk9::write_trace') and 'KiUserExceptionDispatcher' not in n]
        groups[names.get(pc + 1, '?')].append((page, thread, chain[:a.depth]))
    host = {k: v for k, v in groups.items() if not k.startswith('sub_') and not k.startswith('__imp__sub_')}
    guest = {k: v for k, v in groups.items() if k not in host}
    print(f'{len(rows)} writes logged: {sum(len(v) for v in guest.values())} by the game\'s code (carded), '
          f'{sum(len(v) for v in host.values())} by host code (no card)')
    for name, items in sorted(host.items(), key=lambda kv: -len(kv[1])):
        pages = sorted({p for p, _, _ in items})
        print(f'\n== {name}: {len(items)} writes, {len(pages)} pages ({", ".join(f"{p:08X}" for p in pages[:8])}'
              f'{" ..." if len(pages) > 8 else ""})')
        for chain in collections.Counter(' < '.join(c) for _, _, c in items).most_common(3):
            print(f'   {chain[1]}x {chain[0]}')


if __name__ == '__main__':
    main()
