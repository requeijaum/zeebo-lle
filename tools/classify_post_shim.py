#!/usr/bin/env python3
"""Classify what APPS does after MAP_CONTROL (loop vs slide) + hook UTCB/scheduler.
Track last-N PCs and MMIO touches in a shimmed run."""
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
sticky={}
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
    if a==UC_MEM_WRITE: sticky[ad]=v; return
    if 0xa9400200<=ad<=0xa94002ff: val=3
    elif 0xa9400040<=ad<=0xa94000ff: val=0x80000002
    elif ad in sticky: val=sticky[ad]
    else: val=0
    uc.mem_write(ad,struct.pack("<I",val&0xffffffff)[:sz])
uc.hook_add(UC_HOOK_MEM_READ_UNMAPPED|UC_HOOK_MEM_WRITE_UNMAPPED|UC_HOOK_MEM_FETCH_UNMAPPED,hu)
uc.hook_add(UC_HOOK_MEM_READ|UC_HOOK_MEM_WRITE,hm)
svc_done={"n":0}
def hi(uc,n,u):
    pc=uc.reg_read(UC_ARM_REG_PC)-4
    ins=bytes(uc.mem_read(pc,4)); imm=struct.unpack("<I",ins)[0]&0xffffff
    svc_done["n"]+=1
    # MAP_CONTROL success
    uc.reg_write(UC_ARM_REG_R0,1); uc.reg_write(UC_ARM_REG_R1,0)
    uc.reg_write(UC_ARM_REG_R2,0); uc.reg_write(UC_ARM_REG_R3,0)
uc.hook_add(UC_HOOK_INTR,hi)
trace=collections.deque(maxlen=2000)
st={"n":0,"mmio":collections.Counter()}
def hc(uc,ad,sz,u):
    st["n"]+=1
    trace.append(ad)
    if st["n"]>=1_500_000: uc.emu_stop()
uc.hook_add(UC_HOOK_CODE,hc)
uc.reg_write(UC_ARM_REG_SP,0x0fff0000)
uc.emu_start(entry,0xFFFFFFFF,count=0)
print("insns",st["n"],"svcs",svc_done["n"])
tr=list(trace)
# classify: how many distinct PCs in last 2000
distinct=len(set(tr))
print("distinct PCs (last 2000):",distinct)
# consecutive delta check: is it sliding (each +4)?
seq=sum(1 for a,b in zip(tr,tr[1:]) if b==a+4)
print("sequential +4 steps:",seq,"of",len(tr)-1)
PC_X="%08x"%uc.reg_read(UC_ARM_REG_PC)
print("final pc",PC_X)
# show last 12 trace addrs
print("last 12:",[hex(a) for a in tr[-12:]])
md=capstone.Cs(capstone.CS_ARCH_ARM,capstone.CS_MODE_ARM)
# disasm the hottest loop PC region (mode of last 2000)
cm=collections.Counter(tr).most_common(1)
if cm:
    hot=cm[0][0]&~0xF
    data=bytes(uc.mem_read(hot,0x40))
    print("\nhottest pc=0x%x bytes %s"%(hot,data.hex()))
    for ins in md.disasm(data,hot): print("  %08x  %-7s %s"%(ins.address,ins.mnemonic,ins.op_str))