#!/usr/bin/env bash
# run_lle_cputests.sh — Run Zeebo LLE ARM11 against the conformance test suite
# defined in /home/rafaelfrequiao/projects/zeebo-emulator/testkit/cputests/
set -uo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
LLE_PROBE="$HERE/zeebo_lle_mod_probe"
# Suite de vetores fora do repo. Permite sobrepor sem editar o script.
TDIR="${ZEEBO_CPUTESTS_DIR:-/home/rafaelfrequiao/projects/zeebo-emulator/testkit/cputests}"
OUT="$TDIR/build"
STEPS=200

# Diretorio ausente ou sem vetores nao pode terminar com sucesso: um laco que
# nao roda nenhum caso sairia com 0 pass / 0 fail e codigo 0 (falso verde).
if [ ! -d "$TDIR" ]; then
    echo "ERRO: suite de vetores nao encontrada em $TDIR"
    echo "      defina ZEEBO_CPUTESTS_DIR apontando para o diretorio correto."
    exit 2
fi
shopt -s nullglob
vectors=("$TDIR"/*.s)
if [ "${#vectors[@]}" -eq 0 ]; then
    echo "ERRO: nenhum vetor .s em $TDIR — nada foi executado."
    exit 2
fi
mkdir -p "$OUT"

[ -x "$LLE_PROBE" ] || {
    echo "Building zeebo_lle_mod_probe..."
    g++ -std=c++23 -O2 -o "$LLE_PROBE" "$HERE/zeebo_lle_mod_probe.cpp" -I/usr/include -lunicorn || exit 1
}

pass=0
fail=0

echo "=== Zeebo LLE ARM11 Conformance Tests against testkit/cputests ==="

for s in "${vectors[@]}"; do
    name="$(basename "${s%.s}")"
    bin="$OUT/$name.bin"
    exp="$TDIR/$name.expected"

    if [ ! -f "$bin" ]; then
        arm-none-eabi-as -march=armv6 "$s" -o "$OUT/$name.o" || { echo "AS FAIL $name"; fail=$((fail+1)); continue; }
        arm-none-eabi-ld -Ttext=0 -e _start "$OUT/$name.o" -o "$OUT/$name.elf" 2>/dev/null
        arm-none-eabi-objcopy -O binary "$OUT/$name.elf" "$bin"
    fi

    got="$("$LLE_PROBE" "$bin" "$STEPS" 2>&1 | grep -E '^\[' | tail -1 | sed -E 's/^\[[0-9 ]+\] //')"

    if ! printf '%s' "$got" | grep -q 'instr=0xeafffffe'; then
        echo "DERAIL $name: not parked in spin -> $got"
        fail=$((fail+1))
        continue
    fi

    if [ ! -f "$exp" ]; then
        echo "MISSING GOLDEN $name"
        fail=$((fail+1))
        continue
    fi

    expected="$(cat "$exp")"
    if [ "$got" = "$expected" ]; then
        echo "PASS  $name"
        pass=$((pass+1))
    else
        echo "FAIL  $name"
        echo "   expected: $expected"
        echo "   got:      $got"
        fail=$((fail+1))
    fi
done

echo
echo "=== LLE SUMMARY: $pass pass, $fail fail (de ${#vectors[@]} vetores) ==="
if [ "$((pass + fail))" -eq 0 ]; then
    echo "ERRO: nenhum vetor executado — resultado nao conta como aprovacao."
    exit 2
fi
[ "$fail" -eq 0 ] || exit 1
