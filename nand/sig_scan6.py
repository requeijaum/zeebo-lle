#!/usr/bin/env python3
import struct
from sig_scan import *

# resolve veneers used by pump
for v in [0x16f80ef4,0x16f80efc,0x16f80f0c,0x16f80f24]:
    print("veneer 0x%08x -> 0x%08x"%(v,u32(off(v+4))))

# module import table around 0x16611edc (wait veneer) - dump neighborhood
print("\n== import table near 0x16611e00 ==")
for a in range(0x16611e00,0x16611f40,4):
    w=u32(off(a))
    tag=""
    if w==0xe51ff004: tag="<veneer>"
    print("  0x%08x: 0x%08x %s"%(a,w,tag))
