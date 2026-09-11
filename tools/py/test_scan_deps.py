#!/usr/bin/env python3
"""
test_scan_deps.py — Gate do inventario estatico de dependencias (Bloco 0).

CRITERIO (definido ANTES de rodar, ver ROADMAP DD4):
  O scanner e considerado correto se PREVE RETROATIVAMENTE todas as fronteiras
  de vtable/static-base que, ate o commit 0f64ffa, foram descobertas de forma
  REATIVA (rodar -> falhar com ip=0 -> disassemblar -> instalar stub -> repetir).

  Se o scanner nao previr uma fronteira JA CONHECIDA, o scanner esta errado —
  nao o inventario. Esse e o ponto do gate.

Inclui controle negativo: um scanner que devolve lista vazia (ou que perde o
idioma `ldr rF,[rB,#imm] ; bx rF`) DEVE reprovar.

SKIP (exit 77) se o .mod proprietario nao estiver presente na maquina.
"""

import json
import os
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
SCANNER = os.path.join(HERE, "scan_deps.py")
MOD = os.path.expanduser("~/.Tuxality/Infuse/brew/mod/274754/ddragonz.mod")

# (call_va, kind, offset, nome) — fronteiras vencidas em DD3, commit 0f64ffa.
KNOWN_FRONTIERS = [
    (0x12023A48, "vtable", 0x48, "IDisplay slot 18 (GetDestination)"),
    (0x12023A74, "vtable", 0x14, "IDisplay slot 5"),
    (0x120244C8, "vtable", 0x28, "IDisplay slot 10"),
    (0x12024538, "vtable", 0x1C, "IDisplay slot 7"),
    (0x12023B08, "static-base", 0x14, "strlen"),
    (0x12023B28, "static-base", 0xE4, "strtowstr"),
    (0x12004AE0, "static-base", 0xB0, "GetUpTimeMS"),
    (0x12002194, "static-base", 0x68, "malloc"),
]

# Piso de cobertura: abaixo disto o scanner regrediu para a decodificacao
# ingenua (capstone.disasm para no 1o word invalido => ~177 instrucoes, 0 hits).
MIN_HITS = 400


def run_scanner():
    with tempfile.NamedTemporaryFile(suffix=".json", delete=False) as tf:
        out = tf.name
    subprocess.run(
        [sys.executable, SCANNER, MOD, "--json", out],
        check=True,
        stdout=subprocess.DEVNULL,
    )
    with open(out) as f:
        hits = json.load(f)
    os.unlink(out)
    return hits


def main():
    if not os.path.exists(MOD):
        print(f"SKIP: .mod proprietario ausente ({MOD})")
        return 77

    hits = run_scanner()
    by_call = {h["call_va"]: (h["kind"], h["offset"]) for h in hits}

    print(f"[scan-deps] hits={len(hits)}  call-sites distintos={len(by_call)}")

    failures = []

    # --- Gate 1: cobertura minima (pega a regressao de decodificacao ingenua)
    if len(hits) < MIN_HITS:
        failures.append(
            f"cobertura insuficiente: {len(hits)} hits < {MIN_HITS} "
            f"(scanner provavelmente parou no 1o word indecodificavel)"
        )

    # --- Gate 2: previsao retroativa das fronteiras conhecidas
    for va, kind, off, name in KNOWN_FRONTIERS:
        got = by_call.get(va)
        if got == (kind, off):
            print(f"  OK   0x{va:08x} {name} ({kind} +0x{off:02x})")
        else:
            print(f"  MISS 0x{va:08x} {name} esperado=({kind},0x{off:02x}) obtido={got}")
            failures.append(f"fronteira nao prevista: 0x{va:08x} {name}")

    # --- Gate 3 (controle negativo): um inventario vazio DEVE reprovar
    empty = {}
    neg_missed = sum(1 for va, k, o, _ in KNOWN_FRONTIERS if empty.get(va) != (k, o))
    if neg_missed != len(KNOWN_FRONTIERS):
        failures.append("controle negativo quebrado: inventario vazio nao reprovou")
    else:
        print(f"  OK   controle negativo: inventario vazio perde {neg_missed}/"
              f"{len(KNOWN_FRONTIERS)} fronteiras (reprova como esperado)")

    if failures:
        print("\nFAIL:")
        for f in failures:
            print(f"  - {f}")
        return 1

    print(f"\n=== test_scan_deps: PASS "
          f"({len(KNOWN_FRONTIERS)}/{len(KNOWN_FRONTIERS)} fronteiras previstas "
          f"estaticamente) ===")
    return 0


if __name__ == "__main__":
    sys.exit(main())
