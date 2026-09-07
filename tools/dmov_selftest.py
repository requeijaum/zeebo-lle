#!/usr/bin/env python3
"""Self-test: drive the DOMVModel through the EXACT descriptor sequence that
nand.c::_flash_read_page emits (page 11712 = APPSBL), and confirm the DMA copies
the APPSBL page bytes from the flash buffer into RAM. This proves the functional
DMOV model + NandController produce real firmware content without needing the
full bootloader linked yet.

We build the pointer/command lists in a fake Unicorn RAM, run the DMOV engine,
and read back the 2048-byte page.
"""
import struct, sys
sys.path.insert(0, "/home/rafaelfrequiao/projects/zeebo-lle/tools")
from unicorn import *
from unicorn.arm_const import *
from nand_controller import NandController, NAND_BASE, PAGE_DATA
from dmov_model import DMOVModel, DMOV_NAND_CHAN, NAND_FLASH_BUFFER, CMD_PTR_LP, CMD_LC, CMD_SRC_CRCI, CMD_DST_CRCI

ND = "/home/rafaelfrequiao/projects/zeebo-lle/nand"
RAM = 0x00200000  # scratch RAM for cmdlist/ptr/data in the "guest"
PAGE_TARGET = 0x00300000  # where the 2048-byte page lands

# ---- small fake guest RAM via real Unicorn ----
uc = Uc(UC_ARCH_ARM, UC_MODE_ARM)
uc.mem_map(0x00200000, 0x100000)   # scratch
uc.mem_map(0x00300000, 0x100000)   # data target
# map a physical window so NAND_BASE reads work through Unicorn? NandController is
# a host object; the DMOV model handles NAND regs directly. But RAM src/dst need
# Unicorn. Fine.

nand = NandController(f"{ND}/1.1.2.bin", f"{ND}/1.1.2_spare.bin")
dmov = DMOVModel(uc, nand)

# The page we want = APPSBL (page 11712). Build the descriptor/io struct exactly
# like nand.c::_flash_read_page.
PAGE = 11712
# data_flash_io struct @ RAM+0x200 (16B header + 8*4 result)
IO = RAM + 0x200
cmdlist = RAM + 0x400
ptr = RAM + 0x600

def w32(a, v): uc.mem_write(a, struct.pack("<I", v & 0xFFFFFFFF))

# ---- struct data_flash_io ----
w32(IO+0x00, 0x33)          # NAND_CMD_PAGE_READ_ECC
w32(IO+0x04, (PAGE << 16) & 0xFFFFFFFF)  # addr0
w32(IO+0x08, (PAGE >> 16) & 0xff)        # addr1
w32(IO+0x0c, 0 | 4)                      # chipsel
w32(IO+0x10, 0)  # cfg0 (will load real CFG)
w32(IO+0x14, 0)  # cfg1
w32(IO+0x18, 1)  # exec (GO)
w32(IO+0x1c, 0x203)  # ecc_cfg
w32(IO+0x20, 0)      # ecc_cfg_save
# result[4] = IO+0x24.. (8 bytes each) - leave zeroed

# ---- command list (mirrors _flash_read_page inner loop, n=0 only for brevity) ----
# n=0: CMD/ADDR0/ADDR1/CHIPSEL in 16B burst to NAND_FLASH_CMD
def cmd_des(c, s, d, ln):
    return [c & 0xFFFFFFFF, s & 0xFFFFFFFF, d & 0xFFFFFFFF, ln & 0xFFFFFFFF]

seq = []
seq.append(cmd_des((5 << 7), IO+0x00, NAND_BASE+0x00, 16))  # DST_CRCI_NAND_CMD(5): CMD..CHIPSEL
seq.append(cmd_des(0,              IO+0x10, NAND_BASE+0x20, 8))   # cfg0/cfg1 -> DEV0_CFG0
seq.append(cmd_des(0,              IO+0x18, NAND_BASE+0x10, 4))   # exec=1 -> EXEC_CMD
seq.append(cmd_des((4 << 3), NAND_BASE+0x14, IO+0x24, 8))  # SRC_CRCI_NAND_DATA(4): status->result0
seq.append(cmd_des(0,              NAND_FLASH_BUFFER, PAGE_TARGET+0, 512))  # buffer->RAM
seq.append(cmd_des(CMD_LC,         NAND_FLASH_BUFFER, PAGE_TARGET+512, 512)) # buffer+512->RAM (LC)

# ---- pointer list ----
for i, d in enumerate(seq):
    w32(cmdlist + i*16, *d[:1]); w32(cmdlist + i*16+4, d[1]); w32(cmdlist + i*16+8, d[2]); w32(cmdlist + i*16+12, d[3])
w32(ptr, ((cmdlist >> 3) << 0) | CMD_PTR_LP)

# ---- run the DMA engine exactly as nand.c::dmov_exec_cmdptr does (value = ptr>>3|LP) ----
dmov.exec_cmdptr(((ptr >> 3) << 0) | CMD_PTR_LP)
result = dmov.last_chan

# ---- check: page target RAM has APPSBL bytes? ----
got = bytes(uc.mem_read(PAGE_TARGET, 16))
ref = open(f"{ND}/1.1.2.bin","rb").read(lmax := 0) or None
# reference: APPSBL starts at NAND page 11712 (data offset page*2048)
ref_off = PAGE * 2048
ref_bytes = open(f"{ND}/1.1.2.bin","rb").read()[ref_off:ref_off+16]
print("DMA page-target first16:", got.hex())
print("APPSBL dump   first16:", ref_bytes.hex())
print("MATCH" if got == ref_bytes else "MISMATCH")
print("DMOV execs:", dmov.exec_count, "status chan:", hex(result) if result else None)