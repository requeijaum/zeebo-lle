#!/usr/bin/env python3
import capstone, struct, sys
data = open('nand/1.1.2_APPS.bin','rb').read()
assert data[:4]==b'\x7fELF'
e_phoff=struct.unpack_from('<I',data,0x1c)[0]
e_phentsize=struct.unpack_from('<H',data,0x2a)[0]
e_phnum=struct.unpack_from('<H',data,0x2c)[0]
segs=[]
for i in range(e_phnum):
    off=e_phoff+i*e_phentsize
    p_type,p_offset,p_vaddr,p_paddr,p_filesz,p_memsz,p_flags,p_align=struct.unpack_from('<IIIIIIII',data,off)
    if p_type==1:
        segs.append((p_offset,p_vaddr,p_filesz,p_memsz,p_flags))
        print("PT_LOAD off=%08x vaddr=%08x filesz=%08x memsz=%08x flags=%d"%(p_offset,p_vaddr,p_filesz,p_memsz,p_flags))

def va2off(va):
    for o,v,fs,ms,fl in segs:
        if v<=va<v+fs:
            return o+(va-v)
    return None

md=capstone.Cs(capstone.CS_ARCH_ARM,capstone.CS_MODE_ARM)
md.detail=False

def dis(va,n=40,label=""):
    off=va2off(va)
    print("\n=== disasm %s @ %08x (off %s) ==="%(label,va, hex(off) if off else "N/A"))
    if off is None: 
        print("  <unmapped>"); return
    code=data[off:off+n*4]
    for ins in md.disasm(code,va):
        print("  %08x  %s\t%s"%(ins.address,ins.mnemonic,ins.op_str))

for arg in sys.argv[1:]:
    parts=arg.split(':')
    va=int(parts[0],16); n=int(parts[1]) if len(parts)>1 else 40
    dis(va,n)
