#!/usr/bin/env python3
"""Trace ALL control flow from MMU-enable (0x730) until derail, logging bl targets
and their returns, so we see which function jumps to 0x60000 / reads NAND."""
import struct, capstone, collections
from unicorn import *
from unicorn.arm_const import *
import sys
sys.path.insert(0,"/home/rafaelfrequiao/projects/zeebo-lle/tools")
from nand_controller import NandController, NAND_BASE
ND="/home/rafaelfrequiao/projects/zeebo-lle/nand"
md=capstone.Cs(capstone.CS_ARCH_ARM,capstone.CS_MODE_ARM)
code=open(f"{ND}/1.1.2_APPSBL.bin","rb").read()
uc=Uc(UC_ARCH_ARM,UC_MODE_ARM)
try: uc.ctl_set_cpu_model(UC_CPU_ARM_1176)
except: pass
uc.mem_map(0,0x2000000); uc.mem_write(0,code)
nand=NandController(f"{ND}/1.1.2.bin",f"{ND}/1.1.2_spare.bin")
sticky={}; gpt={"c":0}
nand_hits=collections.Counter()
def hu(uc,a,ad,sz,v,u):
    try: uc.mem_map(ad&~0xFFF,0x1000)
    except: pass
    return True
def hm(uc,a,ad,sz,v,u):
    if ad<0x2000000: return
    if NAND_BASE<=ad<NAND_BASE+0x1000:
        o=ad-NAND_BASE; nand_hits[o]+=1
        if a==UC_MEM_WRITE: nand.write(o,v,sz)
        else: uc.mem_write(ad,struct.pack("<I",nand.read(o,sz)&0xffffffff)[:sz])
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
st={"n":0,"last":0,"tr":False}
def hc(uc,ad,sz,u):
    st["n"]+=1
    if ad==0x8ec: uc.reg_write(UC_ARM_REG_R0,1)
    if ad==0x72c: st["tr"]=True
    if st["tr"] and st["last"]:
        b=st["last"]; nxt=ad
        # log bl and far branches
        if nxt!=b+4:
            ins=bytes(uc.mem_read(b,4))
            dis="?"
            try:
                i2=next(md.disasm(ins,b)); dis="%s %s"%(i2.mnemonic,i2.op_str)
            except: pass
            print("0x%04x -> 0x%08x  [%s] lr=0x%08x"%(b,nxt,dis,uc.reg_read(UC_ARM_REG_LR)))
            if st["n"]>200_000_000: uc.emu_stop()
    st["last"]=ad
    if st["n"]>2_000_000: uc.emu_stop()
uc.hook_add(UC_HOOK_CODE,hc)
uc.reg_write(UC_ARM_REG_SP,0x100000)
err=None
try: uc.emu_start(0,len(code),count=0)
except UcError as e: err=e
print("final pc=0x%08x n=%d err=%s"%(uc.reg_read(UC_ARM_REG_PC),st["n"],err))
print("nand_hits:",{hex(k):v for k,v in list(nand_hits.items())[:20]})