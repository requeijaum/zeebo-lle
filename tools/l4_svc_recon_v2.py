#!/usr/bin/env python3
"""
Zeebo LLE — L4 microkernel SVC ABI mapper (ROADMAP Phase 4, recon step).

APPS/AMSS are L4 (Pistachio/Iguana-family) tasks. Their syscall stubs use the
pattern `mov ip,sp; mvn sp,#N; svc #imm` — SP is loaded with a magic value
(~N = 0xffffffXX) that selects the L4 syscall, then SVC traps. On real hardware
the kernel's SWI vector reads that and dispatches.

Here we install an SVC interrupt hook that:
  - captures the syscall selector (sp magic), the svc immediate, and r0-r7,
  - logs each distinct syscall site,
  - returns a benign result (r0=0) and skips past the svc so the stub's
    post-code runs, letting the task advance to the NEXT syscall.
This does not emulate L4 semantics — it MAPS the syscall surface (which numbers,
how often, from where) so we can decide what a minimal L4 shim must implement.
"""
import struct, collections, sys
from unicorn import *
from unicorn.arm_const import *
import capstone
sys.path.insert(0, "/home/rafaelfrequiao/projects/zeebo-lle/tools")
from nand_controller import NandController, NAND_BASE

ND = "/home/rafaelfrequiao/projects/zeebo-lle/nand"
md = capstone.Cs(capstone.CS_ARCH_ARM, capstone.CS_MODE_ARM)

def parse_loads(d):
    e_entry = struct.unpack("<I", d[24:28])[0]
    e_phoff = struct.unpack("<I", d[28:32])[0]
    e_phentsize = struct.unpack("<H", d[42:44])[0]
    e_phnum = struct.unpack("<H", d[44:46])[0]
    loads = []
    for i in range(e_phnum):
        off = e_phoff + i*e_phentsize
        vals = struct.unpack("<8I", d[off:off+32])
        p_type,p_offset,p_vaddr,p_paddr,p_filesz,p_memsz,p_flags,p_align = vals
        if p_type == 1 and p_memsz:
            loads.append((p_vaddr, p_offset, p_filesz, p_memsz))
    return e_entry, loads

def run(stage="APPS", max_insns=15_000_000, max_syscalls=2000):
    d = open(f"{ND}/1.1.2_{stage}.bin", "rb").read()
    entry, loads = parse_loads(d)
    uc = Uc(UC_ARCH_ARM, UC_MODE_ARM)
    try: uc.ctl_set_cpu_model(UC_CPU_ARM_1176)
    except Exception: pass

    for vaddr, off, filesz, memsz in loads:
        base = vaddr & ~0xFFF
        size = ((vaddr + memsz + 0xFFF) & ~0xFFF) - base
        if size > 0x4000000: continue
        try: uc.mem_map(base, size)
        except UcError: pass
        if filesz:
            try: uc.mem_write(vaddr, d[off:off+filesz])
            except UcError: pass
    for b in (0x00000000, 0x10000000, 0xf0000000, 0xffe00000):
        try: uc.mem_map(b, 0x02000000 if b<0xf0000000 else 0x00200000)
        except UcError: pass

    nand = NandController(f"{ND}/1.1.2.bin", f"{ND}/1.1.2_spare.bin")
    sticky = {}; gpt = {"c": 0}

    def hu(uc, a, ad, sz, v, u):
        try: uc.mem_map(ad & ~0xFFF, 0x1000)
        except UcError: pass
        return True
    def hm(uc, a, ad, sz, v, u):
        if ad < 0x20000000 and ad < 0xA0000000 and not (NAND_BASE <= ad): return
        if NAND_BASE <= ad < NAND_BASE + 0x1000:
            off = ad - NAND_BASE
            if a == UC_MEM_WRITE: nand.write(off, v, sz)
            else: uc.mem_write(ad, struct.pack("<I", nand.read(off, sz) & 0xFFFFFFFF)[:sz])
            return
        if a == UC_MEM_WRITE: sticky[ad] = v; return
        if ad == 0xc0100004: gpt["c"] += 64; val = gpt["c"]
        elif 0xa9400200 <= ad <= 0xa94002ff: val = 3
        elif 0xa9400040 <= ad <= 0xa94000ff: val = 0x80000002
        elif ad in sticky: val = sticky[ad]
        else: val = 0
        uc.mem_write(ad, struct.pack("<I", val & 0xFFFFFFFF)[:sz])
    uc.hook_add(UC_HOOK_MEM_READ_UNMAPPED | UC_HOOK_MEM_WRITE_UNMAPPED |
                UC_HOOK_MEM_FETCH_UNMAPPED, hu)
    uc.hook_add(UC_HOOK_MEM_READ | UC_HOOK_MEM_WRITE, hm)

    syscalls = collections.OrderedDict()   # (site_pc, sel, imm) -> count
    st = {"n":0, "sc":0}

    def hook_intr(uc, intno, u):
        pc = uc.reg_read(UC_ARM_REG_PC) - 4   # Unicorn advances PC past the svc
        # decode the svc immediate from the instruction
        ins_bytes = bytes(uc.mem_read(pc, 4))
        imm = struct.unpack("<I", ins_bytes)[0] & 0x00FFFFFF
        sel = uc.reg_read(UC_ARM_REG_SP) & 0xFFFFFFFF   # L4 selector magic
        r = [uc.reg_read(getattr(__import__("unicorn.arm_const", fromlist=["x"]),
                                 f"UC_ARM_REG_R{i}")) for i in range(8)]
        key = (pc, sel, imm)
        if key not in syscalls:
            syscalls[key] = [0, tuple(r)]
        syscalls[key][0] += 1
        st["sc"] += 1
        # benign return: r0 = 0 (success). advance past the svc.
        uc.reg_write(UC_ARM_REG_R0, 0)
        uc.reg_write(UC_ARM_REG_R1, 0)
        uc.reg_write(UC_ARM_REG_R2, 0)
        uc.reg_write(UC_ARM_REG_R3, 0)
        # PC already past svc; do not rewind
        if st["sc"] >= max_syscalls:
            uc.emu_stop()
    uc.hook_add(UC_HOOK_INTR, hook_intr)

    st["last"]=0; st["spin"]=0
    def hc(uc, ad, sz, u):
        st["n"] += 1
        if ad==st["last"]:
            st["spin"]+=1
            if st["spin"]>5_000_000: 
                st["stuck"]=ad; uc.emu_stop()
        else: st["spin"]=0
        st["last"]=ad
        if st["n"] >= max_insns: uc.emu_stop()
    uc.hook_add(UC_HOOK_CODE, hc)

    uc.reg_write(UC_ARM_REG_SP, 0x0FFF0000)
    print(f"== {stage}: entry=0x{entry:08x} — L4 SVC ABI recon ==")
    err = None
    try: uc.emu_start(entry, 0xFFFFFFFF, count=0)
    except UcError as e: err = e
    print(f"insns={st['n']} syscalls_trapped={st['sc']} distinct_sites={len(syscalls)} err={err}")
    print(f"last_pc=0x{uc.reg_read(UC_ARM_REG_PC):08x}\n")
    # selector histogram
    selhist = collections.Counter()
    immhist = collections.Counter()
    for (pc, sel, imm), (cnt, regs) in syscalls.items():
        selhist[sel] += cnt; immhist[imm] += cnt
    print("== SVC immediate histogram ==")
    for imm, c in immhist.most_common(10):
        print(f"  svc #0x{imm:x} : {c}")
    print("== SP-selector (L4 magic ~N) histogram ==")
    for sel, c in selhist.most_common(10):
        print(f"  sp=0x{sel:08x} (~0x{(~sel)&0xff:x}) : {c}")
    print("\n== first 24 distinct syscall sites ==")
    for i, ((pc, sel, imm), (cnt, regs)) in enumerate(list(syscalls.items())[:24]):
        print(f"  [{i:2}] pc=0x{pc:08x} svc#0x{imm:x} sp=0x{sel:08x} r0-3={[hex(x) for x in regs[:4]]} x{cnt}")

if __name__ == "__main__":
    stage = sys.argv[1] if len(sys.argv) > 1 else "APPS"
    run(stage)
