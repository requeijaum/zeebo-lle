#!/usr/bin/env python3
"""
Build the ARM11 VA->PA translation tables from the real MMU dump.
Output: a dict/list usable by the runner's hooks.
Sections (1MB) and COARSE pages (4KB) both become vaddr->pa entries; physical
backing lives in Unicorn flat space indexed by PA.
"""
import re

DUMP = "/home/rafaelfrequiao/projects/zeebo/research/sources/tripleoxygen-wiki/console__zeebo__mmu.txt"

def parse_block(text):
    """VA = PA (SECTION|COARSE|SSECTION). Return list of (va,pa,kind)."""
    out = []
    for line in text.splitlines():
        m = re.match(r'([0-9a-f]{8}) \(VA\) = ([0-9a-f]{8}) \(PA\) \(([A-Z]+)', line)
        if m:
            va = int(m.group(1), 16); pa = int(m.group(2), 16)
            kind = m.group(3).upper()
            out.append((va, pa, kind))
    return out

def main():
    t = open(DUMP).read()
    # ARM11 (BREW) block
    arm11 = t.split('=====ARM11 (BREW)=====')[1].split('</code>')[0]
    # ARM9 block
    arm9 = t.split('=====ARM9 (AMSS)=====')[1].split('=====ARM11')[0].split('</code>')[0]
    a11 = parse_block(arm11)
    a9 = parse_block(arm9)
    print(f"ARM11 entries: {len(a11)}")
    print(f"ARM9 entries: {len(a9)}")
    # Save ARM11 sections+pages as a python module-ish dict
    segs = []
    pgs = []
    for va, pa, k in a11:
        if k in ('SECTION', 'SSECTION'):
            segs.append((va, pa))
        else:
            pgs.append((va, pa))
    with open('/home/rafaelfrequiao/projects/zeebo-lle/tools/arm11_mmu.py','w') as f:
        f.write("# ARM11 (BREW) VA->PA real map from console__zeebo__mmu.txt\n")
        f.write("SECTIONS = [\n")
        for va,pa in segs:
            f.write(f"    (0x{va:08x},0x{pa:08x}),\n")
        f.write("]\n")
        f.write("COARSE = [\n")
        for va,pa in pgs:
            f.write(f"    (0x{va:08x},0x{pa:08x}),\n")
        f.write("]\n")
    # print interesting periphery entries
    print("\nPeriphery re-map (c0 block) ARM11:")
    for va,pa,k in sorted(a11):
        if va>=0xc0000000 and va<0xd0000000:
            print(f"  VA 0x{va:08x} = PA 0x{pa:08x} {k}")
    print("\nKey RAM aliases:")
    for va,pa,k in sorted(a11):
        if va in (0xf0000000,0xf0100000,0x10000000,0x10100000,0x11400000,0xb0000000):
            print(f"  VA 0x{va:08x} = PA 0x{pa:08x} {k}")

if __name__=='__main__': main()