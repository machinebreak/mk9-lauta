#!/usr/bin/env python3
"""Adds to a pipeline list (pipeline_list.bin, written by the renderer next to the executable: the GPU pipelines it
built, which the next start builds ahead) the records of other lists that it does not have yet: the lists of test
copies of the build (a pipeline sweep, pair tests) end up in the one the release ships (out/pc/pipeline_list.bin).

    python tools/release/merge_pipeline_lists.py out/pc/pipeline_list.bin out/pc_sweep/pipeline_list.bin [...]

The file: a 16-byte header (magic, version, key size, streams), then records of <u32 size><size bytes>. Lists with
another header (another renderer version) are left out. The first file is rewritten in place (a .bak beside it).
"""
import shutil
import struct
import sys

HEADER = 16


def records(path):
    data = open(path, 'rb').read()
    if len(data) < HEADER:
        return None, []
    out, pos = [], HEADER
    while pos + 4 <= len(data):
        (size,) = struct.unpack_from('<I', data, pos)
        if pos + 4 + size > len(data):
            break  # cut short (the game closed while writing)
        out.append(data[pos + 4:pos + 4 + size])
        pos += 4 + size
    return data[:HEADER], out


def main():
    if len(sys.argv) < 3:
        sys.exit(__doc__)
    target = sys.argv[1]
    header, kept = records(target)
    if header is None:
        sys.exit(f'{target}: not a pipeline list')
    seen = set(kept)
    added = 0
    for other in sys.argv[2:]:
        h, recs = records(other)
        if h != header:
            print(f'{other}: another list version, left out')
            continue
        new = [r for r in recs if r not in seen]
        for r in new:
            seen.add(r)
            kept.append(r)
        print(f'{other}: {len(recs)} pipelines, {len(new)} new')
        added += len(new)
    if not added:
        print('nothing to add')
        return
    shutil.copyfile(target, target + '.bak')
    with open(target, 'wb') as f:
        f.write(header)
        for r in kept:
            f.write(struct.pack('<I', len(r)))
            f.write(r)
    print(f'{target}: {len(kept)} pipelines ({added} added)')


if __name__ == '__main__':
    main()
