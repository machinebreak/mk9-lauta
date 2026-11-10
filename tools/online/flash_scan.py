"""Finds broken frames in a recording of the game (sa2_capture: captures/<name>.rgba + .txt, e.g. --sa2_video_s=92
--sa2_video_frames=900 on one side of tools/online/pair_test.ps1).

    python tools/online/flash_scan.py <captures/video_Ns.rgba> [--png out_dir] [--top 12]

Counts the frames whose 3D scene is black under the HUD and the ones with large areas of garbage colours (magenta,
yellow, blue or green triangles: draws that read vertices the game had not written yet, mk9_drain_gate), and lists
the worst one-frame flashes: frame i far from both neighbours while the neighbours are close to each other. Writes
the worst ones with their neighbours as PNG strips when --png is given. A recording cut short (no .txt yet) is read
as 640x360.
"""
import argparse
import os
import struct
import zlib

import numpy as np


def write_png(path, rgb):
    h, w, _ = rgb.shape
    raw = b''.join(b'\x00' + rgb[y].tobytes() for y in range(h))

    def chunk(tag, data):
        return struct.pack('>I', len(data)) + tag + data + struct.pack('>I', zlib.crc32(tag + data) & 0xFFFFFFFF)

    with open(path, 'wb') as f:
        f.write(b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', w, h, 8, 2, 0, 0, 0)) +
                chunk(b'IDAT', zlib.compress(raw, 6)) + chunk(b'IEND', b''))


ap = argparse.ArgumentParser()
ap.add_argument('rgba')
ap.add_argument('--png', default='')
ap.add_argument('--top', type=int, default=12)
a = ap.parse_args()

try:
    w, h, fps, n = open(a.rgba[:-5] + '.txt').read().split()[:4]
    w, h, n = int(w), int(h), int(n)
except OSError:
    w, h = 640, 360
    n = os.path.getsize(a.rgba) // (w * h * 4)
frames = np.memmap(a.rgba, dtype=np.uint8, mode='r', shape=(n, h, w, 4))
# a coarse version of each frame (8x8 blocks), RGB as float
small = np.empty((n, h // 8, w // 8, 3), np.float32)
green = np.empty(n, np.float32)
for i in range(n):
    f = frames[i, :, :, :3].astype(np.float32)
    small[i] = f[: h // 8 * 8, : w // 8 * 8].reshape(h // 8, 8, w // 8, 8, 3).mean(axis=(1, 3))
    r, g, b = f[..., 0], f[..., 1], f[..., 2]
    green[i] = np.mean((g > 120) & (g > r * 1.6) & (g > b * 1.6))
center = frames[:, h * 100 // 360:h * 300 // 360, w * 100 // 640:w * 540 // 640, :3].astype(np.float32).mean(axis=(1, 2, 3))
black = [i for i in range(n) if center[i] < 6]
garbage = []
for i in range(n):
    x = frames[i, ::4, ::4, :3].astype(np.int16)
    r, g, b = x[..., 0], x[..., 1], x[..., 2]
    if (((r > 200) & (b > 200) & (g < 80)).mean() > 0.02 or ((r > 200) & (g > 200) & (b < 60)).mean() > 0.05 or
            ((b > 200) & (r < 60) & (g > 60) & (g < 200)).mean() > 0.05 or ((g > 150) & (r < 80) & (b < 80)).mean() > 0.03):
        garbage.append(i)
print(f'{len(black)} frames with the scene black {black[:30]}; {len(garbage)} with garbage colours {garbage[:30]}')
diff = np.abs(small[1:] - small[:-1]).mean(axis=(1, 2, 3))  # diff[i]: frame i+1 vs i
scores = []
for i in range(1, n - 1):
    around = np.abs(small[i + 1] - small[i - 1]).mean()
    d = min(diff[i - 1], diff[i])  # to the previous and to the next
    flash = d - around
    g_jump = green[i] - max(green[i - 1], green[i + 1])
    scores.append((flash + 400 * max(g_jump, 0), i, flash, g_jump, d, around))
scores.sort(reverse=True)
print(f'{n} frames {w}x{h}; mean change between frames {diff.mean():.2f}, 99th percentile {np.percentile(diff, 99):.2f}; '
      f'green share mean {green.mean() * 100:.3f}% max {green.max() * 100:.2f}% (frame {int(green.argmax())})')
for s, i, flash, g_jump, d, around in scores[: a.top]:
    print(f'frame {i:4d}: away from both neighbours by {d:6.2f} (they differ {around:6.2f} from each other), '
          f'green {green[i] * 100:5.2f}% (neighbours {green[i - 1] * 100:5.2f}% / {green[i + 1] * 100:5.2f}%)')
if a.png:
    os.makedirs(a.png, exist_ok=True)
    for s, i, *_ in scores[: a.top]:
        strip = np.concatenate([frames[j, :, :, :3] for j in (i - 1, i, i + 1)], axis=1)
        write_png(os.path.join(a.png, f'flash_{i:04d}.png'), np.ascontiguousarray(strip))
    print('strips in', a.png)
