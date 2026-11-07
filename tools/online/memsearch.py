#!/usr/bin/env python3
"""Searches the game's memory dumps (logs/memdump_<frame>.bin, written by mk9_memdump_at) like a cheat finder.

    python tools/online/memsearch.py find <dump> --u32 75 | --f32 1000 | --u16 3 [--max 50]
        addresses holding a value (big-endian, as the game stores it)
    python tools/online/memsearch.py track <dumpA> <dumpB> ... --u32 75 70 ... | --f32 ...
        addresses holding the first value in the first dump, the second in the second, ... (a timer, a health bar)
    python tools/online/memsearch.py changed <dumpA> <dumpB> [--pages]
        how much differs between two dumps: per region, or per 4 KB page with --pages
    python tools/online/memsearch.py read <dump> <address> [--count 16]
        big-endian words at an address

The dump format (mk9_capture.cpp DumpMemory): "MK9D", u32 version 1, u32 region count; per region u32 start, u32 size,
then the bytes in the guest's byte order. Searches step 4 bytes (u32 / f32) or 2 bytes (u16).
"""
import argparse
import struct
import sys

import numpy as np


def load(path):
    data = open(path, 'rb').read()
    magic, version, count = struct.unpack_from('<III', data, 0)
    if magic != 0x44394B4D or version != 1:
        sys.exit(f'{path}: not a memory dump')
    off, regions = 12, []
    for _ in range(count):
        start, size = struct.unpack_from('<II', data, off)
        off += 8
        regions.append((start, np.frombuffer(data, dtype=np.uint8, count=size, offset=off)))
        off += size
    return regions


def words(buf, kind):
    n = len(buf) // (2 if kind == 'u16' else 4) * (2 if kind == 'u16' else 4)
    if kind == 'u16':
        return buf[:n].view('>u2')
    if kind == 'f32':
        return buf[:n].view('>f4')
    return buf[:n].view('>u4')


def value_args(a):
    if a.u32 is not None:
        return 'u32', [int(v, 0) for v in a.u32]
    if a.f32 is not None:
        return 'f32', [float(v) for v in a.f32]
    if a.u16 is not None:
        return 'u16', [int(v, 0) for v in a.u16]
    sys.exit('give --u32, --f32 or --u16')


def matches(arr, kind, value):
    if kind == 'f32':
        return np.isclose(arr, np.float32(value), rtol=0, atol=1e-3)
    return arr == value


def cmd_find(a):
    kind, values = value_args(a)
    step = 2 if kind == 'u16' else 4
    found = 0
    for start, buf in load(a.dump):
        hits = np.nonzero(matches(words(buf, kind), kind, values[0]))[0]
        for i in hits[: max(0, a.max - found)]:
            print(f'{start + int(i) * step:08X}')
        found += len(hits)
    print(f'{found} matches', file=sys.stderr)


def cmd_track(a):
    kind, values = value_args(a)
    if len(values) != len(a.dumps):
        sys.exit('one value per dump')
    step = 2 if kind == 'u16' else 4
    dumps = [dict(load(p)) for p in a.dumps]
    hits_total = 0
    for start, buf in dumps[0].items():
        mask = matches(words(buf, kind), kind, values[0])
        for d, v in zip(dumps[1:], values[1:]):
            other = d.get(start)
            if other is None or len(other) != len(buf):
                mask[:] = False
                break
            mask &= matches(words(other, kind), kind, v)
        for i in np.nonzero(mask)[0]:
            if hits_total < a.max:
                print(f'{start + int(i) * step:08X}')
            hits_total += 1
    print(f'{hits_total} addresses follow the values', file=sys.stderr)


def cmd_changed(a):
    da, db = dict(load(a.dumpA)), dict(load(a.dumpB))
    total = changed_pages = 0
    for start, buf in sorted(da.items()):
        other = db.get(start)
        if other is None or len(other) != len(buf):
            print(f'{start:08X} size {len(buf)}: not in both dumps alike')
            continue
        diff = buf != other
        pages = np.nonzero(diff.reshape(-1, 4096).any(axis=1))[0] if len(buf) % 4096 == 0 else []
        total += int(diff.sum())
        changed_pages += len(pages)
        if a.pages:
            for p in pages:
                print(f'{start + int(p) * 4096:08X} {int(diff[p * 4096:(p + 1) * 4096].sum())}')
        elif len(pages):
            print(f'{start:08X}-{start + len(buf):08X}: {len(pages)} of {len(buf) // 4096} pages differ, '
                  f'{int(diff.sum())} bytes')
    print(f'{total} bytes in {changed_pages} pages differ', file=sys.stderr)


def cmd_read(a):
    address = int(a.address, 16)
    for start, buf in load(a.dump):
        if start <= address < start + len(buf):
            off = address - start
            w = buf[off:off + a.count * 4].view('>u4')
            f = buf[off:off + a.count * 4].view('>f4')
            for i in range(len(w)):
                print(f'{address + i * 4:08X}  {int(w[i]):08X}  {int(w[i]):11d}  {float(f[i]):.4g}')
            return
    print('address not in the dump')


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest='cmd', required=True)
    for name in ('find', 'track'):
        p = sub.add_parser(name)
        if name == 'find':
            p.add_argument('dump')
        else:
            p.add_argument('dumps', nargs='+')
        g = p.add_mutually_exclusive_group()
        g.add_argument('--u32', nargs='+')
        g.add_argument('--f32', nargs='+')
        g.add_argument('--u16', nargs='+')
        p.add_argument('--max', type=int, default=50)
    p = sub.add_parser('changed')
    p.add_argument('dumpA')
    p.add_argument('dumpB')
    p.add_argument('--pages', action='store_true')
    p = sub.add_parser('read')
    p.add_argument('dump')
    p.add_argument('address')
    p.add_argument('--count', type=int, default=16)
    a = ap.parse_args()
    {'find': cmd_find, 'track': cmd_track, 'changed': cmd_changed, 'read': cmd_read}[a.cmd](a)


if __name__ == '__main__':
    main()
