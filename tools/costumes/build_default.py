"""A costume built on the character's own package (when the game has no costume of another letter to start from, as
for Skarlet, whose Vita costumes use her base script): the package becomes the costume's (its seekfree container and
the summary's lists renamed; the script, the same as the Vita's, untouched), the look transplanted from two costumes
that have it, the costume's materials (Materials.*) taken from the donor's with this character's colors. Its
Babality: the base baby package with a retro baby script (the one of a 7-letter character's retro costume, the names
replaced at the same length), the baby look transplanted the same way.

    python build_default.py <Asset folder> <out folder> <costume> [...]   (skarlet_c skarlet_d)
"""
import os
import struct
import sys

from build_costume import COLORS, adopt, color_params
from mk9hash import mk9_hash
from referencer import referenced, set_referenced
from tagged import walk
from transplant import Model, transplant

RED = {'Character': ((0.8, 0.2, 0.2, 1.0), (0.8, 0.3, 0.45, 1.0)),  # (normal, alternate palette): the Vita's
       'Reflection': ((0.8, 0.2, 0.2, 1.0), (0.8, 0.3, 0.45, 1.0)),
       'Accessory': ((1.0, 1.0, 1.0, 1.0), (1.0, 0.85, 0.85, 1.0))}

FEMALE = {
    'MK2': dict(donors=('CHAR_Kitana_C', 'CHAR_Jade_C'),
                groups=('CHAR_RetroNinja_Female_MK2', 'RetroNinja_Female_MK2_IKSockets'),
                baby_donors=('NPC_Kitana_C_Baby', 'NPC_Jade_C_Baby'),
                baby_groups=('NPC_RetroNinja_Female_MK2_Baby', 'CHAR_RetroNinja_Female_MK2')),
    'MK3': dict(donors=('CHAR_Kitana_D', 'CHAR_Jade_D'),
                groups=('CHAR_RetroNinja_Female_MK3', 'RetroNinja_Female_MK3_IKSockets'),
                baby_donors=('NPC_Kitana_D_Baby', 'NPC_Jade_D_Baby'),
                baby_groups=('NPC_RetroNinja_Female_MK3_Baby', 'CHAR_RetroNinja_Female_MK3')),
}

COSTUMES = {
    'skarlet_c': dict(FEMALE['MK2'], char='Skarlet', look='RetroNinja_Female_MK2', colors=RED,
                      baby_script=('NPC_Mileena_D_Baby', [(b'Mileena', b'Skarlet'), (b'_D_Baby', b'_C_Baby'),
                                                         (b'_D_BABY', b'_C_BABY'), (b'Female_MK3', b'Female_MK2')])),
    'skarlet_d': dict(FEMALE['MK3'], char='Skarlet', look='RetroNinja_Female_MK3', colors=RED,
                      baby_script=('NPC_Mileena_D_Baby', [(b'Mileena', b'Skarlet')])),
}


def rename_name(t, old, new, log):
    """An FName of the package, renamed (every object of that name follows)."""
    for k, (s, flags) in enumerate(t.names):
        if s == old:
            t.names[k] = (new, flags)
            log('  name %s -> %s' % (old, new))
            return True
    return False


def summary_lists_rename(t, mapping, log):
    """The summary's two package lists (the forced exports' packages, the script packages), entries renamed."""
    t.forced_packages = [(mapping.get(n, n), g) for n, g in t.forced_packages]
    r = t.summary_rest
    count, o, out = struct.unpack_from('>i', r, 0)[0], 4, []
    for _ in range(count):
        n = struct.unpack_from('>i', r, o)[0]
        s = r[o + 4:o + 3 + n].decode('latin-1')
        guid = r[o + 3 + n + 1:o + 4 + n + 16]
        o += 4 + n + 16
        if s in mapping:
            log('  script package %s -> %s' % (s, mapping[s]))
            s = mapping[s]
        out.append((s, guid))
    assert o == len(r), 'more after the second list'
    data = struct.pack('>i', len(out))
    for s, guid in out:
        b = s.encode('latin-1') + bytes(1)
        data += struct.pack('>i', len(b)) + b + guid
    t.summary_rest = data


def take_look(t, asset, spec, groups, donors, log):
    s1 = Model.load(os.path.join(asset, donors[0] + '.xxx'))
    s2 = Model.load(os.path.join(asset, donors[1] + '.xxx'))
    transplant(t, s1, s2, lambda p: p.split('.')[0] in groups, log=log)
    have = {n for n, _ in t.forced_packages}
    for n, g in s1.forced_packages:
        if n in groups and n not in have:
            t.forced_packages.append((n, g))
    old = referenced(t)
    new = [t.export_by_path[s1.path(r)] for r in referenced(s1) if s1.path(r).split('.')[0] in groups]
    set_referenced(t, old + [r for r in new if r not in old])
    log('  referencer %d -> %d' % (len(old), len(old) + len(new)))
    return s1


def costume_materials(t, s1, own, donor_own, colors, log):
    """The costume's material instances (Materials.*, <own>.Materials.*) the donor's, with this character's colors;
    the ones the donor has none of hidden, but the body's (Stump, Interior), which do not depend on the look."""
    t.build_index()
    s1.build_index()
    done = 0
    for i, x in enumerate(list(t.exports)):
        p = t.path(i + 1)
        top = p.split('.')[0]
        if 'MaterialInstance' not in t.class_path(x['w'][0]):
            continue
        if top == 'Materials':
            donor_path = p
        elif top == own and p.startswith(own + '.Materials.'):
            donor_path = donor_own + p[len(own):]
        else:
            continue
        mic = p.split('.')[-1]
        if donor_path not in s1.export_by_path:
            if mic.startswith(('Stump', 'Interior')):
                continue
            x['w'] = list(x['w'])
            x['w'][3] = t.add_name(t.name(x['w'][3]) + '_Unused')
            log('  material %s: none in the donor, hidden' % p)
            continue
        adopt(t, p, s1, donor_path, lambda q: own + q[len(donor_own):] if q.split('.')[0] == donor_own else q)
        kind = mic.replace('_AP_MIC', '').replace('_MIC', '').replace('Damage', '')
        alt = '_AP_' in mic
        y = t.exports[i]
        pos = color_params(t, y['data'])
        d = bytearray(y['data'])
        set_ = []
        if kind in colors and 'AlternatePaletteColorA' in pos:
            v = colors[kind][1 if alt else 0]
            struct.pack_into('>4f', d, pos['AlternatePaletteColorA'], *v)
            set_.append('AlternatePaletteColorA %s' % (v[:3],))
        if 'GlowColor' in pos and kind in ('Character', 'Reflection', 'Eyes'):
            struct.pack_into('>4f', d, pos['GlowColor'], 0.0, 0.0, 0.0, 1.0)
        y['data'] = bytes(d)
        done += 1
        log('  material %s: %s' % (p, ', '.join(set_) or 'the donor\'s'))
    return done


def hide_base_groups(t, groups, log):
    """The base package's own groups at its root (the default costume's mesh and animations; the costume's are the
    look's) out of the referencer and renamed, as the PS Vita's Skarlet C and D packages have none: the game looks for
    'Meshes.<character>' in a costume's package first, and found, it takes the costume for one with its own mesh (the
    look's from [CHAR_X_Y] SourceAsset is not used, nor the costume's materials on its baby)."""
    hidden = set()
    for i, x in enumerate(t.exports):
        w = x['w']
        if w[2] == 0 and t.name(w[3]) in groups and w[4] == 0:
            x['w'] = list(w)
            x['w'][3] = t.add_name(t.name(w[3]) + '_Unused')
            hidden.add(i + 1)

    def under(ref):
        seen = 0
        while ref > 0 and seen < 64:
            if ref in hidden:
                return True
            ref = t.exports[ref - 1]['w'][2]
            seen += 1
        return False

    refs = referenced(t)
    keep = [r for r in refs if not under(r)]
    set_referenced(t, keep)
    log('  base groups %s hidden; %d of the referencer\'s %d objects dropped'
        % (sorted(t.name(t.exports[i - 1]['w'][3]) for i in hidden), len(refs) - len(keep), len(refs)))


def bake_baby_colors(t, own, look_baby, log):
    """The baby look's own material instances (<look_baby>.Materials.*) given the costume's: the game does not dress
    Skarlet's baby (a DLC character's) in the costume's materials, so the ones of the baby mesh carry its colors."""
    t.build_index()
    done = []
    for slot in ('Character_MIC', 'Character_AP_MIC', 'Accessory_MIC', 'Accessory_AP_MIC'):
        dst = t.export_by_path.get('%s.Materials.%s' % (look_baby, slot))
        src = t.export_by_path.get('%s.Materials.%s' % (own, slot))
        if dst and src:
            t.exports[dst - 1]['data'] = t.exports[src - 1]['data']
            done.append(slot)
    log('  baby look materials with the costume colors: %s' % done)


def build(asset, out, key, log=print):
    spec = COSTUMES[key]
    name, letter = spec['char'], key.split('_')[1].upper()
    own = 'CHAR_%s_%s' % (name, letter)
    # the fighter
    t = Model.load(os.path.join(asset, 'CHAR_%s.xxx' % name))
    old_container = [s for s, _ in t.names if s.startswith('CHAR_%s-' % name)][0]
    new_container = '%s-%08x' % (own, mk9_hash(own))
    rename_name(t, old_container, new_container, log)
    summary_lists_rename(t, {'CHAR_%s' % name: own, old_container: new_container}, log)
    s1 = take_look(t, asset, spec, spec['groups'], spec['donors'], log)
    hide_base_groups(t, ('Meshes', 'Animations'), log)
    n = costume_materials(t, s1, 'Materials', 'Materials', spec['colors'], log)
    path = os.path.join(out, own + '.xxx')
    log('%s: %d costume materials, %d bytes' % (own, n, t.save(path)))
    # the Babality
    baby_base = 'NPC_%s_Baby' % name
    baby = 'NPC_%s_%s_Baby' % (name, letter)
    t = Model.load(os.path.join(asset, baby_base + '.xxx'))
    old_container = [s for s, _ in t.names if s.startswith(baby_base + '-')][0]
    new_container = '%s-%08x' % (baby, mk9_hash(baby))
    rename_name(t, old_container, new_container, log)
    rename_name(t, baby_base, baby, log)
    rename_name(t, 'CHAR_%s' % name, own, log)
    summary_lists_rename(t, {baby_base: baby, 'CHAR_%s' % name: own, old_container: new_container}, log)
    # the retro baby script: a retro costume's, its names this one's
    src_pkg, swaps = spec['baby_script']
    src = Model.load(os.path.join(asset, src_pkg + '.xxx'))
    src.build_index()
    t.build_index()
    s_script = [src.path(i + 1) for i, x in enumerate(src.exports) if src.class_path(x['w'][0]).endswith('MKScriptBinary')][0]
    t_script = [t.path(i + 1) for i, x in enumerate(t.exports) if t.class_path(x['w'][0]).endswith('MKScriptBinary')][0]
    adopt(t, t_script, src, s_script, lambda q: q, native_ok=True)
    x = t.exports[t.export_by_path[t_script] - 1]
    d = x['data']
    for a, b in swaps:
        assert len(a) == len(b)
        d = d.replace(a, b)
    x['data'] = d
    log('  baby script from %s: %s' % (src_pkg, sorted(set(s for s in __import__('re').findall(rb'[\x20-\x7e]{6,}', d)
                                                          if b'Baby' in s or b'Babality' in s))))
    s1 = take_look(t, asset, spec, spec['baby_groups'], spec['baby_donors'], log)
    hide_base_groups(t, ('Meshes', 'Animations'), log)
    donor_own = [n for n, _ in s1.forced_packages if n.startswith('CHAR_')][0]
    n = costume_materials(t, s1, own, donor_own, spec['colors'], log)
    path = os.path.join(out, baby + '.xxx')
    log('%s: %d costume materials, %d bytes' % (baby, n, t.save(path)))


def main():
    asset, out = sys.argv[1], sys.argv[2]
    os.makedirs(out, exist_ok=True)
    for key in sys.argv[3:]:
        build(asset, out, key, log=lambda *a: print(*a) if not str(a[0]).startswith('  !') else None)


if __name__ == '__main__':
    main()
