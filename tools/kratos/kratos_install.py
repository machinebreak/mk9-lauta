"""Kratos (the PS3 version's guest fighter) into a folder of this port's game files, from the community's Xbox 360 port
(github.com/eLGatoRDV/mk9-kratos-xbox360, release v1.0: Kratos-MK9-360-Share.zip, which the player downloads).

    python -I tools/kratos/kratos_install.py <Kratos-MK9-360-Share folder or .zip> <game_root> [<dlc_root>]

The same changes as that port's patcher (its data, checked against the same MD5s), done for this PC build:
  - game_root/Asset: Kratos' 12 packages, and his 3 sound banks made from the player's Kenshi ones (the names inside
    changed, same lengths: "Kenshi" -> "Kratos", "kens" -> "krat"; not distributed);
  - game_root/Config/Coalesced.ini: Kratos registered (four edits of its INI entries);
  - game_root/Xbox360TOC.txt: Coalesced.ini's new size and Kratos' packages;
  - <dlc_root>/0000000000000000/575207FD/00000002/KRATOS/: the DLC package's files, as the SDK's content manager takes
    DLC (a folder per package). Without <dlc_root>: game_root/dlc, where the game looks (app/src/mk9_kratos.h; the
    cvar dlc_root points it elsewhere).
default.xex is not touched: this build's code is translated from it, and its two changes are the build's own
([[midasm_hook]] entries, on when Kratos is there). Every file is written new (a temporary file renamed over it), so a
game_root made of hard links to another keeps that one as it was.
"""
import hashlib
import os
import re
import struct
import sys
import zipfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from stfs_extract import Stfs  # noqa: E402

XEX_MD5 = '06e4aa55b37abd88de447f831ea6236a'  # the European disc's default.xex, the port's "RF (JTAGRip)" one
COALESCED_ORIG_MD5 = 'bcf624c28f389d2c6ab134d2904836cb'
COALESCED_PATCHED_MD5 = '58e961a70090563a24f337854f3ed6e7'
COALESCED_EDITS = [
    {'entry': 'Xe-MK9Engine.ini', 'section': '[Engine.HDCacheControlPrefixes]',
     'insert_top': ['CHAR_Kratos=false', 'Kratos_=false', 'NPC_Kratos=false', 'ui_e_Kratos=false',
                    'SND_SFX_Kratos=false', 'SND_VO_Kratos=false', 'SND_VO_Announcer_Kratos=false']},
    {'entry': 'Xe-MK9Game.ini', 'section': '[Player]', 'remove': ['IsOnDiscDLC=kratos', 'IsOnDiscDLC=kratos_b']},
    {'entry': 'Xe-MK9Game.ini', 'section': '[Player.DownloadContentID]', 'insert_top': ['kratos=1001', 'kratos_b=1002']},
    {'entry': 'Xe-MK9Game.ini', 'section': '[Player.DownloadContentUnlocked]',
     'insert_top': ['kratos=TRUE', 'kratos_b=TRUE']},
]
ASSETS = ['CHAR_Kratos.xxx', 'CHAR_Kratos_B.xxx', 'Kratos_CharIntro.xxx', 'Kratos_Fatality1.xxx',
          'Kratos_Fatality2.xxx', 'Kratos_Ladder.xxx', 'Kratos_Victory.xxx', 'Kratos_ui.xxx', 'Kratos_ui_n.xxx',
          'NPC_Kratos_Baby.xxx', 'NPC_Kratos_B_Baby.xxx', 'ui_e_Kratos.xxx']
SOUNDS = [('SND_SFX_Kenshi.xxx', '1f664753f33fb36e94f0295e9eb5a038', 'SND_SFX_Kratos.xxx',
           'becda2ab16a3ee00a98778a3af17c410'),
          ('SND_VO_Kenshi.xxx', '4bd2fcb09886d3aecefb319aa662fde3', 'SND_VO_Kratos.xxx',
           '713645297ed21b0cce5c9cd6d91baf10'),
          ('SND_VO_Announcer_Kenshi.xxx', '478041356eac402c11e9faadf3b38aa2', 'SND_VO_Announcer_Kratos.xxx',
           '64c9b977e6ab9db6e4442271b5ecd06d')]
TOC_COALESCED = '..\\Config\\Coalesced.ini'
TOC_LINE = re.compile(r'^(\d+)( \d+ \d+ )(.+?)( \d+)$')


def md5(data):
    return hashlib.md5(data).hexdigest()


def write_new(path, data):
    """A new file over the old one (renamed into place: a hard link's other names keep the old data)."""
    os.makedirs(os.path.dirname(path), exist_ok=True)
    temp = path + '.kratos_tmp'
    with open(temp, 'wb') as f:
        f.write(data)
    if os.path.exists(path):
        os.remove(path)
    os.replace(temp, path)


class Package:
    """The port's release, as a folder or as its zip."""

    def __init__(self, path):
        self.zip = zipfile.ZipFile(path) if path.lower().endswith('.zip') else None
        self.root = path
        names = self.zip.namelist() if self.zip else [os.path.relpath(os.path.join(d, f), path).replace('\\', '/')
                                                      for d, _, fs in os.walk(path) for f in fs]
        self.names = {n.lower(): n for n in names}

    def find(self, tail):
        tail = tail.lower()
        for low, name in self.names.items():
            if low == tail or low.endswith('/' + tail):
                return name
        raise FileNotFoundError(tail)

    def read(self, tail):
        name = self.find(tail)
        if self.zip:
            return self.zip.read(name)
        with open(os.path.join(self.root, name), 'rb') as f:
            return f.read()


def coalesced_patch(data):
    header, entries, o = data[:4], [], 4
    while o < len(data):
        n = struct.unpack_from('>i', data, o)[0]
        name = data[o + 4:o + 4 + n]
        o += 4 + n
        m = struct.unpack_from('>i', data, o)[0]
        entries.append([name, data[o + 4:o + 4 + m]])
        o += 4 + m
    for op in COALESCED_EDITS:
        hits = [i for i, (n, _) in enumerate(entries)
                if n.rstrip(b'\0').decode('latin-1').lower().replace('/', '\\').endswith('\\' + op['entry'].lower())]
        if len(hits) != 1:
            raise ValueError('Coalesced.ini: no single entry %s' % op['entry'])
        body = entries[hits[0]][1]
        tail = b''
        if body.endswith(b'\0'):
            body, tail = body[:-1], b'\0'
        text = body.decode('latin-1')
        eol = '\r\n' if '\r\n' in text else '\n'
        lines = text.split(eol)
        h = lines.index(op['section'])
        e = h + 1
        while e < len(lines) and not lines[e].startswith('['):
            e += 1
        if 'insert_top' in op:
            lines[h + 1:h + 1] = [ln for ln in op['insert_top'] if ln not in lines[h + 1:e]]
        else:
            lines[h + 1:e] = [ln for ln in lines[h + 1:e] if ln not in op['remove']]
        entries[hits[0]][1] = eol.join(lines).encode('latin-1') + tail
    out = bytearray(header)
    for name, body in entries:
        out += struct.pack('>i', len(name)) + name + struct.pack('>i', len(body)) + body
    return bytes(out)


def toc_patch(data, coalesced_size, sizes):
    text = data.decode('latin-1')
    rows = text.split('\n')
    last = rows.pop()
    rows = [[r[:-1], '\r\n'] if r.endswith('\r') else [r, '\n'] for r in rows] + ([[last, '']] if last else [])
    index = {}
    for i, (t, _) in enumerate(rows):
        m = TOC_LINE.match(t)
        if m:
            index.setdefault(m.group(3).lower(), i)
    eol = '\r\n' if sum(e == '\r\n' for _, e in rows) > sum(e == '\n' for _, e in rows) else '\n'

    def set_size(i, size):
        m = TOC_LINE.match(rows[i][0])
        rows[i][0] = '%d%s%s%s' % (size, m.group(2), m.group(3), m.group(4))

    set_size(index[TOC_COALESCED.lower()], coalesced_size)
    for name in ASSETS:
        path = '..\\Asset\\' + name
        if path.lower() in index:
            set_size(index[path.lower()], sizes[name])
        else:
            if rows[-1][1] == '':
                rows[-1][1] = eol
            rows.append(['%d 0 0 %s 0' % (sizes[name], path), eol])
    return ''.join(t + e for t, e in rows).encode('latin-1')


def main():
    if len(sys.argv) not in (3, 4):
        sys.exit(__doc__)
    pkg, game = Package(sys.argv[1]), sys.argv[2]
    dlc = sys.argv[3] if len(sys.argv) == 4 else os.path.join(game, 'dlc')
    with open(os.path.join(game, 'default.xex'), 'rb') as f:
        if md5(f.read()) != XEX_MD5:
            sys.exit('this game\'s default.xex is not the build the Kratos port is for')
    sizes = {}
    for name in ASSETS:
        data = pkg.read('Game_Hdd1_MK9/Asset/' + name)
        sizes[name] = len(data)
        write_new(os.path.join(game, 'Asset', name), data)
    print('assets: %d packages, %.1f MB' % (len(ASSETS), sum(sizes.values()) / 1e6))
    for src, src_md5, dst, dst_md5 in SOUNDS:
        with open(os.path.join(game, 'Asset', src), 'rb') as f:
            data = f.read()
        if md5(data) != src_md5:
            sys.exit('%s is not the original one' % src)
        data = data.replace(b'Kenshi', b'Kratos').replace(b'kens', b'krat')
        if md5(data) != dst_md5:
            sys.exit('%s came out unlike the port\'s' % dst)
        write_new(os.path.join(game, 'Asset', dst), data)
    print('sounds: 3 banks made from Kenshi\'s')
    coal_path = os.path.join(game, 'Config', 'Coalesced.ini')
    with open(coal_path, 'rb') as f:
        coal = f.read()
    if md5(coal) == COALESCED_ORIG_MD5:
        # the disc's two kept, as the game's installer does (F2 > Kratos > Desinstalar puts them back)
        write_new(os.path.join(game, 'kratos_backup', 'Coalesced.ini'), coal)
        with open(os.path.join(game, 'Xbox360TOC.txt'), 'rb') as f:
            write_new(os.path.join(game, 'kratos_backup', 'Xbox360TOC.txt'), f.read())
        coal = coalesced_patch(coal)
        if md5(coal) != COALESCED_PATCHED_MD5:
            sys.exit('Coalesced.ini came out unlike the port\'s')
        write_new(coal_path, coal)
        print('Coalesced.ini: Kratos registered (%d bytes)' % len(coal))
    elif md5(coal) == COALESCED_PATCHED_MD5:
        print('Coalesced.ini: already registered')
    else:
        sys.exit('Coalesced.ini is not the original one')
    toc_path = os.path.join(game, 'Xbox360TOC.txt')
    with open(toc_path, 'rb') as f:
        toc = f.read()
    write_new(toc_path, toc_patch(toc, len(coal), sizes))
    print('Xbox360TOC.txt: %d packages listed' % len(ASSETS))
    stfs_bytes = pkg.read('Content_DLC/KRATOS')
    temp = os.path.join(dlc, 'KRATOS.package_tmp')
    write_new(temp, stfs_bytes)
    package = Stfs(temp)
    folder = os.path.join(dlc, '0000000000000000', '575207FD', '00000002', 'KRATOS')
    entries = package.entries()
    for i, e in enumerate(entries):
        if not e['dir']:
            write_new(os.path.join(folder, package.path_of(entries, i)), package.read(e))
    package.f.close()
    os.remove(temp)
    print('DLC: %d files in %s' % (sum(not e['dir'] for e in entries), folder))


if __name__ == '__main__':
    main()
