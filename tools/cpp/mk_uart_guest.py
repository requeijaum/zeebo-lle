#!/usr/bin/env python3
# Minimal ARM guest that writes 'Z' to UART1 TF (0xa9a0000c) then spins.
import struct
ins = [
    0xE3A0005A,   # mov r0, #0x5A ('Z')
    0xE59F1004,   # ldr r1, [pc, #4]   => loads literal at pc+4+8 = pc+0x0c? fix: [pc,#4] reads pc+8+4
    0xE5810000,   # str r0, [r1]
    0xEAFFFFFE,   # b .
    0x00000000,   # placeholder literal
    0xA9A0000C,   # literal = UART TF (must be at pc+4+8 for the ldr)
]
# ldr r1,[pc,#4]: address = (pc_of_ldr+8) + 4. ldr is at offset 4, pc_of=4, so addr=4+8+4=16 = offset 0x10.
# put the literal at offset 0x10.
ins[4] = 0xA9A0000C
d = b"".join(struct.pack("<I", i) for i in ins)
open("/tmp/uart_guest.bin", "wb").write(d)
print("wrote", len(d), "bytes; code: mov r0,'Z'; str to UART TF@0xa9a0000c via literal@0x10; spin")