"""The fight sound hand-offs only one machine made (mk9_sound_log .frames 'h' lines with their host stacks), with the
game's call chains named, grouped by chain.

    python handoff_report.py <host .frames> <guest .frames> [--depth 8]
"""
import collections
import os
import sys

REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__)))))
sys.path.insert(0, os.path.join(REPO, 'tools', 'online'))
from rand_report import names_for  # noqa: E402

EXE = os.path.join(REPO, 'out', 'pc', 'mk9.exe')
depth = int(sys.argv[sys.argv.index('--depth') + 1]) if '--depth' in sys.argv else 8


def read(path):
    out = collections.defaultdict(list)  # (fight, frame) -> [stack tuple]
    fight, last = 0, -1
    for line in open(path):
        f = line.split()
        if not f or f[0] != 'h':
            continue
        frame = int(f[1])
        if frame >= 0:
            if frame < last - 500:
                fight += 1
            last = frame
        stack = tuple(int(x, 16) for x in f[6:6 + depth])
        out[(fight, frame)].append(stack)
    return out


h = read(sys.argv[1])
g = read(sys.argv[2])
only = {'host': collections.Counter(), 'guest': collections.Counter()}
both = collections.Counter()
for key in set(h) | set(g):
    a = collections.Counter(h.get(key, []))
    b = collections.Counter(g.get(key, []))
    for st in set(a) | set(b):
        common = min(a[st], b[st])
        both[st] += common
        if a[st] > common:
            only['host'][st] += a[st] - common
        if b[st] > common:
            only['guest'][st] += b[st] - common

offsets = sorted({o for c in (both, only['host'], only['guest']) for st in c for o in st if o != 0xFFFFFFFFFFFFFFFF})
names = names_for(EXE, offsets)


def chain(st):
    return ' < '.join(names.get(o, f'{o:x}') for o in st[:depth])


print(f'hand-offs made on both: {sum(both.values())}; host only {sum(only["host"].values())}; '
      f'guest only {sum(only["guest"].values())}')
for side in ('host', 'guest'):
    print(f'== {side} only, by call chain')
    for st, n in only[side].most_common(8):
        print(f'{n:5d}  {chain(st)}   (on both: {both.get(st, 0)})')
print('== made on both, the most common chains')
for st, n in both.most_common(6):
    print(f'{n:5d}  {chain(st)}')
