"""The baseline's numbers (tools/online/dev/baseline.ps1's logs in out/f3/baseline): for each scenario, the first fight
of the session (from its epoch's start plus the intro's 2 s to its mode's end; offline: the log's last 30 s), the shown
frames' percentiles ([fps]), the game thread's budget ([frame]), the render thread's work ([renut] game thread: the
thread that issues the Direct3D calls, UE3's render thread), the rollback's numbers ([net]), the latency probe
([latency]), the start (the log's first line to the first Swap) and the memory (memory.txt).

    python tools/online/dev/baseline_report.py [out/f3/baseline]
"""
import datetime
import os
import re
import statistics
import sys

BASE = sys.argv[1] if len(sys.argv) > 1 else os.path.join(
    os.path.dirname(os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))), 'out', 'f3', 'baseline')
STAMP = re.compile(r'^\[(\d{4}-\d\d-\d\d \d\d:\d\d:\d\d\.\d+)\]')


def when(line):
    m = STAMP.match(line)
    return datetime.datetime.strptime(m.group(1), '%Y-%m-%d %H:%M:%S.%f') if m else None


def floats(pattern, line):
    m = re.search(pattern, line)
    return [float(x) for x in m.groups()] if m else None


def window(lines, offline):
    stamps = [when(l) for l in lines]
    first = next(t for t in stamps if t)
    last = next(t for t in reversed(stamps) if t)
    if offline:
        return first, last - datetime.timedelta(seconds=30), last
    start = end = None
    for line, t in zip(lines, stamps):
        if start is None and '[net] epoch 1 started' in line:
            start = t + datetime.timedelta(seconds=2)
        elif start is not None and end is None and "the fight's mode ended" in line:
            end = t
    return first, start, end or last


def report(path, offline):
    lines = open(path, encoding='utf-8', errors='replace').read().splitlines()
    first, start, end = window(lines, offline)
    out = {}
    swap = next((when(l) for l in lines if '[mk9] Swap 1:' in l), None)
    out['start_s'] = (swap - first).total_seconds() if swap else None
    if not start:
        return out
    inside = [l for l in lines if (t := when(l)) and start <= t <= end]
    out['fight_s'] = (end - start).total_seconds()
    fps = [floats(r'\[fps\] ([\d.]+) fps .*longest frame ([\d.]+) ms, (\d+) over 20 ms.*p50 ([\d.]+) ms, p90 ([\d.]+), p99 ([\d.]+)', l)
           for l in inside]
    fps = [f for f in fps if f]
    if fps:
        out['fps'] = statistics.mean(f[0] for f in fps)
        out['p50'] = statistics.median(f[3] for f in fps)
        out['p99_median'] = statistics.median(f[5] for f in fps)
        out['p99_worst'] = max(f[5] for f in fps)
        out['longest'] = max(f[1] for f in fps)
        out['over20'] = sum(f[2] for f in fps)
    frame = [floats(r'its work ([\d.]+) ms a frame \(p50 ([\d.]+), p99 ([\d.]+), most ([\d.]+)\); waiting for the render '
                    r'thread ([\d.]+) ms \(p99 ([\d.]+), most ([\d.]+)\); the frame boundary ([\d.]+) ms \(p99 ([\d.]+), '
                    r'most ([\d.]+)\)', l) for l in inside]
    frame = [f for f in frame if f]
    if frame:
        out['work'] = statistics.mean(f[0] for f in frame)
        out['work_p99'] = max(f[2] for f in frame)
        out['wait'] = statistics.mean(f[4] for f in frame)
        out['boundary'] = statistics.mean(f[7] for f in frame)
        out['boundary_p99'] = max(f[8] for f in frame)
    render = [floats(r'\[renut\] game thread, [\d.]+ s: \d+ frames, ([\d.]+) ms a frame from Swap to Swap \(most ([\d.]+) '
                     r'ms\), ([\d.]+) ms a frame paced', l) for l in inside]
    render = [r for r in render if r]
    if render:
        out['render_work'] = statistics.mean(r[0] for r in render)
        out['render_most'] = max(r[1] for r in render)
        out['render_paced'] = statistics.mean(r[2] for r in render)
    latency = [floats(r'input -> present ([\d.]+) \(max ([\d.]+)\)', l) for l in inside]
    latency = [x for x in latency if x]
    if latency:
        out['latency'] = statistics.mean(x[0] for x in latency)
        out['latency_max'] = max(x[1] for x in latency)
    net = [l for l in inside if '[net] epoch 1 frame' in l and 'rollback:' in l]
    if net:
        l = net[-1]
        g = floats(r'rtt ([\d.]+) ms, delay (\d+)', l)
        out['rtt'], out['delay'] = g if g else (None, None)
        g = floats(r'rollback: (\d+) rollbacks \((\d+) frames run again, deepest (\d+)\), (\d+) frames predicted, (\d+) wrong', l)
        if g:
            out['rollbacks'], out['resim'], out['deepest'], out['predicted'], out['wrong'] = g
            frames = floats(r'epoch 1 frame (\d+)', l)[0]
            out['rollbacks_min'] = g[0] / max(frames / 3600.0, 1e-9)
            out['resim_share'] = g[1] / max(frames, 1)
            out['depth_mean'] = g[1] / max(g[0], 1)
        g = floats(r'keeping a frame ([\d.]+) ms, a rollback\'s restore ([\d.]+) ms; at rest ([\d.]+) ms a boundary \(render '
                   r'([\d.]+), pools and queries ([\d.]+), park ([\d.]+); longest ([\d.]+)', l)
        if g:
            out['keep'], out['restore'], out['rest'], out['rest_render'], out['rest_pools'], out['rest_park'], out['rest_longest'] = g
        g = floats(r'(\d+) frames run again and (\d+) live with no snapshot', l)
        if g:
            out['light_resim'], out['light_live'] = g
    return out


def fmt(v):
    if v is None:
        return '-'
    if isinstance(v, float):
        return f'{v:.2f}' if v < 100 else f'{v:.0f}'
    return str(v)


rows = {}
for name in sorted(os.listdir(BASE)):
    if not name.endswith('.log'):
        continue
    scenario, role = name[:-4].rsplit('_', 1)
    rows[f'{scenario} {role}'] = report(os.path.join(BASE, name), role == 'offline')
keys = ['start_s', 'fight_s', 'fps', 'p50', 'p99_median', 'p99_worst', 'longest', 'over20', 'work', 'work_p99', 'wait',
        'boundary', 'boundary_p99', 'render_work', 'render_most', 'render_paced', 'latency', 'latency_max', 'rtt', 'delay',
        'rollbacks', 'rollbacks_min', 'resim_share', 'depth_mean', 'deepest', 'wrong', 'keep', 'restore', 'rest',
        'rest_render', 'rest_longest', 'light_resim', 'light_live']
names = list(rows)
print('| measure | ' + ' | '.join(names) + ' |')
print('|---|' + '---|' * len(names))
for k in keys:
    print(f'| {k} | ' + ' | '.join(fmt(rows[n].get(k)) for n in names) + ' |')
mem = os.path.join(BASE, 'memory.txt')
if os.path.exists(mem):
    print()
    print(open(mem, encoding='utf-8-sig').read())
