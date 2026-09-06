# Zeebo LLE — MSM7201A register map (empirical, from APPSBL boot)

Built by iteratively booting APPSBL under Unicorn and modeling each register
until the boot advances. Values are what the bootloader EXPECTS, deduced from the
code that consumes them (not from any datasheet — MSM7201A is undocumented).

## 0xc0000000 — Interrupt controller (VIC-like)
- +0x10 W 0          : disable/clear (INTENCLEAR?)
- +0x14 W 0          : disable/clear
- +0xb0 W 0xffffffff : mask all
- +0xb4 W 0xffffffff : mask all
Init only; no polling. Model: plain sticky registers.

## 0xc0100000 — Clock / PLL controller
- +0x100 : clock-source config. RMW sequence programs a value up in stages:
           0x640000 -> 0x640010 -> 0x64001f -> 0x64101f -> 0x64171f.
- +0x104 : handshake/status for +0x100. Boot writes 2, reads back 2, writes 3,
           reads back 3 (state machine). MUST reflect written value (sticky) or
           the PLL programming loop never converges.
- +0x008 : enable latch. Read 0 then write 1.
Model: sticky registers reflect writes -> boot passes clock setup cleanly.

## 0xa9700000 — Multi-channel device block (clock-gate / port controller?)
Per-channel stride 4 on the status regs; channels seen: 0 (e10/c50/f10),
1 (e14/c54). Per channel the boot:
  1. R +0xe10+ch*4  : status/ready. Layout (from code @0x16b8):
       bit0 = READY (must be 1), bit1 = ERROR (must be 0).
       -> correct return is 0x1, NOT 0xffffffff (all-ones trips the error bit).
  2. R +0xc50+ch*4  : secondary status (returns 0 = ok so far)
  3. W +0xf10+ch*4  : config/ack (writes 2)
Channel 0 passes; channel 1 (e14) currently returns 0 -> fails the bit0=ready
check -> boot still ends at halt 0xc30. Next: return 0x1 for the whole
0xa9700e10+ch*4 status range and re-probe.

## Halt
0xc30 = `b 0xc30` self-loop = the bootloader's error/abort sink. Reaching it
means some check failed. Each modeled register pushes the halt later; the goal is
to reach the point where APPSBL hands off to the next stage (loads/jumps to the
APPS or AMSS ELF entry).

## Boot progression (halt distance)
- v1 (all reads=0):        3 regs, dies immediately at first clock poll
- v2 (sticky + a9700e10=0x1): 12 regs, passes full clock setup + channel 0,
                              dies at a9700 channel 1 ready check
