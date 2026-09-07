#!/usr/bin/env python3
"""PROBE: what does APPS expect the kernel to have set up after MAP_CONTROL?
Captures: the MAP_CONTROL syscall args (registers + UTCB MRs), and every load
from magic/high addresses (UTCB 0xff0f..., KIP region, high stack). Logs the
first ~30 interesting reads with PC, to see what the task hunts for."""
import struct, collections, capstone
from unicorn import *
from unicorn.arm_const import *
import sys
sys.path.insert(0,"tools")
from nand_controller import NandController,NAND_BASE
ND="nand"; d=open(f"{ND}/1.1.2_APPS.bin","rb").read()
def parse(d):
    e=struct.unpack("<I",d[24:28])[0]; po=struct.unpack("<I",d[28:32])[0]
    ps=struct.unpack("<H",d[42:44])[0]; pn=struct.unpack("<H",d[44:46])[0]
    L=[]
    for i in range(pn):
        o=po+i*ps; v=struct.unpack("<8I",d[o:o+32])
        if v[0]==1 and v[6]: L.append((v[2],v[1],v[4],v[5]))
    return e,L
entry,L=parse(d)
uc=Uc(UC_ARCH_ARM,UC_MODE_ARM)
try: uc.ctl_set_cpu_model(UC_CPU_ARM_1176)
except: pass
for va,off,fs,ms in L:
    b=va&~0xFFF; s=((va+ms+0xFFF)&~0xFFF)-b
    if s>0x4000000: continue
    try: uc.mem_map(b,s)
    except: pass
    if fs:
        try: uc.mem_write(va,d[off:off+fs])
        except: pass
for b in (0,0x10000000,0xff000000,0xffe00000):
    try: uc.mem_map(b,0x02000000 if b<0xff000000 else 0x00200000)
    except: pass
uct=0xff0f0000
try: uc.mem_map(uct,0x4000)
except: pass
uc.mem_write(0xff000ff0,struct.pack("<I",uct)); uc.mem_write(uct+64,b"\x00"*64)
nand=NandController(f"{ND}/1.1.2.bin",f"{ND}/1.1.2_spare.bin")
sticky={}; reads=[]
def hu(uc,a,ad,sz,v,u):
    try: uc.mem_map(ad&~0xFFF,0x1000)
    except: pass
    return True
def hm(uc,a,ad,sz,v,u):
    if ad<0x20000000 and ad<0xA0000000 and not(NAND_BASE<=ad): return
    if NAND_BASE<=ad<NAND_BASE+0x1000:
        o=ad-NAND_BASE
        if a==UC_MEM_WRITE: nand.write(o,v,sz)
        else: uc.mem_write(ad,struct.pack("<I",nand.read(o,sz)&0xffffffff)[:sz])
        return
    pc=uc.reg_read(UC_ARM_REG_PC)
    if a==UC_MEM_READ and ad>=0xf0000000 and len(reads)<40:
        reads.append((pc,ad))
    if a==UC_MEM_WRITE: sticky[ad]=v; return
    if 0xa9400200<=ad<=0xa94002ff: val=3
    elif 0xa9400040<=ad<=0xa94000ff: val=0x80000002
    elif ad in sticky: val=sticky[ad]
    else: val=0
    uc.mem_write(ad,struct.pack("<I",val&0xffffffff)[:sz])
uc.hook_add(UC_HOOK_MEM_READ_UNMAPPED|UC_HOOK_MEM_WRITE_UNMAPPED|UC_HOOK_MEM_FETCH_UNMAPPED,hu)
uc.hook_add(UC_HOOK_MEM_READ|UC_HOOK_MEM_WRITE,hm)
mapcalls=[]
def hi(uc,ni,u):
    pc=uc.reg_read(UC_ARM_REG_PC)-4
    ins=bytes(uc.mem_read(pc,4)); imm=struct.unpack("<I",ins)[0]&0xffffff
    r=[uc.reg_read(getattr(__import__("unicorn.arm_const",fromlist=['x']),'UC_ARM_REG_R%d'%i)) for i in range(7)]
    if (imm&0x3f)==0x14:  # MAP_CONTROL
        mapcalls.append((pc,r))
    # generic success
    uc.reg_write(UC_ARM_REG_R0,1); uc.reg_write(UC_ARM_REG_R1,0)
    uc.reg_write(UC_ARM_REG_R2,0); uc.reg_write(UC_ARM_REG_R3,0)
    if len(mapcalls)>=3: uc.emu_stop()
uc.hook_add(UC_HOOK_INTR,hi)
st={"n":0}
def hc(uc,ad,sz,u):
    st["n"]+=1
    if st["n"]>1_500_000: uc.emu_stop()
uc.hook_add(UC_HOOK_CODE,hc)
uc.reg_write(UC_ARM_REG_SP,0x0fff0000)
uc.emu_start(entry,0xFFFFFFFF,count=0)
print("== MAP_CONTROL calls ==")
for pc,r in mapcalls:
    print("  pc=0x%08x r0-6=(%s)"%(pc,",".join("0x%x"%x for x in r)))
print("\n== first 40 high-region (>=0xf0000000) reads (pc,addr) ==")
for pc,ad in reads[:40]:
    print("  0x%08x -> 0x%08x"%(pc,ad))
print("\nsvcs returned success; final pc=0x%08x n=%d"%(uc.reg_read(UC_ARM_REG_PC),st["n"]))