"""MK9 Xbox 360 packages (UE3 472/46, big-endian, LZO) read whole and written back.

The summary (stored as is) holds the compression flags and the chunk table; every byte after it, the name, import
and export tables included, is in the chunks. A package is written back with the same chunks (the same uncompressed
offsets and sizes) and each block stored as an LZO1X stream of literals (valid for any LZO1X decompressor; about the
uncompressed size), so nothing in the uncompressed image moves.

    from pkgio import Package
    p = Package.load(path); p.rename_names({'CHAR_Scorpion_C': 'CHAR_Scorpion_D'}); p.save(out)
"""
import struct

from mk9pkg import lzo1x_decompress

TAG = 0x9E2A83C1


def lzo1x_literals(data):
    """An LZO1X stream holding data as one literal run, then the end marker (an M4 match of distance 0)."""
    n = len(data)
    out = bytearray()
    if n == 0:
        return bytes([0x11, 0, 0])
    if n <= 238:
        out.append(17 + n)  # the stream's first byte: 17 + a literal run
    else:
        ext = n - 18  # instruction 0: a literal run of 15 + extension + 3
        out.append(0)
        while ext > 255:
            out.append(0)
            ext -= 255
        out.append(ext)
    out += data
    out += bytes([0x11, 0, 0])
    return bytes(out)


class Package:
    @classmethod
    def load(cls, path):
        self = cls()
        raw = open(path, 'rb').read()
        assert struct.unpack_from('>I', raw, 0)[0] == TAG, 'not a big-endian MK9 package'
        e = '>'
        self.e = e
        o = 20
        n = struct.unpack_from(e + 'i', raw, o)[0]
        o += 4 + n
        self.flags_at = o
        (self.name_count, self.name_off, self.export_count, self.export_off, self.import_count, self.import_off,
         self.depends_off) = struct.unpack_from(e + '7i', raw, o + 4)
        self.chunks_at = None
        for p in range(o + 32, 0x400):
            cf, cnt = struct.unpack_from(e + '2i', raw, p)
            if cf == 2 and 0 < cnt < 4096:
                uo, us, co, cs = struct.unpack_from(e + '4i', raw, p + 8)
                if 0 < us <= 0x200000 and 0 < co < len(raw) and struct.unpack_from(e + 'I', raw, co)[0] == TAG:
                    self.chunks_at = p + 8
                    self.chunk_count = cnt
                    break
        assert self.chunks_at, 'no LZO chunk table'
        self.chunks = [list(struct.unpack_from(e + '4i', raw, self.chunks_at + 16 * i)) for i in range(self.chunk_count)]
        self.summary = bytearray(raw[:self.chunks[0][2]])  # up to the first chunk's data
        size = max(uo + us for uo, us, _, _ in self.chunks)
        img = bytearray(size)
        first = self.chunks[0][0]
        img[:first] = raw[:first]
        self.block_size = None
        for uo, us, co, cs in self.chunks:
            tag, block, csize, usize = struct.unpack_from(e + '4i', raw, co)
            self.block_size = block
            nblocks = (usize + block - 1) // block
            sizes = [struct.unpack_from(e + '2i', raw, co + 16 + 8 * i) for i in range(nblocks)]
            p = co + 16 + 8 * nblocks
            out = bytearray()
            for bc, bu in sizes:
                out += lzo1x_decompress(raw[p:p + bc], bu)
                p += bc
            assert len(out) >= us, (len(out), us)
            img[uo:uo + us] = out[:us]
        self.image = img
        return self

    # the name table (in the image)
    def names(self):
        out, p = [], self.name_off
        for _ in range(self.name_count):
            ln = struct.unpack_from('>i', self.image, p)[0]
            if ln < 0:
                out.append((p, self.image[p + 4:p + 4 - 2 * ln].decode('utf-16-be').rstrip('\0'), True))
                p += 4 - 2 * ln + 8
            else:
                out.append((p, self.image[p + 4:p + 4 + ln - 1].decode('latin-1'), False))
                p += 4 + ln + 8
        return out

    def rename_names(self, mapping):
        """Names equal to a key, or starting with it and '-' or '.', get the value instead (same length)."""
        changed = []
        for p, s, wide in self.names():
            for old, new in mapping.items():
                assert len(old) == len(new), (old, new)
                if not wide and (s == old or s.startswith(old + '-') or s.startswith(old + '.')):
                    t = new + s[len(old):]
                    self.image[p + 4:p + 4 + len(t)] = t.encode('latin-1')
                    changed.append((s, t))
        return changed

    def replace_bytes(self, mapping):
        """Every occurrence of each key (bytes, case-sensitive) in the summary and the image gets the value (same
        length). The count of each."""
        counts = {}
        for old, new in mapping.items():
            assert len(old) == len(new), (old, new)
            n = 0
            for buf in (self.summary, self.image):
                i = buf.find(old)
                while i >= 0:
                    buf[i:i + len(old)] = new
                    n += 1
                    i = buf.find(old, i + len(new))
            counts[old] = n
        return counts

    def save(self, path):
        e = self.e
        block = self.block_size or 0x20000
        body = bytearray()
        base = len(self.summary)
        new_chunks = []
        for uo, us, _, _ in self.chunks:
            data = bytes(self.image[uo:uo + us])
            blocks = [data[i:i + block] for i in range(0, len(data), block)]
            packed = [lzo1x_literals(b) for b in blocks]
            head = struct.pack(e + 'I3i', TAG, block, sum(len(x) for x in packed), len(data))
            head += b''.join(struct.pack(e + '2i', len(x), len(b)) for x, b in zip(packed, blocks))
            chunk = head + b''.join(packed)
            new_chunks.append((uo, us, base + len(body), len(chunk)))
            body += chunk
        summary = bytearray(self.summary)
        for i, c in enumerate(new_chunks):
            struct.pack_into(e + '4i', summary, self.chunks_at + 16 * i, *c)
        # the image's own copy of the summary (the part the first chunk does not cover) is the file's
        with open(path, 'wb') as f:
            f.write(summary)
            f.write(body)
        return len(summary) + len(body)


class FullPackage:
    """A fully compressed package (the whole file a run of chunks, Startup.xxx and the script packages): its
    uncompressed bytes, written back as a run of chunks of one block each (literal LZO)."""

    @classmethod
    def load(cls, path):
        self = cls()
        raw = open(path, 'rb').read()
        e = '>'
        out = bytearray()
        p = 0
        self.block_size = 0x20000
        self.layout = []  # each chunk's blocks' uncompressed sizes
        while p + 16 <= len(raw) and struct.unpack_from(e + 'I', raw, p)[0] == TAG:
            tag, block, csize, usize = struct.unpack_from(e + '4i', raw, p)
            self.block_size = block
            nb = (usize + block - 1) // block
            sizes = [struct.unpack_from(e + '2i', raw, p + 16 + 8 * i) for i in range(nb)]
            self.layout.append([bu for _, bu in sizes])
            q = p + 16 + 8 * nb
            for bc, bu in sizes:
                out += lzo1x_decompress(raw[q:q + bc], bu)
                q += bc
            p = q
        self.image = out
        return self

    def save(self, path):
        e = '>'
        block = self.block_size
        body = bytearray()
        at = 0
        for blocks in self.layout:  # the original's chunks and blocks (the loader reads the header from the first)
            parts = []
            for bu in blocks:
                data = bytes(self.image[at:at + bu])
                at += bu
                parts.append((lzo1x_literals(data), len(data)))
            body += struct.pack(e + 'I3i', TAG, block, sum(len(x) for x, _ in parts), sum(n for _, n in parts))
            for x, n in parts:
                body += struct.pack(e + '2i', len(x), n)
            for x, _ in parts:
                body += x
        assert at == len(self.image), (at, len(self.image))
        with open(path, 'wb') as f:
            f.write(body)
        return len(body)
