#!/usr/bin/env python3
"""INCREMENT (option a): force APPSBL onto the real NAND MMIO path. The flash
read 0xf088 takes fast-path 0xf25c (RAM geometry lookup) when r0<8 and never
emits NAND MMIO. Forcing r0>=8 should send it to the 0xf63c MMIO writer (which
does str NAND_ADDR1/EXEC_CMD at 0xa0a00000). Measure: does any 0xa0a00000 access
appear, and does APPSBL then load a stage?"""
import struct, collections, sys
from unicorn import *
from unicorn.arm_const import *
sys.path.insert(0,"/home/rafaelfrequiao/projects/zeebo-lle/tools")
from nand_controller import NandController, NAND_BASE
ND="/home/rafaelfrequiao/projects/zeebo-lle/nand"
code=open(f"{ND}/1.1.2_APPSBL.bin","rb").read()
uc=Uc(UC_ARCH_ARM,UC_MODE_ARM)
try: uc.ctl_set_cpu_model(UC_CPU_ARM_1176)
except: pass
uc.mem_map(0,0x2000000); uc.mem_write(0,code)
nand=NandController(f"{ND}/1.1.2.bin",f"{ND}/1.1.2_spare.bin")
sticky={}; gpt={"c":0}
nand_log=[]
def hu(uc,a,ad,sz,v,u):
    try: uc.mem_map(ad&~0xFFF,0x1000)
    except: pass
    return True
def hm(uc,a,ad,sz,v,u):
    if ad<0x2000000: return
    pc=uc.reg_read(UC_ARM_REG_PC)
    if NAND_BASE<=ad<NAND_BASE+0x1000:
        o=ad-NAND_BASE
        if a==UC_MEM_WRITE:
            nand.write(o,v,sz)
            nand_log.append((pc,"W",o,v))
        else:
            val=nand.read(o,sz)
            uc.mem_write(ad,struct.pack("<I",val&0xffffffff)[:sz])
            nand_log.append((pc,"R",o,val))
        return
    if a==UC_MEM_WRITE: sticky[ad]=v; return
    if ad==0xc0100004: gpt["c"]+=64; val=gpt["c"]
    elif 0xa9400200<=ad<=0xa94002ff: val=3
    elif 0xa9400040<=ad<=0xa94000ff: val=0x80000002
    elif 0xa9700e00<=ad<=0xa9700eff and ad not in sticky: val=1
    elif ad==0xaa600028: val=0xffffffff
    elif ad in sticky: val=sticky[ad]
    else: val=0
    uc.mem_write(ad,struct.pack("<I",val&0xffffffff)[:sz])
uc.hook_add(UC_HOOK_MEM_READ_UNMAPPED|UC_HOOK_MEM_WRITE_UNMAPPED|UC_HOOK_MEM_FETCH_UNMAPPED,hu)
uc.hook_add(UC_HOOK_MEM_READ|UC_HOOK_MEM_WRITE,hm)
forced=[0]
st={"n":0,"last":0,"spin":0}
def hcc(uc,ad,sz,u):
    st["n"]+=1
    if ad==0x8ec: uc.reg_write(UC_ARM_REG_R0,1)
    if ad==0xf088:
        if uc.reg_read(UC_ARM_REG_R0)<8: uc.reg_write(UC_ARM_REG_R0,8)
    if ad==st["last"]:
        st["spin"]+=1
        if st["spin"]>500_000: st["stuck"]=ad; uc.emu_stop()
    else: st["spin"]=0
    st["last"]=ad
    if st["n"]>25_000_000: uc.emu_stop()
uc.hook_add(UC_HOOK_CODE,hcc)
uc.reg_write(UC_ARM_REG_SP,0x100000)
err=None
try: uc.emu_start(0,len(code),count=0)
except UcError as e: err=e
print("insns",st["n"],"last_pc=0x%08x"%(st["last"]),"stuck=0x%x"%st.get("stuck",0),"err",err)
print("NAND MMIO touches:",len(nand_log))
for pc,rw,o,v in nand_log[:40]:
    print("  0x%08x %s off=%#x val=%#x"%(pc,rw,o,v))