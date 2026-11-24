"""MK9 packages read whole, LZO-compressed (tools/costumes' Package/Model) or not (the sound and some map packages).

pkg_any.Model.load(path) gives the names, imports and exports (each with its data) either way. An uncompressed
package keeps its summary lists unread: enough to read names and objects, not to write the package back.

Run as a script: pkg_any.py <package.xxx> [Class,Class,...] lists the exports of those classes (or a class count).
"""
import collections
import os
import struct
import sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'costumes'))
import pkgio  # noqa: E402
from transplant import Model  # noqa: E402

_package_load = pkgio.Package.load.__func__
_model_load = Model.load.__func__


def _load_package(cls, path):
    try:
        return _package_load(cls, path)
    except AssertionError as e:
        if 'chunk' not in str(e):
            raise
    self = cls()
    raw = open(path, 'rb').read()
    self.e = '>'
    o = 20
    n = struct.unpack_from('>i', raw, o)[0]
    o += 4 + n
    self.flags_at = o
    (self.name_count, self.name_off, self.export_count, self.export_off, self.import_count, self.import_off,
     self.depends_off) = struct.unpack_from('>7i', raw, o + 4)
    self.chunks_at, self.chunk_count, self.chunks = 0, 0, []
    self.summary = bytearray(raw[:self.depends_off])
    self.image = bytearray(raw)
    return self


def _load_model(cls, path):
    try:
        return _model_load(cls, path)
    except (AssertionError, struct.error, IndexError):
        pass
    p = pkgio.Package.load(path)
    self = cls()
    self.pkg = p
    img = p.image
    self.names, o = [], p.name_off
    for _ in range(p.name_count):
        n = struct.unpack_from('>i', img, o)[0]
        self.names.append([bytes(img[o + 4:o + 4 + n - 1]).decode('latin-1'), b''])
        o += 12 + n
    self.imports, o = [], p.import_off
    for _ in range(p.import_count):
        self.imports.append(list(struct.unpack_from('>7i', img, o)))
        o += 28
    self.exports, o = [], p.export_off
    for _ in range(p.export_count):
        w = list(struct.unpack_from('>11i', img, o))
        self.exports.append({'w': w, 'data': bytes(img[w[9]:w[9] + w[8]])})
        o += 44 + 12 * w[10] + 20
    return self


pkgio.Package.load = classmethod(_load_package)
Model.load = classmethod(_load_model)

if __name__ == '__main__':
    m = Model.load(sys.argv[1])
    want = sys.argv[2].split(',') if len(sys.argv) > 2 else []
    count = collections.Counter()
    for i, x in enumerate(m.exports):
        cls = m.class_path(x['w'][0]).split('.')[-1]
        count[cls] += 1
        if cls in want:
            print(cls, m.path(i + 1), len(x['data']))
    if not want:
        for c, k in count.most_common():
            print('%5d %s' % (k, c))
