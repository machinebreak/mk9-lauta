"""Read-only: an MK9 cooked package (.xxx) of the Xbox 360 (UE3 472/46, big-endian, LZO) or the PS Vita (UE3 501/48,
little-endian, zlib): its summary, names, imports and exports (classes counted).

    python mk9pkg.py <package.xxx> [--names] [--exports] [--grep TEXT]

Layout found 2026-11-19 (both): tag, version (low 16 engine, high 16 licensee), header size, 'MK  ', a 62, the folder
name, package flags, names (count, offset), exports (count, offset), imports (count, offset), depends offset, then
NRS's fields; the compression flags (Xbox 360: 2 LZO; Vita: 1 zlib) and the compressed chunks follow the cooker
version. Each chunk: tag, block size, sizes, then (compressed, uncompressed) per block and the blocks.
"""
import struct
import sys
import zlib

TAG = 0x9E2A83C1


def lzo1x_decompress(src, out_len=None):
    """LZO1X decompression (pure Python; minilzo's lzo1x_decompress state machine)."""
    out = bytearray()
    ip = 0

    def copy_match(m_pos, n):
        if m_pos + n <= len(out):
            out.extend(out[m_pos:m_pos + n])
        else:
            for _ in range(n):
                out.append(out[m_pos])
                m_pos += 1

    def run(base):
        nonlocal ip
        t = 0
        while src[ip] == 0:
            t += 255
            ip += 1
        t += base + src[ip]
        ip += 1
        return t

    if src[0] > 17:
        t = src[0] - 17
        ip = 1
        if t < 4:
            state = 'match_next'
        else:
            out.extend(src[ip:ip + t])
            ip += t
            state = 'first_literal_run'
    else:
        t = 0
        state = 'loop'
    while True:
        if state == 'loop':
            t = src[ip]
            ip += 1
            if t >= 16:
                state = 'match'
                continue
            if t == 0:
                t = run(15)
            out.extend(src[ip:ip + t + 3])
            ip += t + 3
            state = 'first_literal_run'
            continue
        if state == 'first_literal_run':
            t = src[ip]
            ip += 1
            if t >= 16:
                state = 'match'
                continue
            m_pos = len(out) - (1 + 0x0800) - (t >> 2) - (src[ip] << 2)
            ip += 1
            copy_match(m_pos, 3)
            state = 'match_done'
            continue
        if state == 'match':
            if t >= 64:
                m_pos = len(out) - 1 - ((t >> 2) & 7) - (src[ip] << 3)
                ip += 1
                copy_match(m_pos, (t >> 5) + 1)
            elif t >= 32:
                t &= 31
                if t == 0:
                    t = run(31)
                m_pos = len(out) - 1 - ((src[ip] >> 2) + (src[ip + 1] << 6))
                ip += 2
                copy_match(m_pos, t + 2)
            elif t >= 16:
                m_pos = len(out) - ((t & 8) << 11)
                t &= 7
                if t == 0:
                    t = run(7)
                m_pos -= (src[ip] >> 2) + (src[ip + 1] << 6)
                ip += 2
                if m_pos == len(out):
                    break
                m_pos -= 0x4000
                copy_match(m_pos, t + 2)
            else:
                m_pos = len(out) - 1 - (t >> 2) - (src[ip] << 2)
                ip += 1
                copy_match(m_pos, 2)
            state = 'match_done'
            continue
        if state == 'match_done':
            t = src[ip - 2] & 3
            state = 'loop' if t == 0 else 'match_next'
            continue
        if state == 'match_next':
            out.extend(src[ip:ip + t])
            ip += t
            t = src[ip]
            ip += 1
            state = 'match'
            continue
    return bytes(out if out_len is None else out[:out_len])


class Pkg:
    def __init__(self, path, need=None):
        raw = open(path, 'rb').read()
        self.e = '<' if struct.unpack_from('<I', raw, 0)[0] == TAG else '>'
        e = self.e
        assert struct.unpack_from(e + 'I', raw, 0)[0] == TAG
        ver = struct.unpack_from(e + 'I', raw, 4)[0]
        self.version, self.licensee = ver & 0xFFFF, ver >> 16
        self.header_size = struct.unpack_from(e + 'i', raw, 8)[0]
        o = 16 + 4  # 'MK  ', the 62
        n = struct.unpack_from(e + 'i', raw, o)[0]
        o += 4 + n
        self.flags = struct.unpack_from(e + 'I', raw, o)[0]
        (self.name_count, self.name_off, self.export_count, self.export_off, self.import_count, self.import_off,
         self.depends_off) = struct.unpack_from(e + '7i', raw, o + 4)
        # the compression flags: the first of (1|2|4) followed by a plausible chunk count and a chunk table whose
        # first uncompressed offset is the header's end (searched: NRS's fields differ by platform)
        self.compression, chunks_at = 0, None
        for p in range(o + 32, min(len(raw), 0x400), 4 if False else 1):
            cf, cnt = struct.unpack_from(e + '2i', raw, p)
            if cf in (1, 2, 4) and 0 < cnt < 4096:
                uo, us, co, cs = struct.unpack_from(e + '4i', raw, p + 8)
                if 0 < us <= 0x200000 and 0 < co < len(raw) and 0 < cs <= len(raw) and uo > 0:
                    if struct.unpack_from(e + 'I', raw, co)[0] == TAG:
                        self.compression, chunks_at = cf, p + 8
                        break
        self.chunks = []
        if chunks_at is not None:
            cnt = struct.unpack_from(e + 'i', raw, chunks_at - 4)[0]
            for i in range(cnt):
                self.chunks.append(struct.unpack_from(e + '4i', raw, chunks_at + 16 * i))
        # the uncompressed image: the summary as is, then the chunks needed
        size = max([uo + us for uo, us, _, _ in self.chunks] + [len(raw)])
        img = bytearray(size)
        first = self.chunks[0][0] if self.chunks else len(raw)
        img[:first] = raw[:first]
        for uo, us, co, cs in self.chunks:
            if need is not None and uo > need:
                continue
            img[uo:uo + us] = self.inflate(raw, co, us)
        if not self.chunks:
            img[:len(raw)] = raw
        self.img = bytes(img)

    def inflate(self, raw, co, us):
        e = self.e
        tag, block, csize, usize = struct.unpack_from(e + '4i', raw, co)
        nblocks = (usize + block - 1) // block
        sizes = [struct.unpack_from(e + '2i', raw, co + 16 + 8 * i) for i in range(nblocks)]
        p = co + 16 + 8 * nblocks
        out = bytearray()
        for bc, bu in sizes:
            data = raw[p:p + bc]
            p += bc
            if self.compression == 1:
                out += zlib.decompress(data)
            elif self.compression == 2:
                out += lzo1x_decompress(data, bu)
            else:
                raise ValueError('compression %d' % self.compression)
        return bytes(out[:us])

    def i32(self, o):
        return struct.unpack_from(self.e + 'i', self.img, o)[0]

    def names(self):
        out, o = [], self.name_off
        for _ in range(self.name_count):
            n = self.i32(o)
            if n < 0:  # UTF-16
                s = self.img[o + 4:o + 4 - 2 * n].decode('utf-16-le' if self.e == '<' else 'utf-16-be').rstrip('\0')
                o += 4 - 2 * n
            else:
                s = self.img[o + 4:o + 4 + n].rstrip(b'\0').decode('latin-1')
                o += 4 + n
            o += 8  # the name's flags
            out.append(s)
        return out

    def imports(self, names):
        out, o = [], self.import_off
        for _ in range(self.import_count):
            cp, _, cn, _, outer, on, num = struct.unpack_from(self.e + '7i', self.img, o)
            out.append((names[cp] if 0 <= cp < len(names) else '?', names[cn] if 0 <= cn < len(names) else '?',
                        outer, names[on] if 0 <= on < len(names) else '?'))
            o += 28
        return out

    def exports(self, names, imports):
        """The class and name of each export; the entry size found from the table's span."""
        span = (self.import_off if self.import_off > self.export_off else self.depends_off) - self.export_off
        out, o = [], self.export_off
        for i in range(self.export_count):
            cls, sup, outer, name, num, arch = struct.unpack_from(self.e + '6i', self.img, o)
            flags = struct.unpack_from(self.e + 'Q', self.img, o + 24)[0]
            ssize, soff = struct.unpack_from(self.e + '2i', self.img, o + 32)
            p = o + 40
            comps = self.i32(p)
            p += 4 + 12 * max(comps, 0)
            p += 4  # export flags
            gens = self.i32(p)
            p += 4 + 4 * max(gens, 0)
            p += 16 + 4  # package guid and flags
            cname = ('Class' if cls == 0 else imports[-cls - 1][1] if cls < 0 and -cls - 1 < len(imports)
                     else 'export#%d' % cls)
            out.append((cname, names[name] if 0 <= name < len(names) else '?%d' % name, ssize, soff))
            o = p
        return out, span, o - self.export_off


def main():
    path = sys.argv[1]
    pkg = Pkg(path, need=0x200000)
    print('%s: %s UE3 %d/%d header %d flags %#x compression %d chunks %d names %d@%d exports %d@%d imports %d@%d' % (
        path.split('/')[-1], 'LE' if pkg.e == '<' else 'BE', pkg.version, pkg.licensee, pkg.header_size, pkg.flags,
        pkg.compression, len(pkg.chunks), pkg.name_count, pkg.name_off, pkg.export_count, pkg.export_off,
        pkg.import_count, pkg.import_off))
    names = pkg.names()
    imports = pkg.imports(names)
    if '--names' in sys.argv:
        print(' '.join(names))
    if '--grep' in sys.argv:
        g = sys.argv[sys.argv.index('--grep') + 1].lower()
        print('names with', g, ':', [n for n in names if g in n.lower()])
    try:
        exports, span, used = pkg.exports(names, imports)
        print('export table: span %d, parsed %d' % (span, used))
        counts = {}
        for c, n, s, _ in exports:
            counts.setdefault(c, [0, 0])
            counts[c][0] += 1
            counts[c][1] += s
        for c, (k, s) in sorted(counts.items(), key=lambda kv: -kv[1][1])[:25]:
            print('  %-34s %5d objects %9d bytes' % (c, k, s))
        if '--exports' in sys.argv:
            for c, n, s, off in exports:
                print('   ', c, n, s)
    except Exception as ex:  # the export entry's layout differs: names and imports still stand
        print('exports not parsed:', ex)
    classes = sorted({c for _, c, _, _ in imports})
    print('imported classes:', ', '.join(classes))


if __name__ == '__main__':
    main()
