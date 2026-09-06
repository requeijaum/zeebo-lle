#!/usr/bin/env python3
"""
Zeebo LLE — peripheral-access probe.

Boots the APPSBL (ARM11 bootloader) under a Unicorn ARMv6 core with ONLY RAM
mapped. Every access outside RAM (i.e. an MMIO/peripheral register) is caught,
logged with the guest PC, and (for reads) satisfied with 0 so execution can
continue as far as possible. The output is the "map of the hole": the exact set
of Qualcomm MSM7201A registers the bootloader touches and in what order — the
concrete RE backlog for a real QEMU board.

Read-only: operates on the project COPY of the NAND, never the original.
"""
import struct, sys, collections
from unicorn import *
from unicorn.arm_const import *
import capstone

NAND = "/home/rafaelfrequiao/projects/zeebo-lle/nand"
APPSBL = f"{NAND}/1.1.2_APPSBL.bin"

# Memory layout (from skill KB + ELF phdrs): zloader runs at 0x00a00000,
# stack 0x00bff000, heap 0x00c00000. We give a generous low-RAM window.
RAM_BASE  = 0x00000000
RAM_SIZE  = 0x02000000          # 32 MB covers code@0xa00000 + stack + heap
LOAD_ADDR = 0x00000000          # APPSBL image starts with the vector table
MAX_INSNS = 2_000_000

md = capstone.Cs(capstone.CS_ARCH_ARM, capstone.CS_MODE_ARM)

def load():
    return open(APPSBL, "rb").read()

def main():
    code = load()
    uc = Uc(UC_ARCH_ARM, UC_MODE_ARM)
    # ARM1136 = ARMv6; select an ARM11 cpu model if available
    try:
        uc.ctl_set_cpu_model(UC_CPU_ARM_1176)
    except Exception:
        pass

    uc.mem_map(RAM_BASE, RAM_SIZE)
    uc.mem_write(LOAD_ADDR, code)

    mmio = collections.OrderedDict()   # addr -> [count, first_pc, kind]
    order = []                          # chronological unique (pc,addr,kind)
    seen_pc_addr = set()

    def log(kind, addr, pc, size, value=None):
        e = mmio.get(addr)
        if e is None:
            mmio[addr] = [1, pc, kind]
        else:
            e[0] += 1
            e[2] = e[2] if kind in e[2] else e[2] + "/" + kind
        key = (pc, addr, kind)
        if key not in seen_pc_addr:
            seen_pc_addr.add(key)
            order.append((len(order), pc, kind, addr, size, value))

    # Unmapped access hook: RAM is mapped, so this fires only on peripherals.
    def hook_mem_invalid(uc, access, address, size, value, user):
        pc = uc.reg_read(UC_ARM_REG_PC)
        if access in (UC_MEM_READ_UNMAPPED, UC_MEM_FETCH_UNMAPPED):
            log("R", address, pc, size)
            # lazily map a scratch page so the read returns 0 and we continue
            page = address & ~0xFFF
            try:
                uc.mem_map(page, 0x1000)
                uc.mem_write(page, b"\x00" * 0x1000)
            except UcError:
                pass
            return True
        elif access == UC_MEM_WRITE_UNMAPPED:
            log("W", address, pc, size, value)
            page = address & ~0xFFF
            try:
                uc.mem_map(page, 0x1000)
            except UcError:
                pass
            return True
        return False

    uc.hook_add(UC_HOOK_MEM_READ_UNMAPPED | UC_HOOK_MEM_WRITE_UNMAPPED |
                UC_HOOK_MEM_FETCH_UNMAPPED, hook_mem_invalid)

    # Count instructions so we stop on runaway loops.
    state = {"n": 0, "last_pc": 0}
    def hook_code(uc, address, size, user):
        state["n"] += 1
        state["last_pc"] = address
        if state["n"] >= MAX_INSNS:
            uc.emu_stop()
    uc.hook_add(UC_HOOK_CODE, hook_code)

    # Reset: ARM starts at the reset vector (offset 0). SVC mode, IRQ/FIQ off.
    uc.reg_write(UC_ARM_REG_SP, 0x00100000)
    err = None
    try:
        uc.emu_start(LOAD_ADDR, LOAD_ADDR + len(code), count=0)
    except UcError as e:
        err = e

    print(f"# APPSBL peripheral probe")
    print(f"# instructions executed: {state['n']}  last_pc=0x{state['last_pc']:08x}")
    if err:
        print(f"# stopped on: {err} at pc=0x{uc.reg_read(UC_ARM_REG_PC):08x}")
    print(f"# distinct MMIO addresses touched: {len(mmio)}\n")

    print("== chronological first-touch (order, pc, kind, addr, size) ==")
    for i, pc, kind, addr, size, val in order[:120]:
        extra = f" val=0x{val:x}" if val is not None else ""
        print(f"  [{i:3}] pc=0x{pc:08x} {kind} 0x{addr:08x} ({size}b){extra}")

    print("\n== MMIO addresses grouped by 0x...00000 region ==")
    regions = collections.Counter()
    for addr in mmio:
        regions[addr & 0xFFF00000] += 1
    for base, n in sorted(regions.items()):
        print(f"  0x{base:08x}xxxxx : {n} distinct regs")

if __name__ == "__main__":
    main()
