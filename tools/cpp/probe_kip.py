#!/usr/bin/env python3
# zeebo_kipsys - probe AMSS/APPS for L4e KIP syscall links and svc sites.
# refman N1 rev2 ARM: syscalls are `bl` to KIP links (base ~0xFE0000xx), ret r14.
import struct, capstone, sys

md = capstone.Cs(capstone.CS_ARCH_ARM, capstone.CS_MODE_ARM)
d = open(sys.argv[1] if len(sys.argv) > 1 else "/tmp/a.bin", "rb").read()

def segs(df):
    e_phoff = struct.unpack_from("<I", df, 28)[0]
    e_phentsize = struct.unpack_from("<H", df, 42)[0]
    e_phnum = struct.unpack_from("<H", df, 44)[0]
    out = []
    for i in range(e_phnum):
        o = e_phoff + i * e_phentsize
        pt, p_off, p_va, p_pa, p_fs, p_ms = struct.unpack_from("<6I", df, o)
        if pt == 1 and p_ms:
            out.append((p_va, p_off, p_fs, p_ms))
    return out

S = segs(d)

def vaddr_of(fo):
    for va, off, fs, ms in S:
        if off <= fo < off + fs:
            return va + (fo - off)
    return None

print("== svc sites (EFxxxxxx, byte-verified) ==")
n = 0
for i in range(0, len(d) - 4, 2):
    w = struct.unpack_from("<I", d, i)[0]
    if (w & 0x0F000000) == 0x0F000000 and w != 0x0FFFFFFF:
        va = vaddr_of(i)
        if va:
            print("  0x%08x svc#%#x" % (va, w & 0xFFFFFF))
            n += 1
            if n > 25:
                break
print("(first %d svc sites)" % n)

print("\n== bl -> 0xFE000000..0xFE0FFFFF (KIP link style) ==")
found = 0
for i in range(0, len(d) - 4, 2):
    w = struct.unpack_from("<I", d, i)[0]
    if (w & 0xFF000000) == 0xEB000000:  # bl
        imm = w & 0xFFFFFF
        if imm & 0x800000:
            imm -= 0x1000000
        tgt = (i + 8 + (imm << 2)) & 0xFFFFFFFF
        if 0xFE000000 <= tgt < 0xFE100000:
            va = vaddr_of(i)
            print("  @0x%08x bl -> 0x%08x   (KIP-link style)" % (va if va else i, tgt))
            found += 1
            if found > 20:
                break
print("found %d bl->KIP-range" % found)