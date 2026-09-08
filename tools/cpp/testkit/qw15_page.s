@ qw15_page.s -- Zeebo LLE ARM11 (ARMv6) cross-page transactional vector QW15.
@ Locally authored (clean-room): NOT derived from any GPL/proprietary suite.
@
@ Purpose: pin the REAL bus behavior of data transactions that touch BOTH sides
@ of a 4KiB page boundary (0x00102000), plus ARM<->Thumb interwork. We exercise
@ address / width / order / value across the boundary and record the ordered
@ transaction trace + full INIT/FINAL CPSR.
@
@ ARCHITECTURAL HONESTY: the ARM1176 here runs with SCTLR.A=0 (alignment fault
@ OFF, the reset/probe default -- see INIT cpsr=0x00000013). Under that config
@ an unaligned word access that straddles a page boundary is NOT trapped by the
@ core; it is serviced as a single flat access across the contiguous mapping.
@ We therefore PIN a genuine crossing transaction, not a trap. If A=1 were set
@ this would fault instead; that is a different vector and is not claimed here.
@
@ Boundary layout (scratch straddling the 4KiB line at 0x00102000):
@   0x00101FFE : word  store 0xAABBCCDD  -> DD@1FFE CC@1FFF | BB@2000 AA@2001
@   read back word @0x00101FFE (crossing) -> r0 = 0xAABBCCDD
@   read word @0x00102000 (high side, aligned on boundary) -> r1 = 0x0000AABB
@   0x00101FFF : byte  store 0x55  (last byte of the low page)
@   0x00102000 : byte  store 0x66  (first byte of the high page)
@   0x00101FFF : half  store 0x7788 (straddles: 88@1FFF | 77@2000)
@   thumb: read half @0x00101FFF (crossing) -> r2 = 0x00007788
@   thumb: read byte @0x00102000 -> r3 = 0x00000077 (high byte of the straddle)
@   back to ARM, final spin.

.arm
.global _start
_start:
    ldr  r4, =0x00101FFE        @ low-side straddle base (literal: data read)
    ldr  r5, =0xAABBCCDD        @ value (literal: data read)
    str  r5, [r4]               @ S word  0xAABBCCDD @ 0x00101FFE (CROSSES 0x2000)
    ldr  r0, [r4]               @ L word  0xAABBCCDD -> r0 (CROSSES 0x2000)
    ldr  r6, =0x00102000        @ boundary base (aligned)
    ldr  r1, [r6]               @ L word  0x0000AABB -> r1 (high side of straddle)
    mov  r7, #0x55
    strb r7, [r4, #1]           @ S byte  0x55 @ 0x00101FFF (last byte low page)
    mov  r7, #0x66
    strb r7, [r6]               @ S byte  0x66 @ 0x00102000 (first byte high page)
    ldr  r7, =0x00007788
    strh r7, [r4, #1]           @ S half  0x7788 @ 0x00101FFF (CROSSES 0x2000)
    add  r5, r4, #1             @ r5 = 0x00101FFF (straddle base for Thumb load)
    adr  r3, thumb_fn + 1       @ Thumb entry (bit0=1)
    bx   r3                     @ interwork -> Thumb

.align 2
.thumb
thumb_fn:
    ldrh r2, [r5]              @ L half  0x7788 -> r2 (CROSSES 0x2000, unaligned)
    ldrb r3, [r6]              @ L byte  0x77 -> r3 (from 0x00102000)
    bx   pc                     @ back to ARM (aligned)
    nop

.align 2
.arm
after:
    nop
spin:
    b    spin
.ltorg
