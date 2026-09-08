#!/usr/bin/env python3
import struct
from sig_scan import *

for base in [0x16f80f0c,0x16f80f1c]:
    print("veneer @0x%08x:"%base)
    for k in range(-2,4):
        a=base+k*4
        print("   [0x%08x]=0x%08x"%(a,u32(off(a))))

for tgt in [0x1730f2ab,0x1730f443,0x1730f2aa,0x1730f442]:
    print("\npool refs 0x%08x:"%tgt, [hex(va(j)) for j in find_word(tgt)])

# BLX from ARM callers: search whole image for ARM BLX(imm) and BL to veneers.
# But simpler: find ALL veneers (0xe51ff004) whose stored ptr == set_sigs/wait, giving alt vectors.
VEN=struct.pack("<I",0xe51ff004)
idx=0
setv=[];waitv=[]
while True:
    j=d.find(VEN,idx)
    if j<0:break
    idx=j+4
    ptr=u32(j+4)
    if ptr in (0x1730f2aa,0x1730f2ab): setv.append(va(j))
    if ptr in (0x1730f442,0x1730f443): waitv.append(va(j))
print("\nset_sigs veneers:",[hex(x) for x in setv])
print("wait veneers:",[hex(x) for x in waitv])
