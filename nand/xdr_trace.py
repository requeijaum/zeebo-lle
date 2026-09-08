#!/usr/bin/env python3
# Trace the audmgr xdr serializer to recover the set_device_mode arg layout.
import struct, sys
from capstone import *

IMG = "/home/rafaelfrequiao/projects/zeebo-lle/nand/1.1.2_AMSS.bin"
BASE = 0x163a8000
d = open(IMG, "rb").read()

def off(va): return va - BASE
def va(o):   return o + BASE
def u32(o):  return struct.unpack_from("<I", d, o)[0]

def find_pool_ref(target_va):
    p = struct.pack("<I", target_va)
    refs = []
    idx = 0
    while True:
        j = d.find(p, idx)
        if j < 0: break
        refs.append(j); idx = j + 4
    return refs

def disasm_thumb(start_va, length, detail=False):
    md = Cs(CS_ARCH_ARM, CS_MODE_THUMB)
    md.detail = True
    o = off(start_va)
    return list(md.disasm(d[o:o+length], start_va))

if __name__ == "__main__":
    # arg1 = hex VA to disasm, arg2 = length
    sva = int(sys.argv[1], 16)
    ln  = int(sys.argv[2], 16) if len(sys.argv) > 2 else 0x100
    for ins in disasm_thumb(sva, ln):
        print(f"{ins.address:08x}: {ins.bytes.hex():12} {ins.mnemonic:8} {ins.op_str}")
