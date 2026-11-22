"""The PS Vita's MK3 retro costumes (Scorpion, Sub-Zero, Reptile, Ermac, Smoke, Noob E) for the Xbox 360 game: the
character's costume package with a retro look (C: MK1, D: MK2) made the E costume's, its look turned into the MK3 one
in place (the look's names MK1/MK2 -> MK3; mesh, reflection, materials and textures converted from the Vita's
package; the Xbox 360 look's physics, dismemberment states and sockets kept, the swords' sockets moved where the MK3
look carries them), and the costume's own materials and textures (Materials.*, Textures.<Char>_E_*) the Vita's.

    python build_mk3.py <Asset folder> <Vita Asset folder> <out folder> <costume> [...]   (scorpion_e ...)
"""
import hashlib
import os
import re
import struct
import sys

from build_costume import rename
from pkgio import Package
from referencer import referenced, set_referenced
from tagged import walk
from transplant import Model
import tagged_le
from vita_convert import ARRAY_ELEMENTS, Ctx, Out, convert_skelmesh, convert_tagged, tag, texture_native, vita_levels
from vitapkg import VitaPkg

MK3 = {
    'scorpion_e': dict(char='Scorpion', base='c', drop='MK1'),
    'subzero_e': dict(char='SubZero', base='c', drop='MK1'),
    'reptile_e': dict(char='Reptile', base='c', drop='MK1'),
    'ermac_e': dict(char='Ermac', base='c', drop='MK1'),
    'smoke_e': dict(char='Smoke', base='d', drop='MK2'),
    'noob_e': dict(char='Noob', base='d', drop='MK2'),
}

# The costume's own pieces the fighter's script names by the costume's letter (found by diffing the scripts' strings):
# the mask's mesh Scorpion and Reptile pull off, its material (in the costume's FX group). Names renamed in the package
# (an FX group's in the forced packages too) and in the script (same length); the meshes and materials converted from
# the Vita's ('{c}': the costume's seekfree container).
PIECES = {
    'scorpion_e': dict(names={'SC_C_Cowl_Sk': 'SC_E_Cowl_Sk', 'fx_scorpion_c': 'fx_scorpion_e',
                              'SC_Retro_Cowl_MIC': 'SC_E_Cowl_MIC'},
                       meshes=['{c}.SC_E_Cowl_Sk'], mics=['fx_scorpion_e.Materials.SC_E_Cowl_MIC'],
                       script={'SC_C_Cowl_Sk': 'SC_E_Cowl_Sk', 'fx_scorpion_c': 'fx_scorpion_e'}),
    'reptile_e': dict(names={'RE_C_Cowl_Sk': 'RE_E_Cowl_Sk', 'FX_Reptile_C': 'FX_Reptile_E',
                             'RE_C_Cowl_MIC': 'RE_E_Cowl_MIC', 'RE_C_Cowl_Sk_Physics': 'RE_E_Cowl_Sk_Physics'},
                      meshes=['{c}.RE_E_Cowl_Sk'], mics=['fx_reptile_e.Materials.RE_E_Cowl_MIC'],
                      script={'fx_reptile_c': 'fx_reptile_e'}),
}

# the Xbox 360 look's damage textures take the Vita's names
TEXTURE_RENAMES = {'_DMG_Diff': '_Diff_Damage', '_DMG_Spec': '_Spec_Damage'}
SWORDS = ('Sword_Left', 'Sword_Right')


def vita_forced_guid(path, package):
    """The GUID a Vita package's summary gives a forced package, as the Xbox 360's 16 bytes (four ints)."""
    raw = open(path, 'rb').read(0x10000)
    key = package.encode('latin-1') + b'\0'
    k = raw.find(struct.pack('<i', len(key)) + key)
    assert k >= 0, 'no %s in the forced packages' % package
    g = raw[k + 4 + len(key):k + 4 + len(key) + 16]
    return struct.pack('>4I', *struct.unpack('<4I', g))


def rename_look(t, drop, new, package, log):
    """Every name of the dropped look (RetroNinja_Male_MK1...) the new look's; the look's package (CHAR_<drop>) gets
    `package`, the costume's own."""
    done = []
    for i, (s, flags) in enumerate(t.names):
        if drop not in s:
            continue
        n = package if s == 'CHAR_' + drop else s.replace(drop, new)
        for a, b in TEXTURE_RENAMES.items():
            if n.endswith(a):
                n = n[:-len(a)] + b
        assert n not in t.name_index, ('the new name is in the package already', n)
        t.names[i][0] = n
        done.append((s, n))
    t.name_index = {s: i for i, (s, _) in enumerate(t.names)}
    t.build_index()
    log('  %d names renamed (%s ...)' % (len(done), ', '.join('%s -> %s' % d for d in done[:3])))


class Converter:
    def __init__(self, t, v, log, renames=None):
        self.t, self.v, self.log = t, v, log
        self.renames = renames or {}  # a Vita path's first part -> the Xbox 360 package's
        t.build_index()
        self.ctx = Ctx(v.names, self.name, self.obj)

    def tp(self, vpath):
        if vpath in self.renames:  # a whole path renamed
            return self.renames[vpath]
        head, _, rest = vpath.partition('.')
        head = self.renames.get(head, head)
        return head + ('.' + rest if rest else '')

    def name(self, s):
        return self.t.add_name(s)

    def obj(self, ref):
        if ref == 0:
            return 0
        p = self.tp(self.v.path(ref))
        t = self.t
        r = t.export_by_path.get(p) or t.import_by_path.get(p)
        if r is None:
            raise LookupError('%s: not in the Xbox 360 package' % p)
        return r

    def vita_export(self, path):
        for i, x in enumerate(self.v.exports):
            if self.v.path(i + 1) == path:
                return x
        raise LookupError('no %s in the Vita package' % path)

    def new_export(self, path, template, eflags=None):
        """A new export at `path` (its outer existing), its entry like `template`'s (`eflags`: its export flags
        instead, 0 for an object of this package, 1 for a forced export)."""
        t = self.t
        tx = t.exports[t.export_by_path[template] - 1]
        outer, _, name = path.rpartition('.')
        w = list(tx['w'])
        w[2] = t.export_by_path[outer] if outer else 0
        w[3] = t.add_name(name)
        w[4] = 0
        w[8] = w[9] = w[10] = 0
        t.exports.append({'w': w, 'comps': [], 'eflags': tx['eflags'] if eflags is None else eflags,
                          'tail': tx['tail'], 'data': b'', 'bulk': []})
        t.export_by_path[path] = len(t.exports)
        self.log('  + %s (like %s)' % (path, template.rsplit('.', 1)[-1]))
        return len(t.exports)

    def put(self, path, data, bulk=()):
        x = self.t.exports[self.t.export_by_path[path] - 1]
        x['data'] = bytes(data)
        x['bulk'] = list(bulk)
        x.pop('orig', None)  # placed anew, its bulk offsets set when saved

    def tagged_only(self, vpath, path, **kw):
        """An object made of tagged properties only (material instances, sockets), converted."""
        x = self.vita_export(vpath)
        out = Out()
        end = convert_tagged(self.ctx, x['data'], 0, out, **kw)
        assert end == len(x['data']), ('%s: native data after the properties' % vpath)
        self.put(path, out.b)

    def tag_value(self, path, prop):
        """The (first) value word of an Xbox 360 object's tagged property."""
        t = self.t
        x = t.exports[t.export_by_path[path] - 1]
        w, _ = walk(x['data'], [n for n, _ in t.names])
        for q in w.props:
            if q['name'] == prop:
                return struct.unpack_from('>i', x['data'], q['pos'] + 24)[0]
        return None

    def mesh(self, vpath, path):
        t = self.t
        phys = self.tag_value(path, 'PhysicsAsset') or 0
        # the cloth (PhysX data cooked for the Vita) is left out, but the mesh keeps a cloth set: the Xbox 360 mesh's,
        # emptied (no descriptors; the Vita's reflections have such): a fighter with its own mesh called into a null
        # one at the fight's start
        cloth = self.tag_value(path, 'ClothSet') or 0
        if cloth > 0:
            self.put(t.path(cloth), struct.pack('>ii', t.add_name('None'), 0))
        x = self.vita_export(vpath)
        _, end = tagged_le.walk(x['data'], self.v.names)
        out = Out()
        # the Xbox 360 mesh's physics asset, when it has one (none set when it has none: the Babality's baby
        # crashed with an explicit None)
        extra = tag(t.add_name('PhysicsAsset'), t.add_name('ObjectProperty'), 4, struct.pack('>i', phys)) if phys else b''
        convert_tagged(self.ctx, x['data'], 0, out, drop={'SphereProxy'},
                       replace={'ClothSet': lambda o, d, v, size: o.i32(cloth if cloth > 0 else 0)}, extra=extra)
        head = len(out.b)
        native, left = convert_skelmesh(self.ctx, x['data'], end)
        self.put(path, out.b + native.b, [head + p for p in native.bulk])
        self.log('  mesh %s: %d bytes (physics %s; the Vita\'s %d bytes of tables left out)' % (
            path, head + len(native.b), t.path(phys) if phys else None, left))

    def texture(self, vpath, path):
        t = self.t
        x = self.vita_export(vpath)
        _, end = tagged_le.walk(x['data'], self.v.names)
        levels, w, h = vita_levels(x['data'], end)
        native, tb = texture_native(levels, w, h)
        out = Out()
        replace = {'SizeX': lambda o, d, v, s: o.i32(w), 'SizeY': lambda o, d, v, s: o.i32(h),
                   'MipTailBaseIdx': lambda o, d, v, s: o.i32(tb)}
        convert_tagged(self.ctx, x['data'], 0, out, drop={'SRGB'}, replace=replace)
        head = len(out.b)
        self.put(path, out.b + native.b, [head + p for p in native.bulk])
        self.log('  texture %s: %dx%d, %d levels, tail at %d' % (path, w, h, len(levels), tb))

    def stub_texture(self, path):
        """A texture nothing uses any more, made 4x4 (one block: the look's one it replaces is megabytes)."""
        t = self.t
        x = t.exports[t.export_by_path[path] - 1]
        w, end = walk(x['data'], [n for n, _ in t.names])
        d = bytearray(x['data'][:end])
        for q in w.props:
            if q['name'] in ('SizeX', 'SizeY'):
                struct.pack_into('>i', d, q['pos'] + 24, 4)
            elif q['name'] == 'MipTailBaseIdx':
                struct.pack_into('>i', d, q['pos'] + 24, 0)
        native, _ = texture_native([bytes(16)], 4, 4)
        self.put(path, bytes(d) + native.b, [len(d) + p for p in native.bulk])


def mic_params(t, data):
    """A material instance's parameters: {(kind, name): (offset of the value, size)}; kind texture/vector/scalar."""
    w, end = walk(data, [n for n, _ in t.names])
    out, cur = {}, None
    for q in w.props:
        if q['name'].endswith('].ParameterName'):
            cur = q.get('value')
        elif q['name'].endswith('].ParameterValue') and cur:
            kind = q['name'].split('ParameterValues')[0].lower()
            at = q['pos'] + 24 + (8 if q['type'] == 'StructProperty' else 0)
            out[(kind, cur)] = (at, q['size'])
            cur = None
    return out


def costume_into_look(t, look, log, prefix='Materials.'):
    """The look's material instances take the costume's values (its textures, its colors). The Xbox 360 game draws a
    fighter with instances made from the look's, and copies only the colors of the costume's own (Materials.*) into
    them: its textures never replace the look's. The Vita's costumes set their own textures (Textures.<Char>_E_*) in
    their Materials.*; here the look (this costume's own: CHAR_<look>_<Char>) has them."""
    done = 0
    names = [n for n, _ in t.names]
    for i, x in enumerate(t.exports):
        p = t.path(i + 1)
        if not p.startswith(prefix) or not t.class_path(x['w'][0]).endswith('MaterialInstanceConstant'):
            continue
        w, _ = walk(x['data'], names)
        parent = [struct.unpack_from('>i', x['data'], q['pos'] + 24)[0] for q in w.props if q['name'] == 'Parent']
        if not parent or parent[0] <= 0 or not t.path(parent[0]).startswith(look + '.Materials.'):
            continue
        y = t.exports[parent[0] - 1]
        base = bytearray(y['data'])
        own = mic_params(t, x['data'])
        theirs = mic_params(t, bytes(base))
        for key, (at, size) in own.items():
            if key not in theirs:
                # a color the look's instance has not: the game copies the costume's colors itself
                if key[0] != 'texture':
                    continue
                raise LookupError('%s: %s %s not in %s' % (p, key[0], key[1], t.path(parent[0])))
            bat, bsize = theirs[key]
            assert bsize == size
            base[bat:bat + size] = x['data'][at:at + size]
        y['data'] = bytes(base)
        y['bulk'] = []
        y.pop('orig', None)
        done += 1
    log("  %d material instances of the look with the costume's values" % done)


def reflection_diffuse(t, look, log):
    """The fighter's reflection on shiny floors: the Vita's look's instances set no texture (a grey figure); the Xbox
    360 look's are kept, their diffuse map the costume's (the look's Character instance's, as costume_into_look set
    it)."""
    for slot, source in (('Reflection_MIC', 'Character_MIC'), ('Reflection_AP_MIC', 'Character_AP_MIC')):
        dst = t.export_by_path.get('%s.Materials.%s' % (look, slot))
        src = t.export_by_path.get('%s.Materials.%s' % (look, source))
        if not dst or not src:
            continue
        sp = mic_params(t, t.exports[src - 1]['data']).get(('texture', 'DiffuseMap'))
        x = t.exports[dst - 1]
        dp = mic_params(t, x['data']).get(('texture', 'DiffuseMap'))
        if not sp or not dp:
            continue
        tex = struct.unpack_from('>i', t.exports[src - 1]['data'], sp[0])[0]
        d = bytearray(x['data'])
        struct.pack_into('>i', d, dp[0], tex)
        x['data'] = bytes(d)
        x['bulk'] = []
        x.pop('orig', None)
        log('  %s: diffuse map %s' % (slot, t.path(tex)))


def referred(t, candidates):
    """Which of the candidate exports (textures) some object refers to: tagged references; native data only of the
    classes that hold references in it (not animations, textures or shader caches, whose bytes match by chance)."""
    used = set()
    names = [n for n, _ in t.names]
    for i, x in enumerate(t.exports):
        cls = t.class_path(x['w'][0]).split('.')[-1]
        if cls in ('ObjectReferencer',) or '_Unused' in t.path(i + 1):  # hidden objects use nothing
            continue
        try:
            w, end = walk(x['data'], names)
            refs = [struct.unpack_from('>i', x['data'], r.pos)[0] for r in w.refs if r.kind == 'object']
            raw = b''.join(x['data'][a:b] for a, b, name in w.raw if ARRAY_ELEMENTS.get(name.split('.')[-1]) == 'o')
            native = x['data'][end:]
        except (struct.error, IndexError, ValueError, KeyError):
            refs, raw, native = [], b'', x['data']
        used.update(r for r in refs if r in candidates)
        scan = raw + (native if cls in ('Material', 'MaterialInstanceConstant', 'MaterialInstanceTimeVarying',
                                        'StaticMesh', 'ParticleSystem') else b'')
        for k in candidates:
            if struct.pack('>i', k) in scan:
                used.add(k)
    return used


def convert_look(t, v, look_v, look, log):
    """The package's look (its group, already renamed `look`) turned into the Vita's (`look_v`), in place."""
    c = Converter(t, v, log, {look_v: look})
    new_look = look_v[len('CHAR_'):]
    # meshes
    for name in (new_look, 'Reflection'):
        p = '%s.Meshes.%s' % (look_v, name)
        c.mesh(p, c.tp(p))
    # the look's material instances
    vmics = [v.path(i + 1) for i, x in enumerate(v.exports)
             if v.path(i + 1).startswith(look_v + '.Materials.') and v.obj_name(x['class']) == 'MaterialInstanceConstant']
    for p in vmics:
        if c.tp(p) not in t.export_by_path:
            c.new_export(c.tp(p), look + '.Materials.Eyes_MIC')
    # the look's textures
    vtex = [v.path(i + 1) for i, x in enumerate(v.exports)
            if v.path(i + 1).startswith(look_v + '.Textures.') and v.obj_name(x['class']) == 'Texture2D']
    for p in vtex:
        if c.tp(p) not in t.export_by_path:
            raise LookupError('%s: no texture of that name in the Xbox 360 look' % c.tp(p))
    keep_x360 = ('Reflection_MIC', 'Reflection_AP_MIC')  # see reflection_diffuse
    for p in vmics:
        if p.split('.')[-1] in keep_x360 and c.tp(p) in t.export_by_path:
            continue
        c.tagged_only(p, c.tp(p))
    for p in vtex:
        c.texture(p, c.tp(p))
    # the costume's own textures and material instances (Textures.<Char>_E_*, Materials.*)
    for i, x in enumerate(v.exports):
        p = v.path(i + 1)
        cls = v.obj_name(x['class'])
        if p.startswith('Textures.') and cls == 'Texture2D' and p not in t.export_by_path:
            if 'Textures' not in t.export_by_path:  # the package's own textures group (Scorpion's has one)
                c.new_export('Textures', 'Materials', eflags=0)
                c.put('Textures', t.exports[t.export_by_path['Materials'] - 1]['data'])
            c.new_export(p, c.tp(vtex[0]), eflags=0)
            c.texture(p, p)
    for i, x in enumerate(v.exports):
        p = v.path(i + 1)
        if p.startswith('Materials.') and v.obj_name(x['class']) == 'MaterialInstanceConstant':
            if p not in t.export_by_path:
                c.new_export(p, 'Materials.Character_MIC')
            c.tagged_only(p, p)
    costume_into_look(t, look, log)
    reflection_diffuse(t, look, log)
    # the swords' sockets where the MK3 look carries them
    vs = {}
    for i, x in enumerate(v.exports):
        if v.obj_name(x['class']) == 'SkeletalMeshSocket' and v.path(i + 1).startswith(look_v + '.'):
            w, _ = tagged_le.walk(x['data'], v.names)
            props = {q['name']: q for q in w.props}
            if props.get('m_SocketName', {}).get('value') in SWORDS:
                vs[props['m_SocketName']['value']] = x['data'], props
    names = [n for n, _ in t.names]
    for i, x in enumerate(t.exports):
        if not (t.class_path(x['w'][0]).endswith('SkeletalMeshSocket') and t.path(i + 1).startswith(look + '.')):
            continue
        w, _ = walk(x['data'], names)
        props = {q['name']: q for q in w.props}
        sname = props.get('m_SocketName', {}).get('value')
        if sname not in vs:
            continue
        vd, vprops = vs[sname]
        d = bytearray(x['data'])
        for key, fmt in (('m_RelativeLocation', 'fff'), ('m_RelativeRotation', 'iii')):
            vals = struct.unpack_from('<' + fmt, vd, vprops[key]['pos'] + 32)
            struct.pack_into('>' + fmt, d, props[key]['pos'] + 32, *vals)
        x['data'] = bytes(d)
        loc = struct.unpack_from('>3f', d, props['m_RelativeLocation']['pos'] + 32)
        log('  socket %s: %s' % (sname, tuple(round(f, 1) for f in loc)))
    return c


def stub_unused(t, c, look, log):
    """The look's textures no object refers to any more (the look's own diffuse maps, the Xbox 360 look's normal maps
    and masks) made 4x4; their exports, for the referencer."""
    candidates = {i + 1 for i in range(len(t.exports)) if t.path(i + 1).startswith(look + '.Textures.')}
    used = referred(t, candidates)
    dead = []
    for k in sorted(candidates):
        if k not in used:
            c.stub_texture(t.path(k))
            dead.append(k)
    log('  %d textures of the look unused, made 4x4: %s' % (len(dead), ', '.join(t.path(k).split('.')[-1] for k in dead)))
    kept = sorted(t.path(k).split('.')[-1] for k in candidates if k in used)
    log('  textures of the look in use: %s' % ', '.join(kept))
    return dead


def rename_pieces(t, names, log):
    """The costume's pieces' names (matched ignoring case, as UE3 does), the forced packages' among them."""
    low = {a.lower(): b for a, b in names.items()}
    done = []
    for i, (s, flags) in enumerate(t.names):
        if s.lower() in low:
            t.names[i][0] = low[s.lower()]
            done.append('%s -> %s' % (s, low[s.lower()]))
    for k, (n, g) in enumerate(t.forced_packages):
        if n.lower() in low:
            t.forced_packages[k] = (low[n.lower()], look_guid(low[n.lower()]))
            done.append('forced %s' % n)
    t.name_index = {s: i for i, (s, _) in enumerate(t.names)}
    t.build_index()
    log('  pieces renamed: %s' % '; '.join(done))


def convert_pieces(t, v, look_v, look, spec, log):
    """The costume's pieces (a mask's mesh, its material) converted from the Vita's."""
    container = [p for p in t.export_by_path if '.' not in p and p.startswith('CHAR_') and '-' in p][0]
    vcontainer = [v.path(i + 1) for i in range(len(v.exports)) if '.' not in v.path(i + 1)
                  and v.path(i + 1).startswith('CHAR_') and '-' in v.path(i + 1)][0]
    tops = {p.split('.')[0].lower(): p.split('.')[0] for p in t.export_by_path}
    renames = {look_v: look, vcontainer: container}
    for p in spec['mics'] + spec['meshes']:
        head = p.split('.')[0]
        if head != '{c}' and head.lower() in tops:
            renames[head] = tops[head.lower()]
    c = Converter(t, v, log, renames)
    for p in spec['mics']:
        c.tagged_only(p, c.tp(p))
        log('  piece %s' % c.tp(p))
    for p in spec['meshes']:
        vp = p.replace('{c}', vcontainer)
        c.mesh(vp, c.tp(vp))
    for i, x in enumerate(t.exports):
        if not t.class_path(x['w'][0]).endswith('MKScriptBinary'):
            continue
        d = x['data']
        for a, b in spec['script'].items():
            assert len(a) == len(b)
            n = d.count(a.encode())
            d = d.replace(a.encode(), b.encode())
            log('  script %s: %d x %s -> %s' % (t.path(i + 1).split('.')[-1], n, a, b))
        x['data'] = d


def look_guid(package):
    """A forced package's GUID for a look made here: from its name (the same in every build)."""
    return hashlib.md5(('mk9-lauta ' + package).encode()).digest()


def build(asset, vita_asset, out, key, log=print):
    spec = MK3[key]
    name, base, new = spec['char'], spec['base'], key.split('_')[-1]
    drop_look = 'RetroNinja_Male_' + spec['drop']
    new_look = 'RetroNinja_Male_MK3'
    look = 'CHAR_%s_%s' % (new_look, name)  # the costume's own look package (see costume_into_look)
    char_base = os.path.join(asset, 'CHAR_%s_%s.xxx' % (name, base.upper()))
    vita_path = os.path.join(vita_asset, 'CHAR_%s_%s.xxx' % (name, new.upper()))
    tmp = os.path.join(out, '_renamed.xxx')
    char_out = os.path.join(out, 'CHAR_%s_%s.xxx' % (name, new.upper()))
    log('%s: %s' % (key, rename(char_base, tmp, name, base, new)))
    t = Model.load(tmp)
    v = VitaPkg.load(vita_path)
    rename_look(t, drop_look, new_look, look, log)
    k = [n for n, _ in t.forced_packages].index('CHAR_' + drop_look)
    t.forced_packages[k] = (look, look_guid(look))
    pieces = PIECES.get(key)
    if pieces:
        rename_pieces(t, pieces['names'], log)
    old = referenced(t)
    c = convert_look(t, v, 'CHAR_' + new_look, look, log)
    if pieces:
        convert_pieces(t, v, 'CHAR_' + new_look, look, pieces, log)
    dead = stub_unused(t, c, look, log)
    keep = [r for r in old if r not in dead]
    added = [t.export_by_path[p] for p in t.export_by_path if p.startswith(look + '.')
             and t.export_by_path[p] not in old and t.export_by_path[p] not in dead
             and 'Material' in t.class_path(t.exports[t.export_by_path[p] - 1]['w'][0])]
    set_referenced(t, keep + added)
    size = t.save(char_out)
    os.remove(tmp)
    log('  referencer %d -> %d; %d bytes; ini: SourceAsset=%s MeshName=%s' % (len(old), len(keep) + len(added), size,
                                                                            look, new_look))
    return char_out, look


def new_group(c, path, template):
    """A group (Package export) at `path`, its entry like the `template` group's."""
    if path not in c.t.export_by_path:
        c.new_export(path, template)
        c.put(path, c.t.exports[c.t.export_by_path[template] - 1]['data'])


def build_baby(asset, vita_asset, out, key, log=print):
    """The costume's Babality package: the base costume's renamed (its package and the costume package it names), the
    baby's look MK1/MK2 -> MK3 converted from the Vita's, the fighter's look under the costume's own name with the
    costume's textures (the baby's clothes are the fighter's material), the Babality's script naming the new mesh."""
    spec = MK3[key]
    name, base, new = spec['char'], spec['base'], key.split('_')[-1]
    drop_look = 'RetroNinja_Male_' + spec['drop']
    new_look = 'RetroNinja_Male_MK3'
    look = 'CHAR_%s_%s' % (new_look, name)
    costume = 'CHAR_%s_%s' % (name, new.upper())
    baby_base = os.path.join(asset, 'NPC_%s_%s_Baby.xxx' % (name, base.upper()))
    vita_path = os.path.join(vita_asset, 'NPC_%s_%s_Baby.xxx' % (name, new.upper()))
    tmp = os.path.join(out, '_renamed_baby.xxx')
    baby_out = os.path.join(out, 'NPC_%s_%s_Baby.xxx' % (name, new.upper()))
    log('%s baby: %s' % (key, rename(baby_base, tmp, name, base, new, baby=True)))
    q = Package.load(tmp)  # the Babality's package also names the character's costume package
    rx = re.compile(rb'(?i)(char_' + re.escape(name.lower().encode()) + rb'_)' + base.encode() + rb'(?![A-Za-z0-9-])')
    for buf in (q.summary, q.image):
        for m in list(rx.finditer(bytes(buf))):
            letter = buf[m.end() - 1:m.end()]
            buf[m.end() - 1:m.end()] = (new.upper() if letter.isupper() else new.lower()).encode()
    q.save(tmp)
    t = Model.load(tmp)
    v = VitaPkg.load(vita_path)
    rename_look(t, drop_look, new_look, look, log)
    for k, (n, g) in enumerate(t.forced_packages):
        if n == 'CHAR_' + drop_look:
            t.forced_packages[k] = (look, look_guid(look))
        elif n == 'NPC_%s_Baby' % drop_look:
            t.forced_packages[k] = ('NPC_%s_Baby' % new_look, look_guid('NPC_%s_Baby' % new_look))
    old = referenced(t)
    c = Converter(t, v, log, {'CHAR_' + new_look: look})
    baby_look = 'NPC_%s_Baby' % new_look
    c.mesh('%s.Meshes.%s_Baby' % (baby_look, new_look), '%s.Meshes.%s_Baby' % (baby_look, new_look))
    # the costume's textures (the X360 baby has none: the fighter's look is tinted)
    new_group(c, costume + '.Textures', costume + '.Materials')
    for i, x in enumerate(v.exports):
        p = v.path(i + 1)
        cls = v.obj_name(x['class'])
        if cls == 'Texture2D' and p.startswith((costume + '.Textures.', 'CHAR_%s.Textures.' % new_look)):
            tp = c.tp(p)
            if tp not in t.export_by_path:
                c.new_export(tp, look + '.Textures.%s_Spec' % new_look)
            c.texture(p, tp)
    for i, x in enumerate(v.exports):
        p = v.path(i + 1)
        if v.obj_name(x['class']) != 'MaterialInstanceConstant':
            continue
        if p.startswith((baby_look + '.Materials.', 'CHAR_%s.Materials.' % new_look, costume + '.Materials.',
                         'Materials.')):
            tp = c.tp(p)
            if tp not in t.export_by_path:
                raise LookupError('%s: no such material instance in the Xbox 360 Babality' % tp)
            c.tagged_only(p, tp)
    costume_into_look(t, look, log, prefix=costume + '.Materials.')
    old_b, new_b = drop_look.encode(), new_look.encode()
    for i, x in enumerate(t.exports):
        if t.class_path(x['w'][0]).endswith('MKScriptBinary') and old_b in x['data']:
            log('  script %s: %d x %s -> %s' % (t.path(i + 1), x['data'].count(old_b), drop_look, new_look))
            x['data'] = x['data'].replace(old_b, new_b)
    dead = stub_unused(t, c, look, log)
    keep = [r for r in old if r not in dead]
    added = [t.export_by_path[p] for p in t.export_by_path if p.startswith(costume + '.Textures.')
             and t.export_by_path[p] not in old]
    set_referenced(t, keep + added)
    size = t.save(baby_out)
    os.remove(tmp)
    log('  referencer %d -> %d; %d bytes; ini: SourceAsset=%s MeshName=%s_Baby' % (
        len(old), len(keep) + len(added), size, baby_look, new_look))
    return baby_out


def main():
    asset, vita_asset, out = sys.argv[1], sys.argv[2], sys.argv[3]
    os.makedirs(out, exist_ok=True)
    for key in sys.argv[4:]:
        build(asset, vita_asset, out, key)
        build_baby(asset, vita_asset, out, key)


if __name__ == '__main__':
    main()
