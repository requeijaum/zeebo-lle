#!/usr/bin/env python3
# Path B: bounded seeded property/differential fuzzing of firmware ABI + fpage routines.
# Extracts ACTUAL routine bytes from ELF PT_LOAD into a bounded Unicorn harness (no full machine).
# Compares actual ARM32 execution vs independent mathematical contracts.
import struct, json, os, sys, time, random
from unicorn import *
from unicorn.arm_const import *

NAND = '/home/rafaelfrequiao/projects/zeebo-lle/nand/1.1.2_APPS.bin'
OUT  = '/home/rafaelfrequiao/projects/zeebo-lle/notes/boot-investigation/path-b'
os.makedirs(OUT, exist_ok=True)
if not os.path.exists(NAND):
    sys.stderr.write('FATAL: NAND working copy missing: %s\n'%NAND); sys.exit(2)
D = open(NAND,'rb').read()

# --- locate RX segment holding routines (vaddr 0xb0000000, fileoff 0x30000) ---
SEG_VA=0xb0000000; SEG_OFF=0x30000
e_phoff,=struct.unpack_from('<I',D,28); e_phnum,=struct.unpack_from('<H',D,44)
seg=None
for i in range(e_phnum):
    o=e_phoff+i*32
    pt,po,pv,pp,pf,pm,pflags,pa=struct.unpack_from('<IIIIIIII',D,o)
    if pt==1 and pv==SEG_VA:
        seg=(po,pv,pf); break
assert seg and seg[0]==SEG_OFF, seg
SEG_BYTES=D[SEG_OFF:SEG_OFF+0x10000]   # bounded copy of the code page window

CODE_BASE=0xb0000000
STACK_BASE=0x00100000; STACK_SIZE=0x10000
DATA_BASE =0x00200000; DATA_SIZE =0x10000   # scratch: KIP, out-ptrs, sentinels
GLOBAL_R4 =0x00210000                        # target of minpage's global ptr load

def mk_uc():
    uc=Uc(UC_ARCH_ARM,UC_MODE_ARM)
    uc.mem_map(CODE_BASE,0x10000)
    uc.mem_write(CODE_BASE,SEG_BYTES)
    uc.mem_map(STACK_BASE,STACK_SIZE)
    uc.mem_map(DATA_BASE,DATA_SIZE)
    return uc

RET_MAGIC=0xdead0000  # sentinel return address; we stop when PC hits it

# ============================================================
# CONTRACT MODELS (independent of firmware bytes; pure ARM32 math)
# ============================================================
def ctz32(x):
    x&=0xffffffff
    if x==0: return None
    n=0
    while (x>>n)&1==0: n+=1
    return n

def model_minpage(pageinfo):
    """l4e_min_pagesize contract: ctz(PageInfo & ~0x3ff), guard mask==0."""
    masked=pageinfo & (~0x3ff & 0xffffffff)
    return ctz32(masked)   # None => infinite-loop hazard in firmware

def model_largest_fpage(addr, lo, hi, minlog2):
    """Independent re-derivation of largest_aligned_fpage (0xb000d4dc) by ARM32 semantics.
    Returns (fpage_descriptor, size_log2) or (0,None) if r2<r1 or range too small."""
    addr&=0xffffffff; lo&=0xffffffff; hi&=0xffffffff
    if hi<lo: return (0,None)               # cmp r2,r1; bhs -> if hi<lo return 0
    span=(hi-lo)&0xffffffff
    minsz=(1<<minlog2)&0xffffffff           # bl d4c8 => 1<<minpage
    if (span+1)&0xffffffff < minsz:         # add r4,#1; cmp r4,r0; blo -> return 0
        return (0,None)
    k=minlog2                               # start size_log2 = minpage
    # loop d520..d550: try to grow k while aligned block stays within [lo,hi]
    while k<=0x1f:
        k1=k+1
        base=((addr>>k1)<<k1)&0xffffffff    # align_down(addr, 2^(k+1))
        blk=(1<<k1)&0xffffffff
        top=(base+blk-1)&0xffffffff
        if base < lo: break                 # blo d554
        if top > hi:  break                 # bhi d554
        if base+blk-1 < base: break         # overflow guard (blo after add,sub)
        k=k1
        if k1>=0x1f: break                  # cmp r1,#0x1f ; bls
    # encode d554..d568: base=align_down(addr,2^k); fpage=(base&~0x3f0)|((k&0x3f)<<4); clear low nibble
    base=((addr>>k)<<k)&0xffffffff
    v=(base & (~0x3f0 & 0xffffffff)) | ((k&0x3f)<<4)
    v&=(~0xf & 0xffffffff)
    return (v,k)

# ============================================================
# ACTUAL EXECUTION under Unicorn (real firmware bytes)
# ============================================================
def run_minpage(pageinfo, max_insns=20000):
    """Execute 0xb000d464. Global ptr [pc,#0x54] resolves to some VA; we intercept the
    KernelInterface call (bl 0xb000c720) by stubbing it to return, and pre-seed the KIP at
    the pointer the routine dereferences. Simpler: patch to skip cache, feed KIP via [r0]."""
    uc=mk_uc()
    # global at [pc(0xb000d468)+8 +0x54] = 0xb000d4c4 -> read that ptr from firmware
    gptr_addr=0xb000d468+8+0x54
    gptr=struct.unpack_from('<I',SEG_BYTES,gptr_addr-CODE_BASE)[0]
    # map the global's target page if in reach; else redirect via DATA. We map GLOBAL_R4 region
    # and rewrite the literal so [pc,#0x54] yields GLOBAL_R4 (bounded, deterministic).
    uc.mem_write(gptr_addr, struct.pack('<I',GLOBAL_R4))
    uc.mem_map(GLOBAL_R4 & ~0xfff, 0x2000)
    uc.mem_write(GLOBAL_R4, struct.pack('<I',0))   # cache counter = 0 -> forces recompute path
    # KIP object: KernelInterface returns r0 = KIP base; KIP[0xc8]=pageinfo
    KIP=0x00220000; uc.mem_map(KIP,0x1000); uc.mem_write(KIP+0xc8, struct.pack('<I',pageinfo))
    # stub KernelInterface (0xb000c720): set r0=KIP and return to LR
    def hook_code(uc,addr,size,ud):
        if addr==0xb000c720:
            uc.reg_write(UC_ARM_REG_R0,KIP)
            uc.reg_write(UC_ARM_REG_PC,uc.reg_read(UC_ARM_REG_LR))
    uc.hook_add(UC_HOOK_CODE,hook_code,begin=0xb000c720,end=0xb000c721)
    sp=STACK_BASE+STACK_SIZE-0x100
    uc.reg_write(UC_ARM_REG_SP,sp); uc.reg_write(UC_ARM_REG_LR,RET_MAGIC)
    try:
        uc.emu_start(0xb000d464, RET_MAGIC, count=max_insns)
    except UcError as ex:
        if uc.reg_read(UC_ARM_REG_PC)!=RET_MAGIC: return ('ERR',str(ex),uc.reg_read(UC_ARM_REG_PC))
    if uc.reg_read(UC_ARM_REG_PC)!=RET_MAGIC:
        return ('TIMEOUT',None,None)
    return ('OK',uc.reg_read(UC_ARM_REG_R0),None)

def run_largest_fpage(addr,lo,hi,pageinfo,max_insns=50000):
    uc=mk_uc()
    gptr_addr=0xb000d468+8+0x54
    uc.mem_write(gptr_addr, struct.pack('<I',GLOBAL_R4))
    uc.mem_map(GLOBAL_R4 & ~0xfff,0x2000); uc.mem_write(GLOBAL_R4,struct.pack('<I',0))
    KIP=0x00220000; uc.mem_map(KIP,0x1000); uc.mem_write(KIP+0xc8,struct.pack('<I',pageinfo))
    def hook_code(uc,a,s,ud):
        if a==0xb000c720:
            uc.reg_write(UC_ARM_REG_R0,KIP); uc.reg_write(UC_ARM_REG_PC,uc.reg_read(UC_ARM_REG_LR))
    uc.hook_add(UC_HOOK_CODE,hook_code,begin=0xb000c720,end=0xb000c721)
    sp=STACK_BASE+STACK_SIZE-0x100
    uc.reg_write(UC_ARM_REG_SP,sp); uc.reg_write(UC_ARM_REG_LR,RET_MAGIC)
    uc.reg_write(UC_ARM_REG_R0,addr); uc.reg_write(UC_ARM_REG_R1,lo); uc.reg_write(UC_ARM_REG_R2,hi)
    try:
        uc.emu_start(0xb000d4dc,RET_MAGIC,count=max_insns)
    except UcError as ex:
        if uc.reg_read(UC_ARM_REG_PC)!=RET_MAGIC: return ('ERR',str(ex))
    if uc.reg_read(UC_ARM_REG_PC)!=RET_MAGIC: return ('TIMEOUT',None)
    return ('OK',uc.reg_read(UC_ARM_REG_R0)&0xffffffff)

# ============================================================
# KernelInterface wrapper (0xb000c720) ABI / saved-register fuzz
#   Primary hypothesis: a host SVC hook that writes IP+0/4/8 clobbers saved r4,r5,r6.
#   MODELED hook (labeled) — NOT production integration (Path A owns that).
# ============================================================
def run_wrapper(out4,out5,out6, sent_r4,sent_r5,sent_r6, ret1,ret2,ret3, hook_mode):
    """hook_mode: 'good' restores sp=ip only; 'bad_stack' writes ip+0/4/8; 'bad_reg' clobbers r4."""
    uc=mk_uc()
    for p in (out4,out5,out6):
        if p: uc.mem_write(p, b'\x00\x00\x00\x00')
    sp=STACK_BASE+STACK_SIZE-0x200
    uc.reg_write(UC_ARM_REG_SP,sp); uc.reg_write(UC_ARM_REG_LR,RET_MAGIC)
    # caller passes output ptrs in r0,r1,r2 (wrapper copies to r4,r5,r6)
    uc.reg_write(UC_ARM_REG_R0,out4); uc.reg_write(UC_ARM_REG_R1,out5); uc.reg_write(UC_ARM_REG_R2,out6)
    # caller-saved sentinels in r4,r5,r6 must survive the whole call (push/pop)
    uc.reg_write(UC_ARM_REG_R4,sent_r4); uc.reg_write(UC_ARM_REG_R5,sent_r5); uc.reg_write(UC_ARM_REG_R6,sent_r6)
    def hook_intr(uc,intno,ud):
        # SVC #0x14: model kernel. ip holds caller's post-push sp (mov ip,sp before sp clobber).
        ip=uc.reg_read(UC_ARM_REG_IP)
        uc.reg_write(UC_ARM_REG_R1,ret1); uc.reg_write(UC_ARM_REG_R2,ret2); uc.reg_write(UC_ARM_REG_R3,ret3)
        if hook_mode in ('good','bad_stack','bad_reg'):
            uc.reg_write(UC_ARM_REG_SP,ip)  # restore magic sp -> caller frame (required)
        if hook_mode=='bad_stack':
            uc.mem_write(ip+0, struct.pack('<I',0xbad0bad0))
            uc.mem_write(ip+4, struct.pack('<I',0xbad0bad4))
            uc.mem_write(ip+8, struct.pack('<I',0xbad0bad8))
        if hook_mode=='bad_reg':
            uc.reg_write(UC_ARM_REG_R4,0xdeadbeef)
        # advance PC past svc
        uc.reg_write(UC_ARM_REG_PC,0xb000c73c)
    uc.hook_add(UC_HOOK_INTR,hook_intr)
    try:
        uc.emu_start(0xb000c720,RET_MAGIC,count=200)
    except UcError as ex:
        return {'err':str(ex),'pc':hex(uc.reg_read(UC_ARM_REG_PC))}
    res={'pc':uc.reg_read(UC_ARM_REG_PC)}
    res['r4']=uc.reg_read(UC_ARM_REG_R4); res['r5']=uc.reg_read(UC_ARM_REG_R5); res['r6']=uc.reg_read(UC_ARM_REG_R6)
    res['sp']=uc.reg_read(UC_ARM_REG_SP)
    res['w4']=struct.unpack('<I',uc.mem_read(out4,4))[0] if out4 else None
    res['w5']=struct.unpack('<I',uc.mem_read(out5,4))[0] if out5 else None
    res['w6']=struct.unpack('<I',uc.mem_read(out6,4))[0] if out6 else None
    return res

if __name__=='__main__':
    t0=time.time(); print('harness self-check', flush=True)
    print('minpage(0x01111006) actual=',run_minpage(0x01111006),' model=',model_minpage(0x01111006))
    print('largest(0xb0d00000,0xb0d00000,0xb6d00000) actual=',
          run_largest_fpage(0xb0d00000,0xb0d00000,0xb6d00000,0x01111006),
          ' model=',model_largest_fpage(0xb0d00000,0xb0d00000,0xb6d00000,12))
    print('elapsed',round(time.time()-t0,2))
