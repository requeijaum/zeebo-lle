#!/usr/bin/env bash
# QW1 regression — prova a separação entre o `check` agregado (compile-only,
# sem NAND) e o gate real por bytes (`test-bootinfo-real`, exige a NAND).
#
# Verifica três invariantes de "sem falso-verde":
#   1) Sem NAND, `zeebo_bootinfo_harness` compila e NÃO alega validação real.
#   2) `test-bootinfo-real` sem a NAND FALHA com exit != 0 (nunca SKIP=PASS).
#   3) `test-bootinfo-real` com a NAND real PASSA (exit 0).
#
# Uso:
#   ./test_bootinfo_gate_regression.sh [/caminho/para/1.1.2_APPS.bin]
# Se o caminho da NAND real não for dado nem existir, o passo (3) é omitido
# (marcado como N/A), mas os passos (1) e (2) — os que garantem a honestidade
# num clone limpo — são sempre executados.
set -u
cd "$(dirname "$0")"

REAL_NAND="${1:-/home/rafaelfrequiao/projects/zeebo-lle/nand/1.1.2_APPS.bin}"
fail=0
pass() { echo "  PASS: $1"; }
bad()  { echo "  FAIL: $1"; fail=1; }

echo "[regression] (1) build compile-only do harness (sem executar / sem NAND)"
make -s zeebo_bootinfo_harness >/dev/null 2>&1 \
  && [ -x ./zeebo_bootinfo_harness ] \
  && pass "zeebo_bootinfo_harness compilou sem exigir a NAND" \
  || bad  "harness nao compilou"

echo "[regression] (1b) alvo compile-only nao afirma validacao real-byte"
# O alvo de build nao deve rodar o binario nem imprimir PASS de bytes reais.
out="$(make -s zeebo_bootinfo_harness 2>&1)"
if echo "$out" | grep -qi 'QW1] PASS'; then
  bad "o build compile-only imprimiu um PASS de validacao (falso-verde)"
else
  pass "build compile-only silencioso quanto a validacao real"
fi

echo "[regression] (2) test-bootinfo-real SEM a NAND deve falhar nonzero"
MISS="$(mktemp -u)/nao_existe.bin"
set +e
make -s test-bootinfo-real BOOTINFO_APPS="$MISS" >/tmp/qw1_reg_miss.log 2>&1
rc=$?
set -e 2>/dev/null || true
if [ "$rc" -ne 0 ]; then
  pass "NAND ausente -> exit $rc (nao-zero, sem falso-verde)"
else
  bad "NAND ausente retornou 0 (falso-verde!)"
fi
grep -qi 'ausente' /tmp/qw1_reg_miss.log \
  && pass "mensagem de firmware ausente presente" \
  || bad  "mensagem de ausencia nao encontrada"

echo "[regression] (3) test-bootinfo-real COM a NAND real deve passar (exit 0)"
if [ -f "$REAL_NAND" ]; then
  set +e
  make -s test-bootinfo-real BOOTINFO_APPS="$REAL_NAND" >/tmp/qw1_reg_real.log 2>&1
  rc=$?
  set -e 2>/dev/null || true
  if [ "$rc" -eq 0 ] && grep -qi 'QW1] PASS' /tmp/qw1_reg_real.log; then
    pass "NAND real -> exit 0 + PASS por bytes/mutacao"
  else
    bad "NAND real nao passou (exit $rc) — ver /tmp/qw1_reg_real.log"
  fi
else
  echo "  N/A : NAND real ausente em $REAL_NAND (passo 3 omitido)"
fi

echo
if [ "$fail" -eq 0 ]; then
  echo "[regression] TODOS OS INVARIANTES OK"
  exit 0
else
  echo "[regression] FALHOU"
  exit 1
fi
