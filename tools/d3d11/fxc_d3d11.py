"""Compile the translator's HLSL (with its RENUT_D3D11 branches) with FXC: ps_5_0 / vs_5_0, no text transforms.

    python fxc_d3d11.py <hlsl dir>
"""
import collections
import os
import re
import subprocess
import sys
import time

FXC = r'C:\Program Files (x86)\Windows Kits\10\bin\10.0.26100.0\x64\fxc.exe'


def main():
    d = sys.argv[1]
    files = sorted(f for f in os.listdir(d) if f.endswith('.hlsl'))
    ok = collections.Counter()
    bad = collections.Counter()
    errors = {}
    times = []
    for f in files:
        p = os.path.join(d, f)
        head = open(p, encoding='utf-8', errors='replace').readline()
        profile = 'ps_5_0' if 'pixel shader' in head else 'vs_5_0'
        t0 = time.time()
        r = subprocess.run([FXC, '/nologo', '/D', 'RENUT_RECOMP=1', '/D', 'RENUT_D3D11=1', '/T', profile, '/E', 'main',
                            '/Fo', p + '.dxbc', p], capture_output=True, text=True)
        times.append(time.time() - t0)
        if r.returncode == 0:
            ok[profile] += 1
        else:
            bad[profile] += 1
            lines = [l for l in (r.stdout + r.stderr).split('\n') if 'error' in l]
            first = lines[0] if lines else (r.stdout + r.stderr)[:200]
            key = re.sub(r'^.*?\.hlsl\(\d+,\d+(-\d+)?\): ', '', first)[:160]
            errors.setdefault(key, []).append(f)
    print(f'compiled {dict(ok)}, failed {dict(bad)}; FXC {sum(times) / max(1, len(times)):.2f} s mean, '
          f'{max(times) if times else 0:.2f} s max')
    for key, fs in sorted(errors.items(), key=lambda kv: -len(kv[1]))[:10]:
        print(f'  {len(fs)}x {key}   (e.g. {fs[0]})')


if __name__ == '__main__':
    main()
