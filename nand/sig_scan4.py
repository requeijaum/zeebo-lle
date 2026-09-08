#!/usr/bin/env python3
import struct
from sig_scan import *

SET_VEN  = [0x16f80f04]
WAIT_VEN = [0x16f80f14, 0x16611edc]

def report(label, veneers):
    print("\n#### %s callers ####" % label)
    allhits=[]
    for v in veneers:
        for cva,dst,kind in scan_bl_calls(v):
            allhits.append((cva,dst,kind,v))
    allhits.sort()
    for cva,dst,kind,v in allhits:
        regs = recover_args(cva)
        r0=regs.get(ARM_REG_R0); r1=regs.get(ARM_REG_R1)
        r0s = ("0x%08x"%r0) if isinstance(r0,int) else str(r0)
        r1s = ("0x%08x"%r1) if isinstance(r1,int) else str(r1)
        print("  %s @0x%08x ->ven0x%08x  r0(tcb)=%s  r1(mask)=%s" % (kind,cva,v,r0s,r1s))
    return allhits

set_hits  = report("rex_set_sigs", SET_VEN)
wait_hits = report("rex_wait",     WAIT_VEN)

# dump masks summary for set_sigs
print("\n#### set_sigs mask histogram ####")
from collections import Counter
c=Counter()
for cva,dst,kind,v in set_hits:
    r1=recover_args(cva).get(ARM_REG_R1)
    if isinstance(r1,int): c[r1]+=1
for mask,n in c.most_common():
    bits=[i for i in range(32) if mask>>i&1]
    print("  mask 0x%08x  x%d  bits=%s"%(mask,n,bits))
