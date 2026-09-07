#!/usr/bin/env python3
"""D: locate exact L4e syscall thunks in APPS code segment (bytes, not every word)."""
import re, struct, capstone
d=open("/home/rafaelfrequiao/projects/zeebo-lle/nand/1.1.2_APPS.bin","rb").read()
md=capstone.Cs(capstone.CS_ARCH_ARM,capstone.CS_MODE_ARM)
# code segments only (executable): 0x1013a000@0x76000 covers most text
CODE=[(0x00076000,0x1013a000,0x12d2000),
      (0x00030000,0xb0000000,0xf207),
      (0x00060000,0xb0100000,0x702b),
      (0x00041000,0xb0400000,0x152a0),
      (0x00072000,0x10137000,0x3000)]
def tova(fo):
    for off,va,sz in CODE:
        if off<=fo<off+sz: return va+(fo-off)
    return None
# known L4e syscall immediates from our recon
targets={0x14:"svc#0x14 (sel ~0x4b)", 0x140c:"svc#0x140c", 0x540000:None}
# search exact bytes EF000014, EF00140C across whole file, keep hits in code segs
for imm in (0x14,0x140c,0x30000):
    pat=struct_bytes=bytes([0xEF,(imm>>16)&0xff,(imm>>8)&0xff,imm&0xff])
    hits=[m.start() for m in re.finditer(re.escape(pat),d)]
    code=[h for h in hits if tova(h) is not None]
    print(f"svc #0x{imm:x}: total {len(hits)}, in-code {len(code)} at {[hex(tova(h)) for h in code[:10]]}")

# Now the rex_self leaf pattern on L4e variant: it may be a global load via a
# different pattern (may use 2 ldr or ldr rX;[rX];bx on different reg). Search
# common "ldr r_any,[pc,#]; ldr r_any,[r_any]; bx lr" leaves in code.
print("\nscan code for leaf 'ldr r,[pc]; ldr r,[r]; bx lr'")
cand=[]
for off,va,sz in CODE:
    for i in range(off,off+sz-8,2):
        w0=struct.unpack_from("<I",d,i)[0]; w1=struct.unpack_from("<I",d,i+4)[0]
        # w0 ldr rd,[pc,#im] E59Fdxxx ; w1 ldr rd2,[rn] E59[rn]... detect ldr rd,[rm] = E59r0rm ... 
        if (w0 & 0x0FFF0000)==0x059F0000:
            rd=(w0>>12)&0xF
            # w1 = ldr rd2,[rm]  E5900000|rm<<16|rd2<<12|0 ; check rd2==rd and rm==rd
            if (w1 & 0x0FFFF000)==(0xE5900000|(rd<<12)| (rd<<16)):
                cand.append(va+(i-off))
# also try returning r0 immediately after (pattern may differ)
print("cands:",[hex(c) for c in cand[:20]])

# Also: disasm around the known thunk 0x103dcd14 region to see the full syscall stub set
fo=None
for off,va,sz in CODE:
    if va<=0x103dcd00<va+sz: fo=off+(0x103dcd00-va)
if fo:
    print("\n=== disasm 0x103dcc00..0x103dcd80 (L4e syscall stubs) ===")
    for ins in md.disasm(d[fo:fo+0x180],0x103dcc00):
        print(f"  {ins.address:08x}: {ins.mnemonic:7} {ins.op_str}")