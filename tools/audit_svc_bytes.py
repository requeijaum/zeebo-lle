#!/usr/bin/env python3
# Audit: what instructions live at the PCs the LLE claims as "Iguana user-space"
# and at the ARM9 linear-advance PCs. Golden rule: verify svc bytes / real insns.
import struct, sys
sys.path.insert(0, 'tools')
from capstone import Cs, CS_ARCH_ARM, CS_MODE_ARM, CS_MODE_THUMB

def load_apps_seg(vaddr, n=16):
    # APPS ELF loaded at its p_vaddr; read raw from the split file by phdr
    f = open('nand/1.1.2_APPS.bin','rb').read()
    e_phoff = struct.unpack('<I', f[0x1c:0x20])[0]
    e_phnum = struct.unpack('<H', f[0x2c:0x2e])[0]
    for i in range(e_phnum):
        ph = e_phoff + i*32
        p_type,p_off,p_va,p_pa,p_fsz,p_msz = struct.unpack('<IIIIII', f[ph:ph+24])
        if p_type==1 and p_va <= vaddr < p_va+p_msz:
            off = p_off + (vaddr - p_va)
            if vaddr - p_va < p_fsz:
                return f[off:off+n], p_va, p_fsz, p_msz, (vaddr-p_va)
            else:
                return b'\x00'*n, p_va, p_fsz, p_msz, (vaddr-p_va)  # in .bss (zero)
    return None, None, None, None, None

for va in (0xb0000028, 0xb000002c, 0xb0000030, 0x1013a000):
    data, pva, fsz, msz, delta = load_apps_seg(va)
    if data is None:
        print(f"{va:#010x}: NOT in any PT_LOAD segment -> unmapped/zero in real HW")
        continue
    inbss = delta >= fsz
    md = Cs(CS_ARCH_ARM, CS_MODE_ARM)
    dis = list(md.disasm(data, va))
    txt = f"{dis[0].mnemonic} {dis[0].op_str}" if dis else "(undisasm)"
    print(f"{va:#010x}: seg_va={pva:#x} filesz={fsz:#x} {'[BSS/zero-fill]' if inbss else '[file-backed]'} bytes={data[:4].hex()} -> {txt}")
