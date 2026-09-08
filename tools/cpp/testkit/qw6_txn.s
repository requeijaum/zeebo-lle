@ qw6_txn.s -- Zeebo LLE ARM11 (ARMv6) transactional conformance vector QW6.
@ Locally authored (clean-room): NOT derived from any GPL test suite.
@
@ Purpose: exercise an ORDERED sequence of bus transactions -- instruction
@ prefetch (P), data load (L) and data store (S) -- with explicit width,
@ address and value, plus ARM<->Thumb interwork. A conformant core must
@ reproduce not only the final register/CPSR state but the exact ordered
@ transaction trace. Altering order/address/value/width must fail the test.
@
@ Load base is 0x00100000 (matches probe default). A 16-byte scratch region
@ at 0x00101000 receives the stores; loads read them back.
@
@ Register/memory contract (hand-computed, see qw6_txn.expected.txt):
@   r4 = scratch base 0x00101000
@   store word  0xAABBCCDD @ 0x00101000
@   store byte  0x11       @ 0x00101008
@   store half  0x2233     @ 0x0010100A
@   load  word  -> r0      (0xAABBCCDD)
@   load  byte  -> r1      (0x000000DD, from 0x00101000)
@   thumb: load half -> r2 (0x00002233 from 0x0010100A), store word r2 @0x0010100C
@   back to ARM, final spin.

.arm
.global _start
_start:
    ldr  r4, =0x00101000        @ scratch base (literal load: data read)
    ldr  r5, =0xAABBCCDD        @ value (literal load: data read)
    str  r5, [r4]               @ S word  0xAABBCCDD @ 0x00101000
    mov  r6, #0x11
    strb r6, [r4, #8]           @ S byte  0x11 @ 0x00101008
    ldr  r7, =0x00002233
    strh r7, [r4, #10]          @ S half  0x2233 @ 0x0010100A
    ldr  r0, [r4]               @ L word  0xAABBCCDD -> r0
    ldrb r1, [r4]               @ L byte  0xDD -> r1 (from 0x00101000)
    adr  r3, thumb_fn + 1       @ Thumb entry (bit0=1)
    bx   r3                     @ interwork -> Thumb

.align 2
.thumb
thumb_fn:
    ldrh r2, [r4, #10]          @ L half  0x2233 -> r2 (from 0x0010100A)
    str  r2, [r4, #12]          @ S word  0x00002233 @ 0x0010100C
    bx   pc                     @ back to ARM (aligned)
    nop

.align 2
.arm
after:
    nop
spin:
    b    spin
.ltorg
