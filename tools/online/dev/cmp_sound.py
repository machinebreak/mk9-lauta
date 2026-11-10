"""Compares two machines' fight sound bookkeeping (mk9_sound_log's .frames files: h hand-off numbered, a this machine
said a key, r the peer said one, x a callback dropped) and their callback logs.

    python cmp_sound.py <host .frames> <guest .frames> [<host log> <guest log>]
"""
import collections
import sys


def read_frames(path):
    hand = collections.defaultdict(list)   # (fight, frame) -> numbers
    said = {}                               # (fight, key) -> frame this machine said it
    heard = {}                              # (fight, key) -> frame the peer said it
    dropped = []
    fight, last_h = 0, -1
    for line in open(path):
        f = line.split()
        if not f:
            continue
        kind, frame = f[0], int(f[1])
        # the hand-offs are numbered in frame order: a jump back is the next fight's epoch
        if kind == 'h' and frame >= 0:
            if frame < last_h - 500:
                fight += 1
            last_h = frame
        if kind == 'h':
            hand[(fight, frame)].append(int(f[2], 16))
            continue
        if kind in ('a', 'r', 'x'):
            f[2] = str((fight << 32) | int(f[2], 16))
        if kind == 'a':
            said.setdefault(int(f[2]), frame)
        elif kind == 'r':
            heard.setdefault(int(f[2]), frame)
        elif kind == 'x':
            dropped.append((frame, int(f[2])))
    return hand, said, heard, dropped


hh, hs, hr, hx = read_frames(sys.argv[1])
gh, gs, gr, gx = read_frames(sys.argv[2])
frames = sorted(set(hh) | set(gh))
print(f'hand-offs numbered: host {sum(len(v) for v in hh.values())}, guest {sum(len(v) for v in gh.values())}; '
      f'frames with hand-offs: {len(frames)}')
mism = [f for f in frames if len(hh.get(f, [])) != len(gh.get(f, []))]
print(f'frames whose hand-off counts differ: {len(mism)}', mism[:20])
only_h = sorted(set(hs) - set(gs))
only_g = sorted(set(gs) - set(hs))
print(f'keys said: host {len(hs)}, guest {len(gs)}; only the host {len(only_h)}, only the guest {len(only_g)}')
for k in only_h[:15]:
    print(f'  host only: fight {k >> 32} key {k & 0xFFFFFFFF:08x} said at {hs[k]}')
for k in only_g[:15]:
    print(f'  guest only: fight {k >> 32} key {k & 0xFFFFFFFF:08x} said at {gs[k]}')
both = sorted(set(hs) & set(gs))
gaps = [abs(hs[k] - gs[k]) for k in both]
if gaps:
    gaps.sort()
    print(f'keys both said: {len(both)}; frames apart: median {gaps[len(gaps) // 2]}, 90% {gaps[int(len(gaps) * 0.9)]}, '
          f'max {gaps[-1]}')
print(f'dropped: host {len(hx)}, guest {len(gx)}')
