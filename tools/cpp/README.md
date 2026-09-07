# Zeebo LLE — C++23 debug harness + device models

Native (host x86_64) interactive LLE debugger for the Zeebo/MSM7201A, in C++23.
Runs a firmware image in Unicorn (ARM1176) and provides full CPU/memory/MMU
inspection plus the FUNCTIONAL device models ported from Python.

## Files
- `zeebo_harness.cpp`   — interactive REPL debug harness
- `zeebo_devices.h`     — C++23 port of NandController + DMOVModel (functional)
- `zeebo_devices_test.cpp` — self-test of the device models (6 checks)
- `Makefile`

## Build
    make            # builds zeebo_harness + zeebo_devices_test
    make test       # runs the device-model self-test

Linux deps: `libunicorn-dev`, `libcapstone-dev` (apt; g++-14+ for C++23).

## Run
    ./zeebo_harness <blob> <load_vaddr>
    e.g. ./zeebo_harness ../../firmware/openzeebo-zloader.bin 0xa00000
         ./zeebo_harness ../../nand/1.1.2_APPSBL.bin 0x00000000

## Commands
  help regs cpsr peek [n] poke pc sp sreg vtop mmio run [n] step [n]
  bp <addr> | cl  dis <addr> [cnt]  dump <addr> [len]  dev  file <addr> reset quit
  dev  — show DMOV exec count + NAND state (last_cmd, cfg0/cfg1, READ_ID)
Addr prefixes: '#' = loadbase+off ; 'f:' = file offset into loaded blob ;
hex (with optional 0x) = VA by default. `vtop` uses the real ARM11 MMU map.

## Device models (in zeebo_devices.h, wired into the harness)
- NandController: dump-backed EBI2 flash (PAGE_READ from 1.1.2.bin 2048B/page,
  FETCH_ID=0x5580b1ad, CFG0/CFG1 real defaults, READ_ID/status).
- DMOVModel: executes nand.c descriptor lists (pointer list -> command list,
  16B {cmd,src,dst,len}; EXEC resets the FLASH_BUFFER drain cursor; CRCI-NAND
  routed; RAM copies). DMOV_CMD_PTR(NAND chan3) write triggers it.
These replicate the Python mini-boot (partitions AMSS/APPS byte-identical) inside
the interactive tool. The zloader `flash_read_config` runs via real DMA (dev shows
DMOV execs=1, cfg0=0xa25400c0, cfg1=0x4745e).