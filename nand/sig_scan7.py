#!/usr/bin/env python3
import struct
from sig_scan import *

for m in [0x00180000,0x00100000,0x00080000]:
    refs=find_word(m)
    print("mask 0x%08x : %d literal-pool refs"%(m,len(refs)))
    for j in refs:
        print("   VA 0x%08x"%va(j))
    print()

# who references pump tcb pointer word 0x176b79ee besides init?
print("refs to pump tcb 0x176b79ee:",[hex(va(j)) for j in find_word(0x176b79ee)])
print("refs to 0x176b79ef(thumb):",[hex(va(j)) for j in find_word(0x176b79ef)])
