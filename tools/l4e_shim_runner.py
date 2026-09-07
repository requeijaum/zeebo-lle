#!/usr/bin/env python3
"""
Zeebo LLE — minimal L4e (OKL4) syscall shim + APPS runner.

Purpose: show how far the REX-on-L4e APPS core runs when the SIX L4e syscalls
it actually uses are serviced by a small shim (no full microkernel).

Design (matches the OKL4 ARM ABI, docs/l4e-syscall-abi.md):
  - Intercept the 6 svc immediates found in 1.1.2_APPS.bin via UC_HOOK_INTR.
  - Unicorn advances PC past the svc, so the thunk's post-code (`strne r1,[r4]`
    etc) then writes our chosen out-regs through the caller output pointers.
  - We therefore set the OUT registers (r0..r6 as needed) to forward-progress
    values so REX doesn't derail into a NOP-slide. Key insight from recon: the
    syscall's RESULT semantics matter; zero-everything corrupts the task.
  - Provide a UTCB (per-thread control block): ipc.spp reads its pointer from
    0xff000ff0 and message regs at UTCB+64..+84. We allocate one.

Syscall -> minimal behavior:
  THREAD_SWITCH(0x04) : cooperative yield -> return success, no work
  THREAD_CONTROL(0x08): thread create/config -> return success (non-zero)
  EXCHANGE_REGS(0x0c) : get/set registers  -> return old values (zeros ok)
  SCHEDULE     (0x10) : set scheduling     -> return success
  MAP_CONTROL  (0x14) : permission mapping -> no-op, return success
  LIPC/IPC     (0x00) : message pass       -> return aMsgTag success + set *from
We log each distinct svc site + regs so over a run we see which REX calls
predominate and can deepen semantics where the task stalls.
"""
import struct, collections, sys
from unicorn import *
from unicorn.arm_const import *
sys.path.insert(0, "/home/rafaelfrequiao/projects/zeebo-lle/tools")
from nand_controller import NandController, NAND_BASE

ND = "/home/rafaelfrequiao/projects/zeebo-lle/nand"

# --- L4e syscall numbers (OKL4 syscalls_asm.h low bits) ---
S_THREAD_SWITCH   = 0x04
S_THREAD_CONTROL  = 0x08
S_EXCHANGE_REGS   = 0x0c
S_SCHEDULE        = 0x10
S_MAP_CONTROL     = 0x14

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

def name_for_imm(imm):
    n = imm & 0x3f
    return {S_THREAD_SWITCH:"THREAD_SWITCH", S_THREAD_CONTROL:"THREAD_CONTROL",
            S_EXCHANGE_REGS:"EXCHANGE_REGS", S_SCHEDULE:"SCHEDULE",
            S_MAP_CONTROL:"MAP_CONTROL"}.get(n, f"syscall#{n:#x}")

def run(stage="APPS", max_insns=6_000_000):
    d = open(f"{ND}/1.1.2_{stage}.bin","rb").read()
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
    for b in (0x00000000, 0x10000000, 0xff000000, 0xffe00000):
        try: uc.mem_map(b, 0x02000000 if b<0xff000000 else 0x00200000)
        except UcError: pass

    # UTCB support: alloc at 0xff0f0000, install pointer at 0xff000ff0
    UTCB = 0xff0f0000
    try: uc.mem_map(UTCB, 0x4000)
    except UcError: pass
    uc.mem_write(0xff000ff0, struct.pack("<I", UTCB))
    # Message registers (MR) area at UTCB+64..+96: zero
    uc.mem_write(UTCB+64, b"\x00"*64)

    nand = NandController(f"{ND}/1.1.2.bin", f"{ND}/1.1.2_spare.bin")
    sticky = {}
    def hu(uc, a, ad, sz, v, u):
        try: uc.mem_map(ad & ~0xFFF, 0x1000)
        except UcError: pass
        return True
    def hm(uc, a, ad, sz, v, u):
        if ad < 0x20000000 and ad < 0xA0000000 and not (NAND_BASE<=ad): return
        if NAND_BASE <= ad < NAND_BASE+0x1000:
            off = ad - NAND_BASE
            if a == UC_MEM_WRITE: nand.write(off, v, sz)
            else: uc.mem_write(ad, struct.pack("<I", nand.read(off,sz)&0xFFFFFFFF)[:sz])
            return
        if a == UC_MEM_WRITE: sticky[ad] = v; return
        if 0xa9400200<=ad<=0xa94002ff: val=3
        elif 0xa9400040<=ad<=0xa94000ff: val=0x80000002
        elif ad in sticky: val=sticky[ad]
        else: val=0
        uc.mem_write(ad, struct.pack("<I", val&0xFFFFFFFF)[:sz])
    uc.hook_add(UC_HOOK_MEM_READ_UNMAPPED | UC_HOOK_MEM_WRITE_UNMAPPED |
                UC_HOOK_MEM_FETCH_UNMAPPED, hu)
    uc.hook_add(UC_HOOK_MEM_READ | UC_HOOK_MEM_WRITE, hm)

    calls = collections.OrderedDict()
    st = {"n":0,"sc":0,"last":0,"spin":0}

    def hook_intr(uc, intno, u):
        pc = uc.reg_read(UC_ARM_REG_PC) - 4   # svc addr
        ins = bytes(uc.mem_read(pc, 4))
        imm = struct.unpack("<I", ins)[0] & 0x00FFFFFF
        n = imm & 0x3f
        regs = tuple(uc.reg_read(getattr(__import__("unicorn.arm_const", fromlist=["u"]), "UC_ARM_REG_R%d"%i)) for i in range(7))
        key = (pc, imm)
        if key not in calls: calls[key] = [0, regs]
        calls[key][0] += 1
        st["sc"] += 1
        if n == S_THREAD_SWITCH:
            uc.reg_write(UC_ARM_REG_R0, 0)     # Tiger succeeded
            uc.reg_write(UC_ARM_REG_R1, 0)
        elif n == S_THREAD_CONTROL:
            uc.reg_write(UC_ARM_REG_R0, 1)     # TRUE = success
            uc.reg_write(UC_ARM_REG_R1, 0); uc.reg_write(UC_ARM_REG_R2, 0); uc.reg_write(UC_ARM_REG_R3, 0)
        elif n == S_EXCHANGE_REGS:
            uc.reg_write(UC_ARM_REG_R0, regs[0])        # keep dest id
            uc.reg_write(UC_ARM_REG_R1, 0); uc.reg_write(UC_ARM_REG_R2, 0); uc.reg_write(UC_ARM_REG_R3, 0)
        elif n == S_SCHEDULE:
            uc.reg_write(UC_ARM_REG_R0, 0); uc.reg_write(UC_ARM_REG_R1, 0)
        elif n == S_MAP_CONTROL:
            uc.reg_write(UC_ARM_REG_R0, 1)     # TRUE success
            uc.reg_write(UC_ARM_REG_R1, 0); uc.reg_write(UC_ARM_REG_R2, 0); uc.reg_write(UC_ARM_REG_R3, 0)
        else:
            # generic IPC-ish: r0 = success tag, r1..r3 = 0
            uc.reg_write(UC_ARM_REG_R0, 0); uc.reg_write(UC_ARM_REG_R1, 0)
            uc.reg_write(UC_ARM_REG_R2, 0); uc.reg_write(UC_ARM_REG_R3, 0)
        if st["sc"] >= 5000:
            uc.emu_stop()

    uc.hook_add(UC_HOOK_INTR, hook_intr)

    def hc(uc, ad, sz, u):
        st["n"] += 1
        if ad == st["last"]:
            st["spin"] += 1
            if st["spin"] > 3_000_000:
                st["stuck"]=ad; uc.emu_stop()
        else: st["spin"] = 0
        st["last"] = ad
        if st["n"] >= max_insns: uc.emu_stop()
    uc.hook_add(UC_HOOK_CODE, hc)

    uc.reg_write(UC_ARM_REG_SP, 0x0fff0000)
    print(f"== {stage} L4e-shim run: entry=0x{entry:08x} ==")
    err=None
    try: uc.emu_start(entry, 0xFFFFFFFF, count=0)
    except UcError as e: err=e
    print(f"insns={st['n']} syscalls={st['sc']} distinct={len(calls)} "
          f"last_pc=0x{uc.reg_read(UC_ARM_REG_PC):08x} stuck=0x{st.get('stuck',0):x} err={err}")
    print("\n-- syscall call sites (pc, name, count, first-arg-regs r0-r3) --")
    from collections import Counter
    byname = Counter()
    for (pc,imm),(cnt,regs) in list(calls.items())[:40]:
        nm = name_for_imm(imm)
        byname[nm]+=cnt
        print(f"  0x{pc:08x} {nm:16s} x{cnt:<4} r0-3=({','.join(hex(x) for x in regs[:4])})")
    print("\n-- syscall totals by name --")
    for nm,c in byname.most_common():
        print(f"  {nm:18s} {c}")

if __name__ == "__main__":
    run(sys.argv[1] if len(sys.argv)>1 else "APPS")