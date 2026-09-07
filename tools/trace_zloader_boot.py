#!/usr/bin/env python3
"""Trace the OpenZeebo zloader boot from entry: log control transfers + MMIO so we
see where it derails (or loops) and what it touches. Same wiring as
boot_zloader_unuicorn."""
import struct, collections, sys, capstone
from unicorn import *
from unicorn.arm_const import *
sys.path.insert(0,"/home/rafaelfrequiao/projects/zeebo-lle/tools")
from nand_controller import NandController, NAND_BASE
from dmov_model import DMOVModel, DMOV_SD1_BASE, DMOV_NAND_CHAN
ND="/home/rafaelfrequiao/projects/zeebo-lle/nand"
IMG="/home/rafaelfrequiao/projects/zeebo-lle/firmware/openzeebo-zloader.bin"
LOAD=0x00a00000
md=capstone.Cs(capstone.CS_ARCH_ARM,capstone.CS_MODE_ARM)
bl=open(IMG,'rb').read()
uc=Uc(UC_ARCH_ARM,UC_MODE_ARM)
try: uc.ctl_set_cpu_model(UC_CPU_ARM_1176)
except: pass
for base,size in ((0x00a00000,0x00400000),(0x00000000,0x00200000),(0xff000000,0x00200000),(0xffe00000,0x00200000)):
    try: uc.mem_map(base,size)
    except: pass
for base in (0xb8000000,0xc0000000,0xa9a00000,0xa8600000,0xa9000000,0xa9700000,0xa9400000,0xaa600000,0xa0a00000,0xa0800000):
    try: uc.mem_map(base,0x1000)
    except: pass
uc.mem_write(LOAD, bl); uc.mem_write(0x00c00000,b"\x00"*4096)
nand=NandController(f"{ND}/1.1.2.bin",f"{ND}/1.1.2_spare.bin")
dmov=DMOVModel(uc,nand)
sticky={}
mmio=collections.OrderedDict()
def hu(uc,a,ad,sz,v,u):
    try: uc.mem_map(ad&~0xFFF,0x1000)
    except: pass
    return True
def hm(uc,a,ad,sz,v,u):
    if ad<0x80000000: return
    pc=uc.reg_read(UC_ARM_REG_PC)
    if DMOV_SD1_BASE<=ad<DMOV_SD1_BASE+0x400:
        off=ad-DMOV_SD1_BASE
        if a==UC_MEM_WRITE:
            if ad==DMOV_SD1_BASE+0x00c: dmov.exec_cmdptr(v)
            sticky[ad]=v; return
        val=sticky.get(ad,0)
        if off in (0x200,0x204,0x208,0x20c): val=3
        elif off in (0x40,0x44,0x48,0x4c): val=0x80000002
        uc.mem_write(ad,struct.pack("<I",val&0xffffffff)[:sz])
        mmio.setdefault(ad,[]).append((pc,a))
        return
    if NAND_BASE<=ad<NAND_BASE+0x400:
        o=ad-NAND_BASE
        if a==UC_MEM_WRITE: nand.write(o,v,sz)
        else: uc.mem_write(ad,struct.pack("<I",nand.read(o,sz)&0xffffffff)[:sz])
        mmio.setdefault(ad,[]).append((pc,a))
        return
    if a==UC_MEM_WRITE: sticky[ad]=v; return
    if ad==0xc0100004:
        val=64; uc.mem_write(0xc0100000,struct.pack("<I",val))
    elif ad in sticky: val=sticky[ad]
    else: val=0
    uc.mem_write(ad,struct.pack("<I",val&0xffffffff)[:sz])
uc.hook_add(UC_HOOK_MEM_READ_UNMAPPED|UC_HOOK_MEM_WRITE_UNMAPPED|UC_HOOK_MEM_FETCH_UNMAPPED,hu)
uc.hook_add(UC_HOOK_MEM_READ|UC_HOOK_MEM_WRITE,hm)
st={"n":0,"prev":0,"tr":True,"b":0,"mmio_touched":collections.Counter()}
def hc(uc,ad,sz,u):
    st["n"]+=1
    p=st["prev"]
    if p and ad>0x10000000 and st["b"]<40:
        pass
    if p and ad!=p+4 and st["b"]<80:
        b=bytes(uc.mem_read(p,4))
        try: dis=next(md.disasm(b,p)); dd="%s %s"%(dis.mnemonic,dis.op_str)
        except: dd="?"
        print("0x%08x -> 0x%08x [%s]"%(p,ad,dd))
        st["b"]+=1
        if st["b"]>=80: uc.emu_stop()
    st["prev"]=ad
    if st["n"]>3_000_000: uc.emu_stop()
uc.hook_add(UC_HOOK_CODE,hc)
uc.reg_write(UC_ARM_REG_SP,0x00bff000)
entry=0x00a00028
try: uc.emu_start(entry,0xFFFFFFFF,count=0)
except UcError as e: print("err",e)
print("---"); print("final pc=0x%08x n=%d"%(uc.reg_read(UC_ARM_REG_PC),st["n"]))
print("DMOV execs",dmov.exec_count)
print("MMIO touched addrs:",[hex(x) for x in list(mmio.keys())[:12]])