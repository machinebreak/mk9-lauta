"""The package's ObjectReferencer (CombinedReferencer: ReferencedObjects, the objects a seekfree package keeps
loaded): read, and rewritten with some objects swapped for others."""
import struct

from tagged import walk


def referencer(model):
    """(export index, array data start, count) of the package's ReferencedObjects array."""
    names = [n for n, _ in model.names]
    for i, x in enumerate(model.exports):
        if model.class_path(x['w'][0]).endswith('ObjectReferencer'):
            w, end = walk(x['data'], names)
            for p in w.props:
                if p['name'] == 'ReferencedObjects':
                    return i + 1, p['pos'], p['count']
    raise LookupError('no ReferencedObjects')


def referenced(model):
    idx, pos, count = referencer(model)
    d = model.exports[idx - 1]['data']
    return [struct.unpack_from('>i', d, pos + 28 + 4 * k)[0] for k in range(count)]


def set_referenced(model, refs):
    """ReferencedObjects := refs (the tag's size and count follow)."""
    idx, pos, count = referencer(model)
    x = model.exports[idx - 1]
    d = bytearray(x['data'])
    old_size = struct.unpack_from('>i', d, pos + 16)[0]
    start = pos + 24  # the value: count, then the elements
    body = struct.pack('>i', len(refs)) + b''.join(struct.pack('>i', r) for r in refs)
    d[start:start + old_size] = body
    struct.pack_into('>i', d, pos + 16, len(body))
    x['data'] = bytes(d)
    return len(refs)
