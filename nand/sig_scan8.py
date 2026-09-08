#!/usr/bin/env python3
import struct
from sig_scan import *

def cstr(a,maxn=64):
    o=off(a); s=b""
    for k in range(maxn):
        if d[o+k]==0: break
        s+=d[o+k:o+k+1]
    return s.decode("latin1","replace")

# 0x16e56b6a: adr r2,#0x74  -> (PC&~3)+0x74 ; PC=addr+4
pc=(0x16e56b6a+4)&~3
print("adr@16e56b6a ->0x%08x : %r"%(pc+0x74,cstr(pc+0x74)))
# also ldr r0/r1 literals at 0x16e56b66/68 [pc,#0x98]
for insva in [0x16e56b66,0x16e56b68,0x16e56b5c]:
    pc=(insva+4)&~3
    for dispv in [0x98,0x7c]:
        addr=pc+dispv
        val=u32(off(addr))
        print("ldr@0x%08x disp0x%x -> 0x%08x  str?=%r"%(insva,dispv,val,cstr(val) if BASE<=val<BASE+N else "?"))
