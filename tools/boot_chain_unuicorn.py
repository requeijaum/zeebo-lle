#!/usr/bin/env python3
"""
Boot APPSBL through its FULL boot with NAND controller wired. Goal: reach the
point where APPSBL reads the NAND to load the next stage, and service it via our
NAND model. Address the handoff honestly: watch for NAND ctrl (0xa0a00000)
accesses, MMU enable, and where it next jumps.
"""
import struct, collections, sys
from unicorn import *
from unicorn.arm_const import *
import capstone
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
touched=collections.OrderedDict(); order=[]; seen=set()
def note(k,ad,pc,val=None):
    kk=(pc,ad,k)
    if kk not in seen: seen.add(kk); order.append((len(order),pc,k,ad,val))
    touched[ad]=touched.get(ad,0)+1
def hu(uc,a,ad,sz,v,u):
    try: uc.mem_map(ad&~0xFFF,0x1000)
    except: pass
    return True
def hm(uc,a,ad,sz,v,u):
    if ad<0x2000000: return
    pc=uc.reg_read(UC_ARM_REG_PC)
    if NAND_BASE<=ad<NAND_BASE+0x1000:
        o=ad-NAND_BASE
        if a==UC_MEM_WRITE: nand.write(o,v,sz); note("Wn",ad,pc,v)
        else:
            val=nand.read(o,sz)
            uc.mem_write(ad,struct.pack("<I",val&0xffffffff)[:sz]); note("Rn",ad,pc,val)
        return
    if a==UC_MEM_WRITE: sticky[ad]=v; note("W",ad,pc,v); return
    if ad==0xc0100004: gpt["c"]+=64; val=gpt["c"]
    elif 0xa9400200<=ad<=0xa94002ff: val=3
    elif 0xa9400040<=ad<=0xa94000ff: val=0x80000002
    elif 0xa9700e00<=ad<=0xa9700eff and ad not in sticky: val=1
    elif ad==0xaa600028: val=0xffffffff
    elif ad in sticky: val=sticky[ad]
    else: val=0
    uc.mem_write(ad,struct.pack("<I",val&0xffffffff)[:sz]); note("R",ad,pc,val)
uc.hook_add(UC_HOOK_MEM_READ_UNMAPPED|UC_HOOK_MEM_WRITE_UNMAPPED|UC_HOOK_MEM_FETCH_UNMAPPED,hu)
uc.hook_add(UC_HOOK_MEM_READ|UC_HOOK_MEM_WRITE,hm)
st={"n":0,"last":0,"spin":0}
def hc(uc,ad,sz,u):
    st["n"]+=1
    if ad==0x8ec: uc.reg_write(UC_ARM_REG_R0,1)   # short-circuit udelay
    if ad==st["last"]:
        st["spin"]+=1
        if st["spin"]>500_000: st["stuck"]=ad; uc.emu_stop()
    else: st["spin"]=0
    st["last"]=ad
    if st["n"]>25_000_000: uc.emu_stop()
uc.hook_add(UC_HOOK_CODE,hc)
uct=0xff0f0000
try: uc.mem_map(uct,0x4000)
except: pass
try: uc.mem_write(0xff000ff0,struct.pack("<I",uct))
except: pass
uc.reg_write(UC_ARM_REG_SP,0x100000)
err=None
try: uc.emu_start(0,len(code),count=0)
except UcError as e: err=e
print(f"# APPSBL full boot (NAND wired)")
print(f"insns={st['n']} last_pc=0x{st['last']:08x} stuck=0x{st.get('stuck',0):x} err={err}")
nand_hits=[a for a in touched if NAND_BASE<=a<NAND_BASE+0x1000]
print(f"NAND ctrl touches: {len(nand_hits)}")
print("\n-- NAND register accesses (chronological, last 40) --")
nord=[x for x in order if x[1]>=NAND_BASE and x[1]<NAND_BASE+0x1000]
for i,pc,k,ad,val in nord[-40:]:
    ex=f" val=0x{val:x}" if val is not None else ""
    print(f"  {k:2} pc=0x{pc:08x} 0x{ad-NAND_BASE:03x}{ex}")
print("\n-- last 30 general touches --")
for i,pc,k,ad,val in order[-30:]:
    ex=f" val=0x{val:x}" if val is not None else ""
    print(f"  [{i:3}] pc=0x{pc:08x} {k:2} 0x{ad:08x}{ex}")
print("\nregions:",dict(collections.Counter(a&0xFFF00000 for a in touched)))