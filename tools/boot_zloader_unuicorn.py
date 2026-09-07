#!/usr/bin/env python3
"""
Boot the COMPILED OpenZeebo zloader (firmware/openzeebo-zloader.bin) in Unicorn.
This is the real MSM7201A bootloader, so it exercises the ACTUAL firmware path:
peripheral init -> (via DMOV DMA) read NAND -> find the BREW signature pattern.

Wiring:
  - load image at 0x00a00000 (entry), stack 0x00bff000, heap 0x00c00000
  - DMOV_CMD_PTR write (SD1 base 0xa9400000, chan 3 @+0x00) -> DMOVModel.exec
  - DMOV result/status bands -> valid DONE
  - NAND (0xa0a00000) -> NandController (already DMOV services it)
  - periphs the boot touches: VIC/GPT/GPIO/UART/SSBI/CLK sticky+sane
  - hook INTR (we don't expect svc on a bare loader) + trace to NAND touches
"""
import struct, collections, sys
from unicorn import *
from unicorn.arm_const import *
import sys
sys.path.insert(0, "/home/rafaelfrequiao/projects/zeebo-lle/tools")
from nand_controller import NandController, NAND_BASE
from dmov_model import DMOVModel, DMOV_SD1_BASE, DMOV_NAND_CHAN, DMOV_CMD_PTR, DMOV_RSLT, DMOV_STATUS

ND = "/home/rafaelfrequiao/projects/zeebo-lle/nand"
IMG = "/home/rafaelfrequiao/projects/zeebo-lle/firmware/openzeebo-zloader.bin"
LOAD = 0x00a00000

def main(max_insns=8_000_000):
    bl = open(IMG, "rb").read()
    uc = Uc(UC_ARCH_ARM, UC_MODE_ARM)
    try: uc.ctl_set_cpu_model(UC_CPU_ARM_1176)
    except Exception: pass
    # map RAM: code 0xa00000..+0x30000, heap 0xc00000.., stack high
    for base,size in ((0x00a00000, 0x00400000), (0x00000000, 0x00300000),
                      (0x00c00000, 0x00400000),
                      (0xb0000000, 0x01000000),
                      (0xff000000,0x00200000),(0xffe00000,0x00200000)):
        try: uc.mem_map(base, size)
        except UcError: pass
    # early map 0xb8000000 (wd reg) and 0xc0000000 etc
    for base in (0xb8000000,0xc0000000,0xa9a00000,0xa8600000,0xa9000000,0xa9700000,0xa9400000,0xaa600000,0xa0a00000,0xa0800000):
        try: uc.mem_map(base, 0x1000)
        except UcError: pass
    uc.mem_write(LOAD, bl)
    uc.mem_write(0x00c00000, b"\x00"*4096)  # heap

    nand = NandController(f"{ND}/1.1.2.bin", f"{ND}/1.1.2_spare.bin")
    dmov = DMOVModel(uc, nand)
    sticky = {}

    def hu(uc, a, ad, sz, v, u):
        try:
            m=uc.mem_map(ad & ~0xFFF, 0x1000)
        except UcError: pass
        return True

    def hm(uc, a, ad, sz, v, u):
        # code/RAM ranges are real memory, not MMIO - let them pass through
        if ad < 0x80000000:
            return
        nonlocal_sticky = sticky
        pc = uc.reg_read(UC_ARM_REG_PC)
        if isinstance(st.get("mmio"), collections.Counter):
            st["mmio"][ad & 0xFFF00000] += 1
        # DMOV SD1 command/result/status/readback
        if DMOV_SD1_BASE <= ad < DMOV_SD1_BASE+0x400:
            off = ad - (DMOV_SD1_BASE)
            if a == UC_MEM_WRITE:
                # writing CMD_PTR of channel 3 => run DMA
                if off == ((DMOV_NAND_CHAN)<<2):   # 0x00c
                    dmov.exec_cmdptr(v)
                    sticky[ad]=v
                    return
                sticky[ad]=v; return
            else:
                # status / result: return DONE-ish
                val = 0
                c = off>>2
                cham = (off >> 2) & 0x7
                sub = off - (cham<<2)
                # result reg +0x40, status +0x200
                if sub  in (0x200,0x204,0x208,0x20c) or off in (0x200,0x204,0x208,0x20c):
                    val = 3  # RSLT_VALID|CMD_PTR_RDY
                elif sub in (0x40,0x44,0x48,0x4c) or off in (0x40,0x44,0x48,0x4c):
                    val = 0x80000002
                else:
                    val = sticky.get(ad, 0)
                uc.mem_write(ad, struct.pack("<I", val&0xFFFFFFFF)[:sz])
                return
        # NAND handled by dmov already when accessed via DMA; direct reg access too
        if NAND_BASE <= ad < NAND_BASE+0x400:
            o = ad - NAND_BASE
            if a == UC_MEM_WRITE: nand.write(o, v, sz)
            else:
                val = nand.read(o, sz)
                uc.mem_write(ad, struct.pack("<I", val&0xFFFFFFFF)[:sz])
            return
        if a == UC_MEM_WRITE:
            sticky[ad] = v; return
        # generic sticky reads; make GPT count advance
        if ad == 0xc0100004:
            val = ((struct.unpack("<I",bytes(uc.mem_read(0xc0100000,4)))[0] or 0) + 64)
            uc.mem_write(0xc0100000, struct.pack("<I",val))
            val = val
        elif ad in sticky:
            val = sticky[ad]
        else:
            val = 0
        uc.mem_write(ad, struct.pack("<I", val&0xFFFFFFFF)[:sz])

    uc.hook_add(UC_HOOK_MEM_READ_UNMAPPED|UC_HOOK_MEM_WRITE_UNMAPPED|UC_HOOK_MEM_FETCH_UNMAPPED, hu)
    uc.hook_add(UC_HOOK_MEM_READ|UC_HOOK_MEM_WRITE, hm)

    st = {"n":0,"last":0,"spin":0}
    def hc(uc, ad, sz, u):
        st["n"]+=1
        if ad == st["last"]:
            st["spin"]+=1
            if st["spin"]>1_000_000: st["stuck"]=ad; uc.emu_stop()
        else: st["spin"]=0
        st["last"]=ad
        if st["n"]>=max_insns: uc.emu_stop()
    uc.hook_add(UC_HOOK_CODE, hc)
    # MMIO capture for the boot run
    st["mmio"]=collections.Counter()

    # console: capture dprintf via uart_putc writes? skip. Watch for NAND EMI separately.
    v=[struct.unpack_from("<I", bl, i)[0] for i in range(0,0x20,4)]
    entry=LOAD + (v[2] - LOAD)  # start: symbol is in preamble word[2]
    # actually start: is at 0xa00028 as recorded; use it
    entry = 0x00a00028
    uc.reg_write(UC_ARM_REG_SP, 0x00bff000)
    err=None
    try: uc.emu_start(entry, 0xFFFFFFFF, count=0)
    except UcError as e: err=e
    print("== OpenZeebo zloader boot (Unicorn) ==")
    print("entry=0x%08x insns=%d last_pc=0x%08x stuck=0x%x err=%s"%(entry, st["n"], st.get("last",0), st.get("stuck",0), err))
    print("DMOV execs:", dmov.exec_count, "log entries:", len(dmov.log) if dmov.log else 0)
    if dmov.log:
        print("DMOV log:")
        for e in dmov.log[:30]:
            print("   ", e)
    mmio_touch = getattr(st, "mmio", None)
    if st.get("mmio") is not None:
        print("MMIO touched:", {hex(k):v for k,v in list(st["mmio"].items())[:15]})
    # did the DMA read the NAND? count FLASH_BUFFER reads (reflected in NandController buffer)
    print("final reg ADDR0=%s" % (hex(nand.reg.get(0x04,0))))

if __name__=="__main__":
    main()