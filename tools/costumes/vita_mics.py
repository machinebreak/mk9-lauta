"""Per-costume material colors (Materials.* and CHAR_<costume>.Materials.*): parent and the vector/scalar parameter
values, from a Vita package (little-endian) or an Xbox 360 one (transplant.Model)."""
import struct
import sys

import tagged
import tagged_le
from transplant import Model
from vitapkg import VitaPkg


def costume_mics(path, vita):
    if vita:
        m = VitaPkg.load(path)
        names, e, walker = m.names, '<', tagged_le
        exports = [(m.path(i + 1), m.obj_name(x['class']), x['data']) for i, x in enumerate(m.exports)]
        pathof = m.path
    else:
        m = Model.load(path)
        names, e, walker = [n for n, _ in m.names], '>', tagged
        exports = [(m.path(i + 1), m.class_path(x['w'][0]).split('.')[-1], x['data']) for i, x in enumerate(m.exports)]
        pathof = m.path
    out = {}
    for p, cls, data in exports:
        if cls != 'MaterialInstanceConstant':
            continue
        if not (p.startswith('Materials.') or '.Materials.' in p and p.split('.')[0].startswith('CHAR_') and 'Retro' not in p):
            continue
        w, end = walker.walk(data, names)
        par, params, cur = None, {}, None
        for q in w.props:
            if q['name'] == 'Parent':
                par = pathof(q['value'])
            elif q['name'].endswith('ParameterName'):
                cur = q.get('value')
            elif q['name'].endswith('ParameterValue') and cur:
                if q.get('struct') == 'LinearColor':
                    params[cur] = tuple(round(f, 4) for f in struct.unpack_from(e + '4f', data, q['pos'] + 32))
                elif q['type'] == 'FloatProperty':
                    params[cur] = round(q['value'], 4)
                elif q['type'] == 'ObjectProperty':
                    params[cur] = pathof(q['value']) if q['value'] else None
                cur = None
        out[p] = (par, params)
    return out


if __name__ == '__main__':
    for path in sys.argv[1:]:
        vita = '/vita_files/' in path.replace(chr(92), '/')
        print('==', path.split('/')[-1], '(vita)' if vita else '')
        for p, (par, params) in sorted(costume_mics(path, vita).items()):
            print('   %-46s parent=%s' % (p, par))
            for k, v in params.items():
                print('        %-26s %s' % (k, v))
