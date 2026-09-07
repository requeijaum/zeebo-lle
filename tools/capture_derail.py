#!/usr/bin/env python3
"""Probe: capture the EXACT derail point after MAP_CONTROL — last real-code PC,
the control-transfer instruction, its target, and register context.
Identifies what the kernel interface/MAP_CONTROL should have provided."""
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
# which file vaddrs hold real code (nonzero memsz words)? build a set of "code ok" pages
uc=Uc(UC_ARCH_ARM,UC_MODE_ARM)
try: uc.ctl_set_cpu_model(UC_CPU_ARM_1176)
except: pass
codepages=set()
for va,off,fs,ms in L:
    b=va&~0xFFF; s=((va+ms+0xFFF)&~0xFFF)-b
    if s>0x4000000: continue
    try: uc.mem_map(b,s)
    except: pass
    if fs:
        try: uc.mem_write(va,d[off:off+fs])
        except: pass
    # mark pages that have any nonzero file data as "real"
    for pg in range(b, va+ms, 0x1000):
        fb=off+(pg-b)
        if fb<len(d) and any(x!=0 for x in d[fb:fb+0x1000]):
            codepages.add(pg)
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
def hi(uc,n,u):
    pc=uc.reg_read(UC_ARM_REG_PC)-4
    ins=bytes(uc.mem_read(pc,4)); imm=struct.unpack("<I",ins)[0]&0xffffff
    uc.reg_write(UC_ARM_REG_R0,1); uc.reg_write(UC_ARM_REG_R1,0)
    uc.reg_write(UC_ARM_REG_R2,0); uc.reg_write(UC_ARM_REG_R3,0)
uc.hook_add(UC_HOOK_INTR,hi)
md=capstone.Cs(capstone.CS_ARCH_ARM,capstone.CS_MODE_ARM)
st={"prev":0,"srvc":False,"captured":False}
def hc(uc,ad,sz,u):
    st["n"]=st.get("n",0)+1
    st["lastpc"]=ad
    if st["prev"] and ad==st["prev"]:
        st["spin"]=st.get("spin",0)+1
        if st["spin"]>300_000: uc.emu_stop()
    else: st["spin"]=0
    if st["n"]>2_000_000: uc.emu_stop()
    p=st["prev"]
    # detect jump INTO a page that is NOT a real code page (zeroed low RAM)
    if p and ad!=p+4:
        pg=ad&~0xFFF
        if pg not in codepages and ad>0x400000:  # jump into unmapped/empty low RAM
            if not st["captured"]:
                st["captured"]=True
                b=bytes(uc.mem_read(p,4))
                try: dis=next(md.disasm(b,p)); dd="%s %s"%(dis.mnemonic,dis.op_str)
                except: dd="?"
                print("DERAIL: transfer from 0x%08x -> 0x%08x  [%s]"%(p,ad,dd))
                print("  lr=0x%08x sp=0x%08x"%(uc.reg_read(UC_ARM_REG_LR),uc.reg_read(UC_ARM_REG_SP)))
                for i in range(6):
                                    reg=getattr(__import__("unicorn.arm_const",fromlist=['u']),'UC_ARM_REG_R%d'%i)
                                    print("  r%d=0x%08x"%(i,uc.reg_read(reg)))
                print("  ra=r0-bit?" )
                uc.emu_stop()
    st["prev"]=ad
uc.hook_add(UC_HOOK_CODE,hc)
uc.reg_write(UC_ARM_REG_SP,0x0fff0000)
uc.emu_start(entry,0xFFFFFFFF,count=0)
if not st["captured"]:
    print("no derail caught; final pc=0x%08x n=%d spin=%d"%(st.get("lastpc",0),st.get("n",0),st.get("spin",0)))