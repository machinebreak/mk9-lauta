"""A costume package as a patch over the player's own game files (no game data in it): the built file, byte for byte,
as copies from its source packages' decompressed images (pkgio.Package.image: the file's first bytes up to the first
chunk's uncompressed offset, then each chunk decompressed at its offset) and the few bytes that are new.

Format (big-endian):
    'MK9C' u32 version 1
    u16 n sources; per source: u16 path length, path ('Asset/CHAR_Scorpion_C.xxx'), u64 file size, 32-byte SHA-256
    u16 target path length, path, u64 target size, 32-byte target SHA-256
    u32 zlib length, zlib of the operations:
        0x01 COPY  varint source, varint offset, varint length
        0x02 DATA  varint length, bytes
        0x00 END

    python makepatch.py <game Asset folder> <built file> <out patch> <source> [<source> ...]
    python makepatch.py --apply <game Asset folder> <patch> <out file>     (the check: rebuilds the file)
"""
import hashlib
import os
import struct
import sys
import zlib

from pkgio import Package

BLOCK = 64
MAGIC = b'MK9C'


def varint(n):
    out = bytearray()
    while True:
        b = n & 0x7F
        n >>= 7
        if n:
            out.append(b | 0x80)
        else:
            out.append(b)
            return bytes(out)


def read_varint(d, p):
    n = shift = 0
    while True:
        b = d[p]
        p += 1
        n |= (b & 0x7F) << shift
        shift += 7
        if not b & 0x80:
            return n, p


def source_image(path):
    return bytes(Package.load(path).image)


def make(asset, built, out, sources, log=print):
    target = open(built, 'rb').read()
    images = [source_image(os.path.join(asset, s)) for s in sources]
    index = {}
    for si, img in enumerate(images):
        for o in range(0, len(img) - BLOCK + 1, BLOCK):
            index.setdefault(hash(img[o:o + BLOCK]), (si, o))
    ops = bytearray()
    lit_start = 0
    t = 0
    copied = 0
    n = len(target)

    def flush_literal(end):
        if end > lit_start:
            ops.extend(b'\x02' + varint(end - lit_start) + target[lit_start:end])

    while t + BLOCK <= n:
        hit = index.get(hash(target[t:t + BLOCK]))
        if hit:
            si, o = hit
            img = images[si]
            if img[o:o + BLOCK] == target[t:t + BLOCK]:
                # backwards into the pending literal, then forwards as far as both agree
                while t > lit_start and o > 0 and img[o - 1] == target[t - 1]:
                    t -= 1
                    o -= 1
                length = BLOCK
                step = 4096
                while t + length < n and o + length < len(img):
                    k = min(step, n - t - length, len(img) - o - length)
                    if img[o + length:o + length + k] == target[t + length:t + length + k]:
                        length += k
                        continue
                    while k > 1:  # the first difference in this piece
                        h = k // 2
                        if img[o + length:o + length + h] == target[t + length:t + length + h]:
                            length += h
                            k -= h
                        else:
                            k = h
                    if img[o + length] == target[t + length]:
                        length += 1
                    break
                flush_literal(t)
                ops.extend(b'\x01' + varint(si) + varint(o) + varint(length))
                copied += length
                t += length
                lit_start = t
                continue
        t += 1
    t = n
    flush_literal(t)
    ops.extend(b'\x00')
    z = zlib.compress(bytes(ops), 9)
    head = bytearray(MAGIC + struct.pack('>I', 1) + struct.pack('>H', len(sources)))
    for s in sources:
        path = os.path.join(asset, s)
        head += struct.pack('>H', len('Asset/' + s)) + ('Asset/' + s).encode()
        head += struct.pack('>Q', os.path.getsize(path)) + hashlib.sha256(open(path, 'rb').read()).digest()
    name = 'Asset/' + os.path.basename(built)
    head += struct.pack('>H', len(name)) + name.encode() + struct.pack('>Q', n) + hashlib.sha256(target).digest()
    head += struct.pack('>I', len(z))
    open(out, 'wb').write(bytes(head) + z)
    log('%s: %d bytes, %.1f %% copied from %d sources; patch %d bytes (operations %d before zlib)' % (
        os.path.basename(built), n, 100.0 * copied / max(1, n), len(sources), len(head) + len(z), len(ops)))
    return len(head) + len(z)


def apply(asset, patch_path, out):
    d = open(patch_path, 'rb').read()
    assert d[:4] == MAGIC and struct.unpack_from('>I', d, 4)[0] == 1
    p = 8
    count = struct.unpack_from('>H', d, p)[0]
    p += 2
    images = []
    for _ in range(count):
        ln = struct.unpack_from('>H', d, p)[0]
        name = d[p + 2:p + 2 + ln].decode()
        p += 2 + ln
        size = struct.unpack_from('>Q', d, p)[0]
        sha = d[p + 8:p + 40]
        p += 40
        path = os.path.join(asset, name.split('/', 1)[1])
        raw = open(path, 'rb').read()
        assert len(raw) == size and hashlib.sha256(raw).digest() == sha, 'not the disc\'s ' + name
        images.append(source_image(path))
    ln = struct.unpack_from('>H', d, p)[0]
    p += 2 + ln
    size = struct.unpack_from('>Q', d, p)[0]
    sha = d[p + 8:p + 40]
    p += 40
    zl = struct.unpack_from('>I', d, p)[0]
    ops = zlib.decompress(d[p + 4:p + 4 + zl])
    out_data = bytearray()
    q = 0
    while True:
        op = ops[q]
        q += 1
        if op == 0:
            break
        if op == 1:
            si, q = read_varint(ops, q)
            o, q = read_varint(ops, q)
            length, q = read_varint(ops, q)
            out_data += images[si][o:o + length]
        elif op == 2:
            length, q = read_varint(ops, q)
            out_data += ops[q:q + length]
            q += length
        else:
            raise ValueError('bad operation %d' % op)
    assert len(out_data) == size and hashlib.sha256(out_data).digest() == sha, 'the rebuilt file differs'
    open(out, 'wb').write(out_data)
    return len(out_data)


if __name__ == '__main__':
    if sys.argv[1] == '--apply':
        print(apply(sys.argv[2], sys.argv[3], sys.argv[4]), 'bytes rebuilt, checked')
    else:
        make(sys.argv[1], sys.argv[2], sys.argv[3], sys.argv[4:])
