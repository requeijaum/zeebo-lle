#!/bin/bash
# All-clang build+link of zloader (no prebuilt .a; everything from source so ABI
# matches). Replaces libc/inttypes.h via a clean inttypes.
set -e
Z="$HOME/projects/zeebo/research/openzeebo-repo/tools/zloader"
OUT=/tmp/zload2
rm -rf "$OUT"; mkdir -p "$OUT"
CC="clang -target armv6k-none-eabi -march=armv6 -mcpu=arm1136j-s -marm"
# Use clang's own headers; add a src-inttypes so <inttypes.h> resolves. Provide
# stdint via a shim if needed.
CLANGINC="$(clang -print-resource-dir)/include"
FL="-std=gnu99 -g -DARM9 -DDEBUG -DPATCHNAME=sig_r -ffreestanding -fno-builtin -I$Z/include -I$Z -I$OUT/inc"

# clean inttypes.h shim (avoids the repo's unsigned-long/unsigned-int clash)
mkdir -p "$OUT/inc"
cat > "$OUT/inc/inttypes.h" <<'EOF'
#ifndef _INTTYPES_H_
#define _INTTYPES_H_
typedef unsigned char uint8_t;
typedef unsigned short uint16_t;
typedef unsigned int uint32_t;
typedef unsigned long long uint64_t;
typedef signed char int8_t;
typedef signed short int16_t;
typedef signed int int32_t;
typedef int intptr_t;
typedef unsigned uintptr_t;
#define ARRAY_SIZE(a) (sizeof(a)/sizeof((a)[0]))
void dprintf(const char *fmt, ...);
void dprintf_set_putc(void (*f)(unsigned));
void dprintf_set_flush(void (*f)(void));
void nopdelay(unsigned d);
int snprintf(char *s, unsigned n, const char *fmt, ...);
#endif
EOF

compile() { # $1=src $2=out
  $CC $FL -c "$1" -o "$2" 2>&1 | grep -i error | head -3 || true
}
echo "== arch_armv6 =="
for f in irq jtag misc; do compile "$Z/arch_armv6/$f.S" "$OUT/${f}.o"; done
echo "== arch_msm7k =="
for f in clock gpio mddi mddi_console nand smem ssbi uart vic hsusb shared; do
  [ -f "$Z/arch_msm7k/$f.c" ] && compile "$Z/arch_msm7k/$f.c" "$OUT/${f}.o"
done
echo "== libboot =="
for f in flash init poll tags tags_cmdline tags_partition tags_revision tags_serialno gpio_keypad; do
  [ -f "$Z/libboot/$f.c" ] && compile "$Z/libboot/$f.c" "$OUT/${f}.o"
done
echo "== libc (sha + mem + xprintf) =="
for f in sha memcmp memcpy memset strcpy strcmp strncmp strlen dprintf xprintf cprintf sprintf malloc; do
  for d in libc; do [ -f "$Z/$d/$f.c" ] && compile "$Z/$d/$f.c" "$OUT/${f}.o" && break; done
done
echo "== zloader + patch =="
compile "$Z/zloader/init.S"    "$OUT/init.o"
compile "$Z/zloader/board.c"   "$OUT/board.o"
compile "$Z/zloader/main.c"    "$OUT/main.o"
compile "$Z/zloader/patch/patch_sig_r.c" "$OUT/patch.o"

echo "== link =="
cat > "$OUT/boot.ld" <<'EOF'
ENTRY(_start)
BOOTLOADER_START = 0x00a00000;
BOOTLOADER_STACK = 0x00bff000;
BOOTLOADER_BSS = 0x00c04000;
BOOTLOADER_END = 0x00c20000;
BOOTLOADER_HEAP = 0x00c00000;
SECTIONS { . = BOOTLOADER_START; .text : { *(.text) *(.text.*) } =0; .rodata : { *(.rodata .rodata.*) } .data : { *(.data .data.*) } .bss : { *(.bss .bss.*) *(COMMON) } PROVIDE(.rodata = BOOTLOADER_START); PROVIDE(.bss = ABSOLUTE(.)); }
EOF
OBJS=$(find "$OUT" -name '*.o' | tr '\n' ' ')
# provide __aeabi_uidiv (software divide) + scalar shims via compiler-rt/libgcc
RT=$(clang -print-libgcc-file-name --target=armv6k-none-eabi 2>/dev/null || echo "")
GCCRT=""
for g in aarch64-linux-gnu-gcc arm-linux-gnueabi-gcc; do
  L=$(which $g 2>/dev/null); [ -n "$L" ] && GCCRT=$(dirname "$L")/../lib/gcc/arm-linux-gnueabi/*/libgcc.a 2>/dev/null && break
done
EXTRA=""
if [ -n "$RT" ] && [ -f "$RT" ]; then EXTRA="$RT";
elif ls $GCCRT >/dev/null 2>&1; then EXTRA=$(ls $GCCRT | head -1); fi
echo "compiler-rt for divide: $EXTRA"
clang -target armv6k-none-eabi -march=armv6 -marm -nostdlib -Wl,-T,"$OUT/boot.ld" -Wl,--gc-sections \
  $OBJS $EXTRA -o "$OUT/zloader.elf" 2>&1 | grep -iE 'error|undefined' | head -20 || true
echo "== result =="
if [ -f "$OUT/zloader.elf" ]; then ls -la "$OUT/zloader.elf"; llvm-objdump -h "$OUT/zloader.elf" 2>/dev/null | head -14; else echo "NO ELF"; fi