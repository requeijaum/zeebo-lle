#!/usr/bin/env python3
"""After flash_read_config, where does boot go? Map heap generously (0xc00000-0x1000000),
also 0xb0000000 identity RAM. Log control transfers leaving .text region
[0xa00000,0xa30000] (i.e. jumps into heap/RAM) to catch alloc/board_init derail."""
import struct, sys, capstone
from unicorn import *
from unicorn.arm_const import *
sys.path.insert(0,"/home/rafaelfrequiao/projects/zeebo-lle/tools")
from nand_controller import NandController, NAND_BASE
from dmov_model import DMOVModel, DMOV_SD1_BASE, DMOV_NAND_CHAN
ND="nand"; IMG="firmware/openzeebo-zloader.bin"
bl=open(IMG,'rb').read()
md=capstone.Cs(capstone.CS_ARCH_ARM,capstone.CS_MODE_ARM)
uc=Uc(UC_ARCH_ARM,UC_MODE_ARM)
try: uc.ctl_set_cpu_model(UC_CPU_ARM_1176)
except: pass
# map code + low RAM + heap large + high
for base,size in ((0x00a00000,0x00600000),(0x00000000,0x00300000),(0x00c00000,0x00400000),
                  (0xb0000000,0x01000000),(0xff000000,0x00200000),(0xffe00000,0x00200000)):
    try: uc.mem_map(base,size)
    except: pass
for base in (0xb8000000,0xc0000000,0xa9a00000,0xa8600000,0xa9000000,0xa9700000,0xa9400000,0xaa600000,0xa0a00000,0xa0e00000,0xa8240000):
    try: uc.mem_map(base,0x1000)
    except: pass
uc.mem_write(0xa00000, bl)
nand=NandController(f"{ND}/1.1.2.bin",f"{ND}/1.1.2_spare.bin")
dmov=DMOVModel(uc,nand)
sticky={}
def hu(uc,a,ad,sz,v,u):
    try: uc.mem_map(ad&~0xFFF,0x1000)
    except: pass
    return True
def hm(uc,a,ad,sz,v,u):
    if ad<0x80000000: return
    if DMOV_SD1_BASE<=ad<DMOV_SD1_BASE+0x400:
        if a==UC_MEM_WRITE:
            if ad==DMOV_SD1_BASE+(DMOV_NAND_CHAN<<2): dmov.exec_cmdptr(v)
            sticky[ad]=v
        else:
            off=ad-DMOV_SD1_BASE; val=sticky.get(ad,0)
            if off in (0x200,0x204,0x208,0x20c): val=3
            elif off in (0x40,0x44,0x48,0x4c): val=0x80000002
            uc.mem_write(ad,struct.pack("<I",val&0xffffffff)[:sz])
        return
    if NAND_BASE<=ad<NAND_BASE+0x400:
        o=ad-NAND_BASE
        if a==UC_MEM_WRITE: nand.write(o,v,sz)
        else: uc.mem_write(ad,struct.pack("<I",nand.read(o,sz)&0xffffffff)[:sz])
        return
    if a==UC_MEM_WRITE: sticky[ad]=v; return
    val=sticky.get(ad,0)
    uc.mem_write(ad,struct.pack("<I",val&0xffffffff)[:sz])
uc.hook_add(UC_HOOK_MEM_READ_UNMAPPED|UC_HOOK_MEM_WRITE_UNMAPPED|UC_HOOK_MEM_FETCH_UNMAPPED,hu)
uc.hook_add(UC_HOOK_MEM_READ|UC_HOOK_MEM_WRITE,hm)
import collections
out=collections.deque(maxlen=20)
st={"n":0,"prev":0,"gone":False}
def hc(uc,ad,sz,u):
    st["n"]+=1
    p=st["prev"]
    if p and ad!=p+4:
        # leaving text span [a00000,a30000] into other region
        if not (0xa00000<=ad<0xa30000) and (0xa00000<=p<0xa30000) and not st["gone"]:
            st["gone"]=True
            b=bytes(uc.mem_read(p,4))
            try: dis=next(md.disasm(b,p)); dd="%s %s"%(dis.mnemonic,dis.op_str)
            except: dd="?"
            print("EXIT .text: %08x -> %08x [%s] lr=%08x"%(p,ad,dd,uc.reg_read(UC_ARM_REG_LR)))
            print("  last 20 transfers:")
            for t in out: print("   %08x -> %08x [%s]"%t)
            uc.emu_stop()
    if p and ad!=p+4:
        b=bytes(uc.mem_read(p,4))
        try: dis=next(md.disasm(b,p)); dd="%s %s"%(dis.mnemonic,dis.op_str)
        except: dd="?"
        out.append((p,ad,dd))
    st["prev"]=ad
    if st["n"]>2_000_000: uc.emu_stop()
uc.hook_add(UC_HOOK_CODE,hc)
uc.reg_write(UC_ARM_REG_SP,0x00bff000)
try: uc.emu_start(0xa00028,0xFFFFFFFF,count=0)
except UcError as e: print("err",e)
print("gone:",st["gone"],"final pc=0x%x n=%d DMOV=%d"%(uc.reg_read(UC_ARM_REG_PC),st["n"],dmov.exec_count))