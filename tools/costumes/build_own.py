"""The PS Vita's costumes with a mesh of their own (Kenshi B, Skarlet B): the character's default Xbox 360 package made
the costume's, its mesh (Meshes.<Char>, renamed Meshes.<Char>_B as the Vita's) and reflection converted from the Vita's
costume package, its materials (Materials.*) and textures the Vita's; the Xbox 360 default's physics, dismemberment
states and sockets kept (the sockets moved where the Vita's are), its materials the costume has none of hidden (the game
would put them on: the default's accessory damage), its textures no object uses any more made 4x4. The Babality: the
default baby package, its baby mesh (Meshes.<Char>_Baby, the name its script looks for) the Vita's costume baby's.

    python build_own.py <Asset folder> <Vita Asset folder> <out folder> <costume> [...]   (kenshi_b skarlet_b)
"""
import os
import struct
import sys

from build_default import rename_name, summary_lists_rename
from build_mk3 import Converter, look_guid, referred  # noqa: F401 (look_guid: the same GUIDs scheme)
from mk9hash import mk9_hash
from referencer import referenced, set_referenced
from tagged import walk
from transplant import Model
import tagged_le
from vitapkg import VitaPkg

OWN = {'kenshi_b': dict(char='Kenshi'), 'skarlet_b': dict(char='Skarlet')}


def rename_export(t, path, new_name, log):
    """An export's own name (not the name table's string: other objects may share it)."""
    x = t.exports[t.export_by_path[path] - 1]
    x['w'] = list(x['w'])
    x['w'][3] = t.add_name(new_name)
    x['w'][4] = 0
    log('  %s -> %s' % (path, new_name))


def hide(t, path, log):
    x = t.exports[t.export_by_path[path] - 1]
    x['w'] = list(x['w'])
    x['w'][3] = t.add_name(t.name(x['w'][3]) + '_Unused')
    log('  %s hidden' % path)


def move_sockets(t, v, t_set, v_set, log):
    """The sockets of the Xbox 360 set placed where the Vita's of the same name are."""
    vs = {}
    for i, x in enumerate(v.exports):
        if v.obj_name(x['class']) == 'SkeletalMeshSocket' and v.path(i + 1).startswith(v_set + '.'):
            w, _ = tagged_le.walk(x['data'], v.names)
            props = {q['name']: q for q in w.props}
            if 'm_SocketName' in props:
                vs[props['m_SocketName']['value']] = x['data'], props
    names = [n for n, _ in t.names]
    moved, only_vita = [], set(vs)
    for i, x in enumerate(t.exports):
        if not (t.class_path(x['w'][0]).endswith('SkeletalMeshSocket') and t.path(i + 1).startswith(t_set + '.')):
            continue
        w, _ = walk(x['data'], names)
        props = {q['name']: q for q in w.props}
        sname = props.get('m_SocketName', {}).get('value')
        if sname not in vs:
            continue
        only_vita.discard(sname)
        vd, vprops = vs[sname]
        d = bytearray(x['data'])
        for key, fmt in (('m_RelativeLocation', 'fff'), ('m_RelativeRotation', 'iii')):
            if key in props and key in vprops:
                vals = struct.unpack_from('<' + fmt, vd, vprops[key]['pos'] + 32)
                struct.pack_into('>' + fmt, d, props[key]['pos'] + 32, *vals)
        if 'BoneName' in props and 'BoneName' in vprops:
            bone = vprops['BoneName']['value']
            struct.pack_into('>ii', d, props['BoneName']['pos'] + 24, t.add_name(bone), 0)
        x['data'] = bytes(d)
        moved.append(sname)
    log('  sockets placed as the Vita\'s: %d (%s); only in the Vita\'s set: %s' % (len(moved), ', '.join(moved[:6]),
                                                                                   sorted(only_vita)))


def build_fighter(asset, vita_asset, out, key, log=print):
    char = OWN[key]['char']
    costume = 'CHAR_%s_B' % char
    t = Model.load(os.path.join(asset, 'CHAR_%s.xxx' % char))
    old_container = [s for s, _ in t.names if s.startswith('CHAR_%s-' % char)][0]
    new_container = '%s-%08x' % (costume, mk9_hash(costume))
    rename_name(t, old_container, new_container, log)
    summary_lists_rename(t, {'CHAR_%s' % char: costume, old_container: new_container}, log)
    t.build_index()
    v = VitaPkg.load(os.path.join(vita_asset, costume + '.xxx'))
    vpaths = {v.path(i + 1): v.obj_name(x['class']) for i, x in enumerate(v.exports)}
    old = referenced(t)
    # the mesh and its socket set keep the Xbox 360 names (Meshes.<Char>: the game finds a costume's own mesh by it;
    # under the Vita's name, Meshes.<Char>_B, the fight's start called into a null object): the Vita's go there
    renames = {'Meshes.%s_B' % char: 'Meshes.%s' % char, 'Meshes.%s_BSocketSet' % char: 'Meshes.%sSocketSet' % char}
    # the default's materials the costume has none of: hidden (the body's kept)
    for p in sorted(p for p in t.export_by_path if p.startswith('Materials.')):
        cls = t.class_path(t.exports[t.export_by_path[p] - 1]['w'][0])
        if 'MaterialInstance' in cls and p not in vpaths and not p.split('.')[-1].startswith(('Stump', 'Interior')):
            hide(t, p, log)
    t.build_index()
    c = Converter(t, v, log, renames)
    # the costume's textures and material instances
    for p, cls in vpaths.items():
        if p.startswith('Textures.') and cls == 'Texture2D':
            if p not in t.export_by_path:
                template = [q for q in t.export_by_path if q.startswith('Textures.')
                            and t.class_path(t.exports[t.export_by_path[q] - 1]['w'][0]).endswith('Texture2D')][0]
                c.new_export(p, template)
            c.texture(p, p)
    for p, cls in vpaths.items():
        if p.startswith('Materials.') and cls == 'MaterialInstanceConstant':
            if p not in t.export_by_path:
                c.new_export(p, 'Materials.Character_MIC')
            c.tagged_only(p, p)
    move_sockets(t, v, 'Meshes.%sSocketSet' % char, 'Meshes.%s_BSocketSet' % char, log)
    for name in ('%s_B' % char, 'Reflection'):
        c.mesh('Meshes.' + name, c.tp('Meshes.' + name))
    # the default's textures no object uses now
    candidates = {t.export_by_path[p] for p in t.export_by_path if p.startswith('Textures.')
                  and t.class_path(t.exports[t.export_by_path[p] - 1]['w'][0]).endswith('Texture2D')}
    used = referred(t, candidates)
    dead = [k for k in sorted(candidates) if k not in used]
    for k in dead:
        c.stub_texture(t.path(k))
    log('  %d textures of the default unused, made 4x4' % len(dead))
    keep = [r for r in old if r not in dead]
    added = [t.export_by_path[p] for p in t.export_by_path if p.startswith(('Textures.', 'Materials.'))
             and t.export_by_path[p] not in old and t.export_by_path[p] not in dead and 'Unused' not in p]
    set_referenced(t, keep + added)
    path = os.path.join(out, costume + '.xxx')
    log('%s: %d bytes' % (costume, t.save(path)))
    return path


def build_baby(asset, vita_asset, out, key, log=print):
    char = OWN[key]['char']
    costume = 'CHAR_%s_B' % char
    baby_base, baby = 'NPC_%s_Baby' % char, 'NPC_%s_B_Baby' % char
    t = Model.load(os.path.join(asset, baby_base + '.xxx'))
    old_container = [s for s, _ in t.names if s.startswith(baby_base + '-')][0]
    new_container = '%s-%08x' % (baby, mk9_hash(baby))
    rename_name(t, old_container, new_container, log)
    rename_name(t, baby_base, baby, log)
    rename_name(t, 'CHAR_%s' % char, costume, log)
    summary_lists_rename(t, {baby_base: baby, 'CHAR_%s' % char: costume, old_container: new_container}, log)
    t.build_index()
    v = VitaPkg.load(os.path.join(vita_asset, baby + '.xxx'))
    vpaths = {v.path(i + 1): v.obj_name(x['class']) for i, x in enumerate(v.exports)}
    old = referenced(t)
    # the baby mesh: the Vita's (Meshes.<Char>_B_Baby) under the name the baby's script looks for
    c = Converter(t, v, log)
    c.mesh('Meshes.%s_B_Baby' % char, 'Meshes.%s_Baby' % char)
    for p, cls in vpaths.items():
        if cls == 'Texture2D' and p.startswith(costume + '.Textures.'):
            if p not in t.export_by_path:
                template = [q for q in t.export_by_path if q.startswith(costume + '.Textures.')][0]
                c.new_export(p, template)
            c.texture(p, p)
    for p, cls in vpaths.items():
        if cls == 'MaterialInstanceConstant' and p.startswith((costume + '.Materials.', 'Materials.')):
            if p not in t.export_by_path:
                raise LookupError('%s: no such material instance in the Xbox 360 Babality' % p)
            c.tagged_only(p, p)
    candidates = {t.export_by_path[p] for p in t.export_by_path if p.startswith(costume + '.Textures.')}
    used = referred(t, candidates)
    dead = [k for k in sorted(candidates) if k not in used]
    for k in dead:
        c.stub_texture(t.path(k))
    keep = [r for r in old if r not in dead]
    added = [t.export_by_path[p] for p in t.export_by_path if p.startswith(costume + '.Textures.')
             and t.export_by_path[p] not in old and t.export_by_path[p] not in dead]
    set_referenced(t, keep + added)
    path = os.path.join(out, baby + '.xxx')
    log('%s: %d bytes (%d textures of the default unused, made 4x4)' % (baby, t.save(path), len(dead)))
    return path


def main():
    asset, vita_asset, out = sys.argv[1], sys.argv[2], sys.argv[3]
    os.makedirs(out, exist_ok=True)
    for key in sys.argv[4:]:
        build_fighter(asset, vita_asset, out, key)
        build_baby(asset, vita_asset, out, key)


if __name__ == '__main__':
    main()
