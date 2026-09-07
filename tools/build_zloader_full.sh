#!/bin/bash
# Full build+link of openzeebo zloader with clang-arm target.
set -e
ZDIR="$HOME/projects/zeebo/research/openzeebo-repo/tools/zloader"
OUT=/tmp/zloader_build
rm -rf "$OUT"; mkdir -p "$OUT"
CC="clang -target armv6k-none-eabi -march=armv6 -mcpu=arm1136j-s -marm"
CFLAGS="-std=gnu99 -g -DARM9 -DDEBUG -DPATCHNAME=sig_r -I$ZDIR/include -I$ZDIR -I$ZDIR/libc"

echo "== arch_armv6 =="
for f in irq jtag misc; do
  $CC $CFLAGS -c "$ZDIR/arch_armv6/$f.S" -o "$OUT/${f}.o" 2>&1 | grep -i error | head -3 || true
done
echo "== arch_msm7k =="
for f in clock gpio mddi mddi_console nand smem ssbi uart vic hsusb shared; do
  [ -f "$ZDIR/arch_msm7k/$f.c" ] && { $CC $CFLAGS -c "$ZDIR/arch_msm7k/$f.c" -o "$OUT/${f}.o" 2>&1 | grep -i error | head -3 || true; }
done
echo "== libboot =="
for f in flash init poll tags tags_cmdline tags_partition tags_revision tags_serialno gpio_keypad; do
  [ -f "$ZDIR/libboot/$f.c" ] && { $CC $CFLAGS -c "$ZDIR/libboot/$f.c" -o "$OUT/${f}.o" 2>&1 | grep -i error | head -3 || true; }
done
echo "== libc =="
for f in sha; do
  [ -f "$ZDIR/libc/$f.c" ] && { $CC $CFLAGS -c "$ZDIR/libc/$f.c" -o "$OUT/${f}.o" 2>&1 | grep -i error | head -3 || true; }
done
echo "== zloader + patch =="
$CC $CFLAGS -c "$ZDIR/zloader/init.S" -o "$OUT/init.o" 2>&1 | grep -i error | head -3 || true
$CC $CFLAGS -c "$ZDIR/zloader/board.c" -o "$OUT/board.o" 2>&1 | grep -i error | head -3 || true
$CC $CFLAGS -c "$ZDIR/zloader/main.c" -o "$OUT/main.o" 2>&1 | grep -i error | head -5 || true
$CC $CFLAGS -c "$ZDIR/zloader/patch/patch_sig_r.c" -o "$OUT/patch.o" 2>&1 | grep -i error | head -3 || true

echo "== link =="
# write a minimal linker script for 0x00a00000 (reuse their boot.ld concepts)
cat > "$OUT/boot.ld" <<'EOF'
ENTRY(_start)
SECTIONS {
  . = 0x00a00000;
  .text : { *(.text) *(.text.*) } =0
  .rodata : { *(.rodata .rodata.*) }
  .data : { *(.data .data.*) }
  .bss : { *(.bss .bss.*) *(COMMON) }
}
EOF
OBJS=$(find "$OUT" -name '*.o' ! -name 'boot.ld' | tr '\n' ' ')
clang -target armv6k-none-eabi -march=armv6 -marm -nostdlib -Wl,-T,"$OUT/boot.ld" -Wl,--gc-sections -e _start \
  $OBJS -o "$OUT/zloader.elf" 2>&1 | grep -iE 'error|undefined|cannot' | head -20 || true
echo "== result =="
ls -la "$OUT/zloader.elf" 2>/dev/null && llvm-readelf -h "$OUT/zloader.elf" 2>/dev/null | grep -iE 'entry|machine|flags' && llvm-objdump -d "$OUT/zloader.elf" 2>/dev/null | head -5 || echo "no ELF"