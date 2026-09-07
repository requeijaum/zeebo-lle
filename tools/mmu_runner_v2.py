#!/usr/bin/env python3
"""mmu_runner v2: proper coarse-page mirroring. The ARM11 coarse pages alias
vaddr blocks to physical RAM pages high up. A vaddr write must reach its PA and
a PA change must be visible at the vaddr. We don't have a real MMU, so we KEEP a
single physical buffer at the PA and translate every access (fetch+data) to PA.
The derail-at-b000fffc is because that address is beyond the ELF segment but the
relocated runtime image (what the loader builds) belongs there — so we let all
coarse-aliased and identity RAM be a big writable pool: writes to any identity
low RAM and to aliases both land on the same PA via translation.

KEY FIX vs v1: build a full VA-space view backed by CC(OHW)M — Actually simplest
correct model: physical RAM is the source of truth. Map the RAM so that EVERY
PA the MMU references is mapped. Then translate all accesses to PA and write the
ELF at PA. This makes aliases see writes automatically.

But the derail to b000fffc is code that the LOADER must have placed (relocated
image), which we never populated. So pre-populate: copy the ENTIRE APPS code
segments into the b0xxx identity region AND their coarse PA so any vaddr lookup
hits real code. We'll mirror segment bytes into the b0000000 1MB window fully.
"""
import struct, collections, sys
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
        k=va>>12
        if k in self.pg: return self.pg[k]+(va&0xFFF)
        if (va>>20) in self.sec: return self.sec[va>>20]+(va&0xFFFFF)
        return va
mmu=MMU()
uc=Uc(UC_ARCH_ARM,UC_MODE_ARM)
try: uc.ctl_set_cpu_model(UC_CPU_ARM_1176)
except: pass

# Map PHYSICAL RAM generously: 0..0x4000000 and 0x10000000..0x18000000
for b,sz in ((0x00000000,0x04000000),(0x10000000,0x08000000),
             (0xff000000,0x00400000),(0xffe00000,0x00400000)):
    try: uc.mem_map(b,sz)
    except UcError: pass
# Map the VA windows used by ELF segments too (identity) so fetch at vaddr works
for va,off,fs,ms in L:
    base=va&~0xFFF; size=((va+ms+0xFFF)&~0xFFF)-base
    if size>0x4000000: continue
    try: uc.mem_map(base,size)
    except UcError: pass

# LOAD ELF at PHYSICAL PA (source of truth)
for va,off,fs,ms in L:
    pa=mmu.lookup(va)
    try:
        if fs: uc.mem_write(pa,d[off:off+fs])
    except UcError:
        pass
# ALSO mirror each segment into its identity vaddr and fill the whole b0000000
# 1MB window with the b0000000 seg so b000fffc isn't empty (loader-relocated image)
for va,off,fs,ms in L:
    try:
        if fs: uc.mem_write(va,d[off:off+fs])
    except UcError: pass
# fill b0000000 window with its segment full-size (memsz) not filesz
for va,off,fs,ms in L:
    if (va>>20)==0xb00:
        try: uc.mem_write(0xb0000000,d[off:off+ms][:0x100000])
        except: pass
# UTCB
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
svc_sites=collections.Counter(); svc_trace=collections.deque(maxlen=5)
def hi(uc,ni,u):
    pcs=uc.reg_read(UC_ARM_REG_PC); pc=pcs-4
    try: ins=bytes(uc.mem_read(pc,4)); imm=struct.unpack("<I",ins)[0]&0xffffff
    except: imm=-1; ins=b""
    op=struct.unpack("<I",ins)[0] if len(ins)==4 else None
    svc_sites[(pc,imm,op)]+=1
    r=[uc.reg_read(getattr(__import__("unicorn.arm_const",fromlist=['x']),'UC_ARM_REG_R%d'%i)) for i in range(4)]
    if len(svc_trace)<5: svc_trace.append((pc,imm,op,[hex(x) for x in r]))
    n=imm&0x3f
    if n==0x14 or n==0x08: uc.reg_write(UC_ARM_REG_R0,1)
    else: uc.reg_write(UC_ARM_REG_R0,0)
    uc.reg_write(UC_ARM_REG_R1,0); uc.reg_write(UC_ARM_REG_R2,0); uc.reg_write(UC_ARM_REG_R3,0)
    if sum(svc_sites.values())>3000: uc.emu_stop()
uc.hook_add(UC_HOOK_INTR,hi)
st={"n":0,"prev":0}
def hc(uc,ad,sz,u):
    st["n"]+=1
    t=mmu.lookup(ad)
    if t!=ad:
        if st.get("j")==ad: uc.reg_write(UC_ARM_REG_PC,t); return
        st["j"]=ad
    if st["n"]>6_000_000: uc.emu_stop()
uc.hook_add(UC_HOOK_CODE,hc)
try: uc.mem_map(mmu.lookup(entry)&~0xFFF,0x1000)
except: pass
uc.reg_write(UC_ARM_REG_SP,0x0fff0000)
err=None
try: uc.emu_start(mmu.lookup(entry),0xFFFFFFFF,count=0)
except UcError as e: err=e
print("insns",st["n"],"distinct_svc",len(svc_sites),"err",err)
print("final pc 0x%08x"%(uc.reg_read(UC_ARM_REG_PC)))
print("top svc:")
for (pc,imm,op),c in svc_sites.most_common(8):
    print("  0x%08x imm=%#x op=%s x%d"%(pc,imm,hex(op) if op else None,c))
print("trace:")
for pc,imm,op,r in svc_trace:
    print("  0x%08x imm=%#x op=%s r=%s"%(pc,imm,hex(op) if op else None,r))