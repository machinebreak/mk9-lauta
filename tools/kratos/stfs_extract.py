"""Lists or extracts an Xbox 360 STFS package (LIVE / PIRS / CON) as the SDK reads it
(sdk/src/filesystem/devices/stfs_container_device.cpp: the same block arithmetic and hash-table chains).

    python -I tools/kratos/stfs_extract.py <package> [<out folder>]

Without an out folder it lists the files. The SDK's content manager takes DLC as the package's files in a folder
(content/0000000000000000/<title id>/00000002/<name>/), which is what this writes.
"""
import os
import struct
import sys

BLOCK = 0x1000
PER_LEVEL = (0xAA, 0x70E4, 0x4AF768)
END_OF_CHAIN = 0xFFFFFF


def u24le(b, o):
    return b[o] | (b[o + 1] << 8) | (b[o + 2] << 16)


class Stfs:
    def __init__(self, path):
        self.f = open(path, 'rb')
        head = self.f.read(0x971A)
        self.magic = head[:4]
        if self.magic not in (b'LIVE', b'PIRS', b'CON '):
            raise ValueError('not an STFS package: %r' % self.magic)
        self.header_size = struct.unpack_from('>I', head, 0x340)[0]
        self.content_type = struct.unpack_from('>I', head, 0x344)[0]
        self.title_id = struct.unpack_from('>I', head, 0x360)[0]
        volume_type = struct.unpack_from('>I', head, 0x3A9)[0]
        if volume_type != 0:
            raise ValueError('an SVOD package (volume type %d): not handled' % volume_type)
        vd = head[0x379:0x379 + 0x24]
        if vd[0] != 0x24:
            raise ValueError('unexpected volume descriptor length %d' % vd[0])
        self.flags = vd[2]
        self.read_only = bool(self.flags & 1)
        self.root_active = bool(self.flags & 2)
        self.table_blocks = struct.unpack_from('<H', vd, 3)[0]
        self.table_block = u24le(vd, 5)
        self.total_blocks = struct.unpack_from('>I', vd, 0x1C)[0]
        self.per_hash = 1 if self.read_only else 2
        self.step0 = PER_LEVEL[0] + self.per_hash
        self.step1 = PER_LEVEL[1] + (PER_LEVEL[0] + 1) * self.per_hash
        self.base = (self.header_size + BLOCK - 1) // BLOCK * BLOCK
        self.cache = {}
        self.display_name = head[0x411:0x411 + 0x80].decode('utf-16-be', 'ignore').split('\0')[0]

    def block_offset(self, index):
        base, block = PER_LEVEL[0], index
        for _ in range(3):
            block += ((index + base) // base) * self.per_hash
            if index < base:
                break
            base *= PER_LEVEL[0]
        return self.base + (block << 12)

    def hash_block_number(self, index, level):
        if level == 0:
            if index < PER_LEVEL[0]:
                return 0
            block = (index // PER_LEVEL[0]) * self.step0
            block += ((index // PER_LEVEL[1]) + 1) * self.per_hash
            return block if index < PER_LEVEL[1] else block + self.per_hash
        if level == 1:
            if index < PER_LEVEL[1]:
                return self.step0
            return (index // PER_LEVEL[1]) * self.step1 + self.per_hash
        return self.step1

    def hash_offset(self, index, level):
        return self.base + (self.hash_block_number(index, level) << 12)

    def table(self, offset):
        if offset not in self.cache:
            self.f.seek(offset)
            self.cache[offset] = self.f.read(BLOCK)
        return self.cache[offset]

    def entry_info(self, table, record):
        return struct.unpack_from('>I', table, record * 0x18 + 0x14)[0]

    def next_block(self, index):
        secondary = BLOCK if self.root_active else 0
        lv0 = self.hash_offset(index, 0)
        if lv0 not in self.cache:
            if self.read_only:
                secondary = 0
            elif self.total_blocks > PER_LEVEL[0]:
                lv1 = self.hash_offset(index, 1)
                if lv1 not in self.cache:
                    if self.total_blocks > PER_LEVEL[1]:
                        lv2 = self.hash_offset(index, 2)
                        t2 = self.table(lv2 + secondary)
                        self.cache[lv2] = t2
                        info = self.entry_info(t2, (index // PER_LEVEL[1]) % PER_LEVEL[0])
                        secondary = BLOCK if info & 0x40000000 else 0
                    self.cache[lv1] = self.table(lv1 + secondary)
                info = self.entry_info(self.cache[lv1], (index // PER_LEVEL[0]) % PER_LEVEL[0])
                secondary = BLOCK if info & 0x40000000 else 0
            self.f.seek(lv0 + secondary)
            self.cache[lv0] = self.f.read(BLOCK)
        return self.entry_info(self.cache[lv0], index % PER_LEVEL[0]) & 0xFFFFFF

    def entries(self):
        out, index = [], self.table_block
        for _ in range(self.table_blocks):
            self.f.seek(self.block_offset(index))
            block = self.f.read(BLOCK)
            for m in range(0x40):
                e = block[m * 0x40:(m + 1) * 0x40]
                if e[0] == 0:
                    break
                flags = e[0x28]
                name = e[:flags & 0x3F].decode('latin-1')
                out.append({'name': name, 'dir': bool(flags & 0x80),
                            'start': u24le(e, 0x2F), 'blocks': u24le(e, 0x2C),
                            'parent': struct.unpack_from('>H', e, 0x32)[0],
                            'size': struct.unpack_from('>I', e, 0x34)[0]})
            index = self.next_block(index)
            if index == END_OF_CHAIN:
                break
        return out

    def path_of(self, entries, i):
        parts, e = [], entries[i]
        while True:
            parts.append(e['name'])
            if e['parent'] == 0xFFFF:
                break
            e = entries[e['parent']]
        return '/'.join(reversed(parts))

    def read(self, entry):
        data, index, left = bytearray(), entry['start'], entry['size']
        while left and index != END_OF_CHAIN:
            self.f.seek(self.block_offset(index))
            chunk = self.f.read(min(BLOCK, left))
            data += chunk
            left -= len(chunk)
            if left:
                index = self.next_block(index)
        if left:
            raise ValueError('%s: %d bytes missing (a broken block chain)' % (entry['name'], left))
        return bytes(data)


def main():
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    pkg = Stfs(sys.argv[1])
    print('%s package "%s": content type %08X, title %08X, %d blocks' % (
        pkg.magic.decode(), pkg.display_name, pkg.content_type, pkg.title_id, pkg.total_blocks))
    entries = pkg.entries()
    out = sys.argv[2] if len(sys.argv) > 2 else None
    for i, e in enumerate(entries):
        path = pkg.path_of(entries, i)
        if e['dir']:
            print('  [dir] %s' % path)
            if out:
                os.makedirs(os.path.join(out, path), exist_ok=True)
            continue
        print('  %10d  %s' % (e['size'], path))
        if out:
            target = os.path.join(out, path)
            os.makedirs(os.path.dirname(target) or out, exist_ok=True)
            with open(target, 'wb') as f:
                f.write(pkg.read(e))


if __name__ == '__main__':
    main()
