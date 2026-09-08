#!/usr/bin/env python3
import struct, sys
from sig_scan import *

VEC_SET = 0x16f80f0c
VEC_WAIT= 0x16f80f1c

print("== vector contents ==")
for name,vv in [("VEC_SET",VEC_SET),("VEC_WAIT",VEC_WAIT)]:
    print(name, hex(vv), "->", hex(u32(off(vv))))

for label,tgt in [("SET_SIGS_fn",SET_SIGS),("REX_WAIT_fn",REX_WAIT),
                  ("VEC_SET",VEC_SET),("VEC_WAIT",VEC_WAIT)]:
    print("\n== pool refs to %s 0x%08x ==" % (label,tgt))
    for j in find_word(tgt):
        print("  pool@file 0x%x VA 0x%08x" % (j, va(j)))

for label,tgt in [("SET_SIGS_fn",SET_SIGS),("REX_WAIT_fn",REX_WAIT),
                  ("VEC_SET",VEC_SET),("VEC_WAIT",VEC_WAIT)]:
    print("\n== BL/BLX to %s 0x%08x ==" % (label,tgt))
    hits = scan_bl_calls(tgt)
    print("  count:", len(hits))
    for cva,dst,kind in hits[:40]:
        print("  %s @0x%08x -> 0x%08x" % (kind, cva, dst))
