"""Sets keys of a section of Xe-MK9Game.ini inside a test root's Config/Coalesced.ini (added at the section's top,
replacing the same key's lines), then the TOC's size of it.

    python ini_set.py <test_root> "[SECTION]" key=value [key=value ...]
"""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from make_test_root import coalesced_build, coalesced_entries, edit_ini, section_span  # noqa: E402


def main():
    root, section = sys.argv[1], sys.argv[2]
    pairs = [a.split('=', 1) for a in sys.argv[3:]]
    path = os.path.join(root, 'Config', 'Coalesced.ini')
    header, entries = coalesced_entries(open(path, 'rb').read())

    def fn(lines):
        h, e = section_span(lines, section)
        if h is None:
            end = section_span(lines, '[IniVersion]')[0]
            lines[end:end] = [section, '']
            h, e = section_span(lines, section)
        keys = {k.lower() for k, _ in pairs}
        body = [ln for ln in lines[h + 1:e] if ln.split('=', 1)[0].strip().lower() not in keys]
        lines[h + 1:e] = ['%s=%s' % (k, v) for k, v in pairs] + body
        return lines

    for i, (name, body) in enumerate(entries):
        if name.rstrip(b'\0').decode('latin-1').lower().endswith('xe-mk9game.ini'):
            entries[i][1] = edit_ini(body, fn)
    data = coalesced_build(header, entries)
    tmp = path + '.tmp'
    open(tmp, 'wb').write(data)
    os.remove(path)
    os.replace(tmp, path)
    print('%s: %s set (%d bytes)' % (section, ', '.join(k for k, _ in pairs), len(data)))


if __name__ == '__main__':
    main()
