"""The three machines' scripts of a King of the Hill test (tools/online/mesh_test.ps1 -Machines 3 -Extra
"--mk9_net_koth=true"), timed by frames: machine 0 (player 1, the king) takes the menus to VERSUS and attacks without a
break; machine 1 (player 2) joins, picks its fighter and stands still: it loses by KO and goes to the line. Machine 2
takes its place a second after the match ends and answers the loser's CONTINUE? with REMATCH, stands still and loses;
machine 1 comes back and answers the next CONTINUE? the same way. Presses of a machine waiting in line reach no player.

    python tools/online/gen_koth_scripts.py [out/f3] [--rematch-at 200,330,460]   writes koth_p1.txt ... koth_p3.txt
"""
import os
import random
import sys

args = [a for a in sys.argv[1:] if not a.startswith('--')]
folder = args[0] if args else os.path.join('out', 'f3')
rematch = [200.0, 330.0, 460.0]
if '--rematch-at' in sys.argv:
    rematch = [float(x) for x in sys.argv[sys.argv.index('--rematch-at') + 1].split(',')]
end = rematch[-1] + 150

rng = random.Random(7)
king = ['32 START', '37 RB', '44 A', '50 A', '56 A', '62 A']
t = 78.0
while t < end:
    king.append(f'{t:.2f} RIGHT 0.6')
    t += 0.7
    for _ in range(6):
        king.append(f'{t:.2f} {rng.choice(["X", "Y", "A"])}')
        t += 0.25
first = ['40 START', '50 A', '56 A', '62 A']
second = []
# the CONTINUE? of each lost match: REMATCH (A on its first choice), by whoever holds the loser's seat then: machine 2
# after the first match, machine 1 after the second, machine 2 after the third
for i, at in enumerate(rematch):
    presses = [f'{at:.2f} A', f'{at + 5:.2f} A', f'{at + 10:.2f} A']
    (second if i % 2 == 0 else first).extend(presses)
for number, lines in ((1, king), (2, first), (3, second)):
    out = os.path.join(folder, f'koth_p{number}.txt')
    with open(out, 'w', encoding='utf-8', newline='\n') as f:
        f.write(f'# machine {number - 1} of a King of the Hill test (gen_koth_scripts.py)\n')
        f.write('\n'.join(lines) + '\n')
    print(out, len(lines), 'lines')
