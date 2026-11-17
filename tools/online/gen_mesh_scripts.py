"""The four players' scripts of a two against two on four machines (tools/online/mesh_test.ps1), from the 1.1.0 test's
two scripts of two players each (out/f3/four_room_host.txt: 1P and 3P; four_room_guest.txt: 2P and 4P; the "P2:"
lines are each machine's second player): machine 0 is 1P, 1 is 2P, 2 is 3P and 3 is 4P, the game's players in a
session of four machines (netplay.cpp, SeatOf).

    python tools/online/gen_mesh_scripts.py [out/f3]      writes out/f3/mesh4_p1.txt ... mesh4_p4.txt
"""
import os
import sys

folder = sys.argv[1] if len(sys.argv) > 1 else os.path.join('out', 'f3')


def split(path):
    first, second = [], []
    with open(path, encoding='utf-8') as f:
        for line in f:
            line = line.rstrip('\n')
            if not line.strip() or line.lstrip().startswith('#'):
                continue
            parts = line.split(None, 1)
            if len(parts) < 2:
                continue
            when, what = parts
            if what.startswith('P2:'):
                second.append(f'{when} {what[3:]}')
            else:
                first.append(f'{when} {what}')
    return first, second


p1, p3 = split(os.path.join(folder, 'four_room_host.txt'))
p2, p4 = split(os.path.join(folder, 'four_room_guest.txt'))
for number, lines in ((1, p1), (2, p2), (3, p3), (4, p4)):
    out = os.path.join(folder, f'mesh4_p{number}.txt')
    with open(out, 'w', encoding='utf-8', newline='\n') as f:
        f.write(f'# {number}P of a two against two on four machines (gen_mesh_scripts.py)\n')
        f.write('\n'.join(lines) + '\n')
    print(out, len(lines), 'lines')
