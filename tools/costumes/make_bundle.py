"""app/costumes/vita_costumes.bin: the costumes mk9-lauta makes at the first start (mk9_vita_costumes.cpp reads it).
Per costume: its key, its DLC id, the INI lines that register it (as the test root that was played had them), and the
patches of its two packages (makepatch.py: copies from the player's own game files plus the new bytes).

    python make_bundle.py <game_root> <built folder> <tested test root> <out .bin>
"""
import os
import re
import struct
import sys

from build_costume import COSTUMES as FROM_COSTUME
from build_default import COSTUMES as FROM_BASE
from make_test_root import coalesced_entries
from makepatch import make

ORDER = ['scorpion_d', 'subzero_d', 'reptile_d', 'ermac_d', 'mileena_c', 'skarlet_c', 'skarlet_d']
FIRST_ID = 20001


def sources(key):
    """Each package's source packages (the base costume and the look's donors; for a costume on the base package,
    the retro baby whose script it takes too)."""
    if key in FROM_COSTUME:
        s = FROM_COSTUME[key]
        name, base = s['char'], s['base'].upper()
        char = ['CHAR_%s_%s.xxx' % (name, base)] + [d + '.xxx' for d in s['donors']]
        baby = ['NPC_%s_%s_Baby.xxx' % (name, base)] + [d + '.xxx' for d in s['baby_donors']]
        return name, char, baby
    s = FROM_BASE[key]
    name = s['char']
    char = ['CHAR_%s.xxx' % name] + [d + '.xxx' for d in s['donors']]
    baby = ['NPC_%s_Baby.xxx' % name, s['baby_script'][0] + '.xxx'] + [d + '.xxx' for d in s['baby_donors']]
    return name, char, baby


def tested_sections(test_root):
    data = open(os.path.join(test_root, 'Config', 'Coalesced.ini'), 'rb').read()
    header, entries = coalesced_entries(data)
    for name, body in entries:
        if name.rstrip(b'\0').decode('latin-1').lower().endswith('xe-mk9game.ini'):
            return body.decode('latin-1').replace('\r', '')
    raise LookupError('no Xe-MK9Game.ini')


def section_lines(text, header):
    m = re.search(r'(?im)^' + re.escape(header) + r'\n(.*?)(?=^\[|\Z)', text, re.S)
    if not m:
        raise LookupError(header)
    return [ln for ln in m.group(1).split('\n') if ln.strip()]


def u16s(text):
    b = text.encode('latin-1')
    return struct.pack('>H', len(b)) + b


def main():
    game, built, test_root, out = sys.argv[1:5]
    asset = os.path.join(game, 'Asset')
    ini = tested_sections(test_root)
    blob = bytearray(b'MK9V' + struct.pack('>I', 1) + struct.pack('>I', len(ORDER)))
    total = 0
    for n, key in enumerate(ORDER):
        did = FIRST_ID + n
        name, char_src, baby_src = sources(key)
        letter = key.split('_')[1].upper()
        edits = [(0, '[Player]', 'IsDLC=%s' % key), (0, '[Player]', 'IsOnDiscDLC=%s' % key),
                 (0, '[Player.DownloadContentID]', '%s=%d' % (key, did)),
                 (0, '[Player.DownloadContentUnlocked]', '%s=TRUE' % key)]
        for sec in ('[CHAR_%s]' % key.upper(), '[NPC_%s_BABY]' % key.upper()):
            edits += [(1, sec, ln) for ln in section_lines(ini, sec)]
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
