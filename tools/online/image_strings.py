#!/usr/bin/env python3
"""Prints the printable strings of the decrypted game image with their guest addresses (like `strings -t x`).

    python tools/online/image_strings.py [image] [min_len] > out/image_strings.txt

ASCII strings and UTF-16BE strings (UE3 keeps many names as wide strings on the Xbox 360) are both listed; wide ones
are marked with 'W'. The image is out/default_image.bin from tools/xex_image.py (load address 0x82000000).
"""
import os
import re
import sys

REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
IMAGE = sys.argv[1] if len(sys.argv) > 1 else os.path.join(REPO, 'out', 'default_image.bin')
MIN = int(sys.argv[2]) if len(sys.argv) > 2 else 5
BASE = 0x82000000

data = open(IMAGE, 'rb').read()
out = sys.stdout
for m in re.finditer(rb'[\x20-\x7e]{%d,}' % MIN, data):
    out.write(f'{BASE + m.start():08X}   {m.group().decode("ascii")}\n')
for m in re.finditer(rb'(?:\x00[\x20-\x7e]){%d,}' % MIN, data):
    out.write(f'{BASE + m.start():08X} W {m.group()[1::2].decode("ascii")}\n')
