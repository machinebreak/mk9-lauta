#!/usr/bin/env python3
"""Checks an extracted MK9 Komplete Edition disc (assets/game_root) against mk9-nx's app/mk9_game_files.txt.

Every listed file must exist with the listed size and SHA-256. Files on disk that the list does not name are reported
too (the dashboard update in $SystemUpdate is expected and ignored).

    python tools/online/verify_game_files.py [game_root] [list]
"""
import hashlib
import os
import sys

REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
GAME = sys.argv[1] if len(sys.argv) > 1 else os.path.join(REPO, 'assets', 'game_root')
LIST = sys.argv[2] if len(sys.argv) > 2 else os.path.join(REPO, 'app', 'mk9_game_files.txt')


def sha256(path):
    h = hashlib.sha256()
    with open(path, 'rb') as f:
        while chunk := f.read(8 << 20):
            h.update(chunk)
    return h.hexdigest()


def main():
    expected = {}
    with open(LIST, encoding='utf-8') as f:
        for line in f:
            if not line.strip() or line.startswith('#'):
                continue
            size, digest, rel = line.rstrip('\n').split(' ', 2)
            expected[rel.replace('\\', '/').lower()] = (int(size), digest, rel)

    on_disk = {}
    for dirpath, _, files in os.walk(GAME):
        for name in files:
            full = os.path.join(dirpath, name)
            rel = os.path.relpath(full, GAME).replace('\\', '/')
            on_disk[rel.lower()] = full

    missing, bad_size, bad_hash = [], [], []
    total = len(expected)
    for i, (key, (size, digest, rel)) in enumerate(sorted(expected.items()), 1):
        full = on_disk.get(key)
        if not full:
            missing.append(rel)
        elif os.path.getsize(full) != size:
            bad_size.append(f'{rel} ({os.path.getsize(full)} != {size})')
        elif sha256(full) != digest:
            bad_hash.append(rel)
        if i % 200 == 0:
            print(f'  ... {i}/{total}', flush=True)

    extra = sorted(k for k in on_disk if k not in expected and not k.startswith('$systemupdate/'))
    print(f'listed {total}, on disk {len(on_disk)}; missing {len(missing)}, wrong size {len(bad_size)}, '
          f'wrong SHA-256 {len(bad_hash)}, not listed {len(extra)}')
    for title, items in (('missing', missing), ('wrong size', bad_size), ('wrong SHA-256', bad_hash), ('not listed', extra)):
        for item in items[:20]:
            print(f'  {title}: {item}')
    sys.exit(0 if not (missing or bad_size or bad_hash) else 1)


if __name__ == '__main__':
    main()
