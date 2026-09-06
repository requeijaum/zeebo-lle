# Zeebo LLE Emulator — ROADMAP

Low-level emulation of the Zeebo: boot the REAL firmware from the NAND dump on an
emulated Qualcomm MSM7201A (ARM11 apps core), no HLE of BREW. Decided by Rafael
2026-09-06 despite the emulation ROADMAP marking HLE-LLE "OUT" — accepting the
closed-hardware RE cost, now sharply reduced by the discovery below.

## GAME-CHANGER: we have the real bring-up source
`~/projects/zeebo/research/openzeebo-repo/tools/zloader/arch_msm7k/` is the
OpenZeebo/Google little-kernel port for THIS SoC — real, compilable C with the
authoritative MSM7201A register map (headers under `.../zloader/include/msm7k/`).
This converts most peripheral RE from guesswork into transcription. Peripheral
bases (verified from the headers, and cross-checked against our probe):

| Base         | Block | Header | Probe hit |
|--------------|-------|--------|-----------|
| 0xC0000000   | VIC (interrupt controller)      | vic.h   | yes (init) |
| 0xC0100000   | GPT/DGT (general-purpose timer)  | gpt.h   | yes (was mislabeled "clock/PLL") |
| 0xA9700000   | ADM/DMOV (DMA controller)        | dmov.h  | yes (24 "channels" = DMA chans, was mislabeled "clock-gate") |
| 0xA9000000/0xA9200000 | GPIO1                   | gpio.h  | yes (pinmux) |
| 0xAA600000   | MDDI (display serial link)       | mddi.h  | yes (was mislabeled "UART") |
| 0xA8600000   | CLK_CTL (clock control)          | clock.c | not yet reached |
| 0xA9A00000   | UART1 (debug console)            | uart.h  | not yet reached |
| 0xA0A00000   | NAND controller (EBI2)           | nand.h  | not yet reached |
| 0xAA200000   | MDP (display processor)          | mdp.h   | not yet reached |
| 0xA0800000   | HSUSB                            | hsusb.h | not yet reached |

CORRECTION to session-1 notes: the standalone APPSBL did NOT derail at a NAND
hand-off. It ran its peripheral init (VIC/GPT/DMOV/GPIO/MDDI) then fell through
into a zeroed/0xFF data region around 0xf000-0x5fef4 — i.e. it ran off the end of
the routine it was in because a called sub-init returned into uninitialized RAM,
NOT because it jumped to a NAND-loaded stage. Re-diagnose with the real map.

## SoC facts (datasheet + DevGuide, [CONF])
- Apps: ARM1136J-S (ARMv6) @528MHz + ARM9 @256MHz baseband (never runs games).
- QDSP4000+QDSP5000; Adreno 130 (Q3Dimension, GLES1.1, tile/binning, ring-buffer
  command stream in EBI/SMI; 1.6M tri/s Zeebo-specific).
- RAM: 32MB SMI @0x00000000 + 128MB EBI1 @0x10000000. NAND 128MB (system) page
  2048B/64pp/spare64B/ID 0x5580b1ad + 1GB eNAND (games).
- Peripheral-port remap: `MCR p15,0,r0,c15,c2,4` = 0x80000016 (ARM1136 TRM).
- MPU enables: NAND MPU 0xa0b00000+0x0, periph MPU 0xa0e00000+0x400,
  0xa8240000/0xa8250000+0x800.

## Strategy: staged, each stage a runnable milestone
Keep the Unicorn harness as the fast iteration core (QEMU board later, only if we
need real IRQ/DMA timing). Model peripherals from arch_msm7k, not from guesses.

### Phase 0 — DONE (session 1)
APPSBL peripheral-init runs with no error halt; 41 registers modeled empirically;
harness + register discovery loop proven.

### Phase 1 — Correct device models from arch_msm7k  [NEXT]
Rewrite the probe's MMIO handler as a small dispatch of real device models:
- VIC: mask/clear registers per vic.h (init-only, no behavior needed yet).
- GPT/DGT: a free-running COUNT_VAL that increments; delay loops that poll it must
  see it advance (return an incrementing counter, not sticky). This is likely why
  session-1 delay loops burned 54M instructions — a real counter lets them exit.
- DMOV: model command-pointer execution enough that flash_read/dmov_exec_cmdptr
  see RSLT_VALID/DONE. This is the gateway to NAND (NAND goes through ADM DMA).
- GPIO/MDDI/CLK_CTL: sticky config, sane status bits.
Goal: APPSBL reaches its main flow (load next stage) deliberately, not by
falling off the end. Success metric = it issues NAND controller reads at
0xA0A00000.

### Phase 2 — NAND controller + flash-backed reads
Model 0xA0A00000 per nand.h (FLASH_CMD/ADDR/EXEC/STATUS/BUFFER + READ_ID) and the
ADM DMA path nand.c uses. Back it with 1.1.2.bin (+ spare for ECC). READ_ID must
return maker/device matching 0x5580b1ad. Let APPSBL actually READ pages.

### Phase 3 — Stage hand-off
Let APPSBL load and jump to the next stage (AMSS/APPS). We already have the ELF
phdrs (APPS entry 0x10000000, AMSS entry 0x00a00000). Watch the next register
wave the loaded stage touches.

### Phase 4 — The two big undocumented blocks
- ARM9 modem (AMSS): ONCRPC / PROC_COMM shared-memory RPC between cores. BREW
  expects the modem alive. Option: stub PROC_COMM responses (HLE-of-modem inside
  an otherwise-LLE apps core) rather than emulate the ARM9 — decide at Phase 3.
- Adreno 130: tile-based renderer + ring-buffer command stream. Biggest GPU RE.
  Likely the point where a pure-LLE payoff must be weighed against redirecting GS
  command stream to host GLES (a hybrid).

## Honest risk statement
Phases 1-3 are now tractable because arch_msm7k hands us the register map and the
NAND/DMA sequences. Phase 4 (modem RPC + Adreno) remains the true unknown and may
take the project from "boots firmware" to "runs a game" only via hybrid HLE at the
modem and GPU boundaries. The staged plan surfaces that decision at Phase 3 with
data, instead of committing to full-LLE-or-bust up front.

## Clean-room note
arch_msm7k is BSD/Apache (Google little-kernel derivative) — usable as a
reference and even portable into an emulator with attribution. It is NOT the
game/BREW code, so it does not touch the BREW clean-room boundary. a1Sim stays
black-box-only and is irrelevant to LLE (x86/HLE).
