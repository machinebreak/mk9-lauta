"""UE3 tagged properties of an export (MK9: big-endian; a tag is name, type, size, array index; a BoolProperty holds a
32-bit UBOOL after it, a ByteProperty has no enum name): a small reader for the cinematics' tracks and keys."""
import struct


class Reader:
    def __init__(self, model, data):
        self.m, self.d, self.o = model, data, 0

    def i32(self):
        v = struct.unpack_from('>i', self.d, self.o)[0]
        self.o += 4
        return v

    def f32(self):
        v = struct.unpack_from('>f', self.d, self.o)[0]
        self.o += 4
        return v

    def name(self):
        i, n = struct.unpack_from('>ii', self.d, self.o)
        self.o += 8
        return self.m.fname(i, n) if 0 <= i < len(self.m.names) else '?%d' % i

    def string(self):
        n = self.i32()
        if n == 0:
            return ''
        if n < 0:
            s = self.d[self.o:self.o - 2 * n].decode('utf-16-be', 'replace').rstrip('\0')
            self.o += -2 * n
            return s
        s = self.d[self.o:self.o + n].decode('latin-1').rstrip('\0')
        self.o += n
        return s

    def props(self, depth=0):
        """A list of (name, type, value) up to 'None'."""
        out = []
        while self.o + 8 <= len(self.d):
            name = self.name()
            if name == 'None':
                break
            typ = self.name()
            size = self.i32()
            index = self.i32()
            if typ == 'StructProperty':
                sname = self.name()
                end = self.o + size
                try:
                    val = ('struct', sname, self.props(depth + 1))
                except Exception:  # noqa: BLE001 (a native struct: binary)
                    val = ('struct-bin', sname, self.d[end - size:end].hex())
                self.o = end
            elif typ == 'BoolProperty':  # this engine's tag holds a 32-bit UBOOL
                val = self.i32()
            elif typ == 'ByteProperty':  # no enum name in this engine's tag: the value (a byte, or an FName)
                if size == 8:
                    val = self.name()
                else:
                    val = self.d[self.o]
                    self.o += size
            elif typ == 'ArrayProperty':
                end = self.o + size
                count = self.i32()
                elems = []
                # struct elements as tagged properties (non-native structs), else raw
                try:
                    for _ in range(count):
                        elems.append(self.props(depth + 1))
                    if self.o != end:
                        raise ValueError('size')
                    val = ('array', elems)
                except Exception:  # noqa: BLE001
                    self.o = end
                    val = ('array-bin', count, size)
                self.o = end
            elif typ == 'FloatProperty':
                val = self.f32()
            elif typ == 'IntProperty':
                val = self.i32()
            elif typ == 'StrProperty':
                val = self.string()
            elif typ == 'NameProperty':
                val = self.name()
            elif typ == 'ObjectProperty':
                ref = self.i32()
                val = self.m.path(ref) if ref else 'None'
            else:
                val = ('raw', self.d[self.o:self.o + size].hex())
                self.o += size
            out.append((name, typ, val))
        return out
