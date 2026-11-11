"""The two players' desync reports compared (netplay.cpp WriteDesyncReport, A3): each one's summary, and the first
frame and field of the fighters' state (fight_state.h, the sync hash's ranges) where their games parted.

    python tools/online/desync_report.py <logs\\desync_..._p1> <logs\\desync_..._p2>

The folders come from each player's logs (the session-end window's "Open the desync report" button). The session's
inputs (.mk9net, named in each summary) go to tools/online/session_report.py.
"""
import os
import struct
import sys

KINDS = {0: 'match info block', 1: 'fighter object', 2: 'fighter actor'}
NAMES = {(1, 0x78C0): 'health', (2, 0xDC): 'position (depth)', (2, 0xE0): 'position (the fight\'s axis)',
         (2, 0xE4): 'position (height)'}


def read(folder):
    path = os.path.join(folder, 'fighters.bin')
    data = open(path, 'rb').read()
    magic, version, count = struct.unpack_from('<3I', data, 0)
    if magic != 0x48534846 or version != 1:
        sys.exit(f'{path}: not a fighters report (version {version})')
    at = 12
    ranges = []
    for _ in range(count):
        kind, offset, size = struct.unpack_from('<BII', data, at)
        ranges.append((kind, offset, size))
        at += 9
    per_player = sum(r[2] for r in ranges)
    frames = {}
    while at + 10 + 2 * per_player <= len(data):
        key, = struct.unpack_from('<Q', data, at)
        have = data[at + 8:at + 10]
        body = data[at + 10:at + 10 + 2 * per_player]
        frames[key] = (have, body[:per_player], body[per_player:])
        at += 10 + 2 * per_player
    return ranges, frames


def words(b):
    return [struct.unpack_from('>I', b, i)[0] for i in range(0, len(b) - len(b) % 4, 4)]


def as_float(v):
    return struct.unpack('>f', struct.pack('>I', v))[0]


def main():
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    for folder in sys.argv[1:]:
        print(f'== {folder}')
        summary = os.path.join(folder, 'summary.txt')
        if os.path.exists(summary):
            print(open(summary, encoding='utf-8', errors='replace').read().rstrip())
    (ra, fa), (rb, fb) = read(sys.argv[1]), read(sys.argv[2])
    if ra != rb:
        sys.exit('the two reports hash different ranges (two releases?): not comparable')
    common = sorted(set(fa) & set(fb))
    print(f'\nframes kept in both: {len(common)}', end='')
    if common:
        print(f' (epoch {common[0] >> 32}, frames {common[0] & 0xFFFFFFFF}-{common[-1] & 0xFFFFFFFF})')
    else:
        print()
        return
    for key in common:
        for player in (0, 1):
            a, b = fa[key][1 + player], fb[key][1 + player]
            if a == b:
                continue
            at = 0
            for kind, offset, size in ra:
                x, y = a[at:at + size], b[at:at + size]
                if x != y:
                    print(f'\nfirst difference: epoch {key >> 32} frame {key & 0xFFFFFFFF}, player {player + 1}, '
                          f'{KINDS.get(kind, kind)} +{offset:#x} ({size} bytes)')
                    for i, (u, v) in enumerate(zip(words(x), words(y))):
                        o = offset + 4 * i
                        mark = '  <==' if u != v else ''
                        name = NAMES.get((kind, o), '')
                        print(f'    +{o:#06x} {name:28s} {u:08x} ({as_float(u):.6g})  vs  {v:08x} ({as_float(v):.6g}){mark}')
                    differ = sum(1 for k in common if fa[k][1:] != fb[k][1:])
                    print(f'frames that differ from there: {differ} of {len(common)} kept')
                    return
                at += size
    print('\nthe kept frames are the same in both (the difference is in the game\'s own checks: the random draws; '
          'tools/online/rand_report.py)')


if __name__ == '__main__':
    main()
