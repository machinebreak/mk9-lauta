"""Prints a session recording's frames in a range: both pads' busy, ready and sound fields and the buttons.

    python rec_frames.py <file.mk9net> <epoch> <first> <last>
"""
import struct
import sys

path, want_epoch, first, last = sys.argv[1], int(sys.argv[2]), int(sys.argv[3]), int(sys.argv[4])
data = open(path, 'rb').read()
pos = 20
epoch = -1
PAD = struct.Struct('<12sBBI')
changes = []
prev = None
while pos < len(data):
    tag = data[pos]
    if tag == ord('E'):
        epoch, start, rb = struct.unpack_from('<IQB', data, pos + 1)
        print(f'E epoch {epoch} start game frame {start} rollback {rb}')
        pos += 14
    elif tag == ord('F'):
        frame, = struct.unpack_from('<I', data, pos + 1)
        a = PAD.unpack_from(data, pos + 5)
        b = PAD.unpack_from(data, pos + 5 + PAD.size)
        h, = struct.unpack_from('<Q', data, pos + 5 + 2 * PAD.size)
        pos += 1 + 4 + 2 * PAD.size + 8
        key = (a[1], a[2], b[1], b[2])
        if epoch == want_epoch and (first <= frame <= last or (key != prev and frame <= last)):
            if first <= frame <= last or key != prev:
                print(f'{frame}: p1 busy={a[1]} ready={a[2]} snd={a[3]:x} btn={a[0][:2].hex()} | '
                      f'p2 busy={b[1]} ready={b[2]} snd={b[3]:x} btn={b[0][:2].hex()} hash={h:016x}')
        prev = key
    elif tag == ord('M'):
        frame, = struct.unpack_from('<I', data, pos + 1)
        print(f'M mode end at {frame}')
        pos += 5
    else:
        print('unknown tag', tag, 'at', pos)
        break
