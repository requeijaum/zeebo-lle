#!/usr/bin/env bash
# run_qw6_txn.sh -- QW6 transactional conformance runner.
#
# Reuses the existing cputest toolchain (arm-none-eabi assembler + a Unicorn
# probe) but compares the FULL ordered transaction trace (prefetch/load/store
# with width/address/value) AND final state against a locally authored golden.
#
# A mutated trace -- reordered, wrong address/width/value -- fails here even
# when final PC / instruction count are unchanged.
set -uo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
PROBE="$HERE/zeebo_lle_txn_probe"
TDIR="$HERE/testkit"
OUT="$TDIR/build"
STEPS=64
NAME="qw6_txn"

mkdir -p "$OUT"

[ -x "$PROBE" ] || {
    echo "Building zeebo_lle_txn_probe..."
    g++ -std=c++23 -O2 -Wall -Wextra -o "$PROBE" "$HERE/zeebo_lle_txn_probe.cpp" -lunicorn || exit 1
}

s="$TDIR/$NAME.s"
bin="$OUT/$NAME.bin"
exp="$TDIR/$NAME.expected.txt"

arm-none-eabi-as -march=armv6 "$s" -o "$OUT/$NAME.o" || { echo "AS FAIL"; exit 1; }
arm-none-eabi-ld -Ttext=0x00100000 -e _start "$OUT/$NAME.o" -o "$OUT/$NAME.elf" 2>/dev/null || { echo "LD FAIL"; exit 1; }
arm-none-eabi-objcopy -O binary "$OUT/$NAME.elf" "$bin"

got="$("$PROBE" "$bin" "$STEPS" 0x00100000 2>&1)"

if [ ! -f "$exp" ]; then
    echo "MISSING GOLDEN $NAME"
    echo "--- got ---"
    echo "$got"
    exit 1
fi

if [ "$got" = "$(cat "$exp")" ]; then
    echo "PASS  $NAME (transaction trace + final state)"
    exit 0
else
    echo "FAIL  $NAME"
    diff <(cat "$exp") <(echo "$got") | head -60
    exit 1
fi
