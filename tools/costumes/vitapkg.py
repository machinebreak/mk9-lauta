"""Read-only: a PS Vita MK9 package (UE3 501/48, little-endian, zlib chunks): summary, names, imports, exports and
each export's data, with the same `path`/`class_path` helpers as transplant.Model.

    python vitapkg.py <file.xxx> [path prefix]   (lists the exports)
"""
import struct
import sys
import zlib

TAG = 0x9E2A83C1


class VitaPkg:
    @classmethod
    def load(cls, path):
        self = cls()
        d = open(path, 'rb').read()
        assert struct.unpack_from('<I', d, 0)[0] == TAG, 'not a little-endian UE3 package'
        o = 4
        ver, self.header_size = struct.unpack_from('<ii', d, o)
        o += 8
        self.version, self.licensee = ver & 0xFFFF, ver >> 16
        if d[o:o + 4] == b'MK  ':  # NetherRealm's: a tag and an int before the folder
            o += 8
        n = struct.unpack_from('<i', d, o)[0]
        o += 4 + n  # folder
        self.flags = struct.unpack_from('<I', d, o)[0]
        o += 4
        (self.name_count, self.name_off, self.export_count, self.export_off, self.import_count, self.import_off,
         self.depends_off) = struct.unpack_from('<7i', d, o)
        o += 28
        # after the depends offset: (version 501) import/export guids offsets, thumbnail offset, then GUID, gens
        self.raw = d
        self.chunks = self._find_chunks(d, o)
        self.image = self._decompress(d)
        im = self.image
        # names
        self.names = []
        p = self.name_off
        for _ in range(self.name_count):
            n = struct.unpack_from('<i', im, p)[0]
            if n < 0:  # UTF-16
                s = im[p + 4:p + 4 - 2 * n].decode('utf-16-le').rstrip('\0')
                p += 4 - 2 * n
            else:
                s = im[p + 4:p + 4 + n].rstrip(b'\0').decode('latin-1')
                p += 4 + n
            self.names.append(s)
            p += 8  # flags
        # imports: class package, class name, outer, object name
        self.imports = []
        p = self.import_off
        for _ in range(self.import_count):
            cp, cpn, cn, cnn, outer, on, onn = struct.unpack_from('<7i', im, p)
            self.imports.append({'class_package': cp, 'class': cn, 'outer': outer, 'name': on, 'num': onn})
            p += 28
        # exports (501): class, super, outer, name, num, archetype, flags x2, size, offset, comps, eflags,
        # net object counts, guid, package flags
        self.exports = []
        p = self.export_off
        for _ in range(self.export_count):
            w = struct.unpack_from('<11i', im, p)
            p += 44
            comps = [struct.unpack_from('<3i', im, p + 12 * k) for k in range(w[10])]
            p += 12 * w[10]
            eflags = struct.unpack_from('<i', im, p)[0]
            p += 4 + 16  # export flags, the package GUID (the same layout as the Xbox 360's 472)
            self.exports.append({'class': w[0], 'super': w[1], 'outer': w[2], 'name': w[3], 'num': w[4],
                                 'archetype': w[5], 'size': w[8], 'offset': w[9], 'comps': comps, 'eflags': eflags})
        for x in self.exports:
            x['data'] = bytes(im[x['offset']:x['offset'] + x['size']])
        return self

    def _find_chunks(self, d, o):
        # the chunk table: the first (count, entries) whose first entry starts at the name table
        for at in range(o, min(len(d), o + 4096), 4):
            cnt = struct.unpack_from('<i', d, at)[0]
            if 0 < cnt < 4096 and at + 4 + 16 * cnt <= len(d):
                uo, us, co, cs = struct.unpack_from('<4i', d, at + 4)
                if uo == self.name_off and co > at and struct.unpack_from('<I', d, co)[0] == TAG:
                    return [struct.unpack_from('<4i', d, at + 4 + 16 * k) for k in range(cnt)]
        return []

    def _decompress(self, d):
        if not self.chunks:
            return bytearray(d)
        end = max(uo + us for uo, us, _, _ in self.chunks)
        im = bytearray(end)
        im[:self.name_off] = d[:self.name_off]
        for uo, us, co, cs in self.chunks:
            tag, block, csize, usize = struct.unpack_from('<Ii2i', d, co)
            nb = (usize + block - 1) // block
            pairs = [struct.unpack_from('<2i', d, co + 16 + 8 * k) for k in range(nb)]
            p = co + 16 + 8 * nb
            out = bytearray()
            for c, u in pairs:
                out += zlib.decompress(d[p:p + c])
                p += c
            im[uo:uo + len(out)] = out
        return im

    def name(self, i, num=0):
        s = self.names[i] if 0 <= i < len(self.names) else '?%d' % i
        return s if num == 0 else '%s_%d' % (s, num - 1)

    def obj_name(self, ref):
        if ref > 0:
            x = self.exports[ref - 1]
            return self.name(x['name'], x['num'])
        if ref < 0:
            x = self.imports[-ref - 1]
            return self.name(x['name'], x['num'])
        return 'None'

    def path(self, ref):
        parts = []
        while ref:
            parts.append(self.obj_name(ref))
            ref = self.exports[ref - 1]['outer'] if ref > 0 else self.imports[-ref - 1]['outer']
        return '.'.join(reversed(parts))

    def class_path(self, ref):
        if ref < 0:
            x = self.imports[-ref - 1]
            return '%s.%s' % (self.name(x['class_package']), self.name(x['class']))
        return self.path(ref) if ref else 'Core.Class'


def main():
    p = VitaPkg.load(sys.argv[1])
    prefix = sys.argv[2] if len(sys.argv) > 2 else ''
    print('version %d/%d, %d names, %d imports, %d exports, %d chunks' % (
        p.version, p.licensee, len(p.names), len(p.imports), len(p.exports), len(p.chunks)))
    for i, x in enumerate(p.exports):
        path = p.path(i + 1)
        if path.startswith(prefix):
            print('%5d %-70s %-34s %8d' % (i + 1, path, p.class_path(x['class']), x['size']))


if __name__ == '__main__':
    main()
