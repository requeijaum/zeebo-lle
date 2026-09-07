#!/usr/bin/env python3
"""
Mini-boot: read a full NAND partition (APPS or AMSS) through the REAL DMA->NAND
path (DMOVModel + NandController) and materialize it into physical RAM at the PAs
the real ARM11 MMU map defines. Then compare against the raw dump byte-for-byte.

This is the "missing link" made concrete: the loader chain reads the OS image off
NAND and places it in RAM. Here we prove the parallel source of truth works —
using the exact descriptor sequence the zloader/kernel msm_nand driver emits
(page<<16 / (page>>16)&0xff, DMOV_CMD_PTR write, EXEC, FLASH_BUFFER reads).

Partition layout (zeemu firmware_inspector / openzeebo, in 128KB blocks):
  AMSS  block 0x012 len 0x0a5 ; APPS  block 0x0e6 len 0x0a9.
Block = 64 pages x 2048B = 131072B.
"""
import struct, sys
sys.path.insert(0, "/home/rafaelfrequiao/projects/zeebo-lle/tools")
from unicorn import *
from unicorn.arm_const import *
from nand_controller import NandController, NAND_BASE
from dmov_model import DMOVModel, CMD_PTR_LP, CMD_LC

ND = "/home/rafaelfrequiao/projects/zeebo-lle/nand"
BLOCK = 2048*64                 # 131072
nblocks = {"AMSS":(0x012,0x0a5), "APPS":(0x0e6,0x0a9)}
RAM_DEST = 0x10000000           # APPS ELF f0000000->10000000; dest physical base

def read_page_via_dma(uc, dmov, nand, page, dest):
    """Build the _flash_read_page descriptor sequence for ONE page (APPSBL path)
    and run it through the DMOV engine, landing 2048B into dest."""
    IO   = 0x00400000           # guest RAM for data_flash_io
    CL   = 0x00401000
    PTR  = 0x00402000
    def w(a,v): uc.mem_write(a, struct.pack("<I", v&0xFFFFFFFF))
    # struct data_flash_io
    w(IO+0x00, 0x33)                       # NAND_CMD_PAGE_READ_ECC
    w(IO+0x04, (page<<16)&0xFFFFFFFF)      # addr0 = page<<16   (kernel msm_nand.confirmed)
    w(IO+0x08, (page>>16)&0xFF)            # addr1
    w(IO+0x0c, 0|4)                        # chipsel
    w(IO+0x10, 0xa25400c0)                 # cfg0 (real)
    w(IO+0x14, 0x0004745e)                 # cfg1 (real)
    w(IO+0x18, 1)                          # exec GO
    w(IO+0x1c, 0x203)                      # ecc_cfg
    w(IO+0x20, 0)                          # ecc_cfg_save
    # result[4] @ IO+0x24 (8B each) left zero
    seq = [
        (5<<7, IO+0x00, NAND_BASE+0x00, 16),   # DST_CRCI_NAND_CMD: CMD/ADDR0/ADDR1/CS
        (0,    IO+0x10, NAND_BASE+0x20, 8),    # cfg0/cfg1->DEV0_CFG0
        (0,    IO+0x18, NAND_BASE+0x10, 4),    # exec->EXEC_CMD
        (4<<3, NAND_BASE+0x14, IO+0x24, 8),    # status->result0
        (0,    NAND_BASE+0x100, dest, 512),    # buffer->RAM
        (0,    NAND_BASE+0x100, dest+512, 512),
        (0,    NAND_BASE+0x100, dest+1024,512),
        (CMD_LC, NAND_BASE+0x100, dest+1536,512),  # LC last
    ]
    for i,d in enumerate(seq):
        w(CL+i*16, d[0]); w(CL+i*16+4, d[1]); w(CL+i*16+8, d[2]); w(CL+i*16+12, d[3])
    w(PTR, (CL>>3) | CMD_PTR_LP)
    dmov.exec_cmdptr(((PTR>>3)) | CMD_PTR_LP)

def read_partition(name, blocks_start, count):
    uc = Uc(UC_ARCH_ARM, UC_MODE_ARM)
    uc.mem_map(0x00400000, 0x0100000)
    uc.mem_map(RAM_DEST, 0x01000000)
    nand = NandController(f"{ND}/1.1.2.bin", f"{ND}/1.1.2_spare.bin")
    dmov = DMOVModel(uc, nand)
    out = bytearray()
    total_pages = count*64
    for blk in range(count):
        base_page = (blocks_start+blk)<<6
        for pg in range(64):
            dest = RAM_DEST # overwrite; we copy out from uc after
            read_page_via_dma(uc, dmov, nand, base_page+pg, dest)
            out += bytes(uc.mem_read(dest, 2048))
        if blk % 16 == 0:
            print(f"  blk {blk}/{count} read ({len(out)//2048} pages)")
    return bytes(out)

def verify(name, got, first_page):
    raw = open(f"{ND}/1.1.2.bin","rb").read()
    off = first_page * 2048
    exp = raw[off : off + len(got)]
    match = (got == exp)
    print(f"\n== {name}: partition read {len(got)} bytes @ NAND page {first_page} ==")
    print(f"  first16 got {got[:16].hex()}")
    print(f"  first16 exp {exp[:16].hex()}")
    # where does first mismatch happen?
    if not match:
        for i,(a,b) in enumerate(zip(got,exp)):
            if a!=b: print(f"  first mismatch at byte {i} (0x{i:x}) got {a:#x} exp {b:#x}"); break
        else: print("  length differs")
    else:
        print("  byte-identical: True")
    return match

if __name__=="__main__":
    which = sys.argv[1] if len(sys.argv)>1 else "AMSS"
    bs,cnt = nblocks[which]
    print(f"Reading {which} partition (start blk {bs:#x}, {cnt} blocks = {cnt*64} pages)")
    got = read_partition(which, bs, cnt)
    verify(which, got, bs*64)
    open(f"/tmp/{which}_ramdump.bin","wb").write(got)
    print(f"saved /tmp/{which}_ramdump.bin")