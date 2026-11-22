"""The downloadable pack of the costumes made from the PS Vita's own data (the MK3 retro look, ...): the same bundle
format as app/costumes/vita_costumes.bin (make_bundle.py), its DLC ids after the embedded bundle's. Its patches copy
from the player's own Xbox 360 game files; their new bytes are the Vita's meshes and textures, converted (build_mk3.py,
build_rain.py) - game data of the Vita's, so the pack is hosted apart, as the Kratos port is, and downloaded by the
game (mk9_vita_costumes.cpp).

    python make_mk3_pack.py <game_root> <built folder> <tested test root> <out .bin>
"""
import os
import struct
import sys

from build_mk3 import MK3
from make_bundle import ORDER as EMBEDDED, FIRST_ID, section_lines, tested_sections, u16s
from makepatch import make

ORDER = ['scorpion_e', 'subzero_e', 'reptile_e', 'ermac_e', 'smoke_e', 'noob_e', 'rain_e', 'kenshi_b', 'skarlet_b']
FIRST = FIRST_ID + len(EMBEDDED)  # 20008: after the embedded bundle's
MAGIC = b'MK9V'


def sources(key):
    """Each package's source packages: what its patch copies from (the player's own files)."""
    if key == 'rain_e':
        return 'Rain', ['CHAR_Rain.xxx', 'CHAR_Smoke_D.xxx', 'CHAR_Noob_D.xxx'], \
            ['NPC_Rain_Baby.xxx', 'NPC_Noob_D_Baby.xxx', 'NPC_Smoke_D_Baby.xxx']
    if key in ('kenshi_b', 'skarlet_b'):  # build_own.py: the default packages
        name = key.split('_')[0].capitalize()
        return name, ['CHAR_%s.xxx' % name], ['NPC_%s_Baby.xxx' % name]
    s = MK3[key]
    name, base = s['char'], s['base'].upper()
    return name, ['CHAR_%s_%s.xxx' % (name, base)], ['NPC_%s_%s_Baby.xxx' % (name, base)]


def main():
    game, built, test_root, out = sys.argv[1:5]
    keys = sys.argv[5:] or ORDER
    asset = os.path.join(game, 'Asset')
    ini = tested_sections(test_root)
    blob = bytearray(MAGIC + struct.pack('>I', 1) + struct.pack('>I', len(keys)))
    total = 0
    for key in keys:
        did = FIRST + ORDER.index(key)
        name, char_src, baby_src = sources(key)
        letter = key.split('_')[1].upper()
        edits = [(0, '[Player]', 'IsDLC=%s' % key), (0, '[Player]', 'IsOnDiscDLC=%s' % key),
                 (0, '[Player.DownloadContentID]', '%s=%d' % (key, did)),
                 (0, '[Player.DownloadContentUnlocked]', '%s=TRUE' % key)]
        for sec in ('[CHAR_%s]' % key.upper(), '[NPC_%s_BABY]' % key.upper()):
            try:
                edits += [(1, sec, ln) for ln in section_lines(ini, sec)]
            except LookupError:  # the Vita's own-mesh costumes have no baby section
                if 'BABY' not in sec:
                    raise
        blob += u16s(key) + struct.pack('>I', did) + struct.pack('>H', len(edits))
        for kind, sec, ln in edits:
            blob += bytes([kind]) + u16s(sec) + u16s(ln)
        patches = []
        for fname, srcs in (('CHAR_%s_%s.xxx' % (name, letter), char_src),
                            ('NPC_%s_%s_Baby.xxx' % (name, letter), baby_src)):
            tmp = os.path.join(os.path.dirname(os.path.abspath(out)), '_' + fname + '.mk9c')
            make(asset, os.path.join(built, fname), tmp, srcs)
            patches.append(open(tmp, 'rb').read())
            os.remove(tmp)
        blob += struct.pack('>H', len(patches))
        for p in patches:
            blob += struct.pack('>I', len(p)) + p
            total += len(p)
        print('%s: id %d, %d INI lines, patches %s' % (key, did, len(edits), [len(p) for p in patches]))
    os.makedirs(os.path.dirname(os.path.abspath(out)), exist_ok=True)
    open(out, 'wb').write(bytes(blob))
    print('%s: %d bytes (%d of patches)' % (out, len(blob), total))


if __name__ == '__main__':
    main()
