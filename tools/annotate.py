#!/usr/bin/env python3
"""Annotate a slice of otool -tV output with resolved __cfstring/selref names.

usage: python3 annotate.py /tmp/airportd.asm START END
"""
import re
import struct
import sys

PATH = "/usr/libexec/airportd"
IMAGE_BASE = 0x100000000
data = open(PATH, "rb").read()

slice_off = 0
if struct.unpack_from(">I", data, 0)[0] == 0xCAFEBABE:
    n = struct.unpack_from(">I", data, 4)[0]
    for i in range(n):
        cputype, _s, off, _sz, _a = struct.unpack_from(">iiIII", data, 8 + i * 20)
        if cputype == 0x01000007:
            slice_off = off
            break
blob = data[slice_off:]

segs = []
ncmds = struct.unpack_from("<I", blob, 16)[0]
off = 32
for _ in range(ncmds):
    cmd, cmdsize = struct.unpack_from("<II", blob, off)
    if cmd == 0x19:
        vmaddr, vmsize, fileoff, _fs = struct.unpack_from("<QQQQ", blob, off + 24)
        segs.append((vmaddr, vmsize, fileoff))
    off += cmdsize


def va2off(va):
    for vmaddr, vmsize, fileoff in segs:
        if vmaddr <= va < vmaddr + vmsize:
            return fileoff + (va - vmaddr)
    return None


def decode(p):
    if p == 0 or (p & (1 << 63)):
        return None
    return IMAGE_BASE + (p & 0xFFFFFFFFF)


def rd64(va):
    o = va2off(va)
    if o is None or o + 8 > len(blob):
        return 0
    return struct.unpack_from("<Q", blob, o)[0]


def cstr(va, limit=200):
    o = va2off(va)
    if o is None:
        return None
    end = blob.find(b"\0", o, min(o + limit, len(blob)))
    if end <= o:
        return None
    s = blob[o:end]
    if any(c < 0x20 or c > 0x7E for c in s):
        return None
    return s.decode()


def obj_str(va):
    c = decode(rd64(va + 16))          # __CFConstantString.cstr
    return cstr(c) if c is not None else None


def resolve(insn, disp, kind):
    target = (insn + 7 + disp) & 0xFFFFFFFFFFFFFFFF
    if kind == "leaq":
        return target, obj_str(target)
    q = decode(rd64(target))
    if q is None:
        return target, None
    d = cstr(q)
    if d is not None:
        return target, d
    return target, obj_str(q)


pat = re.compile(r"^([0-9a-f]{16})\t(\w+)\t(.*?)0x([0-9a-f]+)\(%rip\)")
start, end = int(sys.argv[2]), int(sys.argv[3])
for i, line in enumerate(open(sys.argv[1], errors="replace"), 1):
    if i < start or i > end:
        continue
    line = line.rstrip("\n")
    m = pat.match(line)
    if m:
        insn = int(m.group(1), 16)
        kind = m.group(2)
        disp = int(m.group(4), 16)
        if disp > 0x7FFFFFFF:
            disp -= 1 << 32
        _t, s = resolve(insn, disp, kind)
        if s:
            line += "    ; " + s
    print(line)
