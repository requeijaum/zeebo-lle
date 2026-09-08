#!/usr/bin/env python3
# Scan AMSS Thumb code for movw/movt pairs that construct a target address,
# to locate the function referencing the audmgr_xdr msg-const (no literal pool).
import struct, sys
from capstone import *

IMG = "/home/rafaelfrequiao/projects/zeebo-lle/nand/1.1.2_AMSS.bin"
BASE = 0x163a8000
d = open(IMG, "rb").read()
def off(va): return va - BASE

# Target address range to hunt (audmgr_xdr send/recv msg-const rows).
LO = int(sys.argv[1], 16) if len(sys.argv) > 1 else 0x17525c14
HI = int(sys.argv[2], 16) if len(sys.argv) > 2 else 0x17525c34

md = Cs(CS_ARCH_ARM, CS_MODE_THUMB)
md.detail = True

# Track per-register movw immediates, then match a movt completing an addr in range.
# Linear sweep in chunks; reset reg state at chunk boundaries (approx, fine for hits).
hits = []
CHUNK = 0x40000
low_va = {}
for insn_bytes_start in range(0, len(d), CHUNK):
    seg = d[insn_bytes_start:insn_bytes_start+CHUNK+4]
    va0 = insn_bytes_start + BASE
    low = {}
    for ins in md.disasm(seg, va0):
        if ins.mnemonic == "movw" and len(ins.operands) == 2:
            reg = ins.operands[0].reg
            imm = ins.operands[1].imm
            low[reg] = (imm, ins.address)
        elif ins.mnemonic == "movt" and len(ins.operands) == 2:
            reg = ins.operands[0].reg
            imm = ins.operands[1].imm
            if reg in low:
                lo_imm, lo_addr = low[reg]
                addr = (imm << 16) | lo_imm
                if LO <= addr <= HI:
                    hits.append((lo_addr, ins.address, addr))
for lo_addr, hi_addr, addr in hits:
    print(f"movw@{lo_addr:08x} movt@{hi_addr:08x} -> 0x{addr:08x}")
if not hits:
    print("no movw/movt pair found in that target range")
