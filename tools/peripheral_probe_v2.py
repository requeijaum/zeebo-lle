#!/usr/bin/env python3
"""
Zeebo LLE — peripheral probe v2, with a minimal device model.

v1 stubbed every MMIO read as 0 and the APPSBL died at the halt 0xc30 because
its clock/PLL and status polls never converged. v2 adds sane behaviors:
  - reads default to a per-address "sticky" store (writes are remembered and
    read back — models plain config registers / RMW), so a PLL config written
    then polled reflects what was written;
  - a small set of status registers return "ready" bits;
This lets the boot advance past the first hardware sanity checks and reveals the
NEXT wave of registers. Read-only on the NAND copy.
"""
import struct, collections
from unicorn import *
from unicorn.arm_const import *
import capstone

NAND = "/home/rafaelfrequiao/projects/zeebo-lle/nand"
APPSBL = f"{NAND}/1.1.2_APPSBL.bin"
RAM_BASE, RAM_SIZE = 0x00000000, 0x02000000
MAX_INSNS = 60_000_000

md = capstone.Cs(capstone.CS_ARCH_ARM, capstone.CS_MODE_ARM)

# status registers that should report "ready/locked" when polled.
# value returned is all-ones so any tst/and mask sees the bit set.
STATUS_READY = {
    0xa9700e10: 0x00000001,   # ready(bit0)=1, error(bit1)=0
}

def main():
    code = open(APPSBL, "rb").read()
    uc = Uc(UC_ARCH_ARM, UC_MODE_ARM)
    try: uc.ctl_set_cpu_model(UC_CPU_ARM_1176)
    except Exception: pass
    uc.mem_map(RAM_BASE, RAM_SIZE)
    uc.mem_write(0, code)

    sticky = {}                      # addr -> last written value (config regs)
    touched = collections.OrderedDict()
    order = []
    seen = set()

    def note(kind, addr, pc, size, val=None):
        e = touched.get(addr)
        if e is None: touched[addr] = [1, pc, kind]
        else: e[0]+=1
        k=(pc,addr,kind)
        if k not in seen:
            seen.add(k); order.append((len(order),pc,kind,addr,val))

    MMIO_LO = 0x02000000             # anything at/above this is peripheral
    def hook_unmapped(uc, access, address, size, value, user):
        pc = uc.reg_read(UC_ARM_REG_PC)
        page = address & ~0xFFF
        try: uc.mem_map(page, 0x1000)
        except UcError: pass
        if access in (UC_MEM_READ_UNMAPPED, UC_MEM_FETCH_UNMAPPED):
            note("R", address, pc, size)
        else:
            note("W", address, pc, size, value)
        return True

    # Model reads/writes on the peripheral band via a MEM hook.
    def hook_mem(uc, access, address, size, value, user):
        if address < MMIO_LO:  # RAM, ignore
            return
        pc = uc.reg_read(UC_ARM_REG_PC)
        if access == UC_MEM_WRITE:
            sticky[address] = value & ((1<<(size*8))-1)
            note("W", address, pc, size, value)
        else:  # READ
            # a9700 status band: any e10/e14/e18... reads ready(bit0)=1,error(bit1)=0
            if 0xa9700e00 <= address <= 0xa9700eff and address not in sticky:
                v = 0x1
            # aa600000 UART: status reg +0x28 polled for bit5 (TX/RX ready) etc
            elif address == 0xaa600028:
                v = 0xFFFFFFFF
            elif address in STATUS_READY:
                v = STATUS_READY[address]
            elif address in sticky:
                v = sticky[address]
            else:
                v = 0
            uc.mem_write(address, struct.pack("<I", v & 0xFFFFFFFF)[:size] if size<=4 else struct.pack("<I",v))
            note("R", address, pc, size, v)

    uc.hook_add(UC_HOOK_MEM_READ_UNMAPPED | UC_HOOK_MEM_WRITE_UNMAPPED |
                UC_HOOK_MEM_FETCH_UNMAPPED, hook_unmapped)
    uc.hook_add(UC_HOOK_MEM_READ | UC_HOOK_MEM_WRITE, hook_mem,
                begin=MMIO_LO, end=0xFFFFFFFF)

    st = {"n":0,"last":0,"spin_from":0,"spin_n":0}
    def hook_code(uc, address, size, user):
        st["n"]+=1
        if address == st["last"]:
            st["spin_n"]+=1
            if st["spin_n"] > 200000:
                st["spin_from"]=address; uc.emu_stop()
        else:
            st["spin_n"]=0
        st["last"]=address
        if st["n"]>=MAX_INSNS: uc.emu_stop()
    uc.hook_add(UC_HOOK_CODE, hook_code)

    uc.reg_write(UC_ARM_REG_SP, 0x00100000)
    err=None
    try: uc.emu_start(0, len(code), count=0)
    except UcError as e: err=e

    print(f"# APPSBL probe v2 (sane device model)")
    print(f"# insns={st['n']} last_pc=0x{st['last']:08x} spin_at=0x{st['spin_from']:08x}")
    if err: print(f"# stopped: {err} pc=0x{uc.reg_read(UC_ARM_REG_PC):08x}")
    print(f"# distinct MMIO addrs: {len(touched)}\n")
    print("== chronological first-touch ==")
    for i,pc,kind,addr,val in order[:200]:
        ex=f" val=0x{val:x}" if val is not None else ""
        print(f"  [{i:3}] pc=0x{pc:08x} {kind} 0x{addr:08x}{ex}")
    print("\n== regions ==")
    reg=collections.Counter(a & 0xFFF00000 for a in touched)
    for b,n in sorted(reg.items()): print(f"  0x{b:08x} : {n}")

if __name__=="__main__": main()
