#!/bin/bash
# Build the openzeebo zloader with clang -target arm (no arm-none-eabi-gcc).
set -e
ZDIR="$HOME/projects/zeebo/research/openzeebo-repo/tools/zloader"
OUT=/tmp/zloader_build
rm -rf "$OUT"; mkdir -p "$OUT"
CC="clang -target armv6k-none-eabi -march=armv6 -mcpu=arm1136j-s -marm"
CFLAGS="-std=gnu99 -g -DARM9 -I$ZDIR/include -I$ZDIR -I$ZDIR/libc"
PATCHSIG="sig_r"   # PATCHNAME for sig patch (bootloader variant)

echo "== compile arch_armv6 (.s) =="
for f in irq dcc jtag misc; do
  $CC $CFLAGS -c "$ZDIR/arch_armv6/$f.S" -o "$OUT/${f}.o" 2>&1 | head -5 || echo "($f.S maybe arm only)"
done
echo "== compile arch_msm7k (.c) =="
for f in clock gpio mddi mddi_console nand smem ssbi uart vic hsusb shared; do
  [ -f "$ZDIR/arch_msm7k/$f.c" ] && \
    $CC $CFLAGS -c "$ZDIR/arch_msm7k/$f.c" -o "$OUT/${f}.o" 2>&1 | head -8 || true
done
echo "== compile libboot (.c) =="
for f in flash init poll tags tags_cmdline tags_partition tags_revision tags_serialno gpio_keypad; do
  [ -f "$ZDIR/libboot/$f.c" ] && \
    $CC $CFLAGS -c "$ZDIR/libboot/$f.c" -o "$OUT/${f}.o" 2>&1 | head -8 || true
done
echo "== compile libc =="
for f in sha; do
  [ -f "$ZDIR/libc/$f.c" ] && $CC $CFLAGS -c "$ZDIR/libc/$f.c" -o "$OUT/${f}.o" 2>&1 | head -8 || true
done
echo "== compile zloader (init.S + board.c + main.c + patch) =="
$CC $CFLAGS -c "$ZDIR/zloader/init.S" -o "$OUT/init.o" 2>&1 | head -10 || true
$CC $CFLAGS -c "$ZDIR/zloader/board.c" -o "$OUT/board.o" 2>&1 | head -10 || true
$CC $CFLAGS -c "$ZDIR/zloader/main.c" -o "$OUT/main.o" 2>&1 | head -10 || true
$CC $CFLAGS -PATCHNAME=$PATCHSIG -c "$ZDIR/zloader/patch/patch_${PATCHSIG}.c" -o "$OUT/patch.o" 2>&1 | head -10 || true

echo "== objects built =="
ls -la "$OUT"/*.o 2>/dev/null | awk '{print $NF" "$5}'