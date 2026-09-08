#!/usr/bin/env python3
import struct
from sig_scan import *
from capstone.arm import *

# TCB of the event-pump thread: loaded at 0x16ef0ac6 ldr r0,[pc,#0x34c]
pc=(0x16ef0ac6+4)&~3
tcb_ptr_addr=pc+0x34c
print("pump ldr literal addr=0x%08x val=0x%08x"%(tcb_ptr_addr,u32(off(tcb_ptr_addr))))
PUMP_TCB=u32(off(tcb_ptr_addr))

# All set_sigs veneers = words 0x16f80f05/04 that are code veneer entrypoints.
# Find pool refs to veneer entry 0x16f80f04 and 0x16f80f05 (thumb bit).
for tgt in [0x16f80f04,0x16f80f05]:
    print("pool refs to set_sigs veneer 0x%08x:"%tgt,[hex(va(j)) for j in find_word(tgt)])
# indirect blx via reg won't be caught by scan_bl_calls. Search pool refs to PUMP_TCB.
print("\npool refs to PUMP_TCB 0x%08x:"%PUMP_TCB,[hex(va(j)) for j in find_word(PUMP_TCB)])
# also the pointer stored there (the actual tcb struct address) 
inner=u32(off(PUMP_TCB)) if BASE<=PUMP_TCB<BASE+N else None
print("word at PUMP_TCB:",hex(inner) if inner else None)
