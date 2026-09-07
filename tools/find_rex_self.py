#!/usr/bin/env python3
"""Fingerprint rex_self() and the svc#0x14 thunk in the APPS ELF."""
import struct, re, capstone
d=open("/home/rafaelfrequiao/projects/zeebo-lle/nand/1.1.2_APPS.bin","rb").read()
md=capstone.Cs(capstone.CS_ARCH_ARM,capstone.CS_MODE_ARM)
segs=[(0x00008000,0xf0000000,0x19714),(0x00024000,0xf001c000,0x6000),
      (0x00030000,0xb0000000,0xf207),(0x00040000,0xb0040000,0x178),
      (0x00041000,0xb0400000,0x152a0),(0x00057000,0xb0d00000,0x2000),
      (0x0005a000,0xb0e00000,0x4000),(0x00060000,0xb0100000,0x702b),
      (0x00068000,0xb0140000,0xd8),(0x00069000,0xb0300000,0x80d4),
      (0x00072000,0x10137000,0x3000),(0x00076000,0x1013a000,0x12d2000),
      (0x01349000,0x1140c000,0x646a4),(0x013ae000,0x14903000,0x14)]
def tova(fo):
    for off,va,sz in segs:
        if off<=fo<off+sz: return va+(fo-off)
    return None
def tofo(va):
    for off,v,sz in segs:
        if v<=va<v+sz: return off+(va-v)
    return None
str_vas=[0x10304274,0x1035fd52,0x103794b2]
print("Part A: find ldr-literal ADRs to rex_self() strings, then the shared bl target.")
bl_targets=set()
for sva in str_vas:
    hits=[m.start() for m in re.finditer(re.escape(struct.pack("<I",sva)),d)]
    code_hits=[]
    for seg_off,seg_va,seg_sz in segs[:11]:
        for i in range(seg_off, seg_off+seg_sz, 4):
            w=struct.unpack_from("<I",d,i)[0]
            if (w & 0x0fff0000)==0x059f0000:
                im=w&0xfff
                target=((seg_va+(i-seg_off)+8+im)&~0x3)
                if target==sva:
                    code_hits.append(seg_va+(i-seg_off))
    print(f"  str 0x{sva:08x}: ldr-literal code at {[hex(h) for h in code_hits[:5]]}")
    for c in code_hits[:3]:
        fo=tofo(c)
        if fo is None: continue
        # disasm ~60 bytes to find bl
        for ins in md.disasm(d[fo-4:fo+60],c-4):
            if ins.mnemonic.startswith('bl'):
                # resolve target
                tgt=ins.address+8+ins.operands[0].imm
                bl_targets.add(tgt)
                print(f"    @0x{c:08x} bl -> 0x{tgt:08x}")
print("\nPart B: candidate rex_self()/rex_wait()/sched call targets (bl destinations)")
for t in sorted(bl_targets):
    print(f"  0x{t:08x}")