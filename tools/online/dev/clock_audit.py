"""Two machines' mk9_clock_audit files compared (virtual_time.h): per frame (its latest run), the game thread's clock
reads in the frame's logic (outside the waits for the render thread); the callers of any frame whose count differs,
named; and the frames' read counts overall. The virtual clock gives every read of a frame the same time, so differing
counts are fine; a frame past the valve (20000 reads outside the waits) has its time going on by read, and a count
that differs there would give the two machines different times.

    python tools/online/dev/clock_audit.py <host audit> <guest audit> [--show 10]
"""
import collections
import os
import sys

REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__)))))
sys.path.insert(0, os.path.join(REPO, 'tools', 'online'))
from rand_report import names_for  # noqa: E402

EXE = os.path.join(REPO, 'out', 'pc', 'mk9.exe')
VALVE = 20000
show = int(sys.argv[sys.argv.index('--show') + 1]) if '--show' in sys.argv else 10


def read(path):
    keys, frames = {}, {}
    for line in open(path):
        f = line.split()
        if not f or not line.endswith('\n'):
            continue  # (the last line of a game closed mid-write)
        try:
            if f[0] == 'K':
                keys[int(f[1])] = tuple(int(x, 16) for x in f[2:6])
            elif f[0] == 'F':
                # "F <frame> <logic reads> <boundary reads> [<wait reads>] <key>:<count>..." (older files: none in waits)
                first = 5 if len(f) > 4 and ':' not in f[4] else 4
                waits = int(f[4]) if first == 5 else 0
                counts = {}
                for kv in f[first:]:
                    k, v = kv.split(':')
                    counts[keys[int(k)]] = int(v)
                frames[int(f[1])] = (int(f[2]), int(f[3]), counts, waits)  # the latest run of each frame wins
        except (IndexError, KeyError, ValueError):
            continue
    return frames


a = read(sys.argv[1])
b = read(sys.argv[2])
common = sorted(set(a) & set(b))
print(f'frames: {len(a)} and {len(b)}, {len(common)} in both')
differ = [f for f in common if a[f][0] != b[f][0]]
print(f'frames whose logic read the clock a different number of times: {len(differ)}')
reads = [a[f][0] for f in common] + [b[f][0] for f in common]
if reads:
    over = [f for f in common if a[f][0] + a[f][1] > VALVE or b[f][0] + b[f][1] > VALVE]
    print(f'logic reads a frame: max {max(reads)}, frames over 100: {sum(r > 100 for r in reads)}, '
          f'frames past the valve ({VALVE}, the boundary\'s reads with them): {len(over)} {over[:10]}')
    bounds = [a[f][1] for f in common] + [b[f][1] for f in common]
    print(f'boundary reads a frame: max {max(bounds)}, total {sum(bounds)}')
    waits = [a[f][3] for f in common] + [b[f][3] for f in common]
    print(f'reads in the waits for the render thread a frame: max {max(waits)}, total {sum(waits)}')
totals = collections.Counter()
for side in (a, b):
    for f in common:
        for k, v in side[f][2].items():
            totals[k] += v
offsets = sorted({o for k in totals for o in k if o and o != 0xFFFFFFFFFFFFFFFF})
names = names_for(EXE, offsets) if offsets else {}


def chain(k):
    return ' < '.join(names.get(o, f'{o:x}') for o in k if o)


print('callers by reads (both machines):')
for k, v in totals.most_common(12):
    print(f'  {v:8d}  {chain(k)}')
for f in differ[:show]:
    ka, kb = a[f][2], b[f][2]
    print(f'frame {f}: host {a[f][0]} guest {b[f][0]}')
    for k in sorted(set(ka) | set(kb), key=lambda k: -(ka.get(k, 0) + kb.get(k, 0))):
        if ka.get(k, 0) != kb.get(k, 0):
            print(f'    {ka.get(k, 0):5d} {kb.get(k, 0):5d}  {chain(k)}')
