"""Rain's MK3 costume (E): the Xbox 360 game has no retro costume of his to start from (its Rain is the default one,
with its own mesh), so his default packages take the MK2 look (transplanted from Smoke D and Noob D, as
build_default.py does for Skarlet), their own mesh and animations hidden, and that look is then turned into the Vita's
MK3 one as for the others (build_mk3.py). The Babality: his default baby package with a retro baby script (Noob D's,
a 4-letter character's, its names Rain E's), the MK2 baby look transplanted, then made MK3 the same way.

    python build_rain.py <Asset folder> <Vita Asset folder> <out folder>
"""
import os
import struct
import sys

from build_costume import adopt
from build_default import hide_base_groups, rename_name, summary_lists_rename, take_look
from build_mk3 import (Converter, convert_look, costume_into_look, look_guid, new_group, rename_look, stub_unused)
from mk9hash import mk9_hash
from referencer import referenced, set_referenced
from tagged import walk
from transplant import Model
from vitapkg import VitaPkg

DROP, NEW = 'RetroNinja_Male_MK2', 'RetroNinja_Male_MK3'
LOOK = 'CHAR_%s_Rain' % NEW
OWN = 'CHAR_Rain_E'


def hide(t, path, log):
    x = t.exports[t.export_by_path[path] - 1]
    x['w'] = list(x['w'])
    x['w'][3] = t.add_name(t.name(x['w'][3]) + '_Unused')
    log('  %s hidden' % path)


def build_fighter(asset, vita_asset, out, log=print):
    t = Model.load(os.path.join(asset, 'CHAR_Rain.xxx'))
    old_container = [s for s, _ in t.names if s.startswith('CHAR_Rain-')][0]
    new_container = '%s-%08x' % (OWN, mk9_hash(OWN))
    rename_name(t, old_container, new_container, log)
    summary_lists_rename(t, {'CHAR_Rain': OWN, old_container: new_container}, log)
    take_look(t, asset, None, ('CHAR_' + DROP, DROP + '_IKSockets'), ('CHAR_Smoke_D', 'CHAR_Noob_D'), log)
    hide_base_groups(t, ('Meshes', 'Animations'), log)
    v = VitaPkg.load(os.path.join(vita_asset, OWN + '.xxx'))
    t.build_index()
    # the default costume's own material instances the MK3 costume has none of (its accessories'): hidden, as for
    # Skarlet's (the game finds a costume's by name); the body's (Stump, Interior) kept
    vroot = {v.path(i + 1) for i in range(len(v.exports)) if v.path(i + 1).startswith('Materials.')}
    for p in sorted(p for p in t.export_by_path if p.startswith('Materials.')):
        cls = t.class_path(t.exports[t.export_by_path[p] - 1]['w'][0])
        if 'MaterialInstance' in cls and p not in vroot and not p.split('.')[-1].startswith(('Stump', 'Interior')):
            hide(t, p, log)
    t.build_index()
    rename_look(t, DROP, NEW, LOOK, log)
    k = [n for n, _ in t.forced_packages].index('CHAR_' + DROP)
    t.forced_packages[k] = (LOOK, look_guid(LOOK))
    old = referenced(t)
    c = convert_look(t, v, 'CHAR_' + NEW, LOOK, log)
    dead = stub_unused(t, c, LOOK, log)
    keep = [r for r in old if r not in dead]
    added = [t.export_by_path[p] for p in t.export_by_path if p.startswith(LOOK + '.')
             and t.export_by_path[p] not in old and t.export_by_path[p] not in dead
             and 'Material' in t.class_path(t.exports[t.export_by_path[p] - 1]['w'][0])]
    set_referenced(t, keep + added)
    path = os.path.join(out, OWN + '.xxx')
    log('%s: %d bytes; ini: SourceAsset=%s MeshName=%s' % (OWN, t.save(path), LOOK, NEW))
    return path


def build_baby(asset, vita_asset, out, log=print):
    baby_base, baby = 'NPC_Rain_Baby', 'NPC_Rain_E_Baby'
    t = Model.load(os.path.join(asset, baby_base + '.xxx'))
    old_container = [s for s, _ in t.names if s.startswith(baby_base + '-')][0]
    new_container = '%s-%08x' % (baby, mk9_hash(baby))
    rename_name(t, old_container, new_container, log)
    rename_name(t, baby_base, baby, log)
    rename_name(t, 'CHAR_Rain', OWN, log)
    summary_lists_rename(t, {baby_base: baby, 'CHAR_Rain': OWN, old_container: new_container}, log)
    # the retro baby script: Noob D's (an MK2 retro costume of a 4-letter character), its names Rain E's
    src = Model.load(os.path.join(asset, 'NPC_Noob_D_Baby.xxx'))
    src.build_index()
    t.build_index()
    s_script = [src.path(i + 1) for i, x in enumerate(src.exports)
                if src.class_path(x['w'][0]).endswith('MKScriptBinary')][0]
    t_script = [t.path(i + 1) for i, x in enumerate(t.exports) if t.class_path(x['w'][0]).endswith('MKScriptBinary')][0]
    adopt(t, t_script, src, s_script, lambda q: q, native_ok=True)
    x = t.exports[t.export_by_path[t_script] - 1]
    d = x['data']
    for a, b in ((b'Noob', b'Rain'), (b'noob', b'rain'), (b'NOOB', b'RAIN'), (b'_D_Baby', b'_E_Baby'),
                 (b'_D_BABY', b'_E_BABY'), (DROP.encode(), NEW.encode())):
        d = d.replace(a, b)
    x['data'] = d
    take_look(t, asset, None, ('NPC_%s_Baby' % DROP, 'CHAR_' + DROP), ('NPC_Smoke_D_Baby', 'NPC_Noob_D_Baby'), log)
    hide_base_groups(t, ('Meshes', 'Animations'), log)
    v = VitaPkg.load(os.path.join(vita_asset, baby + '.xxx'))
    t.build_index()
    rename_look(t, DROP, NEW, LOOK, log)
    for k, (n, g) in enumerate(t.forced_packages):
        if n == 'CHAR_' + DROP:
            t.forced_packages[k] = (LOOK, look_guid(LOOK))
        elif n == 'NPC_%s_Baby' % DROP:
            t.forced_packages[k] = ('NPC_%s_Baby' % NEW, look_guid('NPC_%s_Baby' % NEW))
    old = referenced(t)
    c = Converter(t, v, log, {'CHAR_' + NEW: LOOK})
    baby_look = 'NPC_%s_Baby' % NEW
    c.mesh('%s.Meshes.%s_Baby' % (baby_look, NEW), '%s.Meshes.%s_Baby' % (baby_look, NEW))
    new_group(c, OWN + '.Textures', OWN + '.Materials')
    for i, y in enumerate(v.exports):
        p = v.path(i + 1)
        if v.obj_name(y['class']) == 'Texture2D' and p.startswith((OWN + '.Textures.', 'CHAR_%s.Textures.' % NEW)):
            tp = c.tp(p)
            if tp not in t.export_by_path:
                c.new_export(tp, LOOK + '.Textures.%s_Spec' % NEW)
            c.texture(p, tp)
    # the default baby's own material instances by other names (Materials.Character): hidden; the Vita's made
    for p in ('Materials.Character', 'Materials.Character_AP'):
        if p in t.export_by_path:
            hide(t, p, log)
    for i, y in enumerate(v.exports):
        p = v.path(i + 1)
        if v.obj_name(y['class']) != 'MaterialInstanceConstant':
            continue
        if p.startswith((baby_look + '.Materials.', 'CHAR_%s.Materials.' % NEW, OWN + '.Materials.', 'Materials.')):
            tp = c.tp(p)
            if tp not in t.export_by_path:
                c.new_export(tp, 'Materials.Baby_MIC' if tp.startswith('Materials.') else OWN + '.Materials.Character_MIC')
            c.tagged_only(p, tp)
    costume_into_look(t, LOOK, log, prefix=OWN + '.Materials.')
    dead = stub_unused(t, c, LOOK, log)
    keep = [r for r in old if r not in dead]
    added = [t.export_by_path[p] for p in t.export_by_path
             if p.startswith((OWN + '.Textures.', 'Materials.')) and t.export_by_path[p] not in old
             and 'Unused' not in p]
    set_referenced(t, keep + added)
    path = os.path.join(out, baby + '.xxx')
    log('%s: %d bytes; ini: SourceAsset=%s MeshName=%s_Baby' % (baby, t.save(path), baby_look, NEW))
    return path


def main():
    asset, vita_asset, out = sys.argv[1], sys.argv[2], sys.argv[3]
    os.makedirs(out, exist_ok=True)
    build_fighter(asset, vita_asset, out)
    build_baby(asset, vita_asset, out)


if __name__ == '__main__':
    main()
