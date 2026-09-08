#!/usr/bin/env python3
# Cheap continuous-vs-stepped comparison on the SAME Unicorn frontend (NOT an independent CPU).
# Runs largest_aligned_fpage once continuously and once one-instruction-at-a-time; asserts identical r0.
import json,os,time
import fuzz_pathb as H
from unicorn import *
from unicorn.arm_const import *
def stepped_largest(addr,lo,hi,pageinfo,max_insns=40000):
    uc=H.mk_uc()
    ga=0xb000d468+8+0x54
    uc.mem_write(ga, (H.GLOBAL_R4).to_bytes(4,'little'))
    uc.mem_map(H.GLOBAL_R4 & ~0xfff,0x2000); uc.mem_write(H.GLOBAL_R4,b'\0\0\0\0')
    KIP=0x00220000; uc.mem_map(KIP,0x1000); uc.mem_write(KIP+0xc8,pageinfo.to_bytes(4,'little'))
    def hc(uc,a,s,u):
        if a==0xb000c720: uc.reg_write(UC_ARM_REG_R0,KIP); uc.reg_write(UC_ARM_REG_PC,uc.reg_read(UC_ARM_REG_LR))
    uc.hook_add(UC_HOOK_CODE,hc,begin=0xb000c720,end=0xb000c721)
    sp=H.STACK_BASE+H.STACK_SIZE-0x100
    uc.reg_write(UC_ARM_REG_SP,sp); uc.reg_write(UC_ARM_REG_LR,H.RET_MAGIC)
    uc.reg_write(UC_ARM_REG_R0,addr);uc.reg_write(UC_ARM_REG_R1,lo);uc.reg_write(UC_ARM_REG_R2,hi)
    pc=0xb000d4dc; n=0
    while pc!=H.RET_MAGIC and n<max_insns:
        uc.emu_start(pc,H.RET_MAGIC,count=1); pc=uc.reg_read(UC_ARM_REG_PC); n+=1
    return uc.reg_read(UC_ARM_REG_R0)&0xffffffff, n
cases=[(0xb0d00000,0xb0d00000,0xb6d00000),(0x10000000,0x10000000,0x10100000),
       (0xb0d00001,0xb0d00000,0xb6d00000),(0xffff0000,0xffff0000,0xffffffff)]
res=[]; t0=time.time()
for a,l,h in cases:
    cont=H.run_largest_fpage(a,l,h,0x01111006)
    st,ninsn=stepped_largest(a,l,h,0x01111006)
    match=(cont[0]=='OK' and cont[1]==st)
    res.append({'addr':hex(a),'lo':hex(l),'hi':hex(h),'continuous':hex(cont[1]) if cont[1] is not None else None,
                'stepped':hex(st),'insns_stepped':ninsn,'match':match})
out={'note':'same Unicorn frontend; stepped != independent second CPU. Verifies continuous vs count=1 stepping determinism only.',
     'cases':res,'all_match':all(x['match'] for x in res),'elapsed_s':round(time.time()-t0,2)}
open(os.path.join(H.OUT,'continuous_vs_stepped.json'),'w').write(json.dumps(out,indent=2))
print(json.dumps(out,indent=2))
