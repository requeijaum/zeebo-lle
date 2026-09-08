#!/usr/bin/env python3
"""
fuzz_iguana_kip.py — micro-emulation harness to discover the exact KIP/UTCB
data layout that the Iguana user task (0xb00033d0..0xb000345c) accepts without
falling into the panic loop at 0xb0003450.

Strategy
--------
* Extract every PT_LOAD segment that maps into the Iguana user window
  (0xb0000000..0xb0060000) from nand/1.1.2_APPS.bin.
* Emulate from 0xb00033d0. The only kernel dependency in this window is the
  `svc #0x14` in L4_KernelInterface (0xb000c738): we intercept it and return
  r0 = KIP base (0xf0f00000).
* Provide a synthetic KIP at 0xf0f00000 and a UTCB region at 0xff000000
  (with the USER_UTCB_REF pointer at 0xff000ff0 that main reads through r1).
* Milestone A: reach 0xb0003424 (the mempool_init call) -> proves the KIP/UTCB
  reads at +0xb0/+0xb4/+0xb8 and [utcb] produced mappable pointers.
* Milestone B: reach 0xb000344c / 0xb000345c without branching to the panic
  at 0xb0003450 -> the validator at 0xb00001fc returned success (r0==0).
* We stub the heavy service bls (mempool_init and the later init calls) so the
  run is deterministic and the KIP/UTCB layout is the only free variable; the
  final validator 0xb00001fc is optionally run for real.

Author: zeebo-lle reverse-engineering harness
"""
import struct
import sys
from unicorn import *
from unicorn.arm_const import *

APPS = "/home/rafaelfrequiao/projects/zeebo-lle/nand/1.1.2_APPS.bin"

MAIN      = 0xb00033d0
KIF       = 0xb000c720   # L4_KernelInterface
SVC_ADDR  = 0xb000c738   # svc #0x14 inside KIF
MEMPOOL   = 0xb000d5b4
VALIDATOR = 0xb00001fc
PANIC     = 0xb0003450
SUCCESS   = 0xb000345c
MEMPOOL_CALL = 0xb0003424

KIP_BASE  = 0xf0f00000
UTCB_BASE = 0xff000000
UTCB_REF  = 0xff000ff0    # main reads r1 = *[0xff000ff0]
UTCB_PTR  = 0xff0f0000    # where UTCB_REF points (an actual UTCB / MyLocalId)

# bls between mempool_init and the final validator that we stub to return 0
STUB_CALLS = {
    0xb000d5b4,  # mempool_init
    0xb00055dc,
    0xb000b1dc,
    0xb0001e80,
    0xb00056c4,
    0xb0004de4,
    0xb00070c8,
}

# ---------------------------------------------------------------------------

def load_segments():
    f = open(APPS, "rb").read()
    e_phoff = struct.unpack_from("<I", f, 0x1c)[0]
    e_phnum = struct.unpack_from("<H", f, 0x2c)[0]
    e_phent = struct.unpack_from("<H", f, 0x2a)[0]
    segs = []
    for i in range(e_phnum):
        off = e_phoff + i * e_phent
        (p_type, p_off, p_va, p_pa, p_fsz, p_msz, p_fl, p_al) = struct.unpack_from("<8I", f, off)
        if p_type != 1:
            continue
        if 0xb0000000 <= p_va < 0xb0e00000:
            segs.append((p_va, p_msz, f[p_off:p_off + p_fsz]))
    return segs


def align_down(x, a=0x1000):
    return x & ~(a - 1)


def align_up(x, a=0x1000):
    return (x + a - 1) & ~(a - 1)


def build_kip(fields):
    """Build a 0x1000 KIP page. fields: dict offset->dword."""
    kip = bytearray(0x1000)
    for off, val in fields.items():
        struct.pack_into("<I", kip, off, val & 0xffffffff)
    return kip


def run(kip_fields, utcb_fields, run_validator=False, trace=False):
    uc = Uc(UC_ARCH_ARM, UC_MODE_ARM)

    # map code segments
    mapped = []
    for va, msz, data in load_segments():
        base = align_down(va)
        size = align_up(va + msz) - base
        uc.mem_map(base, size)
        uc.mem_write(va, data)
        mapped.append((base, size))

    # stack
    STACK = 0xa0000000
    uc.mem_map(STACK - 0x10000, 0x20000)
    uc.reg_write(UC_ARM_REG_SP, STACK)

    # KIP page
    uc.mem_map(align_down(KIP_BASE), 0x1000)
    uc.mem_write(KIP_BASE, bytes(build_kip(kip_fields)))

    # UTCB region (cover 0xff000000..0xff100000 so both UTCB_REF and UTCB_PTR fit)
    uc.mem_map(0xff000000, 0x100000)
    # USER_UTCB_REF pointer -> UTCB_PTR
    uc.mem_write(UTCB_REF, struct.pack("<I", UTCB_PTR))
    for off, val in utcb_fields.items():
        uc.mem_write(UTCB_PTR + off, struct.pack("<I", val & 0xffffffff))

    # scratch page that KIP fields may point into (mempool arena etc.)
    ARENA = 0x80000000
    uc.mem_map(ARENA, 0x200000)

    state = {"pc": 0, "reached_mempool": False, "reached_end": False,
             "panic": False, "success": False, "fault": None, "count": 0,
             "regs_at_mempool": None}

    def hook_code(uc, address, size, _):
        state["pc"] = address
        state["count"] += 1
        if trace and MAIN <= address <= 0xb0003490:
            print("  pc=%08x" % address)
        if address == MEMPOOL_CALL:
            state["reached_mempool"] = True
            state["regs_at_mempool"] = [uc.reg_read(r) for r in
                (UC_ARM_REG_R0, UC_ARM_REG_R1, UC_ARM_REG_R2, UC_ARM_REG_R3,
                 UC_ARM_REG_R5, UC_ARM_REG_IP)]
        if address == PANIC:
            state["panic"] = True
            uc.emu_stop()
        if address in (SUCCESS, 0xb000344c) and address != PANIC:
            state["reached_end"] = True
        if address == SUCCESS:
            state["success"] = True
            uc.emu_stop()
        # intercept the svc inside L4_KernelInterface: return KIP base
        if address == SVC_ADDR:
            # syscall ABI: `mov ip,sp; mvn sp,#0x4b` before svc; kernel restores sp.
            uc.reg_write(UC_ARM_REG_SP, uc.reg_read(UC_ARM_REG_IP))
            uc.reg_write(UC_ARM_REG_R0, KIP_BASE)
            uc.reg_write(UC_ARM_REG_R1, 0)
            uc.reg_write(UC_ARM_REG_R2, 0)
            uc.reg_write(UC_ARM_REG_R3, 0)
            uc.reg_write(UC_ARM_REG_PC, address + 4)  # skip svc
            return
        # stub heavy service calls: emulate `bl x; return r0=0`
        if address in STUB_CALLS and not (address == VALIDATOR and run_validator):
            lr = uc.reg_read(UC_ARM_REG_LR)
            uc.reg_write(UC_ARM_REG_R0, 0)
            uc.reg_write(UC_ARM_REG_PC, lr)
            return

    def hook_mem_invalid(uc, access, address, size, value, _):
        state["fault"] = ("mem", access, address)
        return False

    uc.hook_add(UC_HOOK_CODE, hook_code)
    uc.hook_add(UC_HOOK_MEM_READ_UNMAPPED | UC_HOOK_MEM_WRITE_UNMAPPED |
                UC_HOOK_MEM_FETCH_UNMAPPED, hook_mem_invalid)

    try:
        uc.emu_start(MAIN, 0, count=200000)
    except UcError as e:
        if state["fault"] is None:
            state["fault"] = ("uc", str(e), state["pc"])

    return state


# ---------------------------------------------------------------------------

def main():
    print("=== fuzz_iguana_kip: KIP/UTCB layout discovery ===\n")

    # Baseline: empty KIP -> the +0xb0 pointer is 0 -> mempool arg faults or
    # validator faults. We fuzz the KIP fields that main dereferences.
    #
    # main reads:
    #   r5 = KIP[0xb0]           (a pointer -> validator's struct, mempool r1)
    #   r2 = KIP[0xb4] - KIP[0xb8]  (wait: ldm r3,{r2,r3}=KIP[0xb4],KIP[0xb8]; r2-=r3)
    #   r3 = KIP[0xb8]
    #   r2 = (KIP[0xb4]-KIP[0xb8]) + r5     -> mempool r2 (end)
    #   ip = *[utcb]                        -> mempool r0 (pool handle)
    #
    # For mempool_init(r0=ip,r1=r5,r2=end,...) not to fault we need r5 and the
    # derived end to be mappable. Point r5 (KIP[0xb0]) into our ARENA.
    ARENA = 0x80000000

    # KIP[0xb0] = arena base (r5, the pool memory + validator struct)
    # KIP[0xb4], KIP[0xb8] chosen so (b4-b8) is a positive size.
    base_kip = {
        0x00: 0x14b21150,       # magic 'L4\xe6K' style (not checked here)
        0xb0: 0xb0d00000,   # r5: pool base / validated object pointer
        0xb4: ARENA + 0x40000,  # region end
        0xb8: ARENA + 0x10000,  # region start  -> size = 0x30000
    }
    base_utcb = {0x00: ARENA + 0x100}  # *[utcb] -> mempool handle pointer

    st = run(base_kip, base_utcb, run_validator=False)
    report("Milestone A (reach mempool call, stubbed chain)", st)

    if st["reached_mempool"]:
        r = st["regs_at_mempool"]
        print("  mempool_init args: r0=%08x r1=%08x r2=%08x r3=%08x r5=%08x ip=%08x"
              % tuple(r))

    # Milestone B: run the real validator at the end.
    st2 = run(base_kip, base_utcb, run_validator=True)
    report("Milestone B (run to end, real validator)", st2)

    print("\n=== KIP layout that reaches the mempool_init call ===")
    for off in sorted(base_kip):
        print("  KIP+0x%02x = 0x%08x" % (off, base_kip[off]))
    print("  UTCB_REF(0xff000ff0) -> 0x%08x ; *UTCB = 0x%08x"
          % (UTCB_PTR, base_utcb[0x00]))


def report(title, st):
    print("[%s]" % title)
    print("  insns=%d last_pc=%08x reached_mempool=%s reached_end=%s success=%s panic=%s fault=%s"
          % (st["count"], st["pc"], st["reached_mempool"], st["reached_end"],
             st["success"], st["panic"], st["fault"]))
    print()


if __name__ == "__main__":
    main()
