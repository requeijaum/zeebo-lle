#!/usr/bin/env python3
"""
Zeebo LLE — stage loader + runner (ROADMAP Phase 3, first cut).

Bypass the boot chain we don't have: load the APPS/AMSS ELF PT_LOAD segments
directly into an emulated address space, model peripherals (VIC/GPT/DMOV/MDDI/
GPIO) + the NAND controller, jump to e_entry, and measure how far the stage runs
standalone. This tells us what the next stage touches first (its own device
init, PROC_COMM to the modem, or an immediate dependence on state the boot chain
left behind).
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
        p_type,p_offset,p_vaddr,p_paddr,p_filesz,p_memsz,p_flags,p_align = struct.unpack("<8I", d[off:off+32])
        if p_type == 1 and p_memsz:
            loads.append((p_vaddr, p_offset, p_filesz, p_memsz))
    return e_entry, loads

def run(stage="APPS", max_insns=20_000_000):
    d = open(f"{ND}/1.1.2_{stage}.bin", "rb").read()
    entry, loads = parse_loads(d)
    uc = Uc(UC_ARCH_ARM, UC_MODE_ARM)
    try: uc.ctl_set_cpu_model(UC_CPU_ARM_1176)
    except Exception: pass

    # map + load each PT_LOAD (page-align)
    mapped = []
    for vaddr, off, filesz, memsz in loads:
        base = vaddr & ~0xFFF
        end = (vaddr + memsz + 0xFFF) & ~0xFFF
        size = end - base
        # skip absurd sizes (>64MB single seg) to keep the experiment light
        if size > 0x4000000:
            continue
        try: uc.mem_map(base, size)
        except UcError: pass
        if filesz:
            try: uc.mem_write(vaddr, d[off:off+filesz])
            except UcError: pass
        mapped.append((base, size))

    # low RAM + stacks
    for b in (0x00000000, 0x10000000):
        try: uc.mem_map(b, 0x02000000)
        except UcError: pass

    nand = NandController(f"{ND}/1.1.2.bin", f"{ND}/1.1.2_spare.bin")
    sticky = {}
    gpt = {"c": 0}
    touched = collections.OrderedDict(); order = []; seen = set()
    def note(kind, ad, pc):
        k = (pc, ad, kind)
        if k not in seen: seen.add(k); order.append((len(order), pc, kind, ad))
        touched[ad] = touched.get(ad, 0)+1

    def hu(uc, a, ad, sz, v, u):
        try: uc.mem_map(ad & ~0xFFF, 0x1000)
        except UcError: pass
        return True

    def hm(uc, a, ad, sz, v, u):
        if ad < 0x20000000 and not (0xA0000000 <= ad):  # RAM
            return
        pc = uc.reg_read(UC_ARM_REG_PC)
        # NAND controller window
        if NAND_BASE <= ad < NAND_BASE + 0x1000:
            off = ad - NAND_BASE
            if a == UC_MEM_WRITE:
                nand.write(off, v, sz); note("Wnand", ad, pc)
            else:
                val = nand.read(off, sz)
                uc.mem_write(ad, struct.pack("<I", val & 0xFFFFFFFF)[:sz]); note("Rnand", ad, pc)
            return
        if a == UC_MEM_WRITE:
            sticky[ad] = v; note("W", ad, pc); return
        if ad == 0xc0100004: gpt["c"] += 64; val = gpt["c"]
        elif 0xa9400200 <= ad <= 0xa94002ff: val = 3
        elif 0xa9400040 <= ad <= 0xa94000ff: val = 0x80000002
        elif 0xa9700e00 <= ad <= 0xa9700eff and ad not in sticky: val = 1
        elif ad == 0xaa600028: val = 0xffffffff
        elif ad in sticky: val = sticky[ad]
        else: val = 0
        uc.mem_write(ad, struct.pack("<I", val & 0xFFFFFFFF)[:sz]); note("R", ad, pc)

    uc.hook_add(UC_HOOK_MEM_READ_UNMAPPED | UC_HOOK_MEM_WRITE_UNMAPPED |
                UC_HOOK_MEM_FETCH_UNMAPPED, hu)
    uc.hook_add(UC_HOOK_MEM_READ | UC_HOOK_MEM_WRITE, hm)

    st = {"n":0,"last":0,"spin":0}
    def hc(uc, ad, sz, u):
        st["n"] += 1
        if ad == st["last"]:
            st["spin"] += 1
            if st["spin"] > 3_000_000: uc.emu_stop()
        else: st["spin"] = 0
        st["last"] = ad
        if st["n"] >= max_insns: uc.emu_stop()
    uc.hook_add(UC_HOOK_CODE, hc)

    uc.reg_write(UC_ARM_REG_SP, 0x0FFF0000)
    print(f"== {stage}: entry=0x{entry:08x}, {len(mapped)} segments mapped ==")
    err = None
    try: uc.emu_start(entry, 0xFFFFFFFF, count=0)
    except UcError as e: err = e
    print(f"insns={st['n']} last_pc=0x{st['last']:08x} err={err}")
    print(f"distinct MMIO/NAND: {len(touched)}")
    print("first 40 touches:")
    for i, pc, kind, ad in order[:40]:
        print(f"  [{i:3}] pc=0x{pc:08x} {kind:6} 0x{ad:08x}")
    nand_hits = [a for a in touched if NAND_BASE <= a < NAND_BASE+0x1000]
    print(f"NAND ctrl touched: {len(nand_hits)} -> {[hex(x) for x in nand_hits[:8]]}")

if __name__ == "__main__":
    stage = sys.argv[1] if len(sys.argv) > 1 else "APPS"
    run(stage)
