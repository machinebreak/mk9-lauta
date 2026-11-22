"""The game's hashes.

mk9_hash: MK9's name hash (sub_82360DB8): crc = T[(toupper(c) ^ crc) & 0xFF] ^ (crc >> 8) from 0, T the MSB-first
CRC-32 table of 0x04C11DB7 (the engine makes it at run time). It names a package's script container (<pkg>-%08x).
fnv1: FNV-1 (32 bits), the engine's lookup of a fighter script's symbols (char_<x>_player_data, ..._audio_override).
"""


def _table():
    t = []
    for i in range(256):
        c = i << 24
        for _ in range(8):
            c = ((c << 1) ^ 0x04C11DB7) & 0xFFFFFFFF if c & 0x80000000 else (c << 1) & 0xFFFFFFFF
        t.append(c)
    return t


TABLE = _table()


def mk9_hash(s):
    crc = 0
    for ch in s.upper():
        crc = TABLE[(ord(ch) ^ crc) & 0xFF] ^ (crc >> 8)
    return crc


def fnv1(s):
    h = 0x811C9DC5
    for ch in s.encode('latin-1'):
        h = ((h * 0x01000193) & 0xFFFFFFFF) ^ ch
    return h


KNOWN = [('CHAR_SCORPION_C', 0x4eef787d), ('CHAR_Scorpion_D', 0x50a82878), ('CHAR_Scorpion', 0x9a081491),
         ('CHAR_Scorpion_B', 0x4a2e65ca), ('NPC_Scorpion_C_Baby', 0xe1a26c69), ('CHAR_Noob_D', 0xb942838f),
         (r'..\Asset\CHAR_Scorpion.xxx', 0x0d7b130c)]
assert all(mk9_hash(s) == want for s, want in KNOWN)
