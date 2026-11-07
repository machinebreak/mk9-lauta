#!/usr/bin/env python3
"""Writes a mk9_input_script_file for a pair test: the menu path to a Versus fight, then a busy fight (walking in,
attacks, blocks, jumps, crouches) from a fixed seed, so a test exercises hits, damage and the AI-free fight logic.

    python tools/online/fight_script.py host out/f3/host_fight.txt [--seed 1] [--from 78] [--to 120]
    python tools/online/fight_script.py guest out/f3/guest_fight.txt [--seed 2]

host: player 1 (the left fighter: forward = RIGHT); guest: player 2 (forward = LEFT). The menu path: title (START),
"2 PLAYERS" (RB), the guest joins (START), VERSUS MODE (A), both pick their default fighter (A), then confirm.
"""
import argparse
import random

MENU = {
    'host': [(32, 'START'), (37, 'RB'), (44, 'A'), (50, 'A'), (56, 'A'), (62, 'A')],
    'guest': [(40, 'START'), (50, 'A'), (56, 'A'), (62, 'A')],
}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('role', choices=['host', 'guest'])
    ap.add_argument('out')
    ap.add_argument('--seed', type=int, default=None)
    ap.add_argument('--from', dest='start', type=float, default=78)
    ap.add_argument('--to', dest='end', type=float, default=120)
    a = ap.parse_args()
    rng = random.Random(a.seed if a.seed is not None else (1 if a.role == 'host' else 2))
    forward = 'RIGHT' if a.role == 'host' else 'LEFT'
    back = 'LEFT' if a.role == 'host' else 'RIGHT'
    lines = [f'{t} {b}' for t, b in MENU[a.role]]
    t = a.start
    lines.append(f'{t:.2f} {forward} 1.5')  # close the distance
    t += 1.7
    while t < a.end:
        r = rng.random()
        if r < 0.45:
            lines.append(f'{t:.2f} {rng.choice(["X", "Y", "A", "B"])}')
            t += rng.choice([0.15, 0.2, 0.3])
        elif r < 0.6:
            lines.append(f'{t:.2f} {forward} {rng.choice([0.2, 0.4, 0.6])}')
            t += 0.5
        elif r < 0.7:
            lines.append(f'{t:.2f} {back} {rng.choice([0.2, 0.3])}')  # block / walk back
            t += 0.4
        elif r < 0.8:
            lines.append(f'{t:.2f} DOWN {rng.choice([0.2, 0.3])}')
            t += 0.35
        elif r < 0.88:
            lines.append(f'{t:.2f} UP 0.1')
            t += 0.6
        else:
            lines.append(f'{t:.2f} RB 0.2')  # block
            t += 0.3
    open(a.out, 'w').write('\n'.join(lines) + '\n')
    print(f'{a.out}: {len(lines)} lines')


if __name__ == '__main__':
    main()
