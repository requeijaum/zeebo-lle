#!/usr/bin/env python3
"""
Zeebo LLE — peripheral probe v3, device models transcribed from the REAL Zeebo
bring-up source (openzeebo zloader/arch_msm7k). No more guessed labels.

Key fixes over v2 (which burned 54M insns in delay loops and fell off the end):
  - GPT (0xC0100000): GPT_COUNT_VAL (+0x04) is a FREE-RUNNING counter that
    advances every read. arch_msm7k clock.c spins `while(GPT_COUNT_VAL < N)`.
    A sticky value never satisfies `< N` progression, so v2 spun forever. v3
    returns an incrementing counter -> delay loops terminate deterministically.
  - DMOV/ADM (0xA9700000): the 24 "channels" of v2 were DMA channels, not
    clock-gates. Model the command-pointer handshake: STATUS returns
    CMD_PTR_RDY|RSLT_VALID and RSLT returns DONE (0x80000002) so
    dmov_exec_cmdptr() completes.
  - Correct labels: 0xAA600000 = MDDI (not UART); UART1 = 0xA9A00000;
    CLK_CTL = 0xA8600000; NAND = 0xA0A00000.
Read-only on the NAND copy. Unicorn ARMv6 (ARM1176).
"""
import struct, collections
from unicorn import *
from unicorn.arm_const import *
import capstone

NAND = "/home/rafaelfrequiao/projects/zeebo-lle/nand"
APPSBL = f"{NAND}/1.1.2_APPSBL.bin"
RAM_BASE, RAM_SIZE = 0x00000000, 0x02000000
MAX_INSNS = 80_000_000

md = capstone.Cs(capstone.CS_ARCH_ARM, capstone.CS_MODE_ARM)

# --- device bases (from zloader/include/msm7k/*.h) ---
VIC   = 0xC0000000
GPT   = 0xC0100000
DMOV  = 0xA9700000
GPIO1 = 0xA9200000
MDDI  = 0xAA600000
CLK   = 0xA8600000
UART1 = 0xA9A00000
NANDC = 0xA0A00000

def main():
    code = open(APPSBL, "rb").read()
    uc = Uc(UC_ARCH_ARM, UC_MODE_ARM)
    try: uc.ctl_set_cpu_model(UC_CPU_ARM_1176)
    except Exception: pass
    uc.mem_map(RAM_BASE, RAM_SIZE)
    uc.mem_write(0, code)

    sticky = {}
    touched = collections.OrderedDict()
    order = []; seen = set()
    dev = {"gpt": 0}  # free-running counter state

    def note(kind, addr, pc, size, val=None):
        e = touched.get(addr)
        if e is None: touched[addr] = [1, pc, kind]
        else: e[0]+=1
        k=(pc,addr,kind)
        if k not in seen:
            seen.add(k); order.append((len(order),pc,kind,addr,val))

    def model_read(address, size):
        # --- GPT: free-running count at +0x04 ---
        if address == GPT + 0x04:
            dev["gpt"] += 64          # advance fast enough to clear < N loops
            return dev["gpt"] & 0xFFFFFFFF
        if address == GPT + 0x00:     # match val, sticky
            return sticky.get(address, 0)
        # --- DMOV/ADM command handshake (SD1 base 0xA9400000 in dmov.h) ---
        # STATUS = SD1+0x200+ch*4 ; RSLT = SD1+0x040+ch*4
        if 0xA9400200 <= address <= 0xA94002FF:   # DMOV_STATUS band
            return (1 << 1) | (1 << 0)            # RSLT_VALID | CMD_PTR_RDY
        if 0xA9400040 <= address <= 0xA94000FF:   # DMOV_RSLT band
            return 0x80000002                     # valid result, DONE
        # --- a9700 legacy status band kept for compatibility ---
        if 0xa9700e00 <= address <= 0xa9700eff and address not in sticky:
            return 0x1
        # --- MDDI / UART / CLK status: report ready-ish ---
        if address == MDDI + 0x28:
            return 0xFFFFFFFF
        if address in sticky:
            return sticky[address]
        return 0

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

    MMIO_LO = 0x02000000
    def hook_mem(uc, access, address, size, value, user):
        if address < MMIO_LO: return
        pc = uc.reg_read(UC_ARM_REG_PC)
        if access == UC_MEM_WRITE:
            sticky[address] = value & ((1<<(size*8))-1)
            note("W", address, pc, size, value)
        else:
            v = model_read(address, size)
            uc.mem_write(address, struct.pack("<I", v & 0xFFFFFFFF)[:size])
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
            if st["spin_n"] > 500000:
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

    print(f"# APPSBL probe v3 (arch_msm7k-based device models)")
    print(f"# insns={st['n']} last_pc=0x{st['last']:08x} spin_at=0x{st['spin_from']:08x}")
    if err: print(f"# stopped: {err} pc=0x{uc.reg_read(UC_ARM_REG_PC):08x}")
    print(f"# distinct MMIO addrs: {len(touched)}\n")
    print("== chronological first-touch (last 60) ==")
    for i,pc,kind,addr,val in order[-60:]:
        ex=f" val=0x{val:x}" if val is not None else ""
        print(f"  [{i:3}] pc=0x{pc:08x} {kind} 0x{addr:08x}{ex}")
    print("\n== regions ==")
    reg=collections.Counter(a & 0xFFF00000 for a in touched)
    for b,n in sorted(reg.items()): print(f"  0x{b:08x} : {n}")
    # did we reach the NAND controller?
    nand_hits=[a for a in touched if (a & 0xFFF00000)==0xA0A00000]
    print(f"\n# NAND controller (0xA0A00000) touched: {len(nand_hits)} addrs -> {[hex(x) for x in nand_hits[:8]]}")

if __name__=="__main__": main()
