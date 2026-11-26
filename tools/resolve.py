#!/usr/bin/env python3
"""Resolve __cfstring / selref targets of rip-relative loads in airportd.

Input: lines "<insn_va_hex> <disp_hex>" (disp may be signed).  Handles a universal
binary (x86_64 slice) and DYLD_CHAINED_PTR_64 fixups.
"""
import struct
import sys

PATH = "/usr/libexec/airportd"
IMAGE_BASE = 0x100000000

data = open(PATH, "rb").read()

# ---- pick the x86_64 slice of the universal binary -------------------------
slice_off = 0
magic = struct.unpack_from(">I", data, 0)[0]
if magic == 0xCAFEBABE:
    n = struct.unpack_from(">I", data, 4)[0]
    for i in range(n):
        cputype, _sub, off, size, _align = struct.unpack_from(">iiIII", data, 8 + i * 20)
        if cputype == 0x01000007:
            slice_off = off
            break
blob = data[slice_off:]

# ---- segment map (slice-relative) -----------------------------------------
segs = []
ncmds = struct.unpack_from("<I", blob, 16)[0]
off = 32
for _ in range(ncmds):
    cmd, cmdsize = struct.unpack_from("<II", blob, off)
    if cmd == 0x19:
        name = blob[off + 8:off + 24].rstrip(b"\0").decode()
        vmaddr, vmsize, fileoff, filesize = struct.unpack_from("<QQQQ", blob, off + 24)
        segs.append((name, vmaddr, vmsize, fileoff))
    off += cmdsize


def va2off(va):
    for _name, vmaddr, vmsize, fileoff in segs:
        if vmaddr <= va < vmaddr + vmsize:
            return fileoff + (va - vmaddr)
    return None


def decode_ptr(p):
    """DYLD_CHAINED_PTR_64: low 36 bits = offset from image base; bit 63 = bind."""
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


def resolve(target, kind):
    if kind == "lea":
        # leaq: the address IS the object (e.g. a __cfstring in __DATA_CONST)
        c = decode_ptr(rd64(target + 16))
        return cstr(c) if c is not None else None
    q = decode_ptr(rd64(target))
    if q is None:
        return None
    direct = cstr(q)
    if direct is not None:
        return direct                     # selector reference
    c = decode_ptr(rd64(q + 16))          # __CFConstantString.cstr
    if c is not None:
        return cstr(c)
    return None


for line in sys.stdin:
    parts = line.split()
    if len(parts) < 2:
        continue
    insn = int(parts[0], 16)
    disp = int(parts[1], 16)
    kind = parts[2] if len(parts) > 2 else "mov"
    if disp > 0x7FFFFFFF:
        disp -= 1 << 32
    target = (insn + 7 + disp) & 0xFFFFFFFFFFFFFFFF
    print("%x -> %x  %s" % (insn, target, resolve(target, kind) or "?"))
