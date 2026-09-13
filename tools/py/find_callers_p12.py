#!/usr/bin/env python3
# Acha callers (bl/b/blx) de um endereço alvo no 1.1.2_APPS.bin.
# CORRECOES (2026-09-08, auditoria com o fonte OKL4):
#  1. Filtro antigo `v >= 0x10000000` pulava TODO o kernel Iguana (0xb0000000 ~ 2.75GB
#     > 256MB) — sobrava zero segmentos e o script nunca achava nada. O correto e
#     pular apenas a faixa BREW/dados (0x10000000 <= v < 0xb0000000: o BREW vive em
#     0x10137000-0x14953000); o kernel/servers (0xb0000000-0xb0e00000) e o boot
#     (0xf0000000+) precisam ser varridos.
#  2. `op.imm` do capstone e int32 ASSINADO; enderecos >= 0x80000000 (todo o kernel)
#     viram negativos e a comparacao `== target` nunca casava. Mascarar com 0xffffffff.
import capstone, struct, sys
data = open('nand/1.1.2_APPS.bin','rb').read()
e_phoff=struct.unpack_from('<I',data,0x1c)[0]
e_phnum=struct.unpack_from('<H',data,0x2c)[0]
e_phentsize=struct.unpack_from('<H',data,0x2a)[0]
segs=[]
for i in range(e_phnum):
    off=e_phoff+i*e_phentsize
    t,po,pv,pp,pf,pm,pfl,pa=struct.unpack_from('<IIIIIIII',data,off)
    if t==1 and (pfl & 1) and not (0x10000000 <= pv < 0xb0000000):
        segs.append((po,pv,pf,pfl))
md=capstone.Cs(capstone.CS_ARCH_ARM,capstone.CS_MODE_ARM)
md.detail=True
target=int(sys.argv[1],16)
found = 0
for o,v,fs,fl in segs:
    for ins in md.disasm(data[o:o+fs],v):
        if ins.mnemonic in ('bl','b','blx'):
            for op in ins.operands:
                if op.type==capstone.arm.ARM_OP_IMM and (op.imm & 0xffffffff)==target:
                    print("caller %08x seg_v=%08x %s %s"%(ins.address,v,ins.mnemonic,ins.op_str))
                    found += 1
print("# total: %d caller(s)" % found)
