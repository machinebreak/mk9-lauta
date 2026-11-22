"""UE3 tagged properties as MK9 (Xbox 360, 472/46, big-endian) serializes them: parse, print, and locate the name and
object references in them.

A tag: Name (FName: index, number), Type (FName), Size (int), ArrayIndex (int), then StructProperty's struct name
(FName) or BoolProperty's value (int, in this version), then Size bytes of value. "None" ends a list. Struct values
are tagged lists themselves unless the struct is one serialized natively (Vector, Color, LinearColor, Guid, ...).
ArrayProperty values are a count and the elements, whose type is not in the tag: struct elements are recognized as
tagged lists (a valid tag at the start), the rest returned as raw (the caller decides).
"""
import struct

NATIVE_STRUCTS = {
    'Vector': 12, 'Rotator': 12, 'Color': 4, 'LinearColor': 16, 'Guid': 16, 'Vector2D': 8, 'Plane': 16, 'Quat': 16,
    'Matrix': 64, 'Box': 25, 'BoxSphereBounds': 28, 'IntPoint': 8, 'Vector4': 16, 'TwoVectors': 24,
}


class Ref:
    def __init__(self, kind, pos, value):
        self.kind, self.pos, self.value = kind, pos, value  # kind 'name' (pos of the index) or 'object'


def is_tag(data, o, names, end):
    if o + 24 > end:
        return False
    ni, nn, ti, tn = struct.unpack_from('>iiii', data, o)
    if not (0 <= ni < len(names)) or nn < 0 or nn > 1000:
        return False
    if names[ni] == 'None':
        return True
    if not (0 <= ti < len(names)):
        return False
    return names[ti].endswith('Property')


class Walker:
    """Walks a tagged list from `o`; collects refs (names in tags and typed values, objects in typed values) and
    the arrays it could not type (raw spans)."""

    def __init__(self, data, names):
        self.data, self.names = data, names
        self.refs, self.raw, self.props = [], [], []

    def name_at(self, o):
        i = struct.unpack_from('>i', self.data, o)[0]
        return self.names[i] if 0 <= i < len(self.names) else '?%d' % i

    def walk(self, o, end, depth=0, prefix=''):
        d = self.data
        while o + 8 <= end:
            self.refs.append(Ref('name', o, None))
            if self.name_at(o) == 'None':
                return o + 8
            pname = self.name_at(o)
            self.refs.append(Ref('name', o + 8, None))
            ptype = self.name_at(o + 8)
            size, index = struct.unpack_from('>ii', d, o + 16)
            v = o + 24
            info = {'name': prefix + pname, 'type': ptype, 'size': size, 'index': index, 'pos': o}
            if ptype == 'StructProperty':
                self.refs.append(Ref('name', v, None))
                sname = self.name_at(v)
                info['struct'] = sname
                v += 8
                if sname in NATIVE_STRUCTS:
                    pass
                elif is_tag(d, v, self.names, v + size):
                    self.walk(v, v + size, depth + 1, prefix + pname + '.')
                else:
                    self.raw.append((v, v + size, prefix + pname))
            elif ptype == 'BoolProperty':
                info['value'] = struct.unpack_from('>i', d, v)[0]
                v += 4
                size = 0
            elif ptype == 'NameProperty':
                self.refs.append(Ref('name', v, None))
                info['value'] = self.name_at(v)
            elif ptype in ('ObjectProperty', 'ComponentProperty', 'ClassProperty', 'InterfaceProperty'):
                self.refs.append(Ref('object', v, None))
                info['value'] = struct.unpack_from('>i', d, v)[0]
            elif ptype == 'ByteProperty':
                if size == 8:  # an enum's value as a name
                    self.refs.append(Ref('name', v, None))
                    info['value'] = self.name_at(v)
                else:
                    info['value'] = d[v]
            elif ptype == 'ArrayProperty':
                count = struct.unpack_from('>i', d, v)[0]
                info['count'] = count
                e = v + 4
                if count > 0 and is_tag(d, e, self.names, v + size):
                    for k in range(count):
                        e = self.walk(e, v + size, depth + 1, '%s%s[%d].' % (prefix, pname, k))
                elif count > 0:
                    self.raw.append((e, v + size, prefix + pname))
            elif ptype in ('IntProperty', 'FloatProperty'):
                info['value'] = struct.unpack_from('>i' if ptype == 'IntProperty' else '>f', d, v)[0]
            elif ptype == 'StrProperty':
                n = struct.unpack_from('>i', d, v)[0]
                info['value'] = d[v + 4:v + 4 + max(n, 0)].rstrip(b'\0').decode('latin-1')
            self.props.append(info)
            o = v + size
        return o


def walk(data, names, start=0):
    w = Walker(data, names)
    end = w.walk(start, len(data))
    return w, end
