#!/usr/bin/env python3
import capstone, struct, sys
data = open('nand/1.1.2_APPS.bin','rb').read()
e_phoff=struct.unpack_from('<I',data,0x1c)[0]
e_phnum=struct.unpack_from('<H',data,0x2c)[0]
e_phentsize=struct.unpack_from('<H',data,0x2a)[0]
segs=[]
for i in range(e_phnum):
    off=e_phoff+i*e_phentsize
    t,po,pv,pp,pf,pm,pfl,pa=struct.unpack_from('<IIIIIIII',data,off)
    if t==1: segs.append((po,pv,pf))
def va2off(va):
    for o,v,fs in segs:
        if v<=va<v+fs: return o+(va-v)
    return None
target=int(sys.argv[1],16)
tb=struct.pack('<I',target)
# find literal occurrences in b0 code seg
for o,v,fs in segs:
    if not (0xb0000000<=v<0xb0200000): continue
    idx=0
    seg=data[o:o+fs]
    while True:
        p=seg.find(tb,idx)
        if p<0: break
        print("literal %08x -> %08x"%(v+p,target))
        idx=p+1
