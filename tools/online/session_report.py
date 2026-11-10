#!/usr/bin/env python3
"""Reads the two players' session recordings (logs/session_<date>_p1.mk9net and _p2.mk9net, written by the game
with mk9_net_record) and says where the two games parted: the first frame whose sync hashes differ, with the inputs
of the frames around it. Also checks that both files have the same inputs for every frame (each player's own input
as the other one received it).

    python tools/online/session_report.py <p1 file> <p2 file> [--context 6]
    python tools/online/session_report.py <file>                  (one file: its epochs and frames)
"""
import argparse
import struct
import sys

PAD = struct.Struct('<12sBBI')  # the packets' pad record: X_INPUT_GAMEPAD (big-endian), busy, ready, sound


def read(path):
    data = open(path, 'rb').read()
    if data[:8] != b'MK9NET1\0':
        sys.exit(f'{path}: not a session recording')
    version, player, seed, rollback, lockstep_delay, rollback_delay = struct.unpack_from('<IBIBBB', data, 8)
    pos = 8 + struct.calcsize('<IBIBBB')
    info = {'player': player + 1, 'seed': seed, 'rollback': rollback, 'delays': (lockstep_delay, rollback_delay)}
    epochs = []  # (epoch, start game frame, rollback, {frame: (mine, theirs, hash)})
    while pos < len(data):
        tag = data[pos]
        pos += 1
        if tag == ord('E'):
            epoch, start, rb = struct.unpack_from('<IQB', data, pos)
            pos += struct.calcsize('<IQB')
            epochs.append((epoch, start, rb, {}))
        elif tag == ord('F'):
            if pos + 4 + 2 * PAD.size + 8 > len(data):
                break  # cut short (the game was closed)
            (frame,) = struct.unpack_from('<I', data, pos)
            pos += 4
            mine = PAD.unpack_from(data, pos)
            pos += PAD.size
            theirs = PAD.unpack_from(data, pos)
            pos += PAD.size
            (hash_,) = struct.unpack_from('<Q', data, pos)
            pos += 8
            if not epochs:
                epochs.append((0, 0, 0, {}))
            epochs[-1][3][frame] = (mine, theirs, hash_)
        elif tag == ord('M'):
            pos += 4  # the fight's mode ended at that frame (0.3.0 and later)
        else:
            break
    return info, epochs


def pad_text(pad):
    raw, busy, ready, sound = pad
    buttons = struct.unpack('>H', raw[:2])[0]
    names = []
    for bit, name in ((0x0001, 'Up'), (0x0002, 'Down'), (0x0004, 'Left'), (0x0008, 'Right'), (0x0010, 'Start'),
                      (0x0020, 'Back'), (0x0100, 'LB'), (0x0200, 'RB'), (0x1000, 'A'), (0x2000, 'B'),
                      (0x4000, 'X'), (0x8000, 'Y')):
        if buttons & bit:
            names.append(name)
    lt, rt = raw[2], raw[3]
    lx, ly = struct.unpack('>hh', raw[4:8])
    if lt > 30:
        names.append('LT')
    if rt > 30:
        names.append('RT')
    if abs(lx) > 8000 or abs(ly) > 8000:
        names.append(f'stick({lx},{ly})')
    extra = f' busy={busy}' if busy else ''
    extra += f' sound={sound:08x}' if sound else ''
    return ('+'.join(names) or '-') + extra


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('files', nargs='+')
    parser.add_argument('--context', type=int, default=6)
    args = parser.parse_args()
    if len(args.files) == 1:
        info, epochs = read(args.files[0])
        print(f'player {info["player"]}, seed {info["seed"]}, rollback {info["rollback"]}, delays {info["delays"]}')
        for epoch, start, rb, frames in epochs:
            print(f'  epoch {epoch}: game frame {start}, {"rollback" if rb else "lockstep"}, {len(frames)} frames')
        return
    a_info, a_epochs = read(args.files[0])
    b_info, b_epochs = read(args.files[1])
    if a_info['player'] == b_info['player']:
        print('warning: both files are the same player\'s')
    if a_info['seed'] != b_info['seed']:
        print(f'the seeds differ ({a_info["seed"]} and {b_info["seed"]}): not the same session')
        return
    b_by_epoch = {e[0]: e for e in b_epochs}
    first = None
    input_mismatch = None
    compared = 0
    for epoch, start, rb, frames in a_epochs:
        other = b_by_epoch.get(epoch)
        if not other:
            continue
        for frame in sorted(frames):
            if frame not in other[3]:
                continue
            mine, theirs, h = frames[frame]
            o_mine, o_theirs, o_h = other[3][frame]
            compared += 1
            # each player's own input as the other received it
            if input_mismatch is None and (mine != o_theirs or theirs != o_mine):
                input_mismatch = (epoch, frame)
            if h and o_h and h != o_h and first is None:
                first = (epoch, frame, start)
    print(f'{compared} frames in both files')
    if input_mismatch:
        print(f'the inputs differ at epoch {input_mismatch[0]} frame {input_mismatch[1]}: a recording is damaged')
    if not first:
        print('the sync hashes never differ: the two games stayed together')
        return
    epoch, frame, start = first
    print(f'the games parted at epoch {epoch} frame {frame} (game frame {start + frame} on player 1\'s machine)')
    frames = next(e for e in a_epochs if e[0] == epoch)[3]
    other = b_by_epoch[epoch][3]
    for f in range(frame - args.context, frame + 3):
        if f not in frames or f not in other:
            continue
        mine, theirs, h = frames[f]
        p1, p2 = (mine, theirs) if a_info['player'] == 1 else (theirs, mine)
        mark = '  <-- first difference' if f == frame else ''
        same = 'same' if h == other[f][2] else 'DIFFERENT'
        print(f'  {f:7d}  P1 {pad_text(p1):24s} P2 {pad_text(p2):24s} hashes {same}{mark}')


if __name__ == '__main__':
    main()
