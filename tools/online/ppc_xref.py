#!/usr/bin/env python3
"""Cross-references in MK9's Xbox 360 image: which code builds a given address, and which data holds it.

    python tools/online/ppc_xref.py <addr|"text"> [...]        addresses in hex (8200EBE8) or a string to look up first
    python tools/online/ppc_xref.py --func <addr>               the function that contains an address
    python tools/online/ppc_xref.py --calls <addr>        the direct calls and branches to an address
    options: --image path   (default out/default_image.bin, the loaded image from tools/xex_image.py)
             --range N      also report references to target..target+N-1 (a struct or a string's tail)

Code references are found by constant propagation over each function (from .pdata, plus the leaf functions only the
recompiler's list in app/generated/default/mk9_init.cpp has) (lis/addis, addi, ori, mr and the
D/DS-form loads and stores off a known register), linear in program order, with the volatile registers r0, r3-r12
forgotten at every call: the usual way 32-bit addresses are built on PowerPC (lis rX,hi; addi rY,rX,lo or
lwz rY,lo(rX)). Data references are aligned big-endian words equal to the target in .rdata/.data.
Output lines: "code <ins addr> in <func start> : <how>" and "data <word addr>".
"""
import argparse
import bisect
import os
import re
import struct
import sys

BASE = 0x82000000
REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
DEFAULT_IMAGE = os.path.join(REPO, 'out', 'default_image.bin')
# The recompiler's function list: it also has the leaf functions that .pdata leaves out (nothing to unwind).
FUNC_LIST = os.path.join(REPO, 'app', 'generated', 'default', 'mk9_init.cpp')


class Image:
    def __init__(self, path):
        self.data = open(path, 'rb').read()
        d = self.data
        pe = struct.unpack_from('<I', d, 0x3C)[0]
        nsec = struct.unpack_from('<H', d, pe + 6)[0]
        optsize = struct.unpack_from('<H', d, pe + 20)[0]
        self.sections = {}
        for i in range(nsec):
            name, vsize, vaddr = struct.unpack_from('<8sII', d, pe + 24 + optsize + i * 40)
            self.sections[name.rstrip(b'\0').decode()] = (BASE + vaddr, BASE + vaddr + vsize)
        # .pdata: (BeginAddress, Data) big-endian; Data: PrologLength 8 bits | FunctionLength 22 bits (instructions).
        lo, hi = self.sections['.pdata']
        funcs = []
        for off in range(lo - BASE, hi - BASE - 7, 8):
            begin, info = struct.unpack_from('>II', d, off)
            if begin == 0:
                continue
            length = ((info >> 8) & 0x3FFFFF) * 4
            if length:
                funcs.append((begin, begin + length))
        funcs.sort()
        self.funcs = funcs
        self.func_starts = [f[0] for f in funcs]
        # Functions only the recompiler knows run to the next known function's start.
        if os.path.exists(FUNC_LIST):
            listed = {int(m.group(1), 16) for m in
                      re.finditer(r'\{ 0x([0-9A-Fa-f]{8}), sub_', open(FUNC_LIST, encoding='utf-8').read())}
            starts = sorted(listed | set(self.func_starts))
            extra = []
            for i, start in enumerate(starts):
                if start in listed and self.func_of(start) is None and i + 1 < len(starts):
                    extra.append((start, starts[i + 1]))
            funcs = sorted(funcs + extra)
            self.funcs = funcs
            self.func_starts = [f[0] for f in funcs]

    def u32(self, addr):
        return struct.unpack_from('>I', self.data, addr - BASE)[0]

    def func_of(self, addr):
        i = bisect.bisect_right(self.func_starts, addr) - 1
        if i >= 0 and self.funcs[i][0] <= addr < self.funcs[i][1]:
            return self.funcs[i]
        return None

    def find_string(self, text):
        hits = []
        for enc in (text.encode('ascii'), b''.join(b'\0' + bytes([c]) for c in text.encode('ascii'))):
            start = 0
            while (i := self.data.find(enc, start)) >= 0:
                hits.append(BASE + i)
                start = i + 1
        return sorted(set(hits))


def sext16(v):
    return v - 0x10000 if v & 0x8000 else v


LOADS_STORES = {32: 'lwz', 33: 'lwzu', 34: 'lbz', 35: 'lbzu', 36: 'stw', 37: 'stwu', 38: 'stb', 39: 'stbu',
                40: 'lhz', 41: 'lhzu', 42: 'lha', 43: 'lhau', 44: 'sth', 45: 'sthu', 46: 'lmw', 47: 'stmw',
                48: 'lfs', 49: 'lfsu', 50: 'lfd', 51: 'lfdu', 52: 'stfs', 53: 'stfsu', 54: 'stfd', 55: 'stfdu'}
WRITES_RD = {32, 33, 34, 35, 40, 41, 42, 43, 46}  # loads into a GPR (rD); the update forms also write rA


def scan(img, targets, span):
    """Yields (ins_addr, func_start, value, how) for every built address in [t, t+span) of any target."""
    want = sorted(targets)

    def hit(v):
        i = bisect.bisect_right(want, v) - 1
        return i >= 0 and want[i] <= v < want[i] + span

    for fstart, fend in img.funcs:
        val = [None] * 32
        for a in range(fstart, fend, 4):
            ins = img.u32(a)
            op = ins >> 26
            rd = (ins >> 21) & 31
            ra = (ins >> 16) & 31
            imm = ins & 0xFFFF
            if op == 15:  # addis / lis
                base = 0 if ra == 0 else val[ra]
                val[rd] = None if base is None else (base + (sext16(imm) << 16)) & 0xFFFFFFFF
            elif op == 14:  # addi / li
                if ra == 0:
                    val[rd] = sext16(imm) & 0xFFFFFFFF
                elif val[ra] is not None:
                    v = (val[ra] + sext16(imm)) & 0xFFFFFFFF
                    if hit(v):
                        yield a, fstart, v, f'addi r{rd},r{ra},{sext16(imm)}'
                    val[rd] = v
                else:
                    val[rd] = None
            elif op == 24:  # ori rA,rS,UIMM
                if val[rd] is not None:
                    v = val[rd] | imm
                    if hit(v):
                        yield a, fstart, v, f'ori r{ra},r{rd},0x{imm:x}'
                    val[ra] = v
                else:
                    val[ra] = None
            elif op in LOADS_STORES:
                if ra != 0 and val[ra] is not None:
                    v = (val[ra] + sext16(imm)) & 0xFFFFFFFF
                    if hit(v):
                        yield a, fstart, v, f'{LOADS_STORES[op]} r{rd},{sext16(imm)}(r{ra})'
                    if op & 1 and op != 47:  # update forms write the effective address back
                        val[ra] = v
                if op in WRITES_RD:
                    if op == 46:
                        for r in range(rd, 32):
                            val[r] = None
                    else:
                        val[rd] = None
            elif op in (58, 62):  # ld / std (DS-form)
                if ra != 0 and val[ra] is not None:
                    v = (val[ra] + sext16(imm & 0xFFFC)) & 0xFFFFFFFF
                    if hit(v):
                        yield a, fstart, v, f'{"ld" if op == 58 else "std"} r{rd},{sext16(imm & 0xFFFC)}(r{ra})'
                if op == 58:
                    val[rd] = None
            elif op == 31:
                xo = (ins >> 1) & 0x3FF
                if xo == 444 and rd == ((ins >> 11) & 31):  # mr rA,rS (or rA,rS,rS)
                    val[ra] = val[rd]
                elif xo in (151, 183, 215, 247, 407, 439, 663, 695, 727, 759, 918, 662, 150, 214, 981):
                    pass  # indexed stores (no GPR written except update forms, handled loosely)
                else:
                    val[rd] = None
                    val[ra] = None
            elif op == 18:  # b / bl
                if ins & 1:
                    for r in (0, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12):
                        val[r] = None
            elif op == 19:  # bclr / bcctr with link: a call through a register
                if ins & 1:
                    for r in (0, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12):
                        val[r] = None
            elif op in (7, 8, 12, 13, 20, 21, 23, 28, 29, 30, 4):
                # mulli, subfic, addic(.), rlwimi, rlwinm, rlwnm, andi., andis., rld*, VMX: rD or rA written
                val[rd] = None
                val[ra] = None
            elif op in (16, 17, 10, 11, 3, 2, 59, 63, 57, 61, 56, 60):
                pass  # branches, compares, traps, floating point: no GPR written
            else:
                val[rd] = None
                val[ra] = None


def data_refs(img, targets, span):
    out = []
    want = sorted(targets)
    for name in ('.rdata', '.data'):
        lo, hi = img.sections[name]
        d = img.data
        for off in range(lo - BASE, hi - BASE - 3, 4):
            v = struct.unpack_from('>I', d, off)[0]
            i = bisect.bisect_right(want, v) - 1
            if i >= 0 and want[i] <= v < want[i] + span:
                out.append((BASE + off, v))
    return out


def callers(img, targets):
    """Direct calls and branches (b / bl / bc) to any target: (ins addr, func start, kind)."""
    want = set(targets)
    lo, hi = img.sections['.text']
    for a in range(lo, hi, 4):
        ins = img.u32(a)
        op = ins >> 26
        if op == 18:
            li = ins & 0x03FFFFFC
            if li & 0x02000000:
                li -= 0x04000000
            t = (li if ins & 2 else a + li) & 0xFFFFFFFF
            kind = 'bl' if ins & 1 else 'b'
        elif op == 16:
            bd = ins & 0xFFFC
            if bd & 0x8000:
                bd -= 0x10000
            t = (bd if ins & 2 else a + bd) & 0xFFFFFFFF
            kind = 'bcl' if ins & 1 else 'bc'
        else:
            continue
        if t in want:
            f = img.func_of(a)
            yield a, (f[0] if f else 0), kind, t


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('targets', nargs='*')
    ap.add_argument('--image', default=DEFAULT_IMAGE)
    ap.add_argument('--range', type=lambda s: int(s, 0), default=1)
    ap.add_argument('--func', action='store_true', help='print the containing function of each address')
    ap.add_argument('--calls', action='store_true', help='list the direct branches and calls to each address')
    args = ap.parse_args()
    img = Image(args.image)

    if args.calls:
        for a, f, kind, t in callers(img, [int(t, 16) for t in args.targets]):
            print(f'{kind:3} {a:08X} in sub_{f:08X} -> {t:08X}')
        return

    if args.func:
        for t in args.targets:
            a = int(t, 16)
            f = img.func_of(a)
            print(f'{a:08X}: ' + (f'function {f[0]:08X}-{f[1]:08X} (sub_{f[0]:08X})' if f else 'no function'))
        return

    targets = []
    for t in args.targets:
        try:
            targets.append(int(t, 16))
        except ValueError:
            found = img.find_string(t)
            print(f'"{t}": ' + (', '.join(f'{a:08X}' for a in found) if found else 'not found'))
            targets.extend(found)
    if not targets:
        return
    span = max(args.range, 1)
    for a, f, v, how in scan(img, targets, span):
        print(f'code {a:08X} in sub_{f:08X} : {how} -> {v:08X}')
    for a, v in data_refs(img, targets, span):
        print(f'data {a:08X} -> {v:08X}')


if __name__ == '__main__':
    main()
