#!/usr/bin/env python3
"""Find the CODE that reads the NAND register literals (0xa0a00000+). Scan every
word for ldr<reg>,[pc,#imm] that resolves to a literal equal to a 0xa0a00000 offset,
then disasm that code site. This localizes the NAND driver entry points."""
import struct, capstone
d=open("/home/rafaelfrequiao/projects/zeebo-lle/nand/1.1.2_APPSBL.bin","rb").read()
md=capstone.Cs(capstone.CS_ARCH_ARM,capstone.CS_MODE_ARM)
# all literal offsets holding 0xa0a00000+
nand_fos=set()
for i in range(0,len(d)-4,4):
    w=struct.unpack_from("<I",d,i)[0]
    if (w&0xfffff000)==0xa0a00000:
        nand_fos.add(i)
# scan code for pc-relative ldr resolving into one of those
sites=set()
for i in range(0,len(d)-4,2):
    w=struct.unpack_from("<I",d,i)[0]
    if (w & 0x0fff0000)==0x059f0000:  # ldr rX,[pc,#imm]
        im=w&0xfff; rn=(w>>12)&0xf
        lit=(i+8+im)&~0x3
        if lit in nand_fos:
            sites.add(i)
print("ldr-pc code sites referencing NAND literals:",len(sites))
for s in sorted(sites)[:30]:
    print("  code @ 0x%04x -> literal @0x%04x (0x%x)"%(s, s+8+(struct.unpack_from("<I",d,s)[0]&0xfff)&~0x3,
        struct.unpack_from("<I",d,(s+8+(struct.unpack_from("<I",d,s)[0]&0xfff))&~0x3)[0]))
# disasm a padded window around the earliest sites to see the driver's start
if sites:
    lo=min(sites)
    print("\n== disasm window around earliest NAND code site (%04x) =="%lo)
    start=(lo//4)*4
    for ins in md.disasm(d[start-0x40:start+0x80],start-0x40):
        print("  %04x  %-8s %s"%(ins.address,ins.mnemonic,ins.op_str))