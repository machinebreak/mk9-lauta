"""MK9 Xbox 360 packages edited as tables: objects copied from one package into another (their name and object
references remapped), then the package written back whole (names, imports, exports, depends, data, chunks).

The references inside a copied object's data are found two ways: its tagged properties walked exactly (tagged.py),
and the bytes that differ between two donors holding the same object (Smoke MK2 and Noob MK2: the same assets, other
indices): a differing word that names the same name, or the same object, in both is a reference. Anything else that
differs is reported.

    from transplant import Model, transplant
    t = Model.load('CHAR_Scorpion_D.xxx'); s1 = Model.load('CHAR_Smoke_D.xxx'); s2 = Model.load('CHAR_Noob_D.xxx')
    transplant(t, s1, s2, lambda path: path.split('.')[0] in ('CHAR_RetroNinja_Male_MK2', ...))
    t.save('out.xxx')
"""
import bisect
import struct

from pkgio import Package, lzo1x_literals, TAG
from tagged import walk


def bulk_fields(data, serial_offset):
    """The inline bulk data headers' offset fields of an object: a word equal to its own end in the file (the payload
    follows it). Found by that signature (a few hundred KB into a package, nothing else matches)."""
    out = []
    target = serial_offset + 4
    for pos in range(0, len(data) - 3):
        if struct.unpack_from('>i', data, pos)[0] == target + pos:
            out.append(pos)
    return out


class Model:
    @classmethod
    def load(cls, path):
        self = cls()
        p = Package.load(path)
        self.pkg = p
        img = p.image
        self.names, o = [], p.name_off
        for _ in range(p.name_count):
            n = struct.unpack_from('>i', img, o)[0]
            assert n > 0, 'wide names are not handled'
            s = bytes(img[o + 4:o + 4 + n - 1]).decode('latin-1')
            flags = bytes(img[o + 4 + n:o + 12 + n])
            self.names.append([s, flags])
            o += 12 + n
        self.name_index = {s: i for i, (s, _) in enumerate(self.names)}
        self.imports, o = [], p.import_off
        for _ in range(p.import_count):
            self.imports.append(list(struct.unpack_from('>7i', img, o)))
            o += 28
        self.exports, o = [], p.export_off
        for _ in range(p.export_count):
            w = list(struct.unpack_from('>11i', img, o))
            comps = [list(struct.unpack_from('>3i', img, o + 44 + 12 * k)) for k in range(w[10])]
            q = o + 44 + 12 * w[10]
            eflags = struct.unpack_from('>i', img, q)[0]
            tail = bytes(img[q + 4:q + 20])
            data = bytes(img[w[9]:w[9] + w[8]])
            self.exports.append({'w': w, 'comps': comps, 'eflags': eflags, 'tail': tail, 'data': data,
                                 'bulk': bulk_fields(data, w[9]), 'orig': True})
            o = q + 20
        # the summary's lists (after the chunk table): 28 bytes (their last two ints: the sizes of the two lists that
        # follow), then the packages whose objects are forced exports here (count; name, GUID), then the rest (the
        # script packages: count; name, GUID) as is
        t = bytes(p.summary[p.chunks_at + 16 * p.chunk_count:])
        self.summary_lead = t[:28]
        count, o = struct.unpack_from('>i', t, 28)[0], 32
        self.forced_packages = []
        for _ in range(count):
            n = struct.unpack_from('>i', t, o)[0]
            self.forced_packages.append((t[o + 4:o + 3 + n].decode('latin-1'), t[o + 3 + n + 1:o + 4 + n + 16]))
            o += 4 + n + 16
        self.summary_rest = t[o:]
        assert struct.unpack_from('>ii', t, 20) == (o - 28, len(self.summary_rest)), 'the summary lists\' sizes'
        return self

    def summary_tail(self):
        """The lists after the chunk table, their sizes in the lead set again (a list grown or a name renamed at
        another length: the game reads them by those sizes; stale, the costume's baby loses its colors)."""
        forced = struct.pack('>i', len(self.forced_packages))
        for name, guid in self.forced_packages:
            b = name.encode('latin-1') + bytes(1)
            forced += struct.pack('>i', len(b)) + b + guid
        lead = bytearray(self.summary_lead)
        struct.pack_into('>ii', lead, 20, len(forced), len(self.summary_rest))
        return bytes(lead) + forced + self.summary_rest

    # paths
    def name(self, i):
        return self.names[i][0]

    def fname(self, idx, num):
        return self.name(idx) + ('_%d' % (num - 1) if num else '')

    def path(self, ref):
        parts = []
        seen = 0
        while ref != 0 and seen < 64:
            seen += 1
            if ref > 0:
                e = self.exports[ref - 1]['w']
                parts.append(self.fname(e[3], e[4]))
                ref = e[2]
            else:
                im = self.imports[-ref - 1]
                parts.append(self.fname(im[5], im[6]))
                ref = im[4]
        return '.'.join(reversed(parts))

    def class_path(self, ref):
        return self.path(ref) if ref else 'Class'

    def build_index(self):
        self.export_by_path = {}
        for i in range(len(self.exports)):
            self.export_by_path.setdefault(self.path(i + 1), i + 1)
        self.import_by_path = {}
        for i in range(len(self.imports)):
            self.import_by_path.setdefault(self.path(-(i + 1)), -(i + 1))

    def add_name(self, s, flags=b'\0' * 8):
        i = self.name_index.get(s)
        if i is None:
            i = len(self.names)
            self.names.append([s, flags])
            self.name_index[s] = i
        return i

    # writing
    def save(self, path, chunk_blocks=8):
        p = self.pkg
        e = '>'
        names = bytearray()
        for s, flags in self.names:
            b = s.encode('latin-1') + b'\0'
            names += struct.pack(e + 'i', len(b)) + b + flags
        imports = b''.join(struct.pack(e + '7i', *im) for im in self.imports)
        table_size = sum(44 + 12 * len(x['comps']) + 20 for x in self.exports)
        new_tail = self.summary_tail()
        name_off = p.chunks_at + len(new_tail)  # the summary without the chunk table: where the names start
        import_off = name_off + len(names)
        export_off = import_off + len(imports)
        depends_off = export_off + table_size
        header_size = depends_off + 4 * len(self.exports)
        # where each object's data goes: an original one stays at its offset in the file (shader caches and others
        # keep absolute offsets of their own: skip offsets), unless the grown header covers it; those and the new
        # ones go after the last original object's data
        placed = []
        end = max([header_size] + [x['w'][9] + len(x['data']) for x in self.exports if 'orig' in x])
        moved = []
        for x in self.exports:
            if 'orig' in x and x['w'][9] >= header_size and len(x['data']) == x['w'][8]:
                placed.append(x['w'][9])
            else:
                placed.append(end)
                end += len(x['data'])
                if 'orig' in x:
                    moved.append(x)
        self.moved = moved
        image = bytearray(end)
        exports = bytearray()
        for x, at in zip(self.exports, placed):
            w = list(x['w'])
            w[8] = len(x['data'])
            w[9] = at
            w[10] = len(x['comps'])
            blob = bytearray(x['data'])
            if at != x['w'][9] or 'orig' not in x:
                for pos in x.get('bulk', ()):  # inline bulk data: where its payload starts in the file, now
                    struct.pack_into(e + 'i', blob, pos, at + pos + 4)
            image[at:at + len(blob)] = blob
            exports += struct.pack(e + '11i', *w)
            for c in x['comps']:
                exports += struct.pack(e + '3i', *c)
            exports += struct.pack(e + 'i', x['eflags']) + x['tail']
        assert len(exports) == table_size
        header = bytes(name_off) + names + imports + exports + bytes(4 * len(self.exports))
        assert len(header) == header_size
        image[:header_size] = header
        # the summary: counts and offsets, then the chunks
        summary = bytearray(p.summary)
        tail = new_tail
        head = bytearray(summary[:p.chunks_at])
        fa = p.flags_at
        struct.pack_into(e + 'i', head, 8, header_size)
        struct.pack_into(e + '7i', head, fa + 4, len(self.names), name_off, len(self.exports), export_off,
                         len(self.imports), import_off, depends_off)
        block = p.block_size or 0x20000
        span = block * chunk_blocks
        ranges = chunk_ranges(name_off, header_size, placed, len(image), span)
        struct.pack_into(e + 'i', head, p.chunks_at - 4, len(ranges))
        summary_size = len(head) + 16 * len(ranges) + len(tail)
        chunks, entries, co = bytearray(), [], summary_size
        for uo, us in ranges:
            piece = bytes(image[uo:uo + us])
            blocks = [piece[i:i + block] for i in range(0, len(piece), block)]
            packed = [lzo1x_literals(b) for b in blocks]
            body = struct.pack(e + 'I3i', TAG, block, sum(len(x) for x in packed), len(piece))
            body += b''.join(struct.pack(e + '2i', len(x), len(b)) for x, b in zip(packed, blocks))
            body += b''.join(packed)
            entries.append((uo, us, co, len(body)))
            chunks += body
            co += len(body)
        out = bytes(head) + b''.join(struct.pack(e + '4i', *x) for x in entries) + tail + bytes(chunks)
        with open(path, 'wb') as f:
            f.write(out)
        return len(out)


def chunk_ranges(start, header_end, starts, end, span):
    """The compressed chunks' (offset, size), as the cooker cuts them: at object boundaries, as many objects as fit in
    `span`, an object bigger than that alone; the header (names, imports, exports, depends) whole in the first one. No
    object is split between chunks (the game's loader may assume it)."""
    bounds = sorted(set(b for b in starts if b > header_end) | {header_end, end})
    out, cs = [], start
    while cs < end:
        k = bisect.bisect_right(bounds, cs + span) - 1
        b = bounds[k] if k >= 0 else None
        if b is None or b <= cs:  # the object starting here is bigger than a chunk
            b = bounds[bisect.bisect_right(bounds, cs)]
        out.append((cs, b - cs))
        cs = b
    return out


def diff_refs(m1, d1, m2, d2):
    """(pos, kind) of the words that differ between two copies of an object and name the same name or object."""
    refs, bad, covered = {}, [], set()
    n = min(len(d1), len(d2))
    for i in range(n):
        if d1[i] == d2[i] or i in covered:
            continue
        found = False
        for s in range(max(0, i - 3), i + 1):
            if s + 4 > n:
                continue
            v1, v2 = struct.unpack_from('>i', d1, s)[0], struct.unpack_from('>i', d2, s)[0]
            if v1 == v2:
                continue
            if 0 <= v1 < len(m1.names) and 0 <= v2 < len(m2.names) and m1.name(v1) == m2.name(v2):
                refs[s] = 'name'
            elif v1 and v2 and -len(m1.imports) <= v1 <= len(m1.exports) and \
                    -len(m2.imports) <= v2 <= len(m2.exports) and m1.path(v1) == m2.path(v2):
                refs[s] = 'object'
            else:
                continue
            covered.update(range(s, s + 4))
            found = True
            break
        if not found:
            bad.append(i)
    return refs, bad


def transplant(t, s1, s2, wanted, log=print):
    """Copies into t every export of s1 whose path `wanted` takes (and the exports of s1 they refer to that t has
    no object for), their references remapped. Returns the new exports' paths."""
    for m in (t, s1, s2):
        m.build_index()
    chosen = [i + 1 for i in range(len(s1.exports)) if wanted(s1.path(i + 1))]
    chosen_set = set(chosen)
    new_index = {}
    base = len(t.exports)

    def plan(ref):
        """t's reference for s1's ref (adding exports to copy or imports as needed)."""
        if ref == 0:
            return 0
        pth = s1.path(ref)
        if ref > 0 and ref in chosen_set:
            return new_index[ref]
        if pth in t.export_by_path:
            return t.export_by_path[pth]
        if pth in t.import_by_path:
            return t.import_by_path[pth]
        if ref > 0:  # an object of s1 that t lacks: copied too
            chosen.append(ref)
            chosen_set.add(ref)
            new_index[ref] = base + len(new_index) + 1
            log('  + also copied: %s (%s)' % (pth, s1.class_path(s1.exports[ref - 1]['w'][0])))
            return new_index[ref]
        im = s1.imports[-ref - 1]  # an import t lacks: added (its outer first)
        outer = plan(im[4])
        t.imports.append([t.add_name(s1.name(im[0])), im[1], t.add_name(s1.name(im[2])), im[3], outer,
                          t.add_name(s1.name(im[5])), im[6]])
        t.import_by_path[pth] = -len(t.imports)
        log('  + import %s' % pth)
        return -len(t.imports)

    for ref in list(chosen):
        new_index[ref] = base + len(new_index) + 1
    s2_by_path = s2.export_by_path
    done = 0
    problems = 0
    while done < len(chosen):
        ref = chosen[done]
        done += 1
        x = s1.exports[ref - 1]
        pth = s1.path(ref)
        d1 = x['data']
        other = s2_by_path.get(pth)
        refs = {}
        w, end = walk(d1, [n for n, _ in s1.names])
        tagged_refs = {r.pos: r.kind for r in w.refs}
        if other:
            d2 = s2.exports[other - 1]['data']
            if len(d2) == len(d1):
                refs, bad = diff_refs(s1, d1, s2, d2)
                bad = [b for b in bad if not any(p <= b < p + 4 for p in tagged_refs)
                       and not any(p <= b < p + 4 for p in x['bulk'])]
                if bad:
                    problems += 1
                    log('  ! %s: %d differing bytes not references (e.g. %s)' % (pth, len(bad), bad[:6]))
            else:
                log('  ! %s: other size in the second donor (%d vs %d): tagged references only' % (pth, len(d1),
                                                                                                     len(d2)))
        else:
            log('  ! %s: not in the second donor: tagged references only' % pth)
        refs.update(tagged_refs)
        data = bytearray(d1)
        for pos, kind in sorted(refs.items()):
            v = struct.unpack_from('>i', d1, pos)[0]
            if kind == 'name':
                if not (0 <= v < len(s1.names)):
                    log('  ! %s @%d: name index %d out of range' % (pth, pos, v))
                    continue
                nv = t.add_name(s1.name(v), s1.names[v][1])
            else:
                nv = plan(v)
            struct.pack_into('>i', data, pos, nv)
        w0 = list(x['w'])
        w0[0] = plan(w0[0])
        w0[1] = plan(w0[1])
        w0[2] = plan(w0[2])
        w0[3] = t.add_name(s1.name(w0[3]), s1.names[w0[3]][1])
        w0[5] = plan(w0[5])
        comps = [[t.add_name(s1.name(c[0])), c[1], plan(c[2])] for c in x['comps']]
        for pos in x['bulk']:
            refs.pop(pos, None)
        t.exports.append({'w': w0, 'comps': comps, 'eflags': x['eflags'], 'tail': x['tail'], 'data': bytes(data),
                          'bulk': list(x['bulk'])})
        t.export_by_path[pth] = len(t.exports)
    log('transplanted %d objects (%d with unexplained differences); names %d, imports %d, exports %d' % (
        len(chosen), problems, len(t.names), len(t.imports), len(t.exports)))
    return [s1.path(r) for r in chosen]
