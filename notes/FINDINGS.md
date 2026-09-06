# Zeebo LLE — Findings Log

## Goal
Low-level emulation of the Zeebo (Qualcomm MSM7201A: ARM11 apps + ARM9 modem,
Adreno 130, BREW 4.0.2) by booting the REAL firmware from the NAND dump under a
full-SoC emulator (QEMU board or Unicorn-based core), no HLE of the BREW API.

Decision: Rafael chose to build the LLE project from scratch, accepting the cost
of reverse-engineering closed MSM7201A hardware (2026-09-06).

## Ground truth assets (local, verified)
- NAND copy at `~/projects/zeebo-lle/nand/` (sha256-identical to the original in
  zeebo-emulator/research/docs/nand-dump/, which is now chmod a-w read-only).
  Blobs: 1.1.2.bin (128MB full), 1.1.2_spare.bin (ECC), and the three split
  ELF/bootloader images below.
- Firmware 1.1.2. a1Sim is NOT used here — it is x86/HLE and irrelevant to LLE.

## Boot-chain structure (verified via ELF phdrs + capstone)
- **APPSBL** (393216 B): ARM11 bootloader. Starts with the classic 8-entry ARM
  vector table (`ldr pc,[pc,#0x18]` ×8). Handler targets:
  reset=0x8f8 undef=0x9cc swi=0x1028 pabt=0x9d4 dabt=0x9dc irq=0xcac fiq=0xc38.
  Reset handler sets FIQ/SVC modes (cpsr 0xd1/0xd3), SP=0x100000, `bl 0x6bc`.
- **AMSS** (ELF, EM_ARM): entry 0x00a00000 (matches zloader load addr from KB).
  18 LOAD segs; huge segments at 0x00b1a000 (9.7MB), 0x16e00000 (7.5MB) = modem.
- **APPS** (ELF, EM_ARM): entry 0x10000000. 14 LOAD segs; f0000000 = low code,
  b0xxxxxx = BREW/AEE region, 0x1013a000 (19.7MB) + 0x1140c000 = app/asset heap.

## First LLE probe result (tools/peripheral_probe.py, Unicorn ARMv6/ARM1176)
Booted APPSBL from reset with ONLY RAM mapped; trapped all peripheral access.
- Touched only **3 MMIO regs** before halting:
  - `0xc0000010` W=0  @pc 0xb6c  (early init)
  - `0xc0100100` R    @pc 0xc384 (a clock/PLL-ish reg: code does read, uxth,
    orr #0x640000, write-back — a read-modify-write of a control reg)
  - `0xa9700e10` R    @pc 0x169c (0xa9xxxxxx = Qualcomm periph band; UART/clock
    family per KB: UART1 base 0xa9a00000, so 0xa970 is a sibling block)
- Then the CPU falls into `b 0xc30` — an **infinite halt/trap**, reached at the
  end of a word-copy routine (0xc08-0xc2c). Interpretation: the bootloader took
  an ERROR/abort path because a stubbed MMIO read returned 0 instead of a real
  hardware value. This is expected: with no device models, the boot cannot pass
  its first hardware sanity check.

## What this measures (the point of the probe)
The "hole" to fill for a working LLE is not yet even visible — the boot dies at
the FIRST three peripheral touches. To get past 0xc30 we must model, at minimum,
the clock/PLL block at 0xc010xxxx and the 0xa970xxxx block with plausible return
values, then re-probe to reveal the NEXT wave of registers. Each iteration
uncovers more of the (undocumented) MSM7201A register map. This is the honest
shape of the LLE effort: an iterative RE loop, one register wall at a time.

## Boot progression (measured, halt pushed later each iteration)
- v1 (all MMIO reads=0):          3 regs -> dies at first clock poll (0xc30)
- v2 (sticky regs + a9700 ready): 12 regs -> passes clock, dies at a9700 ch1
- v2 (a9700 status band=ready):   41 regs -> passes ALL a9700 channels (24 regs
  = clock-gate init), reaches TWO new blocks:
    * 0xaa600000 : rich config seq (0x400/0x3c00/0xa8500/0x60006...) + poll
      +0x028 -> likely EBI/SDRAM memory controller or a UART. Boot now polls
      0xaa600028 for a status bit.
    * 0xa9200808 : GPIO/pinmux (writes 0x100000 = pin set).
  Register map so far by region:
    0xc0000000(4) interrupt ctrl · 0xc0100000(3) clock/PLL ·
    0xa9700000(24) clock-gate channels · 0xa9200000(1) GPIO ·
    0xaa600000(9) mem-ctrl/UART

## Honest assessment of the loop
Each ~5-min RE iteration (model a register from the code that consumes it,
re-probe, watch the halt move) uncovers one more hardware block. This is the
core LLE grind and it is WORKING — we went 3 -> 41 registers in three iterations.
The backlog ahead is large but now concrete and ordered: memory controller,
GPIO, then whatever the APPSBL touches to load+jump the AMSS/APPS ELF. Only after
APPSBL hands off do we hit the modem (ARM9) and the Adreno 130 GPU — the two
biggest undocumented blocks.

## MILESTONE — APPSBL phase-1 hardware bring-up COMPLETE (2026-09-06)
With the device model (sticky config regs + a9700/aa600028 status = ready) the
APPSBL runs its ENTIRE peripheral-init phase with no error halt:
  interrupt ctrl -> clock/PLL programming -> 24 clock-gate channels -> GPIO ->
  UART (config + status poll) -> ~54.6M instructions of init/delay loops.
Then execution derails into a zero/0xFF region (traced: last real branch target
is a zeroed data area at ~0xfa74, then it plows through NOP-like 0x00 words).

ROOT CAUSE (structural, not a model bug): after peripheral init the APPSBL LOADS
THE NEXT STAGE FROM NAND and jumps to it. Our standalone probe (a) does not model
the NAND controller (base 0xa0a00000 per KB) so the read returns 0, and (b) never
placed AMSS/APPS in RAM, so the jump target is empty. The bootloader cannot
proceed past hand-off without those two pieces. This is the natural ceiling of
the "run APPSBL alone" approach — and the exact next milestone.

## SESSION 2 — root cause of the "fall off the end", corrected (2026-09-06)
Armed with the arch_msm7k register map, re-diagnosed the standalone APPSBL end.
Session-1's "NAND hand-off" guess was WRONG. Real sequence:
1. Peripheral init REAL and complete: VIC(0xC0000000)+GPT(0xC0100000)+
   DMOV/ADM(0xA9700000 — the "24 channels" are DMA channels, NOT clock-gates)+
   GPIO1(0xA9200000)+MDDI(0xAA600000, NOT UART). 41 MMIO regs, no error halt.
2. A calibrated software udelay at 0x8e0-0x8f0 (r0=r0*11>>5; subs;bgt) burned ALL
   54M instructions. Forcing r0=1 at 0x8ec cut the run to 2.4M insns with the
   IDENTICAL endpoint -> the delay masks nothing; emulator must special-case it.
3. True transition: after last MDDI write (pc 0x8d78), boot returns and calls
   0x730 = CP15 MMU bring-up: TTBR(c2,c0,0)=0x00028000 (page table built by
   bl 0x142c), SCTLR(c1,c0,0)|=1 MMU ENABLE at 0x77c (SCTLR literal 0x00c50070),
   peripheral-port remap mcr c15,c2,4.
4. After MMU-on the boot jumps to its virtual entry, which should hold the NEXT
   stage (NAND-loaded) we never populated -> execution slides through zeroed RAM.
CONCLUSION: real boundary = MMU/TTBR + NAND-loaded next stage (ROADMAP Phase2/3),
now with exact CP15 values. Standalone-APPSBL RE essentially DONE.
Constants: TTBR=0x28000, SCTLR=0x00c50070, udelay@0x8e0, mmu@0x730, ptbuild@0x142c.

## SESSION 2b — Phase 2 NAND controller DONE + MMU boundary characterized
MMU dead-end (do not re-chase): the standalone APPSBL does ZERO writes to the
page table at TTBR=0x28000 — it assumes a prior PBL/boot-ROM built it AND that
the next stage is already NAND-loaded. Unicorn doesn't model ARMv6 FCSE/PID, so
chasing MMU-on semantics in Unicorn is a blind alley. The real unblock is the
NAND path + loading the next stage, then re-evaluating on a QEMU board.

NAND controller model DELIVERED: tools/nand_controller.py (0xA0A00000), a
behavioral EBI2 controller transcribed from zloader nand.h, backed by the dump.
Self-tests (run `python3 tools/nand_controller.py`) PASS:
- geometry verified from the real images: 1.1.2.bin = 65536 pages @2048;
  1.1.2_spare.bin = 65536 pages @2112 (2048 data + 64 spare).
- FETCH_ID returns the real 0x5580b1ad.
- PAGE_READ returns dump bytes verbatim (page 0 head matches).
- APPSBL is page-aligned inside NAND at 0x16e0000 (page 11712), first word
  18f09fe5 (ARM vector table). (Offsets 0x199c2c/0x1f9c2c are unaligned false
  hits — ignore.)
Next: wire NandController into the probe as the 0xA0A00000 handler and let a
NAND-aware boot flow (with the DMA/ADM command path from nand.c) actually read
pages; then decide QEMU board vs continue Unicorn for the MMU/stage hand-off.

## Next milestone (bigger piece of work)
1. Model the NAND controller at 0xa0a00000 (+ MPU 0xa0b00000): page 2048B,
   64 pages/block, spare 64B, ID 0x5580b1ad (all in KB). Feed it from 1.1.2.bin.
2. Load AMSS (entry 0x00a00000) and/or APPS (entry 0x10000000) ELF segments into
   RAM at their phdr vaddrs so the hand-off jump lands on real code.
3. Re-probe: watch the NEXT wave of registers the loaded stage touches. Only
   after this do we reach the modem (ARM9 / ONCRPC-PROC_COMM) and Adreno 130.

## Register map (final for this session): see docs/register-map.md
41 MSM7201A registers modeled across 5 blocks; error halt eliminated.
1. Decode `bl 0x6bc` (pre-MMIO early init) and the RMW at 0xc384 / read at 0x169c
   to guess what each reg represents (status vs config vs ID).
2. Give those regs sticky "sane" return values, re-run, and watch how much
   further the boot gets — building the register map empirically.
3. Only once the register set stabilizes, decide QEMU board vs staying on the
   Unicorn harness (Unicorn is faster to iterate for pure register discovery;
   QEMU matters later for interrupts/DMA/timing fidelity).
