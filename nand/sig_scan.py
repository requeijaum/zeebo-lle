#!/usr/bin/env python3
# Scan AMSS for rex_set_sigs / rex_wait call sites and recover (tcb, mask).
import struct, sys
from capstone import *
from capstone.arm import *

IMG  = "/home/rafaelfrequiao/projects/zeebo-lle/nand/1.1.2_AMSS.bin"
BASE = 0x163a8000
d = open(IMG, "rb").read()
N = len(d)

SET_SIGS = 0x1730f2aa
REX_WAIT = 0x1730f442

def off(va): return va - BASE
def va(o):   return o + BASE
def u32(o):  return struct.unpack_from("<I", d, o)[0] if 0 <= o <= N-4 else None

def find_word(target):
    p = struct.pack("<I", target)
    res, idx = [], 0
    while True:
        j = d.find(p, idx)
        if j < 0: break
        res.append(j); idx = j + 4
    return res

md = Cs(CS_ARCH_ARM, CS_MODE_THUMB)
md.detail = True

def disasm(start_va, length):
    o = off(start_va)
    return list(md.disasm(d[o:o+length], start_va))

def recover_args(call_va, window=0x60):
    """Disasm ~window bytes before call, track ldr rX,[pc,#imm] -> literal pool.
       Return dict reg->value for r0..r3 at call point (best effort, linear)."""
    start = call_va - window
    # align to even
    start &= ~1
    regs = {}
    for ins in disasm(start, window + 8):
        if ins.address >= call_va: break
        m = ins.mnemonic
        if m.startswith("ldr") and len(ins.operands) >= 2:
            dst = ins.operands[0]
            src = ins.operands[1]
            if dst.type == ARM_OP_REG and src.type == ARM_OP_MEM:
                if src.mem.base == ARM_REG_PC:
                    pc = (ins.address + 4) & ~3
                    addr = pc + src.mem.disp
                    val = u32(off(addr))
                    regs[dst.reg] = val
                else:
                    regs[dst.reg] = None
        elif m in ("mov","movs","mov.w") and len(ins.operands)>=2:
            dst=ins.operands[0]; src=ins.operands[1]
            if dst.type==ARM_OP_REG:
                if src.type==ARM_OP_IMM: regs[dst.reg]=src.imm
                elif src.type==ARM_OP_REG and src.reg in regs: regs[dst.reg]=regs[src.reg]
                else: regs[dst.reg]=None
        elif m in ("adds","add","sub","subs","lsl","lsls","orr","orrs") and len(ins.operands)>=1:
            dst=ins.operands[0]
            if dst.type==ARM_OP_REG: regs[dst.reg]=None
    return regs

def rn(reg):
    return {ARM_REG_R0:"r0",ARM_REG_R1:"r1",ARM_REG_R2:"r2",ARM_REG_R3:"r3"}.get(reg,str(reg))

def scan_bl_calls(target_va):
    """Find Thumb BL/BLX to target across whole image (brute, aligned to 2)."""
    hits=[]
    tgt = target_va & ~1
    # iterate over 2-byte aligned positions, decode BL pairs
    for o in range(0, N-4, 2):
        h0 = struct.unpack_from("<H", d, o)[0]
        if (h0 & 0xf800) != 0xf000:  # first half of BL/BLX
            continue
        h1 = struct.unpack_from("<H", d, o+2)[0]
        if (h1 & 0xe000) != 0xe000:
            continue
        # BL if bit12 of h1 set (0xf800 pattern), BLX if 0xe800
        S = (h0>>10)&1
        imm10 = h0 & 0x3ff
        j1=(h1>>13)&1; j2=(h1>>11)&1; imm11=h1&0x7ff
        i1 = 1-(j1^S); i2=1-(j2^S)
        imm = (imm11<<1)|(imm10<<12)|(i2<<22)|(i1<<23)|(S<<24)
        if S: imm |= ~((1<<25)-1)  # sign extend
        pc = va(o)+4
        is_blx = (h1 & 0x1000)==0
        if is_blx:
            dst = (pc & ~3) + imm
        else:
            dst = pc + imm
        if (dst & ~1) == tgt:
            hits.append((va(o), dst, "BLX" if is_blx else "BL"))
    return hits

if __name__ == "__main__":
    print("=== literal-pool words holding SET_SIGS 0x%08x ===" % SET_SIGS)
    for j in find_word(SET_SIGS):
        print("  pool@ file 0x%x  VA 0x%08x" % (j, va(j)))
    print("=== literal-pool words holding REX_WAIT 0x%08x ===" % REX_WAIT)
    for j in find_word(REX_WAIT):
        print("  pool@ file 0x%x  VA 0x%08x" % (j, va(j)))
    print("scan done")
