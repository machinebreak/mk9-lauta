"""A new costume's packages from the game's own: the character's costume package of another letter renamed (the
package, its seekfree container, the script's .upk and player_data symbol), the look of the new costume transplanted
from two packages that hold it (references found by diffing them), the referencer and the forced packages list made
to match. The same for its Babality package.

    python build_costume.py <Asset folder> <out folder> <costume> [...]   (costumes: scorpion_d subzero_d ...)
"""
import os
import re
import struct
import sys

from mk9hash import fnv1, mk9_hash
from pkgio import Package
from referencer import referenced, set_referenced
from tagged import walk
from transplant import Model, transplant
from vita_mics import costume_mics

# the PS Vita game's Asset folder (PCSE00023: its costume packages, for their colors)
VITA_ASSET = os.environ.get('MK9_VITA_ASSET', '')

MK2_MALE = {
    'donors': ('CHAR_Smoke_D', 'CHAR_Noob_D'),
    'groups': ('CHAR_RetroNinja_Male_MK2', 'RetroNinja_Male_MK2_IKSockets'),
    'drop': 'RetroNinja_Male_MK1',
    'baby_donors': ('NPC_Smoke_D_Baby', 'NPC_Noob_D_Baby'),
    'baby_groups': ('NPC_RetroNinja_Male_MK2_Baby', 'CHAR_RetroNinja_Male_MK2'),
    'baby_drop': 'RetroNinja_Male_MK1',
    'ini': {'SourceAsset': 'CHAR_RetroNinja_Male_MK2', 'MeshName': 'RetroNinja_Male_MK2',
            '{Char}_Expressions': 'RetroNinja_Male_MK2_Expressions'},
    'baby_ini': {'SourceAsset': 'NPC_RetroNinja_Male_MK2_Baby', 'MeshName': 'RetroNinja_Male_MK2_Baby'},
}

MK2_FEMALE = {
    'donors': ('CHAR_Kitana_C', 'CHAR_Jade_C'),
    'groups': ('CHAR_RetroNinja_Female_MK2', 'RetroNinja_Female_MK2_IKSockets'),
    'drop': 'RetroNinja_Female_MK3',
    'new': 'RetroNinja_Female_MK2',
    'baby_donors': ('NPC_Kitana_C_Baby', 'NPC_Jade_C_Baby'),
    'baby_groups': ('NPC_RetroNinja_Female_MK2_Baby', 'CHAR_RetroNinja_Female_MK2'),
    'baby_drop': 'RetroNinja_Female_MK3',
    'ini': {'SourceAsset': 'CHAR_RetroNinja_Female_MK2', 'MeshName': 'RetroNinja_Female_MK2',
            '{Char}_Expressions': 'RetroNinja_Female_MK2_Expressions'},
    'baby_ini': {'SourceAsset': 'NPC_RetroNinja_Female_MK2_Baby', 'MeshName': 'RetroNinja_Female_MK2_Baby'},
}

COSTUMES = {
    'mileena_c': dict(MK2_FEMALE, char='Mileena', base='d'),
    'scorpion_d': dict(MK2_MALE, char='Scorpion', base='c'),
    'subzero_d': dict(MK2_MALE, char='SubZero', base='c'),
    'reptile_d': dict(MK2_MALE, char='Reptile', base='c'),
    'ermac_d': dict(MK2_MALE, char='Ermac', base='c'),
}


def rename(path_in, path_out, name, base, new, baby=False):
    """The costume's own identifiers, base letter -> new letter, in every case they are written (same length)."""
    p = Package.load(path_in)
    low = 'char_%s_' % name.lower() if not baby else 'npc_%s_' % name.lower()
    suffix = '_baby' if baby else ''
    pattern = re.compile(rb'(?i)(' + re.escape(low.encode()) + rb')' + base.encode() + re.escape(suffix.encode()) +
                         rb'(-[0-9a-f]{8})?(?![A-Za-z0-9])')
    counts = {}

    def fix(buf):
        out = bytearray(buf)
        for m in pattern.finditer(bytes(buf)):
            text = m.group(0).decode('latin-1')
            head = m.group(1).decode('latin-1')
            letter = text[len(head)]
            newletter = new.upper() if letter.isupper() else new.lower()
            ident = head + newletter + text[len(head) + 1:len(head) + 1 + len(suffix)]
            rep = ident
            if m.group(2):
                rep = '%s-%08x' % (ident, mk9_hash(ident))
            assert len(rep) == len(text), (text, rep)
            out[m.start():m.end()] = rep.encode('latin-1')
            counts[text] = counts.get(text, 0) + 1
        return out

    p.summary = fix(p.summary)
    p.image = fix(p.image)
    if not baby:  # the player_data symbol the fight looks up by FNV-1
        for sym in ('player_data', 'audio_override'):
            old = fnv1('char_%s_%s_%s' % (name.lower(), base.lower(), sym)).to_bytes(4, 'big')
            neu = fnv1('char_%s_%s_%s' % (name.lower(), new.lower(), sym)).to_bytes(4, 'big')
            c = p.replace_bytes({old: neu})[old]
            counts['fnv1 ' + sym] = c
    p.save(path_out)
    return counts


COLORS = ('AlternatePaletteColorA', 'SSSColor', 'GlowColor')


NEW_LOOK = {}  # dropped look -> new one, for the costume being built (its spec's 'new')


def new_name(drop):
    """The new look's name for the dropped one's (RetroNinja_Male_MK1 -> RetroNinja_Male_MK2)."""
    return NEW_LOOK.get(drop) or drop.replace('_MK1', '_MK2')


def color_params(model, data):
    """{parameter name: offset of its LinearColor value} of a material instance's VectorParameterValues."""
    w, end = walk(data, [n for n, _ in model.names])
    assert end == len(data), 'native data after the properties'
    out, cur = {}, None
    for q in w.props:
        if q['name'].endswith('ParameterName'):
            cur = q.get('value')
        elif q['name'].endswith('ParameterValue') and cur:
            if q.get('struct') == 'LinearColor':
                out[cur] = q['pos'] + 32  # tag (24) + the struct's name (8)
            cur = None
    return out


def read_colors(model, path):
    data = model.exports[model.export_by_path[path] - 1]['data']
    return {k: struct.unpack_from('>4f', data, o) for k, o in color_params(model, data).items()}


def adopt(t, t_path, s, s_path, rename, native_ok=False):
    """t's object t_path takes s's object s_path's data, its name and object references remapped into t (object
    paths through `rename`). native_ok: data after the properties copied as it is (a script's bytecode)."""
    x = s.exports[s.export_by_path[s_path] - 1]
    d = bytearray(x['data'])
    w, end = walk(x['data'], [n for n, _ in s.names])
    assert native_ok or end == len(d), 'native data after the properties'
    for r in w.refs:
        v = struct.unpack_from('>i', x['data'], r.pos)[0]
        if r.kind == 'name':
            nv = t.add_name(s.name(v), s.names[v][1])
        elif v == 0:
            continue
        else:
            p = rename(s.path(v))
            nv = t.export_by_path.get(p) or t.import_by_path.get(p)
            if nv is None:
                raise LookupError('%s: %s not in the target' % (s_path, p))
        struct.pack_into('>i', d, r.pos, nv)
    t.exports[t.export_by_path[t_path] - 1]['data'] = bytes(d)


def costume_materials(t, s1, drop, new, own, donor_own, vita_path, log):
    """The costume's own material instances (Materials.*, and <costume>.Materials.* in a Babality's package) are
    children of the look's; on the new look they are the donor's (children of the new look's, the few values a
    costume sets), with this character's colors: the Vita costume's where it sets them, else the old costume's."""
    vita = {}
    if vita_path and os.path.exists(vita_path):
        for p, (par, params) in costume_mics(vita_path, True).items():
            vita[p] = {k: v for k, v in params.items() if k in COLORS and isinstance(v, tuple)}
    t.build_index()
    s1.build_index()
    names = [n for n, _ in t.names]
    done = 0
    for i, x in enumerate(list(t.exports)):
        p = t.path(i + 1)
        top = p.split('.')[0]
        if not (top == 'Materials' or top == own) or 'Material' not in t.class_path(x['w'][0]):
            continue
        w, end = walk(x['data'], names)
        parent = [struct.unpack_from('>i', x['data'], q['pos'] + 24)[0] for q in w.props if q['name'] == 'Parent']
        if not parent or drop not in t.path(parent[0]):
            continue
        old_colors = read_colors(t, p)
        donor_path = donor_own + p[len(own):] if top == own else p
        if donor_path not in s1.export_by_path:
            # the new look's costumes have none: hidden under another name (the game finds these by name)
            x['w'] = list(x['w'])
            x['w'][3] = t.add_name(t.name(x['w'][3]) + '_Unused')
            log('  material %s: none in the donor, hidden' % p)
            continue
        donor_data = s1.exports[s1.export_by_path[donor_path] - 1]['data']
        wanted = set(vita.get(p, {}))
        if wanted and not wanted <= set(color_params(s1, donor_data)) and wanted <= set(old_colors):
            # the Vita costume sets a color the donor's does not have (Reptile's green baby): the old costume's,
            # moved onto the new look
            d = bytearray(x['data'])
            for r in w.refs:
                v = struct.unpack_from('>i', x['data'], r.pos)[0] if r.kind == 'object' else 0
                if v and drop in t.path(v).split('.')[0]:
                    q = t.path(v).replace(drop, new_name(drop))
                    nv = t.export_by_path.get(q) or t.import_by_path.get(q)
                    if nv is None:
                        raise LookupError('%s: no %s' % (p, q))
                    struct.pack_into('>i', d, r.pos, nv)
            t.exports[i]['data'] = bytes(d)
            log('  material %s: kept, moved onto the new look' % p)
        else:
            adopt(t, p, s1, donor_path, lambda q: own + q[len(donor_own):] if q.split('.')[0] == donor_own else q)
        colors = {}
        for k in COLORS:
            v = vita.get(p, {}).get(k) or old_colors.get(k)
            if v is not None:
                colors[k] = v
        y = t.exports[i]
        pos = color_params(t, y['data'])
        d = bytearray(y['data'])
        for k, v in colors.items():
            if k in pos:
                struct.pack_into('>4f', d, pos[k], *v)
        y['data'] = bytes(d)
        done += 1
        log('  material %s: %s' % (p, ', '.join('%s %s' % (k, tuple(round(c, 2) for c in v[:3])) for k, v in colors.items())))
    return done


def swap_look(target_path, donor1, donor2, groups, drop, out_path, log, own=None, donor_own=None, vita_path=None):
    t = Model.load(target_path)
    s1 = Model.load(donor1)
    s2 = Model.load(donor2)
    transplant(t, s1, s2, lambda p: p.split('.')[0] in groups, log=log)
    # the summary's forced packages: the new look's in the place of the old one's (the game takes a Babality's
    # mesh from there, not from the ini), the others appended
    have = [n for n, _ in t.forced_packages]
    for n, g in s1.forced_packages:
        if n not in groups or n in have:
            continue
        old_entry = n.replace(new_name(drop), drop)
        if old_entry in have:
            k = have.index(old_entry)
            t.forced_packages[k] = (n, g)
            have[k] = n
            log('  forced package %s -> %s' % (old_entry, n))
        else:
            t.forced_packages.append((n, g))
            have.append(n)
    old = referenced(t)
    keep = [r for r in old if drop not in t.path(r)]
    new = [t.export_by_path[s1.path(r)] for r in referenced(s1) if s1.path(r).split('.')[0] in groups]
    set_referenced(t, keep + new)
    # the scripts name the look too (a Babality's script: the baby's package and mesh, as text): the new one's,
    # the same length
    old_b, new_b = drop.encode(), new_name(drop).encode()
    assert len(old_b) == len(new_b)
    for i, x in enumerate(t.exports):
        if t.class_path(x['w'][0]).endswith('MKScriptBinary') and old_b in x['data']:
            log('  script %s: %d x %s -> %s' % (t.path(i + 1), x['data'].count(old_b), drop, new_name(drop)))
            x['data'] = x['data'].replace(old_b, new_b)
    n = costume_materials(t, s1, drop, groups[0], own, donor_own, vita_path, log)
    size = t.save(out_path)
    log('  referencer %d -> %d; %d costume materials on the new look; %d bytes' % (len(old), len(keep) + len(new), n,
                                                                                      size))
    return size


def build(asset, out, key, log=print):
    spec = COSTUMES[key]
    name, base, new = spec['char'], spec['base'], key.split('_')[-1]
    cap = name
    NEW_LOOK.clear()
    if spec.get('new'):
        NEW_LOOK[spec['drop']] = spec['new']
        NEW_LOOK[spec['baby_drop']] = spec['new']
    char_base = os.path.join(asset, 'CHAR_%s_%s.xxx' % (cap, base.upper()))
    baby_base = os.path.join(asset, 'NPC_%s_%s_Baby.xxx' % (cap, base.upper()))
    tmp = os.path.join(out, '_renamed.xxx')
    char_out = os.path.join(out, 'CHAR_%s_%s.xxx' % (cap, new.upper()))
    baby_out = os.path.join(out, 'NPC_%s_%s_Baby.xxx' % (cap, new.upper()))
    own = 'CHAR_%s_%s' % (cap, new.upper())
    log('%s: %s' % (key, rename(char_base, tmp, name, base, new)))
    swap_look(tmp, os.path.join(asset, spec['donors'][0] + '.xxx'), os.path.join(asset, spec['donors'][1] + '.xxx'),
              spec['groups'], spec['drop'], char_out, log, own='Materials', donor_own='Materials',
              vita_path=os.path.join(VITA_ASSET, own + '.xxx'))
    log('%s baby: %s' % (key, rename(baby_base, tmp, name, base, new, baby=True)))
    # the Babality's package also names the character's costume package
    q = Package.load(tmp)
    rx = re.compile(rb'(?i)(char_' + re.escape(name.lower().encode()) + rb'_)' + base.encode() + rb'(?![A-Za-z0-9-])')
    for buf in (q.summary, q.image):
        for m in list(rx.finditer(bytes(buf))):
            letter = buf[m.end() - 1:m.end()]
            buf[m.end() - 1:m.end()] = (new.upper() if letter.isupper() else new.lower()).encode()
    q.save(tmp)
    swap_look(tmp, os.path.join(asset, spec['baby_donors'][0] + '.xxx'),
              os.path.join(asset, spec['baby_donors'][1] + '.xxx'), spec['baby_groups'], spec['baby_drop'],
              baby_out, log, own=own, donor_own=spec['donors'][0],
              vita_path=os.path.join(VITA_ASSET, 'NPC_%s_%s_Baby.xxx' % (cap, new.upper())))
    os.remove(tmp)
    return char_out, baby_out


def main():
    asset, out = sys.argv[1], sys.argv[2]
    os.makedirs(out, exist_ok=True)
    for key in sys.argv[3:]:
        build(asset, out, key, log=lambda *a: print(*a) if not str(a[0]).startswith('  !') else None)


if __name__ == '__main__':
    main()
