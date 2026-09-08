@ zeebo_input_stub.s — HandleEvent Thumb mínimo p/ zeebo_input_harness.
@ Consome EVT_KEY_PRESS(0x100) e EVT_KEY_RELEASE(0x101), devolve r0=1;
@ qualquer outro evento devolve r0=0. Montar com:
@   arm-none-eabi-as -mthumb -mcpu=arm1176jz-s zeebo_input_stub.s -o stub.o
@   arm-none-eabi-objcopy -O binary stub.o stub.bin
@ Os bytes resultantes estão embutidos como kStub[] em zeebo_input_harness.cpp.
.syntax unified
.thumb
.global _start
_start:
  push {lr}
  movs r3, #1
  lsls r3, r3, #8       @ r3 = 0x100 (EVT_KEY_PRESS)
  cmp  r1, r3
  beq  .Lhit
  adds r3, r3, #1       @ r3 = 0x101 (EVT_KEY_RELEASE)
  cmp  r1, r3
  beq  .Lhit
  movs r0, #0           @ evento nao tratado
  pop  {pc}
.Lhit:
  movs r0, #1           @ TRUE: applet consumiu a tecla
  pop  {pc}
