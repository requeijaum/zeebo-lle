#!/usr/bin/env python3
"""Verify: is APPS in a LEGIT REX idle (real svc) or a slide? Capture the actual
svc instruction bytes at each INTR, the branch-in, and whether execution stays in
code. Also decode the IPC direction bit of L4_Ipc tag."""
import struct, collections, sys, capstone
from unicorn import *
from unicorn.arm_const import *
sys.path.insert(0,"/home/rafaelfrequiao/projects/zeebo-lle/tools")
import arm11_mmu as arm11
from nand_controller import NandController, NAND_BASE
ND="/home/rafaelfrequiao/projects/zeebo-lle/nand"
def parse(d):
    e=struct.unpack("<I",d[24:28])[0]; po=struct.unpack("<I",d[28:32])[0]
    ps=struct.unpack("<H",d[42:44])[0]; pn=struct.unpack("<H",d[44:46])[0]
    L=[]
    for i in range(pn):
        o=po+i*ps; v=struct.unpack("<8I",d[o:o+32])
        if v[0]==1 and v[6]: L.append((v[2],v[1],v[4],v[5]))
    return e,L
d=open(f"{ND}/1.1.2_APPS.bin","rb").read(); entry,L=parse(d)
class MMU:
    def __init__(self):
        self.sec={va>>20:(pa>>20)*0x100000 for va,pa in arm11.SECTIONS}
        self.pg={va>>12:pa for va,pa in arm11.COARSE}
    def lookup(self,va):
        if (va>>12) in self.pg: return self.pg[va>>12]+(va&0xFFF)
        if (va>>20) in self.sec: return self.sec[va>>20]+(va&0xFFFFF)
        return va
mmu=MMU()
uc=Uc(UC_ARCH_ARM,UC_MODE_ARM)
try: uc.ctl_set_cpu_model(UC_CPU_ARM_1176)
except: pass
# map physical RAM regions the coarse pages point at
phys_pages=set()
for va,pa in arm11.COARSE: phys_pages.add(pa&~0xFFF)
for va,pa in arm11.SECTIONS: phys_pages.add((pa>>20)*0x100000)
# map all physical PA pages
for pg in phys_pages:
    try: uc.mem_map(pg,0x1000 if pg<0x10000000 else 0x100000)
    except: pass
# load ELF segments at PA (mirroring coarse: also to identity vaddr)
for va,off,fs,ms in L:
    pa=mmu.lookup(va)
    base=pa&~0xFFF
    size=((pa+ms+0xFFF)&~0xFFF)-base
    if size>0x4000000: continue
    try: uc.mem_map(base,size)
    except: pass
    if fs:
        try: uc.mem_write(pa,d[off:off+fs])
        except: pass
    # also map the identity vaddr alias and copy (for coarse page b0xx)
    try: uc.mem_map(va&~0xFFF,((va+ms+0xFFF)&~0xFFF)-(va&~0xFFF))
    except: pass
    # if coarse: also write the same bytes at vaddr (identity) so any read of the
    # vaddr sees them — but REAL map aliases vaddr->pa, so vaddr identity isn't the
    # physical. We rely on translation. For safety copy to both.
    try:
        if fs: uc.mem_write(va, d[off:off+fs])
    except: pass
for b,sz in ((0,0x2000000),(0x10000000,0x2000000),(0xff000000,0x200000),(0xffe00000,0x200000),(0xf0000000,0x200000)):
    try: uc.mem_map(b,sz)
    except: pass
uct=0xff0f0000
try: uc.mem_map(uct,0x4000)
except: pass
uc.mem_write(0xff000ff0,struct.pack("<I",uct)); uc.mem_write(uct+64,b"\x00"*64)
nand=NandController(f"{ND}/1.1.2.bin",f"{ND}/1.1.2_spare.bin")
sticky={}
def hu(uc,a,ad,sz,v,u):
    try: uc.mem_map(mmu.lookup(ad)&~0xFFF,0x1000)
    except: pass
    return True
def hm(uc,a,ad,sz,v,u):
    tgt=mmu.lookup(ad)
    if NAND_BASE<=tgt<NAND_BASE+0x1000:
        o=tgt-NAND_BASE
        if a==UC_MEM_WRITE: nand.write(o,v,sz)
        else: uc.mem_write(ad,struct.pack("<I",nand.read(o,sz)&0xffffffff)[:sz])
        return
    if a==UC_MEM_WRITE:
        if tgt!=ad:
            try: uc.mem_write(tgt,bytes(uc.mem_read(ad,sz)))
            except UcError: pass
        sticky[tgt]=v
    else:
        try: val=struct.unpack("<I",bytes(uc.mem_read(tgt,sz)).ljust(4,b"\0"))[0]
        except: val=0
        uc.mem_write(ad,struct.pack("<I",val&0xffffffff)[:sz])
uc.hook_add(UC_HOOK_MEM_READ_UNMAPPED|UC_HOOK_MEM_WRITE_UNMAPPED|UC_HOOK_MEM_FETCH_UNMAPPED,hu)
uc.hook_add(UC_HOOK_MEM_READ|UC_HOOK_MEM_WRITE,hm)
md=capstone.Cs(capstone.CS_ARCH_ARM,capstone.CS_MODE_ARM)
svc_sites=collections.Counter(); svc_trace=collections.deque(maxlen=6)
def hi(uc,ni,u):
    pcs=uc.reg_read(UC_ARM_REG_PC)
    pc=pcs-4
    try: ins=bytes(uc.mem_read(pc,4)); imm=struct.unpack("<I",ins)[0]&0xffffff
    except: imm=-1; ins=b""
    # real opcode check
    op=struct.unpack("<I",ins)[0] if isinstance(ins,bytes) and len(ins)==4 else None
    svc_sites[(pc,imm)]+=1
    r=[uc.reg_read(getattr(__import__("unicorn.arm_const",fromlist=['x']),'UC_ARM_REG_R%d'%i)) for i in range(4)]
    if len(svc_trace)<6: svc_trace.append((pc,hex(op) if op else None, r))
    n=imm&0x3f
    if n==0x14: uc.reg_write(UC_ARM_REG_R0,1); uc.reg_write(UC_ARM_REG_R1,0)
    elif n==0x08: uc.reg_write(UC_ARM_REG_R0,1)
    else: uc.reg_write(UC_ARM_REG_R0,0); uc.reg_write(UC_ARM_REG_R1,0)
    uc.reg_write(UC_ARM_REG_R2,0); uc.reg_write(UC_ARM_REG_R3,0)
    if sum(svc_sites.values())>1500: uc.emu_stop()
uc.hook_add(UC_HOOK_INTR,hi)
st={"n":0}
def hc(uc,ad,sz,u):
    st["n"]+=1
    t=mmu.lookup(ad)
    if t!=ad:
        if st.get("j")==ad: uc.reg_write(UC_ARM_REG_PC,t); return
        st["j"]=ad
    if st["n"]>4_000_000: uc.emu_stop()
uc.hook_add(UC_HOOK_CODE,hc)
try: uc.mem_map(mmu.lookup(entry)&~0xFFF,0x1000)
except: pass
uc.reg_write(UC_ARM_REG_SP,0x0fff0000)
try: uc.emu_start(mmu.lookup(entry),0xFFFFFFFF,count=0)
except UcError as e: print("err",e)
print("insns",st["n"],"svc sites d:",len(svc_sites))
print("top svc sites:")
for (pc,imm),c in svc_sites.most_common(6):
    print("  0x%08x imm=%#x opcode-check x%d"%(pc,imm,c))
print("svc trace (pc, opcode, r0-3):")
for pc,op,r in svc_trace:
    print("  0x%08x op=%s r=%s"%(pc,op,[hex(x) for x in r]))