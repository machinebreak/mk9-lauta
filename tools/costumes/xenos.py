"""Xbox 360 (Xenos) texture layout for DXT textures, both ways: tiled 2D surfaces (32x32-block tiles), the packed
mip tail (every level from the first one 16 texels or smaller, in one tile), and 16-bit byte order. Plus the PS Vita's
Morton-swizzled blocks. Square or rectangular power-of-two DXT1/DXT5 textures.

Tiling: UE Viewer's GetXbox360TiledOffset (UnTexture.cpp). Mip tail offsets: Xenia's GetPackedMipOffset.
"""
import struct


def log2(n):
    return n.bit_length() - 1


def align(n, a):
    return (n + a - 1) // a * a


def tiled_offset(x, y, width, log_bpb):
    """The tiled position (in blocks) of block (x, y) of a surface `width` blocks wide."""
    aligned_width = align(width, 32)
    macro = ((x >> 5) + (y >> 5) * (aligned_width >> 5)) << (log_bpb + 7)
    micro = ((x & 7) + ((y & 0xE) << 2)) << log_bpb
    offset = macro + ((micro & ~0xF) << 1) + (micro & 0xF) + ((y & 1) << 4)
    return ((((offset & ~0x1FF) << 3) + ((y & 16) << 7) + ((offset & 0x1C0) << 2)
             + (((((y & 8) >> 2) + (x >> 3)) & 3) << 6) + (offset & 0x3F)) >> log_bpb)


def swap16(b):
    a = bytearray(b)
    a[0::2], a[1::2] = b[1::2], b[0::2]
    return bytes(a)


def surface_blocks(bw, bh):
    """A tiled surface's size in blocks (pitch, height): 32-block aligned."""
    return align(bw, 32), align(bh, 32)


def tile(linear, bw, bh, bpb=16):
    """Linear blocks (bw x bh) -> a tiled surface (32-aligned), in the same byte order."""
    pw, ph = surface_blocks(bw, bh)
    lb = log2(bpb)
    out = bytearray(pw * ph * bpb)
    for y in range(bh):
        for x in range(bw):
            t = tiled_offset(x, y, pw, lb)
            out[t * bpb:(t + 1) * bpb] = linear[(y * bw + x) * bpb:(y * bw + x + 1) * bpb]
    return bytes(out)


def untile(tiled, bw, bh, pw=None, ph=None, bpb=16):
    """A tiled surface -> linear blocks (bw x bh); the surface's pitch/height (blocks) default to 32-aligned."""
    if pw is None:
        pw, ph = surface_blocks(bw, bh)
    lb = log2(bpb)
    out = bytearray(bw * bh * bpb)
    for y in range(bh):
        for x in range(bw):
            t = tiled_offset(x, y, pw, lb)
            out[(y * bw + x) * bpb:(y * bw + x + 1) * bpb] = tiled[t * bpb:(t + 1) * bpb]
    return bytes(out)


def tail_base(w, h):
    """The first packed mip level of a w x h texture."""
    s = min(log2(w), log2(h))
    return max(s - 4, 0)


def packed_offset(w, h, mip):
    """The texel offset (x, y) of mip level `mip` in the packed tail (Xenia GetPackedMipOffset, 2D)."""
    lw, lh = log2(w), log2(h)
    s = min(lw, lh)
    base = s - 4 if s > 4 else 0
    pm = mip - base
    if pm < 3:
        return (0, 16 >> pm) if lw > lh else (16 >> pm, 0)
    if lw > lh:
        return ((1 << (lw - base)) >> (pm - 2), 0)
    return (0, (1 << (lh - base)) >> (pm - 2))


def mip_dims(w, h, k):
    return max(w >> k, 1), max(h >> k, 1)


def blocks_of(w, h):
    return max((w + 3) // 4, 1), max((h + 3) // 4, 1)


def to_x360(levels, w, h, bpb=16):
    """Linear DXT levels (level k: w>>k x h>>k, standard little-endian blocks) -> the X360 mips: a list of
    (payload or None, sx, sy) the way MK9 stores them: each level above the tail tiled on its own surface, the tail
    (every packed level) in one 32x32-block surface at the tail's base index, the levels after it empty."""
    tb = tail_base(w, h)
    out = []
    n = len(levels)
    for k in range(n):
        sx, sy = mip_dims(w, h, k)
        sx, sy = max(sx, 4), max(sy, 4)
        bw, bh = blocks_of(*mip_dims(w, h, k))
        if k < tb:
            out.append((swap16(tile(levels[k], bw, bh, bpb)), sx, sy))
        elif k == tb:
            # the tail surface: the packed levels placed at their offsets, then tiled as one 32x32-block surface
            pw, ph = 32, 32
            lin = bytearray(pw * ph * bpb)
            for m in range(tb, n):
                mbw, mbh = blocks_of(*mip_dims(w, h, m))
                ox, oy = packed_offset(w, h, m)
                ox, oy = ox // 4, oy // 4
                src = levels[m]
                for y in range(mbh):
                    for x in range(mbw):
                        d = ((oy + y) * pw + ox + x) * bpb
                        s = (y * mbw + x) * bpb
                        lin[d:d + bpb] = src[s:s + bpb]
            out.append((swap16(tile(bytes(lin), pw, ph, bpb)), sx, sy))
        else:
            out.append((None, sx, sy))
    return out, tb


def from_x360(mips, w, h, bpb=16):
    """The inverse of to_x360: [(payload or None, sx, sy)] -> linear levels (for checking)."""
    tb = tail_base(w, h)
    levels = []
    tail = None
    for k, (payload, sx, sy) in enumerate(mips):
        bw, bh = blocks_of(*mip_dims(w, h, k))
        if k < tb:
            levels.append(untile(swap16(payload), bw, bh, bpb=bpb))
            continue
        if k == tb:
            tail = untile(swap16(payload), 32, 32, 32, 32, bpb)
        ox, oy = packed_offset(w, h, k)
        ox, oy = ox // 4, oy // 4
        lv = bytearray(bw * bh * bpb)
        for y in range(bh):
            for x in range(bw):
                s = ((oy + y) * 32 + ox + x) * bpb
                lv[(y * bw + x) * bpb:(y * bw + x + 1) * bpb] = tail[s:s + bpb]
        levels.append(bytes(lv))
    return levels


def morton_yx(x, y):
    """The PS Vita's swizzle of a square level: block (x, y) at the Morton index with y in the even bits."""
    z = 0
    for i in range(16):
        z |= ((y >> i) & 1) << (2 * i) | ((x >> i) & 1) << (2 * i + 1)
    return z


def vita_unswizzle(blocks, bw, bh, bpb=16):
    """A Vita level's blocks (Morton order; a rectangle as squares of its short side, one after another) ->
    linear."""
    out = bytearray(bw * bh * bpb)
    side = min(bw, bh)
    per = side * side
    for y in range(bh):
        for x in range(bw):
            sq = (x // side) if bw >= bh else (y // side)
            src = sq * per + morton_yx(x % side, y % side)
            d = (y * bw + x) * bpb
            out[d:d + bpb] = blocks[src * bpb:(src + 1) * bpb]
    return bytes(out)
