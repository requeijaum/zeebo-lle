#!/usr/bin/env python3
"""
Zeebo LLE — APPS runner with REAL ARM11 VA->PA MMU translation (QEMU-free).
Implements the physical-memory model from console__zeebo__mmu.txt:
  - map physical RAM at its PAs (identity + the translate targets)
  - every code fetch translated vaddr->pa via the MMU tables
  - ELF is loaded at PHYSICAL addresses (loader layout), vaddrs become aliases

We load each PT_LOAD at its PA (per MMU map the ELF vaddr->pa), then run from
the entry translated. Any vaddr the firmware executes is translated via our table
before fetching; writes to vaddrs are redirected to their PA.

Because Unicorn executes in a flat address space, we put the ACTUAL image at the
PA and provide vaddr->pa translation in the fetch/data hooks. For identity-mapped
RAM (the bulk) vaddr==pa so it's a no-op; for the f0000000/b0x aliases and
periphery c0 block we translate.
"""
import struct, collections, sys
from unicorn import *
from unicorn.arm_const import *
sys.path.insert(0, "/home/rafaelfrequiao/projects/zeebo-lle/tools")
import arm11_mmu as arm11
from nand_controller import NandController, NAND_BASE

ND = "/home/rafaelfrequiao/projects/zeebo-lle/nand"

def parse_loads(d):
    e_entry = struct.unpack("<I", d[24:28])[0]
    e_phoff = struct.unpack("<I", d[28:32])[0]
    e_phentsize = struct.unpack("<H", d[42:44])[0]
    e_phnum = struct.unpack("<H", d[44:46])[0]
    loads = []
    for i in range(e_phnum):
        off = e_phoff + i*e_phentsize
        v = struct.unpack("<8I", d[off:off+32])
        if v[0]==1 and v[6]:
            loads.append((v[2], v[1], v[4], v[5]))
    return e_entry, loads

# Build vaddr->pa lookup (sections first, then coarse 4K pages override within)
SECTION_SIZE=0x100000; PAGE_SIZE=0x1000
class MMUTrans:
    def __init__(self):
        self.sec = {}
        for va,pa in arm11.SECTIONS: self.sec[va>>20]=(pa>>20)*SECTION_SIZE
        self.pg = {}
        for va,pa in arm11.COARSE: self.pg[va>>12]=pa  # 4K pa base
    def lookup(self, va):
        pg = va & ~0xFFF
        if pg in self.pg: return pg + (va-pg) + (self.pg[pg]-pg)
        key = va>>20
        if key in self.sec: return self.sec[key] + (va & 0xFFFFF)
        return va  # default identity

def run(stage="APPS", max_insns=8_000_000):
    d = open(f"{ND}/1.1.2_{stage}.bin","rb").read()
    entry, loads = parse_loads(d)
    mmu = MMUTrans()
    uc = Uc(UC_ARCH_ARM, UC_MODE_ARM)
    try: uc.ctl_set_cpu_model(UC_CPU_ARM_1176)
    except Exception: pass

    # Map ALL physical address space the MMU references. We map a large flat RAM
    # (0x00000000..0x04000000) as identity for physical, plus high alias targets.
    # The unit that matters is physical RAM; load ELF at PA of each segment.
    # For identity segments paddr==vaddr. For coarse (0x100ad400) load there.
    for va,off,fs,ms in loads:
        pa = mmu.lookup(va)
        base = pa & ~0xFFF
        size = ((pa + ms + 0xFFF)&~0xFFF) - base
        if size > 0x4000000: continue
        try: uc.mem_map(base, size)
        except UcError: pass
        if fs:
            try: uc.mem_write(pa, d[off:off+fs])
            except UcError:
                try: uc.mem_write(base, d[off:off+fs][:size])
                except UcError as e: pass
    # also map the vaddr alias region so fetch translation works smoothly
    for va,off,fs,ms in loads:
        base = va & ~0xFFF
        size = ((va+ms+0xFFF)&~0xFFF)-base
        if size>0x4000000: continue
        try: uc.mem_map(base, size)
        except UcError: pass

    # map generic RAM + high stack + UTCB
    for b,sz in ((0x00000000,0x02000000),(0x10000000,0x02000000),
                 (0xff000000,0x00200000),(0xffe00000,0x00200000)):
        try: uc.mem_map(b,sz)
        except UcError: pass
    uct=0xff0f0000
    try: uc.mem_map(uct,0x4000)
    except UcError: pass
    uc.mem_write(0xff000ff0,struct.pack("<I",uct)); uc.mem_write(uct+64,b"\x00"*64)

    nand = NandController(f"{ND}/1.1.2.bin", f"{ND}/1.1.2_spare.bin")
    sticky={}

    def taddr(a):  # translate vaddr->pa for data/IO
        if a < 0x02000000 or a>=0xf0000000: return mmu.lookup(a)
        return mmu.lookup(a)

    def hu(uc, a, ad, sz, v, u):
        # map the physical target page
        tgt = mmu.lookup(ad)
        page = tgt & ~0xFFF
        try: uc.mem_map(page, 0x1000)
        except UcError: pass
        return True

    def hm(uc, a, ad, sz, v, u):
        tgt = taddr(ad)
        # IO / periphery live at their PA after translation
        if NAND_BASE <= tgt < NAND_BASE+0x1000:
            o = tgt - NAND_BASE
            if a == UC_MEM_WRITE: nand.write(o, v, sz)
            else: uc.mem_write(ad, struct.pack("<I", nand.read(o,sz)&0xFFFFFFFF)[:sz])
            return
        # if tgt != ad, redirect the access physically: copy/mirror
        if a == UC_MEM_WRITE:
            if tgt != ad:
                try: uc.mem_write(tgt, bytes(uc.mem_read(ad, sz)))
                except UcError: pass
            sticky[tgt] = v
            return
        else:
            # read: if aliased, serve from PA
            val = 0
            try:
                raw = bytes(uc.mem_read(tgt, sz)) if tgt!=ad else bytes(uc.mem_read(ad,sz))
                val = struct.unpack("<I", raw.ljust(4,b"\0"))[0]
            except UcError:
                pass
            uc.mem_write(ad, struct.pack("<I", val&0xFFFFFFFF)[:sz])
            return

    uc.hook_add(UC_HOOK_MEM_READ_UNMAPPED|UC_HOOK_MEM_WRITE_UNMAPPED|UC_HOOK_MEM_FETCH_UNMAPPED, hu)
    uc.hook_add(UC_HOOK_MEM_READ|UC_HOOK_MEM_WRITE, hm)

    # instruction fetch translation: translate PC
    st={"n":0,"svc":0,"last":0}
    seen=set()
    ipc_log=[]
    def hc(uc, ad, sz, u):
        st["n"]+=1
        tgt = mmu.lookup(ad)
        if tgt != ad:
            # relocate execution: redirect PC to pa & continue
            if st.get("jump")==ad:
                uc.reg_write(UC_ARM_REG_PC, tgt); return
            st["jump"]=ad
        if st["n"]>=max_insns: uc.emu_stop()
    uc.hook_add(UC_HOOK_CODE, hc)

    def hi(uc,ni,u):
        pc=uc.reg_read(UC_ARM_REG_PC)-4
        ins=bytes(uc.mem_read(pc,4)); imm=struct.unpack("<I",ins)[0]&0xffffff
        st["svc"]+=1
        r=[uc.reg_read(getattr(__import__("unicorn.arm_const",fromlist=['x']),'UC_ARM_REG_R%d'%i)) for i in range(7)]
        seen.add((pc,(imm&0x3f)))
        n=imm&0x3f
        if n==0 and len(ipc_log)<10:
            ipc_log.append((pc, r[0], r[1], r[2]))
        # MAP_CONTROL- real "map" HO: no-op success
        if n==0x14:            # map_control: success, we pre-mapped
            uc.reg_write(UC_ARM_REG_R0,1); uc.reg_write(UC_ARM_REG_R1,0)
            uc.reg_write(UC_ARM_REG_R2,0); uc.reg_write(UC_ARM_REG_R3,0)
        elif n==0x08:          # thread_control TRUE
            uc.reg_write(UC_ARM_REG_R0,1); uc.reg_write(UC_ARM_REG_R1,0)
            uc.reg_write(UC_ARM_REG_R2,0); uc.reg_write(UC_ARM_REG_R3,0)
        else:
            uc.reg_write(UC_ARM_REG_R0,0); uc.reg_write(UC_ARM_REG_R1,0)
            uc.reg_write(UC_ARM_REG_R2,0); uc.reg_write(UC_ARM_REG_R3,0)
        if st["svc"]>=2000: uc.emu_stop()
    uc.hook_add(UC_HOOK_INTR, hi)

    # start from entry translated
    epa = mmu.lookup(entry)
    uc.reg_write(UC_ARM_REG_SP, 0x0fff0000)
    # map the entry PA page
    try:
        pg=epa&~0xFFF; uc.mem_map(pg,0x1000) 
    except UcError: pass
    err=None
    try: uc.emu_start(epa, 0xFFFFFFFF, count=0)
    except UcError as e: err=e
    print(f"== {stage} MMU-translated run ==")
    print(f"entry vaddr=0x{entry:08x} -> pa=0x{epa:08x}")
    print(f"insns={st['n']} svcs={st['svc']} distinct_svc_sites={len(seen)} "
          f"last_pc=0x{uc.reg_read(UC_ARM_REG_PC):08x} err={err}")
    if seen:
        nm={0x0:"IPC",0x4:"THREAD_SWITCH",0x8:"THREAD_CONTROL",0xc:"EXCHANGE_REGS",0x10:"SCHEDULE",0x14:"MAP_CONTROL"}
        print("svc sites:")
        for pc,n in sorted(seen):
            print(f"  0x{pc:08x} {nm.get(n,'#%x'%n)}")
    print("\nIPC calls (pc, to, from, tag):")
    for t in ipc_log:
        print("  0x%08x to=0x%x from=0x%x tag=0x%x"%t)

if __name__=="__main__":
    run(sys.argv[1] if len(sys.argv)>1 else "APPS")