import struct, os
from unicorn import *
from unicorn.arm_const import *

PATH='/home/rafaelfrequiao/projects/zeebo-lle/nand/1.1.2_AMSS.bin'
data=open(PATH,'rb').read()
def rd32(b,o): return struct.unpack('<I',b[o:o+4])[0]
def rd16(b,o): return struct.unpack('<H',b[o:o+2])[0]

uc=Uc(UC_ARCH_ARM,UC_MODE_ARM)
uc.ctl_set_cpu_model(UC_CPU_ARM_1176)
for base,size in [(0x00000000,0x00800000),(0x00a00000,0x00600000),
                  (0x16e00000,0x17a60000-0x16e00000),(0x20000000,0x01000000),
                  (0xf0000000,0x01000000),(0xb0000000,0x01000000)]:
    uc.mem_map(base,size)

e_entry=rd32(data,24); phoff=rd32(data,28); phent=rd16(data,42); phnum=rd16(data,44)
entry_va=e_entry
entry_pa=e_entry
for i in range(phnum):
    o=phoff+i*phent
    if rd32(data,o)!=1: continue
    va=rd32(data,o+8); pa=rd32(data,o+12); off=rd32(data,o+4)
    fs=rd32(data,o+16); ms=rd32(data,o+20); nmem=ms or fs
    if not nmem: continue
    # write to VA
    try: uc.mem_write(va,data[off:off+fs])
    except UcError: pass
    # write to PA too
    if pa:
        try: uc.mem_write(pa,data[off:off+fs])
        except UcError: pass
    if pa and pa<=e_entry<pa+nmem:
        entry_va=va+(e_entry-pa)

uc.reg_write(UC_ARM_REG_CPSR,0xD3)
uc.reg_write(UC_ARM_REG_SP,0x00A197F8)

# seed region table
src=0x00a1d73c
def w32(a,v): uc.mem_write(a,struct.pack('<I',v))
w32(src+0x0,0x00a00000); w32(src+0x4,0x00c00000); w32(src+0x8,0x0000000f)
w32(src+0xc,0x00000000); w32(src+0x18,0x00000000)

START=int(os.environ.get('START','pa'),0) if os.environ.get('START','pa') not in ('pa','va') else (entry_pa if os.environ.get('START','pa')=='pa' else entry_va)
print('e_entry=%08x entry_va=%08x START=%08x'%(e_entry,entry_va,START))

state={'trail':[]}
def hook(uc,addr,size,ud):
    state['trail'].append(addr)
    if len(state['trail'])>16: state['trail'].pop(0)
    if addr in (0xf001773c,0xf0017740,0xf0017744,0xf0017748,0xf001774c,
                0x00a1773c,0x00a17740,0x00a17744,0x00a17748,0x00a1774c):
        r0=uc.reg_read(UC_ARM_REG_R0); r3=uc.reg_read(UC_ARM_REG_R3)
        r6=uc.reg_read(UC_ARM_REG_R6); sp=uc.reg_read(UC_ARM_REG_SP)
        print('  @%08x r0=%08x r3=%08x r6=%08x sp=%08x'%(addr,r0,r3,r6,sp))
    if addr==0xf0017890 or addr==0x00a17890:
        print('*** PANIC %08x *** trail:'%addr,[hex(x) for x in state['trail']]); uc.emu_stop()

uc.hook_add(UC_HOOK_CODE,hook)
def unmapped(uc,typ,addr,size,val,ud):
    print('UNMAPPED type=%d addr=%08x pc=%08x'%(typ,addr,uc.reg_read(UC_ARM_REG_PC)))
    return False
uc.hook_add(UC_HOOK_MEM_UNMAPPED,unmapped)
try:
    uc.emu_start(START,0,0,6000000)
except UcError as e:
    print('UcError',e,'pc=%08x'%uc.reg_read(UC_ARM_REG_PC))
print('done pc=%08x'%uc.reg_read(UC_ARM_REG_PC))
print('final trail:',[hex(x) for x in state['trail']])
