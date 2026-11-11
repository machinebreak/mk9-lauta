"""Two machines' mk9_fighter_dump files compared (fight_state.h DumpFighters), to choose what of the fighters' state
goes into the sync hash: per 4-byte word of each player's block of the match's info (kind 0) and of their fighter's
object (kind 1) and its actor (kind 2), whether it was the same on both machines at every frame both dumped, and whether it changed during
the fight. A word that ever differs (a pointer into heaps laid out apart, something another thread writes) stays out;
one the same on both that changes is the fight's state; one that never changes goes in only when it cannot be a
pointer. Several sessions (pairs of files) together: a word must be the same in all of them. Dumps with the sync
hash's moment too (kinds 3-5, HashFighters, 2026-11-11) are compared there between the machines, and a word that
moved between the boundary's start and that moment on one machine (another thread's write, the boundary's own work:
the render thread wrote two such floats in a fight's first frames) stays out, as does one never seen at that moment.

    python tools/online/dev/fighter_diff.py <host dump> <guest dump> [<host dump 2> <guest dump 2> ...]
        [--out ranges.txt]  (the ranges to hash: "<kind> <offset hex> <size hex>"; a .inc name: the C++ table
                             app/src/online/fight_hash_ranges.inc that fight_state.cpp hashes)
        [--constants]       (also the words that never changed and cannot be pointers: by default only the ones that
                             changed, the fight's state; a constant adds no detection and a word that happened to stay
                             put in these fights may not elsewhere)
"""
import os
import struct
import sys

args = [a for a in sys.argv[1:]]
out_path = None
constants = '--constants' in args
if constants:
    args.remove('--constants')
within = None  # --within <table.inc>: only the words of an earlier table (its sessions' exclusions kept)
if '--within' in args:
    i = args.index('--within')
    within = set()
    import re
    for m in re.finditer(r'\{(\d), 0x([0-9A-F]+), 0x([0-9A-F]+)\}', open(args[i + 1]).read()):
        k, o, z = int(m.group(1)), int(m.group(2), 16), int(m.group(3), 16)
        within.update((k, w) for w in range(o, o + z, 4))
    del args[i:i + 2]
LATE = 120  # a change counts as the fight's own when it comes this many frames after the fight's first
if '--out' in args:
    i = args.index('--out')
    out_path = args[i + 1]
    del args[i:i + 2]
if len(args) < 2 or len(args) % 2:
    sys.exit(__doc__)

MAGIC = 0x504D4446
KINDS = {0: 'match info block', 1: 'fighter object', 2: 'fighter actor'}


def read(path):
    """{(frame, player, kind): bytes}, the latest record of each (a frame run again by a rollback: its last run); the
    last 60 frames left out (a rollback may not have come back for them yet when the game was closed)."""
    records = {}
    with open(path, 'rb') as f:
        data = f.read()
    at = 0
    while at + 24 <= len(data):
        magic, frame, player, kind, address, size = struct.unpack_from('<6I', data, at)
        if magic != MAGIC or at + 24 + size > len(data):
            break
        records[(frame, player, kind)] = data[at + 24:at + 24 + size]
        at += 24 + size
    if records:
        last = max(k[0] for k in records)
        records = {k: v for k, v in records.items() if k[0] <= last - 60}
    return records


def looks_like_pointer(v):
    return 0x30000000 <= v < 0xA0000000 or v >= 0xC0000000


# Words seen to differ between the machines at the hash's moment in a fight's first frames though every sampled dump
# had them alike (2026-11-11, A3's reports): ~7.0-7.4, the seconds the fight took to load (7.20164 against 7.01414 on
# the two machines; 7.3794 against 7.37928), each written once as a load ends, on the real clock and whenever that is
# (the boundary's flush can end one): not the fight's state.
EXCLUDED = {(0, 0x118): 'match info +0x118: a load time (seconds)', (1, 0x240): 'fighter +0x240: a load time (seconds)',
            # the rollback SyncTest with the fighters compared (fight_state.h SyncTestFighters, 2026-11-11): the only
            # words a frame run again brings back otherwise, render feedback (times of the last drawing, a count):
            # a frame run again is not drawn, and the two machines run different frames again
            (2, 0xC8): 'actor +0xc8: a last-drawn time (187.152 first, 187.037 run again)',
            (2, 0x13C): 'actor +0x13c: a last-drawn time', (2, 0x68): 'actor +0x68: a time',
            (2, 0x1B4): 'actor +0x1b4: a count (15 first, 14 run again)'}


# Whole ranges out, by what they hold: each player's match info has, from +0x24 to +0x238, an array of 12-byte entries
# {vtable 0x82003cb0, a time ~7.01 s, a handle 0x1000xxxx} whose times came out 0.1875 s apart on the two machines in
# a fight's first frame (+0x118, +0x148: 2026-11-11), and whose entries move during the fight.
EXCLUDED_RANGES = [(0, 0x24, 0x238, 'match info +0x24-0x238: timed entries (vtable 0x82003cb0, a time, a handle)')]

# The objects that say when a load of theirs ended: {vtable, the time (seconds, ~2.0 or ~7.0-7.4), a handle 0x1000xxxx,
# ...}. The word after one of these vtables, wherever it is in any block, stays out: the same 7.01256 against 7.20006
# came at the match info's +0x260 (2026-11-11, the regression's third run), the time of one of the 0x2C-byte entries
# {vtable 0x820f3b60, the time, a handle, ...} from +0x25c on, after +0x118 and +0x148 before it. When a load ends is up
# to each machine's disk and render thread, not to the fight.
TIMED_VTABLES = {0x82003CB0, 0x820F3B60, 0x820124B8, 0x8200FF28, 0x82010028}
after_timed = {0: set(), 1: set(), 2: set()}  # per kind: the word offsets right after one of them, in any record


def excluded(kind, offset):
    return ((kind, offset) in EXCLUDED or offset in after_timed[kind] or
            any(k == kind and lo <= offset < hi for k, lo, hi, _ in EXCLUDED_RANGES))


def looks_like_handle(v):
    # handles numbered from a counter (0x1000xxxx: in a session's second fight the guest's ran 2 ahead of the host's,
    # 2026-11-11), never a float of the fight (2.5e-29 to 6e-29): out even when both machines had them alike
    return 0x10000000 <= v < 0x11000000


def unpack(b, n=None):
    n = len(b) // 4 if n is None else n
    return struct.unpack_from(f'>{n}I', b)


# per kind: per word offset, [samples, differed, changed, pointer-like, handle-like, moved in the boundary,
# seen at the hash's moment, changed after the fight's first LATE frames]
stats = {0: {}, 1: {}, 2: {}}
sizes = {0: 0, 1: 0, 2: 0}
frames_compared = 0
hash_moment = False  # some session has the hash's moment (kinds 3-5)
for pair in range(0, len(args), 2):
    a, b = read(args[pair]), read(args[pair + 1])
    common = sorted(k for k in set(a) & set(b) if k[2] < 3)
    print(f'{args[pair]} / {args[pair + 1]}: {len(a)} and {len(b)} records, {len(common)} in both')
    frames_compared += len({k[0] for k in common})
    previous, last_frame, segment_start = {}, {}, {}
    for key in common:
        frame, player, kind = key
        s = stats[kind]
        x, y = a[key], b[key]
        n = min(len(x), len(y)) // 4
        sizes[kind] = max(sizes[kind], n * 4)
        wx, wy = unpack(x, n), unpack(y, n)
        before = previous.get((player, kind))
        last = last_frame.get((player, kind))
        if last is None or frame - last > 300:  # a new fight (the dumps only cover GS_PLAY)
            segment_start[(player, kind)] = frame
            before = None
        last_frame[(player, kind)] = frame
        late = frame - segment_start[(player, kind)] > LATE
        for i in range(n):
            e = s.setdefault(i * 4, [0, 0, 0, 0, 0, 0, 0, 0])
            e[0] += 1
            if before is not None and i < len(before) and before[i] != wx[i]:
                e[2] += 1
                if late:
                    e[7] += 1
            if looks_like_pointer(wx[i]) or looks_like_pointer(wy[i]):
                e[3] += 1
            if looks_like_handle(wx[i]) or looks_like_handle(wy[i]):
                e[4] += 1
            if (wx[i] in TIMED_VTABLES or wy[i] in TIMED_VTABLES) and i + 1 < n:
                after_timed[kind].add((i + 1) * 4)
        previous[(player, kind)] = wx
        # between the machines: at the hash's moment when both have it, else at the boundary's start
        hk = (frame, player, kind + 3)
        if hk in a and hk in b:
            hash_moment = True
            hx, hy = unpack(a[hk]), unpack(b[hk])
            for i in range(min(len(hx), len(hy))):
                e = s.setdefault(i * 4, [0, 0, 0, 0, 0, 0, 0, 0])
                e[6] += 1
                if hx[i] != hy[i]:
                    e[1] += 1
            # on each machine: what moved between the boundary's start and the hash's moment
            for side, h in ((wx, hx), (wy, hy)):
                for i in range(min(len(side), len(h))):
                    if side[i] != h[i]:
                        s[i * 4][5] += 1
        else:
            for i in range(n):
                if wx[i] != wy[i]:
                    s[i * 4][1] += 1

ranges = []
for kind in (0, 1, 2):
    s = stats[kind]
    if not s:
        continue
    same = [o for o, e in s.items() if e[1] == 0 and e[4] == 0 and e[5] == 0 and (e[6] > 0 or not hash_moment) and
            not excluded(kind, o)]
    timed_out = [o for o in after_timed[kind] if o in s and s[o][1] == 0 and s[o][4] == 0]
    if timed_out:
        print(f'  out as the time after a timed vtable: {len(timed_out)} words alike on both')
    moved = [o for o, e in s.items() if e[5] > 0]
    if moved:
        print(f"  moved between the boundary's start and the hash's moment: {len(moved)} words: " +
              ', '.join(f'+{o:#x} ({s[o][5]})' for o in sorted(moved, key=lambda o: -s[o][5])[:12]))
    if within is not None:
        same = [o for o in same if (kind, o) in within]
    # the fight's state changes during the fight: a word set once as the fight starts (a load's time, a spawn's,
    # a count of what loading made) and never again is not (2026-11-11: those were the false positives)
    live = [o for o in same if s[o][7] > 0]
    constant_safe = [o for o in same if s[o][2] == 0 and s[o][3] == 0]
    differ = [o for o, e in s.items() if e[1] > 0]
    print(f'{KINDS[kind]} ({sizes[kind]:#x} bytes): {len(s)} words, the same on both always: {len(same)} '
          f'({len(live)} of them change, {len(constant_safe)} constant and not pointer-like); differ: {len(differ)}')
    if differ:
        worst = sorted(differ, key=lambda o: -s[o][1])[:12]
        print('  differ most: ' + ', '.join(f'+{o:#x} ({s[o][1]}/{s[o][0]})' for o in worst))
    keep = sorted(set(live) | (set(constant_safe) if constants else set()))
    start = prev = None
    for o in keep:
        if start is None:
            start = prev = o
        elif o == prev + 4:
            prev = o
        else:
            ranges.append((kind, start, prev + 4 - start))
            start = prev = o
    if start is not None:
        ranges.append((kind, start, prev + 4 - start))
    live_ranges = [r for r in ranges if r[0] == kind]
    print(f'  {len(live_ranges)} ranges to hash, {sum(r[2] for r in live_ranges)} bytes')
    for o in sorted(live, key=lambda o: -s[o][2])[:16]:
        print(f'  changes most: +{o:#x} ({s[o][2]} of {s[o][0]} samples)')
print(f'frames compared: {frames_compared}')
if out_path:
    with open(out_path, 'w') as f:
        if out_path.endswith('.inc'):
            # the table fight_state.cpp includes (kHashed)
            f.write('// Generated by tools/online/dev/fighter_diff.py from ' +
                    ', '.join(os.path.basename(a) for a in args) +
                    f': {len(ranges)} ranges, {sum(r[2] for r in ranges)} bytes a player.\n')
            items = [f'{{{k}, 0x{o:X}, 0x{z:X}}}' for k, o, z in ranges]
            for i in range(0, len(items), 6):
                f.write('    ' + ', '.join(items[i:i + 6]) + ',\n')
        else:
            for kind, offset, size in ranges:
                f.write(f'{kind} {offset:x} {size:x}\n')
    print(f'wrote {len(ranges)} ranges to {out_path}')
