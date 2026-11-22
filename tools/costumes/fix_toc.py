"""A test root's Xbox360TOC.txt with the sizes of the files as they are now (the lines whose file changed).

    python fix_toc.py <test_root>
"""
import os
import re
import sys

TOC_LINE = re.compile(r'^(\d+)( \d+ \d+ )(.+?)( \d+)$')


def main():
    root = sys.argv[1]
    path = os.path.join(root, 'Xbox360TOC.txt')
    text = open(path, 'rb').read().decode('latin-1')
    out, changed = [], 0
    for m in re.finditer(r'([^\r\n]*)(\r\n|\n|$)', text):
        line, eol = m.group(1), m.group(2)
        if not line and not eol:
            continue
        t = TOC_LINE.match(line)
        if t:
            rel = t.group(3).replace('\\', os.sep)
            if rel.startswith('..' + os.sep):
                rel = rel[3:]
            f = os.path.join(root, rel)
            if os.path.exists(f) and os.path.getsize(f) != int(t.group(1)):
                line = '%d%s%s%s' % (os.path.getsize(f), t.group(2), t.group(3), t.group(4))
                changed += 1
        out.append(line + eol)
    tmp = path + '.tmp'
    open(tmp, 'wb').write(''.join(out).encode('latin-1'))
    os.remove(path)
    os.replace(tmp, path)
    print('TOC: %d sizes changed' % changed)


if __name__ == '__main__':
    main()
