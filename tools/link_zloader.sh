#!/bin/bash
# Link zloader using PREBUILT .a libs + clang-compiled main/board/init/patch.
set -e
Z="$HOME/projects/zeebo/research/openzeebo-repo/tools/zloader"
OUT=/tmp/zload
rm -rf "$OUT"; mkdir -p "$OUT"
CC="clang -target armv6k-none-eabi -march=armv6 -mcpu=arm1136j-s -marm"
FL="-std=gnu99 -g -DARM9 -DDEBUG -DPATCHNAME=sig_r -ffreestanding -fno-builtin -I$Z/include -I$Z"

# compile only the app-side objects; drivers come from prebuilt .a
for s in "zloader/init.S init" "zloader/board.c board" "zloader/main.c main" "zloader/patch/patch_sig_r.c patch"; do
  src="${s%% *}"; out="${s##* }"
  $CC $FL -c "$Z/$src" -o "$OUT/$out.o" 2>&1 | grep -i error | head -4 || true
done
echo "app objs:"; ls "$OUT"/*.o 2>/dev/null | wc -l

cat > "$OUT/boot.ld" <<'EOF'
ENTRY(_start)
BOOTLOADER_START = 0x00a00000;
BOOTLOADER_STACK = 0x00bff000;
BOOTLOADER_BSS = 0x00c04000;
BOOTLOADER_END = 0x00c20000;
BOOTLOADER_HEAP = 0x00c00000;
SECTIONS {
  . = BOOTLOADER_START;
  .text : { *(.text) *(.text.*) } =0
  .init : { *(.init.func.0) }
  .rodata : { *(.rodata .rodata.*) }
  .data : { *(.data .data.*) }
  .bss : { *(.bss .bss.*) *(COMMON) }
}
EOF

LIBS="libboot/libboot.a libc/libboot_c.a arch_msm7k/libboot_arch_msm7k.a arch_armv6/libboot_arch_armv6.a"
ARGS=""
for L in $LIBS; do ARGS="$ARGS $Z/$L"; done
clang -target armv6k-none-eabi -march=armv6 -marm -nostdlib -Wl,-T,"$OUT/boot.ld" -Wl,--gc-sections \
  $OUT/init.o $OUT/board.o $OUT/main.o $OUT/patch.o $ARGS -o "$OUT/zloader.elf" 2>&1 | grep -iE 'error|undefined' | head -25 || true
echo "== result =="
[ -f "$OUT/zloader.elf" ] && ls -la "$OUT/zloader.elf" && llvm-objdump -h "$OUT/zloader.elf" 2>/dev/null | head -15 || echo "no ELF"