"""PS Vita MK9 objects (UE3 501/48, little-endian) converted to the Xbox 360 game's format (472/46, big-endian):
tagged properties, SkeletalMesh, Texture2D. Names and object references are mapped by the caller's functions
(`name(text) -> index` in the target package, `obj(vita ref) -> target ref`).

    from vita_convert import Out, convert_tagged, convert_skelmesh, convert_texture
"""
import math
import struct

import skelmesh
import xenos

NATIVE_STRUCTS = {  # each one's words: f float, i int, I 4 bytes swapped, b a byte
    'Vector': 'fff', 'Rotator': 'iii', 'Color': 'I', 'LinearColor': 'ffff', 'Guid': 'iiii', 'Vector2D': 'ff',
    'Plane': 'ffff', 'Quat': 'ffff', 'Matrix': 'f' * 16, 'Box': 'ffffffb', 'BoxSphereBounds': 'f' * 7,
    'IntPoint': 'ii', 'Vector4': 'ffff', 'TwoVectors': 'f' * 6,
}

# arrays whose element type the tags do not tell (not tagged structs): by the property's name
ARRAY_ELEMENTS = {
    'SocketSets': 'o', 'Sockets': 'o', 'Descriptors': 'o', 'BodySetup': 'o', 'Bodies': 'o',
    'ReferencedObjects': 'o', 'LODMaterialMap': 'i', 'BonesEnabled': 'b', 'Sequences': 'o',
}


class Out:
    """Big-endian output with the positions of the inline bulk data offset fields (rewritten when saved)."""

    def __init__(self):
        self.b = bytearray()
        self.bulk = []

    def i32(self, v):
        self.b += struct.pack('>i', v)

    def u32(self, v):
        self.b += struct.pack('>I', v)

    def f32(self, v):
        self.b += struct.pack('>f', v)

    def u8(self, v):
        self.b.append(v)

    def raw(self, data):
        self.b += data

    def bulk_offset(self):
        self.bulk.append(len(self.b))
        self.i32(0)


class Ctx:
    """What a conversion needs from the caller: the Vita package's names, and the target's name and object maps."""

    def __init__(self, vnames, name, obj):
        self.vnames, self.name, self.obj = vnames, name, obj

    def fname(self, out, d, o):
        i, n = struct.unpack_from('<ii', d, o)
        out.i32(self.name(self.vnames[i]))
        out.i32(n)
        return self.vnames[i]

    def ref(self, out, d, o):
        out.i32(self.obj(struct.unpack_from('<i', d, o)[0]))


def is_tag(d, o, vnames, end):
    if o + 8 > end:
        return False
    ni, nn = struct.unpack_from('<ii', d, o)
    if not (0 <= ni < len(vnames)) or not (0 <= nn <= 1000):
        return False
    if vnames[ni] == 'None':
        return True
    if o + 24 > end:
        return False
    ti = struct.unpack_from('<i', d, o + 8)[0]
    return 0 <= ti < len(vnames) and vnames[ti].endswith('Property')


def swap_words(out, d, o, kinds):
    for k in kinds:
        if k == 'b':
            out.u8(d[o])
            o += 1
        elif k == 'f':
            out.f32(struct.unpack_from('<f', d, o)[0])
            o += 4
        elif k == 'i':
            out.i32(struct.unpack_from('<i', d, o)[0])
            o += 4
        else:
            out.u32(struct.unpack_from('<I', d, o)[0])
            o += 4
    return o


def convert_tagged(ctx, d, o, out, drop=(), replace=None, extra=b'', path=''):
    """The tagged list at d[o:] (Vita) written to `out`; returns where it ends in d. `drop`: properties left out;
    `replace`: {property: function(out, d, value offset, size)} writing a value of the same size instead; `extra`:
    whole tags (already big-endian) put before the list's end."""
    replace = replace or {}
    while True:
        ni = struct.unpack_from('<i', d, o)[0]
        pname = ctx.vnames[ni]
        if pname == 'None':
            out.raw(extra)
            ctx.fname(out, d, o)
            return o + 8
        ptype = ctx.vnames[struct.unpack_from('<i', d, o + 8)[0]]
        size, index = struct.unpack_from('<ii', d, o + 16)
        v = o + 24
        head = 8 if ptype == 'StructProperty' else 4 if ptype == 'BoolProperty' else 0
        nxt = v + head + (0 if ptype == 'BoolProperty' else size)
        if pname in drop:
            o = nxt
            continue
        ctx.fname(out, d, o)
        ctx.fname(out, d, o + 8)
        out.i32(size)
        out.i32(index)
        if ptype == 'StructProperty':
            sname = ctx.fname(out, d, v)
            v += 8
        elif ptype == 'BoolProperty':
            out.i32(struct.unpack_from('<i', d, v)[0])
            o = nxt
            continue
        start = len(out.b)
        if pname in replace:
            replace[pname](out, d, v, size)
        elif ptype in ('IntProperty', 'FloatProperty'):
            swap_words(out, d, v, 'f' if ptype == 'FloatProperty' else 'i')
        elif ptype in ('ObjectProperty', 'ComponentProperty', 'ClassProperty', 'InterfaceProperty'):
            ctx.ref(out, d, v)
        elif ptype == 'NameProperty':
            ctx.fname(out, d, v)
        elif ptype == 'ByteProperty':
            if size == 8:
                ctx.fname(out, d, v)
            else:
                out.raw(d[v:v + size])
        elif ptype == 'StrProperty':
            n = struct.unpack_from('<i', d, v)[0]
            out.i32(n)
            if n >= 0:
                out.raw(d[v + 4:v + 4 + n])
            else:
                for k in range(-n):
                    out.raw(struct.pack('>H', struct.unpack_from('<H', d, v + 4 + 2 * k)[0]))
        elif ptype == 'StructProperty':
            if sname in NATIVE_STRUCTS:
                swap_words(out, d, v, NATIVE_STRUCTS[sname])
            elif is_tag(d, v, ctx.vnames, v + size):
                convert_tagged(ctx, d, v, out, path=path + pname + '.')
            else:
                raise ValueError('%s%s: struct %s not known' % (path, pname, sname))
        elif ptype == 'ArrayProperty':
            count = struct.unpack_from('<i', d, v)[0]
            out.i32(count)
            e = v + 4
            if count > 0 and is_tag(d, e, ctx.vnames, v + size):
                for k in range(count):
                    e = convert_tagged(ctx, d, e, out, path='%s%s[%d].' % (path, pname, k))
            elif count > 0:
                kind = ARRAY_ELEMENTS.get(pname)
                if kind is None:
                    raise ValueError('%s%s: array of %d elements of an unknown type' % (path, pname, count))
                for k in range(count):
                    if kind == 'o':
                        ctx.ref(out, d, e)
                        e += 4
                    elif kind == 'b':
                        out.u8(d[e])
                        e += 1
                    elif kind == 'n':
                        ctx.fname(out, d, e)
                        e += 8
                    else:
                        e = swap_words(out, d, e, kind)
        else:
            raise ValueError('%s%s: property type %s not handled' % (path, pname, ptype))
        assert len(out.b) - start == size, (path + pname, ptype, size, len(out.b) - start)
        o = nxt


def tag(name_index, type_index, size, value, index=0, struct_name=None):
    """A big-endian property tag with its value (indices already in the target's name table)."""
    b = struct.pack('>iiiiii', name_index, 0, type_index, 0, size, index)
    if struct_name is not None:
        b += struct.pack('>ii', struct_name, 0)
    return b + value


# --- SkeletalMesh ----------------------------------------------------------------------------------------------

def half4(d, o, n):
    return struct.unpack_from('<%de' % n, d, o)


def pack_normal(v):
    """A unit vector as MK9 X360's packed normal: the bytes of the big-endian DWORD of (x, y, z, w), w 0."""
    q = [max(0, min(255, int(round((c + 1.0) * 127.5)))) for c in v]
    return bytes((0, q[2], q[1], q[0]))


def sub(a, b):
    return (a[0] - b[0], a[1] - b[1], a[2] - b[2])


def dot(a, b):
    return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]


def cross(a, b):
    return (a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0])


def norm(a):
    n = math.sqrt(dot(a, a))
    return (a[0] / n, a[1] / n, a[2] / n) if n > 1e-12 else None


def any_perpendicular(n):
    a = (1.0, 0.0, 0.0) if abs(n[0]) < 0.9 else (0.0, 1.0, 0.0)
    return norm(cross(n, a))


def convert_vertices(raw, count, indices):
    """Vita 24-byte vertices -> Xbox 360 48-byte ones; tangents from the triangles' positions and UVs."""
    pos, nrm, uv = [], [], []
    for k in range(count):
        o = 24 * k
        x, y, z, _ = half4(raw, o, 4)
        pos.append((x, y, z))
        b = raw[o + 8:o + 11]
        nrm.append(norm(tuple(c / 127.5 - 1.0 for c in b)) or (0.0, 0.0, 1.0))
        uv.append(half4(raw, o + 12, 2))
    tan = [[0.0, 0.0, 0.0] for _ in range(count)]
    bit = [[0.0, 0.0, 0.0] for _ in range(count)]
    for t in range(0, len(indices) - 2, 3):
        a, b, c = indices[t], indices[t + 1], indices[t + 2]
        e1, e2 = sub(pos[b], pos[a]), sub(pos[c], pos[a])
        du1, dv1 = uv[b][0] - uv[a][0], uv[b][1] - uv[a][1]
        du2, dv2 = uv[c][0] - uv[a][0], uv[c][1] - uv[a][1]
        r = du1 * dv2 - du2 * dv1
        if abs(r) < 1e-12:
            continue
        r = 1.0 / r
        tv = tuple((e1[i] * dv2 - e2[i] * dv1) * r for i in range(3))
        bv = tuple((e2[i] * du1 - e1[i] * du2) * r for i in range(3))
        for v in (a, b, c):
            for i in range(3):
                tan[v][i] += tv[i]
                bit[v][i] += bv[i]
    out = bytearray()
    for k in range(count):
        n = nrm[k]
        t = tuple(tan[k])
        t = norm(sub(t, tuple(n[i] * dot(n, t) for i in range(3)))) or any_perpendicular(n)
        hand = -1.0 if dot(cross(n, t), tuple(bit[k])) < 0 else 1.0
        bn = tuple(c * hand for c in cross(n, t))
        o = 24 * k
        u, v = uv[k]
        out += struct.pack('>3f', *pos[k])
        out += pack_normal(t) + pack_normal(bn) + pack_normal(n)
        out += struct.pack('>4f', u, v, u, v)
        out += bytes(reversed(raw[o + 16:o + 20])) + bytes(reversed(raw[o + 20:o + 24]))
    return bytes(out)


def dismemberment(ctx, d, o, out):
    """The dismemberment data (the game's reader: sub_82924928 and the functions it calls) converted; returns where
    it ends in d (the Vita's own tables after it are left out)."""
    def u8():
        nonlocal o
        out.u8(d[o])
        o += 1

    def i32():
        nonlocal o
        v = struct.unpack_from('<i', d, o)[0]
        out.i32(v)
        o += 4
        return v

    def obj():
        nonlocal o
        ctx.ref(out, d, o)
        o += 4

    def arr(f):
        for _ in range(i32()):
            f()

    def item():
        i32(); i32(); u8(); i32(); i32(); i32(); i32()

    def group():
        i32(); i32(); arr(item)

    def region():
        u8(); arr(group)

    def x():
        i32(); i32(); arr(u8); arr(i32); arr(i32)

    def records():
        # each region's record: its id, whether used, its pieces, and the record of the mirrored region (the left
        # arm's names the right arm's). The Vita's unused records hold uninitialized memory there (3516, 1583...):
        # the game takes it for a record's index and reads far past the array: 0, as the Xbox 360's baby has in
        # all of its records (a record that is there)
        nonlocal o
        n = i32()
        for _ in range(n):
            u8(); i32(); arr(x)
            last = struct.unpack_from('<i', d, o)[0]
            o += 4
            out.i32(last if -1 <= last < n else 0)

    def h():
        u8(); u8(); i32(); i32(); i32()

    arr(i32)          # A
    obj()             # B
    arr(obj)          # C (one per material)
    arr(obj)          # D
    arr(region)       # 18 regions
    records()         # F
    arr(h)            # H
    arr(obj)          # I
    obj()             # J
    arr(obj)          # K
    arr(obj)          # L
    return o


def convert_skelmesh(ctx, d, end):
    """A Vita SkeletalMesh's native data (from `end`, where its tagged properties end) in the Xbox 360 format."""
    m = skelmesh.read(d, end, '<', 501)
    out = Out()
    for f in m['bounds']:
        out.f32(f)
    out.i32(len(m['materials']))
    for r in m['materials']:
        out.i32(ctx.obj(r))

    def names(pairs):
        out.i32(len(pairs))
        for i, n in pairs:
            out.i32(ctx.name(ctx.vnames[i]))
            out.i32(n)

    out.i32(len(m['refpose']))
    for p in m['refpose']:
        out.raw(struct.pack('>7f', *p))
    out.i32(len(m['parentage']))
    for p in m['parentage']:
        out.raw(struct.pack('>h', p))
    names(m['bone_names'])
    out.i32(len(m['sub_skeletons']))
    for s in m['sub_skeletons']:
        out.i32(ctx.name(ctx.vnames[s[0]]))
        out.i32(s[1])
        out.raw(struct.pack('>8i', *s[2:]))
    names(m['upper_bones'])
    out.i32(len(m['mirror']))
    for i, b in m['mirror']:
        out.i32(i)
        out.u8(b)
    out.raw(bytes(m['mirror_axes']))
    out.i32(m['depth'])
    out.i32(len(m['material_bones']))
    for i, (ni, nn) in m['material_bones']:
        out.i32(i)
        out.i32(ctx.name(ctx.vnames[ni]))
        out.i32(nn)
    out.i32(len(m['lods']))
    for lod in m['lods']:
        out.i32(len(lod['sections']))
        for s in lod['sections']:
            out.raw(struct.pack('>hhiH', *s))
        _, n, raw = lod['indices']
        indices = struct.unpack_from('<%dH' % n, raw, 0)
        out.i32(n)
        out.raw(struct.pack('>%dH' % n, *indices))
        for key, fmt in (('f68', 'H'), ('used_bones', 'h'), ('f74', 'B')):
            out.i32(len(lod[key]))
            for v in lod[key]:
                out.raw(struct.pack('>' + fmt, v))
        out.i32(len(lod['chunks']))
        for c in lod['chunks']:
            out.raw(struct.pack('>3i', c['first_vertex'], 0, 0))
            for key in ('unk1c', 'unk28'):
                out.i32(len(c[key]))
                for q in c[key]:
                    out.raw(struct.pack('>4H', *q))
            out.i32(len(c['bones']))
            for b in c['bones']:
                out.raw(struct.pack('>h', b))
            out.raw(struct.pack('>3i', c['num_rigid'], c['num_soft'], c['max_influences']))
        out.i32(0)  # the Vita's is uninitialized memory (0xCDCDCDCD); the Xbox 360's are 0
        out.i32(lod['num_vertices'])
        out.i32(len(lod['edges']))
        for q in lod['edges']:
            out.raw(struct.pack('>4i', *q))
        out.i32(len(lod['f24']))
        out.raw(bytes(lod['f24']))
        flags, count, size, _, payload = lod['bulk']
        assert flags == 0 and size == 2 * count, ('vertex point indices', flags, count, size)
        out.raw(struct.pack('>3i', flags, count, size))
        out.bulk_offset()
        out.raw(xenos.swap16(payload))
        size_, count, raw = lod['gpu']
        assert count == lod['num_vertices'] == sum(c['num_rigid'] + c['num_soft'] for c in lod['chunks'])
        out.i32(count)
        out.raw(convert_vertices(raw, count, indices))
    rest = m['rest_at']
    na8 = struct.unpack_from('<i', d, rest)[0]
    assert na8 == 0, 'unkA8 records'
    out.i32(0)
    nmap = struct.unpack_from('<i', d, rest + 4)[0]
    out.i32(nmap)
    o = rest + 8
    for _ in range(nmap):
        ctx.fname(out, d, o)
        out.i32(struct.unpack_from('<i', d, o + 8)[0])
        o += 12
    o = dismemberment(ctx, d, o, out)
    return out, len(d) - o  # and the Vita's tables left out


# --- Texture2D -------------------------------------------------------------------------------------------------

def texture_mips(d, end):
    """A Vita Texture2D's mips after its tagged properties: [(flags, payload or None, sx, sy)], and the bytes after."""
    o = end
    for _ in range(2):  # the source art and one more bulk, empty
        flags, count, size, _ = struct.unpack_from('<4i', d, o)
        assert size <= 0 or flags & 0x21, 'a non-empty bulk before the mips'
        o += 16
    n = struct.unpack_from('<i', d, o)[0]
    o += 4
    mips = []
    for _ in range(n):
        flags, count, size, _ = struct.unpack_from('<4i', d, o)
        o += 16
        payload = None
        if size > 0 and not flags & 0x21:
            payload = d[o:o + size]
            o += size
        sx, sy = struct.unpack_from('<ii', d, o)
        o += 8
        mips.append((flags, payload, sx, sy))
    return mips, len(d) - o


def texture_native(levels, w, h, bpb=16):
    """Linear DXT levels -> the Xbox 360 Texture2D's native data (two empty bulks, the mips) and the tail base."""
    mips, tb = xenos.to_x360(levels, w, h, bpb)
    out = Out()
    for _ in range(2):
        out.raw(struct.pack('>3i', 0, 0, 0))
        out.bulk_offset()
    out.i32(len(mips))
    for payload, sx, sy in mips:
        if payload is None:
            out.raw(struct.pack('>4i', 0x21, 0, -1, -1))
        else:
            out.raw(struct.pack('>3i', 0, len(payload), len(payload)))
            out.bulk_offset()
            out.raw(payload)
        out.raw(struct.pack('>ii', sx, sy))
    return out, tb


def vita_levels(d, end):
    """A Vita DXT texture's present levels, unswizzled to linear: (levels, width, height)."""
    mips, _ = texture_mips(d, end)
    present = [(p, sx, sy) for f, p, sx, sy in mips if p is not None]
    w, h = present[0][1], present[0][2]
    levels = []
    for k, (p, sx, sy) in enumerate(present):
        assert (max(w >> k, 1), max(h >> k, 1)) == (sx, sy) or min(sx, sy) == 4, ('mip sizes', k, sx, sy)
        bw, bh = xenos.blocks_of(*xenos.mip_dims(w, h, k))
        assert len(p) == bw * bh * 16, ('a mip of other than 16-byte blocks', len(p), bw, bh)
        levels.append(xenos.vita_unswizzle(p, bw, bh))
    return levels, w, h
