#!/usr/bin/env python3
"""Clean NAND-access probe: count every 0xa0a00000+off touch with direction+value,
over a LONG run, short-circuiting udelay. Report if any real NAND command/read
sequence occurs (FLASH_CMD/FETCH_ID/EXEC/READ_ID/page reads)."""
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
nand_log=[]  # (pc, rw, off, val)
prev_nand=0
def hu(uc,a,ad,sz,v,u):
    try: uc.mem_map(ad&~0xFFF,0x1000)
    except: pass
    return True
def hm(uc,a,ad,sz,v,u):
    global prev_nand
    if ad<0x2000000: return
    pc=uc.reg_read(UC_ARM_REG_PC)
    if NAND_BASE<=ad<NAND_BASE+0x1000:
        o=ad-NAND_BASE
        if a==UC_MEM_WRITE:
            nand.write(o,v,sz)
            if len(nand_log)<400: nand_log.append((pc,"W",o,v))
            prev_nand+=1
        else:
            val=nand.read(o,sz)
            uc.mem_write(ad,struct.pack("<I",val&0xffffffff)[:sz])
            if len(nand_log)<400: nand_log.append((pc,"R",o,val))
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
st={"n":0,"last":0,"spin":0}
def hc(uc,ad,sz,u):
    st["n"]+=1
    if ad==0x8ec: uc.reg_write(UC_ARM_REG_R0,1)
    if ad==st["last"]:
        st["spin"]+=1
        if st["spin"]>400_000: st["stuck"]=ad; uc.emu_stop()
    else: st["spin"]=0
    st["last"]=ad
    if st["n"]>20_000_000: uc.emu_stop()
uc.hook_add(UC_HOOK_CODE,hc)
uc.reg_write(UC_ARM_REG_SP,0x100000)
err=None
try: uc.emu_start(0,len(code),count=0)
except UcError as e: err=e
print("insns",st["n"],"last_pc=0x%08x"%(st["last"]),"stuck=0x%x"%st.get("stuck",0),"err",err)
print("NAND touches:",prev_nand)
# summarize command emissions
cmds=collections.Counter()
seq=[x for x in nand_log]
# FLASH_CMD=0, ADDR0=4, ADDR1=8, CS=0xc, EXEC=0x10, STATUS=0x14, READID=0x40, BUF=0x100
cmd_last=None
for pc,rw,off,val in seq:
    if off==0x00 and rw=="W": cmds[("CMD",val&0xff)]+=1
    if off==0x10 and rw=="W": cmds[("EXEC",val)]+=1
    if off==0x40 and rw=="R": cmds[("READID",val)]+=1
    if off==0x100 and rw=="R": cmds[("BUFREAD",val)]+=1
print("NAND command emission:",dict(cmds))
print("\nfirst 60 nand log:")
for pc,rw,off,val in seq[:60]:
    print("  0x%08x %s off=%#x val=%#x"%(pc,rw,off,val))