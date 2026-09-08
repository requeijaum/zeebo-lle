#!/usr/bin/env bash
# run_qw6_txn.sh -- QW6 + QW15 transactional conformance runner.
#
# Reuses the existing cputest toolchain (arm-none-eabi assembler + a Unicorn
# probe) but compares the FULL ordered transaction trace (prefetch/load/store
# with width/address/value) AND final state against a locally authored golden.
#
# A mutated trace -- reordered, wrong address/width/value -- fails here even
# when final PC / instruction count are unchanged.
#
# Vectors (locally authored, clean-room; NOT derived from any GPL/proprietary
# suite):
#   qw6_txn    -- ordered ARM/Thumb bus transactions + interwork.
#   qw15_page  -- cross-page (4KiB boundary) transactions: genuine straddling
#                 word/half accesses across 0x00102000 under SCTLR.A=0 (pinned
#                 as real crossings, NOT traps).
#
# HARDENING: every AS / LD / objcopy / build / run / diff failure exits nonzero.
# `set -e` plus explicit guards on each toolchain step; no step may silently
# pass. `-o pipefail` so a failing probe inside a pipe is not masked.
set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
PROBE="$HERE/zeebo_lle_txn_probe"
TDIR="$HERE/testkit"
OUT="$TDIR/build"
STEPS=64
BASE="0x00100000"
VECTORS=("qw6_txn" "qw15_page")

mkdir -p "$OUT"

# Always (re)build the probe from source so a stale binary can't mask a
# regression. Any compile error aborts nonzero.
echo "Building zeebo_lle_txn_probe..."
g++ -std=c++23 -O2 -Wall -Wextra -o "$PROBE" "$HERE/zeebo_lle_txn_probe.cpp" -lunicorn || {
    echo "BUILD FAIL zeebo_lle_txn_probe"; exit 1;
}

fail=0
for NAME in "${VECTORS[@]}"; do
    s="$TDIR/$NAME.s"
    bin="$OUT/$NAME.bin"
    exp="$TDIR/$NAME.expected.txt"

    [ -f "$s" ]   || { echo "MISSING SOURCE $s"; exit 1; }
    [ -f "$exp" ] || { echo "MISSING GOLDEN $exp"; exit 1; }

    arm-none-eabi-as -march=armv6 "$s" -o "$OUT/$NAME.o" \
        || { echo "AS FAIL $NAME"; exit 1; }
    arm-none-eabi-ld -Ttext=0x00100000 -e _start "$OUT/$NAME.o" -o "$OUT/$NAME.elf" 2>/dev/null \
        || { echo "LD FAIL $NAME"; exit 1; }
    arm-none-eabi-objcopy -O binary "$OUT/$NAME.elf" "$bin" \
        || { echo "OBJCOPY FAIL $NAME"; exit 1; }
    [ -s "$bin" ] || { echo "EMPTY BIN $NAME"; exit 1; }

    # `set -e` would abort on a nonzero probe; capture explicitly so we can
    # report a clean RUN FAIL and still surface a nonzero overall exit.
    if ! got="$("$PROBE" "$bin" "$STEPS" "$BASE" 2>&1)"; then
        echo "RUN FAIL $NAME (probe exited nonzero)"
        echo "$got"
        fail=1
        continue
    fi

    if [ "$got" = "$(cat "$exp")" ]; then
        echo "PASS  $NAME (transaction trace + final state)"
    else
        echo "FAIL  $NAME"
        diff <(cat "$exp") <(echo "$got") | head -80
        fail=1
    fi
done

if [ "$fail" -ne 0 ]; then
    echo "run_qw6_txn: one or more vectors FAILED"
    exit 1
fi
echo "run_qw6_txn: all transactional vectors PASS"
exit 0
