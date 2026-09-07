#!/usr/bin/env python3
"""Debug: why does the BSS zero loop (0xa00050-58) spin forever? Print r1/r2 each
visit and the memory at the BSS literal, live."""
import struct, sys
from unicorn import *
from unicorn.arm_const import *
sys.path.insert(0,"/home/rafaelfrequiao/projects/zeebo-lle/tools")
from nand_controller import NandController, NAND_BASE
from dmov_model import DMOVModel, DMOV_SD1_BASE
ND="nand"; IMG="firmware/openzeebo-zloader.bin"
bl=open(IMG,'rb').read()
uc=Uc(UC_ARCH_ARM,UC_MODE_ARM)
try: uc.ctl_set_cpu_model(UC_CPU_ARM_1176)
except: pass
for base,size in ((0x00a00000,0x00400000),(0x00000000,0x00200000),(0xff000000,0x00200000),(0xffe00000,0x00200000)):
    try: uc.mem_map(base,size)
    except: pass
for base in (0xb8000000,0xc0000000,0xaa600000,0xa9700000,0xa9400000,0xa0a00000):
    try: uc.mem_map(base,0x1000)
    except: pass
uc.mem_write(0xa00000, bl); uc.mem_write(0x00c00000,b"\x00"*4096)
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
            if ad==DMOV_SD1_BASE+0x00c: dmov.exec_cmdptr(v)
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
printed=0
st={"n":0}
def hc(uc,ad,sz,u):
    global printed
    st["n"]+=1
    if ad in (0xa00050,0xa00058) and printed<6:
        r1=uc.reg_read(UC_ARM_REG_R1); r2=uc.reg_read(UC_ARM_REG_R2)
        print("pc=%04x r1=0x%x r2=0x%x diff=%d"%(ad,r1,r2,r2-r1))
        printed+=1
    if st["n"]>200_000: uc.emu_stop()
uc.hook_add(UC_HOOK_CODE,hc)
uc.reg_write(UC_ARM_REG_SP,0x00bff000)
uc.emu_start(0xa00028,0xFFFFFFFF,count=0)
print("end pc=0x%x n=%d"%(uc.reg_read(UC_ARM_REG_PC),st["n"]))
print("mem @0xa00088 =",hex(struct.unpack("<I",bytes(uc.mem_read(0xa00088,4)))[0]))