#!/usr/bin/env python3
"""Capture the FIRST control flow that takes PC above the loaded image (0xa02400 =
len 0x24c8 of the bin). Log that transfer + the previous function's end."""
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
for base,size in ((0x00a00000,0x00600000),(0x00000000,0x00300000),(0x00c00000,0x00400000),
                  (0xb0000000,0x01000000),(0xff000000,0x00200000),(0xffe00000,0x00200000)):
    try: uc.mem_map(base,size)
    except: pass
for base in (0xb8000000,0xc0000000,0xa9a00000,0xa8600000,0xa9000000,0xa9700000,0xa9400000,0xaa600000,0xa0a00000):
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
st={"n":0,"prev":0,"cap":False}
IMGEND=0xa00000+len(bl)   # ~0xa024c8
def hc(uc,ad,sz,u):
    st["n"]+=1
    p=st["prev"]
    if not st["cap"] and ad>=IMGEND and p<IMGEND:
        st["cap"]=True
        b=bytes(uc.mem_read(p,4))
        try: dis=next(md.disasm(b,p)); dd="%s %s"%(dis.mnemonic,dis.op_str)
        except: dd="?"
        print("PC crossed IMG end: 0x%08x(->)0x%08x [%s]"%(p,ad,dd))
        print("  lr=%08x sp=%08x r0=%08x"%(uc.reg_read(UC_ARM_REG_LR),uc.reg_read(UC_ARM_REG_SP),uc.reg_read(UC_ARM_REG_R0)))
        uc.emu_stop()
    st["prev"]=ad
    if st["n"]>1_000_000: uc.emu_stop()
uc.hook_add(UC_HOOK_CODE,hc)
uc.reg_write(UC_ARM_REG_SP,0x00bff000)
try: uc.emu_start(0xa00028,0xFFFFFFFF,count=0)
except UcError as e: print("err",e)
print("cap:",st["cap"],"final pc=0x%x n=%d"%(uc.reg_read(UC_ARM_REG_PC),st["n"]))