#!/usr/bin/env python3
"""
Functional ADM/DMOV DMA model for the MSM7201A, matching what the openzeebo
zloader's nand.c emits. This replaces the handshake-only stub so the guest's
flash reads actually execute.

Protocol (from arch_msm7k/nand.c + include/msm7k/dmov.h):
 - Firmware builds a POINTER LIST and one or more COMMAND LISTs in RAM, each
   entry a 16-byte dmov_s {u32 cmd; u32 src; u32 dst; u32 len}.
 - It writes DMOV_CMD_PTR(ch) = (ptr_list_addr>>3)|CMD_PTR_LP(1<<31).
 - The DMA controller walks: for each pointer (addr>>3<<3), run that command
   list; stop on the pointer/command with the LP/LC bit.
 - Each command executes "DMA (len bytes) src->dst". Addressing:
     * src/dst == a NAND register (0xa0a00000+off) -> NandController read/write
     * src/dst is RAM (a "physical" address) -> Unicorn mem
     * CRCI bits select the NAND data channel (flow/buffer), just route.
 - On completion it sets DMOV_STATUS RSLT_VALID|CMD_PTR_RDY, and fills the RSLT
   fifo with a DONE result word (0x80000002).
Returns the stop/status state so the runner can observe.

Command word bits (dmov.h): CMD_LC=1<<31 (last cmd), CMD_PTR_LP=1<<31 (last
ptr), CMD_FR=1<<22 (force result), CMD_OCU=1<<21, CMD_OCB=1<<20,
CMD_SRC_CRCI(n)=(n&15)<<3, CMD_DST_CRCI(n)=(n&15)<<7. NAND: chan 3, CRCI
data=4, cmd=5.
"""
import struct

# --- DMOV register layout (dmov.h) ---
DMOV_SD1_BASE = 0xa9400000
def sd1(off, ch=0): return DMOV_SD1_BASE + off + (ch << 2)
DMOV_CMD_PTR = lambda ch: sd1(0x000, ch)
DMOV_RSLT    = lambda ch: sd1(0x040, ch)
DMOV_STATUS  = lambda ch: sd1(0x200, ch)
DMOV_CONFIG  = lambda ch: sd1(0x300, ch)
DMOV_NAND_CHAN = 3

CMD_PTR_LP = 1 << 31
CMD_LC     = 1 << 31
CMD_FR     = 1 << 22
CMD_OCU    = 1 << 21
CMD_OCB    = 1 << 20
CMD_SRC_CRCI = 3
CMD_DST_CRCI = 7

# NAND reg base (nand.h)
NAND_BASE = 0xA0A00000
NAND_FLASH_BUFFER = NAND_BASE + 0x100


class DMOVModel:
    """Executes ADM/DMOV DMA command lists. uc = the Unicorn instance (for RAM
    read/write); nand = a NandController (for NAND reg sides)."""
    def __init__(self, uc, nand):
        self.uc = uc
        self.nand = nand
        self.exec_count = 0
        self.last_chan = None
        self.log = []

    # ---- helpers ----
    def _read_ram(self, addr, size):
        b = bytes(self.uc.mem_read(addr & 0xFFFFFFFF, size))
        return struct.unpack("<I", b.ljust(4, b"\0"))[0] if size == 4 else b

    def _write_ram(self, addr, data, size=4):
        if size == 4:
            self.uc.mem_write(addr, struct.pack("<I", data & 0xFFFFFFFF))
        else:
            self.uc.mem_write(addr, data)

    def _is_nand_reg(self, ad):
        return NAND_BASE <= (ad & 0xFFFFFFFF) < NAND_BASE + 0x400

    def _nand_off(self, ad):
        return (ad & 0xFFFFFFFF) - NAND_BASE

    # ---- the DMA engine ----
    def exec_cmdptr(self, regval):
        """regval = value written to DMOV_CMD_PTR = (phys_addr>>3)|CMD_PTR_LP.
        The hardware the value>>3 into an 8-byte-aligned address, so
        phys_addr = (regval & 0x7FFFFFFF) << 3."""
        self.exec_count += 1
        pptr = (regval & 0x7FFFFFFF) << 3
        idx = 0
        while True:
            # read a pointer entry (u32)
            p = self._read_ram(pptr, 4)
            cmdlist_phys = (p & 0x7FFFFFFF) << 3   # entry is addr>>3 (+LP bit)
            plast = bool(p & CMD_PTR_LP)
            # run this command list
            self._run_cmdlist(cmdlist_phys)
            if self.log is not None:
                self.log.append(("ptr", idx, hex(pptr), hex(cmdlist_phys), "LP" if plast else ""))
            if plast:
                break
            pptr = cmdlist_phys  # advance? in nand.c only 1 pointer; guard
            idx += 1
            if idx > 16: break
        self.last_chan = DMOV_NAND_CHAN
        return 0x80000002  # DONE result word

    def _run_cmdlist(self, base):
        off = 0
        cmdidx = 0
        while True:
            e = base + off
            try:
                cmd, src, dst, ln = struct.unpack("<4I", bytes(self.uc.mem_read(e, 16)))
            except Exception:
                break
            is_last = bool(cmd & CMD_LC)
            self._exec_descriptor(cmd, src, dst, ln)
            if self.log is not None:
                self.log.append(("cmd", cmdidx, hex(src), hex(dst), ln))
            if is_last:
                break
            off += 16
            cmdidx += 1
            if cmdidx > 64: break

    def _exec_descriptor(self, cmd, src, dst, ln):
        uc = self.uc
        # NAND register as source: read from controller into RAM (or into NAND buf)
        src_nand = self._is_nand_reg(src)
        dst_nand = self._is_nand_reg(dst)
        if src_nand:
            off = self._nand_off(src)
            # special: FLASH_BUFFER reads return page data; others scalar regs
            if src == NAND_FLASH_BUFFER:
                # read 512 bytes from the page buffer into RAM dst
                for i in range(0, min(ln, 2048), 4):
                    v = self.nand.read(self._nand_off(src) + i, 4)
                    try: uc.mem_write(dst + i, struct.pack("<I", v & 0xFFFFFFFF))
                    except Exception: pass
            else:
                v = self.nand.read(off, min(ln, 4))
                try: uc.mem_write(dst & 0xFFFFFFFF, struct.pack("<I", v & 0xFFFFFFFF))
                except Exception: pass
            return
        if dst_nand:
            off = self._nand_off(dst)
            # writing NAND regs in a multi-word BURST (e.g. CMD/ADDR0/ADDR1/CS
            # in 16B): program each consecutive register word from RAM src.
            if dst == NAND_FLASH_BUFFER:
                for i in range(0, min(ln, 2048), 4):
                    try:
                        v = struct.unpack("<I", bytes(uc.mem_read(src + i, 4)))[0]
                        self.nand.write(self._nand_off(dst) + i, v, 4)
                    except Exception:
                        pass
            elif ln == 16:
                # driver burst: CMD,ADDR0,ADDR1,CHIPSEL (and sometimes CFG)
                for i in range(0, 16, 4):
                    try:
                        v = struct.unpack("<I", bytes(uc.mem_read(src + i, 4)))[0]
                        self.nand.write(off + i, v, 4)
                    except Exception:
                        break
            else:
                try:
                    v = struct.unpack("<I", bytes(uc.mem_read(src & 0xFFFFFFFF, min(ln, 4))))[0]
                    self.nand.write(off, v, min(ln, 4))
                except Exception:
                    pass
            return
        # RAM->RAM descriptor (or involving CRCI data buffer). Copy bytes.
        length = min(ln, 0x1000)
        try:
            # sourceless CRCI-DATA reads come from NAND buffer; if src==dst==NAND
            # handled above. Plain memcpy otherwise.
            data = bytes(uc.mem_read(src, length))
            uc.mem_write(dst, data[:length])
        except Exception:
            pass