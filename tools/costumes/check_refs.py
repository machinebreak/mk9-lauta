"""Every object reference in an Xbox 360 package within its tables: the export and import tables' (class, super,
outer, archetype), the tagged properties' and the SkeletalMeshes' native ones (materials, dismemberment). A reference
past the tables makes the game's loader (ULinkerLoad::CreateExport, sub_82709600) read past its export map.

    python check_refs.py <package.xxx> [...]
"""
import struct
import sys

import skelmesh
from tagged import walk
from transplant import Model
from vita_convert import ARRAY_ELEMENTS


def check(path):
    t = Model.load(path)
    nm = [n for n, _ in t.names]
    ne, ni = len(t.exports), len(t.imports)
    bad = []

    def ok(r):
        return -ni <= r <= ne

    for i, x in enumerate(t.exports):
        w = x['w']
        for k, what in ((0, 'class'), (1, 'super'), (2, 'outer'), (5, 'archetype')):
            if not ok(w[k]):
                bad.append('export %d %s: %s %d' % (i + 1, t.path(i + 1) if ok(w[2]) else '?', what, w[k]))
    for i, im in enumerate(t.imports):
        if not ok(im[4]):
            bad.append('import %d: outer %d' % (i + 1, im[4]))
    for i, x in enumerate(t.exports):
        d = x['data']
        cls = t.class_path(x['w'][0]).split('.')[-1] if ok(x['w'][0]) else '?'
        try:
            wk, end = walk(d, nm)
        except Exception:  # noqa: BLE001 (not tagged: nothing to check here)
            continue
        refs = [(r.pos, struct.unpack_from('>i', d, r.pos)[0]) for r in wk.refs if r.kind == 'object']
        for a, b, name in wk.raw:
            if ARRAY_ELEMENTS.get(name.split('.')[-1]) == 'o':
                refs += [(p, struct.unpack_from('>i', d, p)[0]) for p in range(a, b, 4)]
        if cls == 'SkeletalMesh':
            m = skelmesh.read(d, end, '>', 472)
            refs += [(0, r) for r in m['materials']]
        for pos, r in refs:
            if not ok(r):
                bad.append('%s (%s) @%d: reference %d (exports %d, imports %d)' % (t.path(i + 1), cls, pos, r, ne, ni))
        for q in wk.props:
            if q['type'] == 'NameProperty' or q.get('struct'):
                pass
    print('%s: %d exports, %d imports; %d bad references' % (path, ne, ni, len(bad)))
    for b in bad[:20]:
        print('   ', b)
    return not bad


if __name__ == '__main__':
    sys.exit(0 if all([check(p) for p in sys.argv[1:]]) else 1)
