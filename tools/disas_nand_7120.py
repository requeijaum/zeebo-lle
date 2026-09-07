#!/usr/bin/env python3
import capstone, struct
d=open("/home/rafaelfrequiao/projects/zeebo-lle/nand/1.1.2_APPSBL.bin","rb").read()
md=capstone.Cs(capstone.CS_ARCH_ARM,capstone.CS_MODE_ARM)
print("== 0x7120 (NAND flash init/read) ==")
for ins in md.disasm(d[0x7120:0x71a0],0x7120):
    print("  %04x  %-8s %s"%(ins.address,ins.mnemonic,ins.op_str))
print("\n== bl 0x7120 call sites ==")
calls=[]
for i in range(0,len(d)-4,2):
    w=struct.unpack_from("<I",d,i)[0]
    if (w & 0xff000000)==0xeb000000:
        imm=w & 0x00ffffff
        if imm & 0x800000: imm -= 0x1000000
        tgt=(i+8+(imm<<2)) & 0xffffffff
        if tgt==0x7120: calls.append(i)
print([hex(c) for c in calls])
print("\n== code around first call site ==")
if calls:
    st=calls[0]-8
    for ins in md.disasm(d[st:st+32],st):
        print("  %04x  %-8s %s"%(ins.address,ins.mnemonic,ins.op_str))