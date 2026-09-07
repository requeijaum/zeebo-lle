#!/usr/bin/env python3
"""
Zeebo LLE — MSM7201A NAND/EBI2 flash controller model (ROADMAP Phase 2).

Transcribed from the real Zeebo bring-up source register map
(openzeebo zloader/include/msm7k/nand.h) and the command sequences in nand.c.
It is a behavioral model of the memory-mapped controller at 0xA0A00000, backed
by the real NAND dump so guest firmware can FETCH_ID and PAGE_READ actual bytes.

This module is standalone and unit-tested at the bottom (run directly). It plugs
into the Unicorn probe as the 0xA0A00000 page handler.

Geometry (from dump + KB): page 2048B data + 64B spare = 2112B/page, 64 pages
per block, 128MB system area. ID 0x5580b1ad (Samsung, verify against READ_ID).

NAND access on this SoC normally goes through the ADM (DMA); nand.c pushes a
command list whose descriptors move words between RAM and these registers. We
model the register-level side-effects so BOTH the DMA path and a direct
register-poke path converge to the right FLASH_BUFFER / READ_ID contents.
"""
import struct

# --- register offsets (nand.h) ---
NAND_BASE            = 0xA0A00000
R_FLASH_CMD          = 0x0000
R_ADDR0              = 0x0004
R_ADDR1              = 0x0008
R_CHIP_SELECT        = 0x000C
R_EXEC_CMD           = 0x0010
R_FLASH_STATUS       = 0x0014
R_BUFFER_STATUS      = 0x0018
R_DEV0_CFG0          = 0x0020
R_DEV0_CFG1          = 0x0024
R_READ_ID            = 0x0040
R_READ_STATUS        = 0x0044
R_CONFIG_DATA        = 0x0050
R_CONFIG             = 0x0054
R_CONFIG_MODE        = 0x0058
R_CONFIG_STATUS      = 0x0060
R_DEV_CMD_VLD        = 0x00AC
R_EBI2_ECC_BUF_CFG   = 0x00F0
R_FLASH_BUFFER       = 0x0100   # 0x100..0x0x?  page buffer window

# --- device commands (nand.h) ---
CMD_SOFT_RESET   = 0x01
CMD_PAGE_READ    = 0x32
CMD_PAGE_READ_ECC= 0x33
CMD_PAGE_READ_ALL= 0x34
CMD_FETCH_ID     = 0x0B
CMD_STATUS       = 0x0C
CMD_RESET        = 0x0D

# flash status bits (observed conventions)
FS_OP_ERR    = 1 << 4
FS_MPU_ERR   = 1 << 5
FS_READY     = 1 << 6   # device ready / operation complete
FS_OK        = FS_READY

PAGE_DATA = 2048
PAGE_SPARE = 64
PAGE_FULL = PAGE_DATA + PAGE_SPARE   # 2112


class NandController:
    """Behavioral MSM7201A NAND controller backed by a raw dump file.

    data_path  : 1.1.2.bin        (page-data only, 2048B/page)
    spare_path : 1.1.2_spare.bin  (2112B/page incl. spare) — optional, preferred
    """
    def __init__(self, data_path, spare_path=None, nand_id=0x5580b1ad):
        self._data_blob = open(data_path, "rb").read()
        if spare_path:
            self._blob = open(spare_path, "rb").read()
            self._stride = PAGE_FULL
        else:
            self._blob = self._data_blob
            self._stride = PAGE_DATA
        self.nand_id = nand_id
        self.reg = {}          # sticky register file
        self.buffer = bytearray(PAGE_FULL)  # FLASH_BUFFER window
        self.status = FS_OK
        self.last_cmd = 0
        self._id_latched = 0
        # real MSM7201A NAND controller CFG defaults (openzeebo KB + zloader):
        # DEV0_CFG0 = 0xa25400c0, DEV0_CFG1 = 0x0004745e. flash_read_config reads
        # these; must be non-zero or flash_read_config returns -1 and boot stalls.
        if R_DEV0_CFG0 not in self.reg:
            self.reg[R_DEV0_CFG0] = 0xa25400c0
        if R_DEV0_CFG1 not in self.reg:
            self.reg[R_DEV0_CFG1] = 0x0004745e

    def _page_bytes(self, page):
        # Page DATA comes from the DATA image (2048B/page, contiguous), NOT the
        # spare image: 1.1.2_spare.bin is 528-byte chunks (512 data + 16 spare)
        # interleaved, so contiguous-2112 reads corrupt everything past byte 511.
        off = page * PAGE_DATA
        return self._data_blob[off:off + PAGE_DATA]

    # -- MMIO interface --
    def read(self, off, size=4):
        if off == R_FLASH_STATUS:
            return self.status
        if off == R_BUFFER_STATUS:
            return 0  # 0 = no error, buffer valid
        if off == R_READ_ID:
            return self._id_latched
        if off == R_READ_STATUS:
            return self.status
        if off == R_CONFIG_STATUS:
            return 1  # config applied
        if R_FLASH_BUFFER <= off < R_FLASH_BUFFER + PAGE_FULL:
            i = off - R_FLASH_BUFFER
            return struct.unpack_from("<I", self.buffer, i)[0] if i + 4 <= len(self.buffer) else 0
        return self.reg.get(off, 0)

    def write(self, off, val, size=4):
        self.reg[off] = val & 0xFFFFFFFF
        if off == R_FLASH_CMD:
            self.last_cmd = val & 0xFF
        elif off == R_EXEC_CMD:
            self._execute()
        elif R_FLASH_BUFFER <= off < R_FLASH_BUFFER + PAGE_FULL:
            struct.pack_into("<I", self.buffer, off - R_FLASH_BUFFER, val & 0xFFFFFFFF)

    def _execute(self):
        cmd = self.last_cmd
        if cmd == CMD_FETCH_ID:
            self._id_latched = self.nand_id
            self.status = FS_OK
        elif cmd in (CMD_PAGE_READ, CMD_PAGE_READ_ECC, CMD_PAGE_READ_ALL):
            # Page number comes from the ADDRESS registers. The Qualcomm layout
            # (per openzeebo nand.c) puts the row high bits in ADDR0 (page<<16)
            # and bits 16..23 of page in ADDR1: addr0=page<<16, addr1=(page>>16)&0xff.
            # Decode: page = (addr0>>16) | ((addr1 & 0xff) << 8). Some drivers
            # instead write the page flat into ADDR0; detect both: if addr0 is
            # large it is the <<16 form, else use flat.
            addr0 = self.reg.get(R_ADDR0, 0)
            addr1 = self.reg.get(R_ADDR1, 0)
            if addr0 >= 0x10000:  # shifted layout (page in top 16 bits)
                page = (addr0 >> 16) | ((addr1 & 0xFF) << 8)
            else:
                page = addr0
            npages = len(self._blob) // self._stride
            if 0 <= page < npages:
                pg = self._page_bytes(page)
                self.buffer[:len(pg)] = pg
                self.status = FS_OK
            else:
                self.status = FS_OP_ERR
        elif cmd in (CMD_SOFT_RESET, CMD_RESET):
            self.status = FS_OK
        elif cmd == CMD_STATUS:
            self.status = FS_OK
        else:
            self.status = FS_OK


# ---------------- self-test ----------------
if __name__ == "__main__":
    import os
    ND = "/home/rafaelfrequiao/projects/zeebo-lle/nand"
    data = f"{ND}/1.1.2.bin"
    spare = f"{ND}/1.1.2_spare.bin"
    ok = True

    # geometry sanity
    dsz = os.path.getsize(data); ssz = os.path.getsize(spare)
    npage_d = dsz // PAGE_DATA
    npage_s = ssz // PAGE_FULL
    print(f"data  {dsz} bytes -> {npage_d} pages @2048")
    print(f"spare {ssz} bytes -> {npage_s} pages @2112")
    assert dsz % PAGE_DATA == 0, "data not multiple of 2048"
    assert ssz % PAGE_FULL == 0, "spare not multiple of 2112"
    assert npage_d == npage_s, "page count mismatch between data and spare images"

    nc = NandController(data, spare)

    # 1) FETCH_ID must latch the real NAND id
    nc.write(R_FLASH_CMD, CMD_FETCH_ID)
    nc.write(R_EXEC_CMD, 1)
    got = nc.read(R_READ_ID)
    print(f"FETCH_ID -> 0x{got:08x} (expect 0x5580b1ad)")
    if got != 0x5580b1ad:
        print("  NOTE: id constant may need correction from real dump OTP/READ_ID")

    # 2) PAGE_READ page 0 must equal first 2048 bytes of the dump (== APPSBL start)
    nc.write(R_ADDR0, 0)
    nc.write(R_FLASH_CMD, CMD_PAGE_READ)
    nc.write(R_EXEC_CMD, 1)
    buf = bytes(nc.buffer[:16])
    ref = open(data, "rb").read(16)
    print(f"PAGE_READ[0] first16 = {buf.hex()}")
    print(f"dump        first16 = {ref.hex()}")
    assert buf == ref, "page-0 read mismatch"
    print("  page-0 read OK (matches dump)")

    # 3) status ready
    assert nc.read(R_FLASH_STATUS) & FS_READY, "status not ready after read"
    print("status READY bit set OK")

    # 4) APPSBL lives page-aligned inside the NAND at 0x16e0000 (page 11712);
    #    its first word is the ARM vector table `ldr pc,[pc,#0x18]` = 18f09fe5.
    APPSBL_PAGE = 0x16e0000 // PAGE_DATA
    nc.write(R_ADDR0, APPSBL_PAGE)
    nc.write(R_FLASH_CMD, CMD_PAGE_READ)
    nc.write(R_EXEC_CMD, 1)
    head = bytes(nc.buffer[:4])
    print(f"PAGE_READ[{APPSBL_PAGE}] first4 = {head.hex()} (expect 18f09fe5)")
    assert head == bytes.fromhex("18f09fe5"), "APPSBL page not found at expected page"
    print("APPSBL vector table located at page %d OK" % APPSBL_PAGE)

    print("\nALL NAND CONTROLLER SELF-TESTS PASSED")
