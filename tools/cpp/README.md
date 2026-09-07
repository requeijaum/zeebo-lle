# zeebo_harness — interactive LLE debug harness (C++23)

Boots a Zeebo firmware image in Unicorn (ARM1176) with a REPL for full debug:
peek/poke of memory AND MMIO (sticky device model), full CPU state (r0-r15+cpsr
with mode decode), VA->PA translation (real ARM11 MMU map, built-in), single-step,
breakpoints, disassembly (Capstone), and dump.

Build:  make  (or g++ -std=c++23 -O2 zeebo_harness.cpp -lunicorn -lcapstone)
Run:    ./zeebo_harness <blob> <load_vaddr>
        e.g. ./zeebo_harness ../../nand/1.1.2_APPSBL.bin 0x00000000

Commands: help regs cpsr peek [n] poke pc sp sreg vtop mmio run [n] step [n]
          bp addr | cl | dis <addr> [n] | dump <addr> [len] | file <addr> | reset | quit
Addr prefixes: '#'=<loadbase>+off, 'f:'=<fileoff into blob>, else hex VA.
'&' = physical (identity). vtop uses the real ARM11 MMU map.

Note: MMIO model is a minimal sticky store (writes read back); the GPT counter /
DMOV DMA execution (functional model) are in the Python tools (dmov_model.py)
if needed for deeper boot. The harness is the CPU/memory/map inspection tool.
