"""A test game root made of hard links to the real one, with costumes registered in its Coalesced.ini and TOC.

    python make_test_root.py <game_root> <test_root> <key=file.xxx[:baby.xxx]> ...

Each costume key (e.g. scorpion_d) gets: Asset/CHAR_<...>.xxx (the given file, hard-linked or copied), its Babality
package, Xe-MK9Game.ini's [Player] IsDLC/IsOnDiscDLC, [Player.DownloadContentID] (20001 on), [Player.Download
ContentUnlocked] TRUE, a [CHAR_<KEY>] section copied from the base costume's (BabyName changed) and the TOC's lines.
"""
import os
import re
import shutil
import struct
import sys

SECTION_BASE = {}
TOC_LINE = re.compile(r'^(\d+)( \d+ \d+ )(.+?)( \d+)$')


def link_tree(src, dst):
    for d, _, files in os.walk(src):
        rel = os.path.relpath(d, src)
        if rel.split(os.sep)[0] in ('dlc', 'kratos_backup'):
            continue
        os.makedirs(os.path.join(dst, rel), exist_ok=True)
        for f in files:
            s, t = os.path.join(d, f), os.path.join(dst, rel, f)
            if not os.path.exists(t):
                os.link(s, t)


def write_new(path, data):
    tmp = path + '.tmp'
    with open(tmp, 'wb') as f:
        f.write(data)
    if os.path.exists(path):
        os.remove(path)
    os.replace(tmp, path)


def coalesced_entries(data):
    entries, o = [], 4
    while o < len(data):
        n = struct.unpack_from('>i', data, o)[0]
        name = data[o + 4:o + 4 + n]
        o += 4 + n
        m = struct.unpack_from('>i', data, o)[0]
        entries.append([name, data[o + 4:o + 4 + m]])
        o += 4 + m
    return data[:4], entries


def coalesced_build(header, entries):
    out = bytearray(header)
    for name, body in entries:
        out += struct.pack('>i', len(name)) + name + struct.pack('>i', len(body)) + body
    return bytes(out)


def edit_ini(body, fn):
    tail = b''
    if body.endswith(b'\0'):
        body, tail = body[:-1], b'\0'
    text = body.decode('latin-1')
    eol = '\r\n' if '\r\n' in text else '\n'
    lines = fn(text.split(eol))
    return eol.join(lines).encode('latin-1') + tail


def section_span(lines, header):
    h = [i for i, ln in enumerate(lines) if ln.strip().lower() == header.lower()]
    if not h:
        return None, None
    e = h[0] + 1
    while e < len(lines) and not lines[e].startswith('['):
        e += 1
    return h[0], e


def main():
    game, test = sys.argv[1], sys.argv[2]
    costumes = []
    for arg in sys.argv[3:]:
        key, files = arg.split('=', 1)
        parts = files.split(':')
        costumes.append((key.lower(), parts[0], parts[1] if len(parts) > 1 else None))
        if len(parts) > 2:
            SECTION_BASE[key.lower()] = parts[2].lower()
    link_tree(game, test)
    asset = os.path.join(test, 'Asset')
    sizes = {}
    for key, char_file, baby_file in costumes:
        name = 'CHAR_' + key.split('_')[0].capitalize() + '_' + key.split('_')[1].upper()
        cap = {'subzero': 'SubZero', 'kunglao': 'KungLao', 'liukang': 'LiuKang', 'quanchi': 'QuanChi',
               'cyberzero': 'CyberZero', 'johnnycage': 'JohnnyCage', 'shaokahn': 'ShaoKahn',
               'shangtsung': 'ShangTsung'}.get(key.split('_')[0], key.split('_')[0].capitalize())
        name = 'CHAR_%s_%s' % (cap, key.split('_')[1].upper())
        baby = 'NPC_%s_%s_Baby' % (cap, key.split('_')[1].upper())
        for src, dst in ((char_file, name + '.xxx'), (baby_file, baby + '.xxx')):
            if not src:
                continue
            t = os.path.join(asset, dst)
            if os.path.exists(t):
                os.remove(t)
            in_game = not (os.path.isabs(src) or os.path.exists(src))
            src_path = os.path.join(game, 'Asset', src) if in_game else src
            os.link(src_path, t) if in_game else shutil.copyfile(src_path, t)
            sizes[dst] = os.path.getsize(t)
        print('%s: %s, %s' % (key, name, baby if baby_file else '(no babality)'))
    coal_path = os.path.join(test, 'Config', 'Coalesced.ini')
    header, entries = coalesced_entries(open(os.path.join(game, 'Config', 'Coalesced.ini'), 'rb').read())

    def game_ini(lines):
        for n, (key, char_file, baby_file) in enumerate(costumes):
            base_key = None
            if os.environ.get('COSTUME_AS_DLC', '1') == '1':
                for header_name in ('[Player]',):
                    h, e = section_span(lines, header_name)
                    have = {ln.strip().lower() for ln in lines[h + 1:e]}
                    add = [ln for ln in ('IsDLC=%s' % key, 'IsOnDiscDLC=%s' % key) if ln.lower() not in have]
                    lines[h + 1:h + 1] = add
                h, e = section_span(lines, '[Player.DownloadContentID]')
                lines[h + 1:h + 1] = ['%s=%d' % (key, 20001 + n)]
                h, e = section_span(lines, '[Player.DownloadContentUnlocked]')
                lines[h + 1:h + 1] = ['%s=TRUE' % key]
            # the costume's own sections, copied from the base costume's (mesh, expressions, Babality)
            base = SECTION_BASE.get(key)
            if base:
                bk, nk = base.split('_')[1].upper(), key.split('_')[1].upper()
                for sec, new_sec in (('[CHAR_%s]' % base.upper(), '[CHAR_%s]' % key.upper()),
                                     ('[NPC_%s_BABY]' % base.upper(), '[NPC_%s_BABY]' % key.upper())):
                    if section_span(lines, new_sec)[0] is not None:
                        print('  %s already there: kept' % new_sec)
                        continue
                    h, e = section_span(lines, sec)
                    if h is None:
                        print('  (no %s to copy)' % sec)
                        continue
                    body = [ln.replace('_%s_Baby' % bk, '_%s_Baby' % nk).replace('_%s_BABY' % bk, '_%s_BABY' % nk)
                            for ln in lines[h + 1:e] if ln.strip()]
                    ini_end = section_span(lines, '[IniVersion]')[0]
                    lines[ini_end:ini_end] = [new_sec] + body + ['']
                    print('  %s from %s: %s' % (new_sec, sec, '; '.join(body)))
        return lines

    for i, (name, body) in enumerate(entries):
        if name.rstrip(b'\0').decode('latin-1').lower().endswith('xe-mk9game.ini'):
            entries[i][1] = edit_ini(body, game_ini)
    coal = coalesced_build(header, entries)
    write_new(coal_path, coal)
    toc_path = os.path.join(test, 'Xbox360TOC.txt')
    text = open(os.path.join(game, 'Xbox360TOC.txt'), 'rb').read().decode('latin-1')
    rows = [[m.group(1), m.group(2)] for m in re.finditer(r'([^\r\n]*)(\r\n|\n|$)', text) if m.group(0)]
    out = []
    for t, e in rows:
        m = TOC_LINE.match(t)
        if m and m.group(3).lower() == '..\\config\\coalesced.ini':
            t = '%d%s%s%s' % (len(coal), m.group(2), m.group(3), m.group(4))
        out.append(t + e)
    eol = '\r\n' if text.count('\r\n') else '\n'
    if out and not out[-1].endswith(('\n', '\r\n')):
        out[-1] += eol
    for dst, size in sizes.items():
        out.append('%d 0 0 ..\\Asset\\%s 0%s' % (size, dst, eol))
    write_new(toc_path, ''.join(out).encode('latin-1'))
    print('test root ready: %s (Coalesced.ini %d bytes, %d packages added)' % (test, len(coal), len(sizes)))


if __name__ == '__main__':
    main()
