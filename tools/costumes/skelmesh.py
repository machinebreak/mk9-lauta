"""MK9 SkeletalMesh native data (after the tagged properties), read for both platforms: Xbox 360 (UE3 472/46,
big-endian) and PS Vita (501/48, little-endian). Field order from UE Viewer (UnMesh3.cpp, GAME_MK) and the game's own
reader (USkeletalMesh::Serialize, sub_82924928).

The GPU vertex buffer: Xbox 360, a TArray of 48-byte vertices (position float3; TangentX, TangentY, TangentZ packed;
two UV float2; bone indices and weights, 4 bytes each); Vita, a count and 24-byte vertices (position half4; normal
ubyte4; UV half2; bone indices and weights). Both store each chunk's vertices together (rigid first) with chunk-local
bone indices.

After the LODs (the `rest`): unkA8 (a TArray of 8-byte records, empty in these meshes), the bone NameIndexMap
(FName, int32), then the dismemberment data (`dismember.py`); the Vita adds 4868 bytes of tables after it that the
Xbox 360 game does not read.
"""
import struct


class Reader:
    def __init__(self, data, pos, e):
        self.d, self.p, self.e = data, pos, e

    def take(self, fmt):
        v = struct.unpack_from(self.e + fmt, self.d, self.p)
        self.p += struct.calcsize(self.e + fmt)
        return v if len(v) > 1 else v[0]

    def raw(self, n):
        b = self.d[self.p:self.p + n]
        self.p += n
        return b

    def array(self, item):
        n = self.take('i')
        assert 0 <= n < 1 << 22, ('array count', n, self.p)
        return [item() for _ in range(n)]

    def plain_array(self, size):
        """TArray of fixed-size elements, read raw: count, data."""
        count = self.take('i')
        assert 0 <= count < 1 << 22, ('plain array', count, self.p)
        return size, count, self.raw(size * count)


def read(data, start, e, arver):
    """The native data's fields. arver: 472 (Xbox 360) or 501 (Vita)."""
    vita = arver >= 493
    r = Reader(data, start, e)
    m = {'start': start}
    m['bounds'] = r.take('7f')
    m['materials'] = r.array(lambda: r.take('i'))
    m['refpose'] = r.array(lambda: r.take('7f'))
    m['parentage'] = r.array(lambda: r.take('h'))
    m['bone_names'] = r.array(lambda: r.take('ii'))
    m['sub_skeletons'] = r.array(lambda: r.take('ii8i'))
    m['upper_bones'] = r.array(lambda: r.take('ii'))
    m['mirror'] = r.array(lambda: (r.take('i'), r.take('B')))
    m['mirror_axes'] = r.take('BB')
    m['depth'] = r.take('i')
    m['material_bones'] = r.array(lambda: (r.take('i'), r.take('ii')))
    m['lods'] = []
    for _ in range(r.take('i')):
        lod = {'at': r.p}
        lod['sections'] = r.array(lambda: r.take('hhiH'))
        lod['indices'] = r.plain_array(2)  # MK9: a plain TArray<uint16>
        lod['f68'] = r.array(lambda: r.take('H'))
        lod['used_bones'] = r.array(lambda: r.take('h'))
        lod['f74'] = r.array(lambda: r.take('B'))
        chunks = []
        for _ in range(r.take('i')):
            c = {'first_vertex': r.take('i')}
            c['rigid'] = r.take('i')
            assert c['rigid'] == 0, 'chunk with CPU rigid vertices'
            c['soft'] = r.take('i')
            assert c['soft'] == 0, 'chunk with CPU soft vertices'
            c['unk1c'] = r.array(lambda: r.take('4H'))
            c['unk28'] = r.array(lambda: r.take('4H'))
            c['bones'] = r.array(lambda: r.take('h'))
            c['num_rigid'], c['num_soft'], c['max_influences'] = r.take('iii')
            chunks.append(c)
        lod['chunks'] = chunks
        lod['f80'], lod['num_vertices'] = r.take('ii')
        lod['edges'] = r.array(lambda: r.take('4i'))
        lod['f24'] = r.array(lambda: r.take('B'))
        flags, count, size, offset = r.take('iiii')
        lod['bulk'] = (flags, count, size, offset, r.raw(size if not flags & 0x20 else 0))
        if vita:
            count = r.take('i')
            lod['gpu'] = (24, count, r.raw(24 * count))
        else:
            lod['gpu'] = r.plain_array(48)
        lod['end'] = r.p
        m['lods'].append(lod)
    m['rest_at'] = r.p
    m['rest'] = data[r.p:]
    return m


def summary(m):
    out = ['bounds %s; %d materials; %d bones (depth %d); %d material bones; %d LODs; %d bytes after the LODs' % (
        tuple(round(x, 1) for x in m['bounds']), len(m['materials']), len(m['refpose']), m['depth'],
        len(m['material_bones']), len(m['lods']), len(m['rest']))]
    for k, lod in enumerate(m['lods']):
        g = lod['gpu']
        out.append('  LOD%d: %d sections; %d indices; %d used bones; %d chunks (bones %s); %d verts; bulk %s; '
                   'gpu %d x %d bytes' % (k, len(lod['sections']), lod['indices'][1], len(lod['used_bones']),
                                          len(lod['chunks']), [len(c['bones']) for c in lod['chunks']],
                                          lod['num_vertices'], lod['bulk'][:4], g[1], g[0]))
    return '\n'.join(out)
