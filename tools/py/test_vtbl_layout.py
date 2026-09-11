#!/usr/bin/env python3
"""
test_vtbl_layout.py — Gate da identificacao de slots de IDisplay (Bloco 1).

PROBLEMA QUE ESTE TESTE RESOLVE
Ate 09eaf3f os slots de IDisplay foram rotulados por SUPOSICAO ("slot 5",
"GetInfo", "GetDestination"). scan_deps.py deu os offsets; nada dava os NOMES.
Rotulo errado leva a stub errado: um stub de "GetInfo" no offset 0x10 na
verdade ocupa DrawText.

CRITERIO (fixado antes de rodar)
1. O layout extraido mecanicamente do SDK deve colocar em 0x14 e 0x1c metodos
   cuja ARIDADE observada no binario bata com a assinatura declarada.
2. Especificamente: 0x14 deve ser um metodo de 5 argumentos (o 5o empilhado)
   e 0x1c um metodo de 2 argumentos sem uso de retorno.
Se o SDK e o binario discordarem, o errado e a identificacao — nao o binario.

CONTROLE NEGATIVO: test_vtbl_layout_mutant() desloca a numeracao em um slot
(a raiz real e INHERIT_IBase, com 2 metodos: AddRef/Release) e exige que a
conferencia REPROVE. Sem isso, o teste passaria com qualquer layout.

SKIP 77 se o SDK ou o .mod proprietario nao estiverem presentes.
"""

import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(os.path.dirname(HERE))

SDK = os.path.expanduser(
    "~/projects/zeebo-emulator/research/docs/sdk-extract/"
    "BrewMPSDK-7.12.5/SDKPro/1.0.4.601 Pro"
)
MOD = os.path.expanduser("~/.Tuxality/Infuse/brew/mod/274754/ddragonz.mod")

# Aridade observada nos call-sites reais (disassembly, nao suposicao).
#   offset -> (n_args, usa_retorno, call_site_va)
# 0x14 @ 0x12023a74: r0..r3 + str r3,[sp]  => 5 args
# 0x1c @ 0x12024538: r0,r1 + bx (tail-call) => 2 args, retorno nao usado
OBSERVED = {
    0x14: (5, False, 0x12023A74),
    0x1C: (2, False, 0x12024538),
}


def skip(msg):
    print(f"SKIP: {msg}")
    sys.exit(77)


def load_layout(shift=0):
    """Executa vtbl_layout.py e devolve {offset: (nome, n_args)}.

    shift>0 simula uma numeracao deslocada (controle negativo).
    """
    out = subprocess.run(
        [sys.executable, os.path.join(HERE, "vtbl_layout.py"),
         "IDisplay", "--sdk", SDK],
        capture_output=True, text=True, check=True,
    ).stdout

    layout = {}
    idx = 0
    for ln in out.split("\n"):
        parts = ln.split()
        if len(parts) < 4 or not parts[0].isdigit():
            continue
        name = parts[2]
        # conta argumentos entre os parenteses da assinatura
        sig = ln[ln.index("(") + 1:ln.rindex(")")] if "(" in ln else ""
        n_args = 0 if sig.strip() in ("", "void") else len(sig.split(","))
        layout[(idx - shift) * 4] = (name, n_args)
        idx += 1
    return layout


def check(layout):
    """Devolve lista de divergencias entre layout e aridade observada."""
    bad = []
    for off, (n_args, _uses_ret, va) in OBSERVED.items():
        if off not in layout:
            bad.append(f"offset 0x{off:02x}: ausente no layout")
            continue
        name, declared = layout[off]
        if declared != n_args:
            bad.append(
                f"offset 0x{off:02x} ({name}) @0x{va:08x}: "
                f"SDK declara {declared} args, binario usa {n_args}"
            )
    return bad


def main():
    if not os.path.isdir(SDK):
        skip(f"SDK ausente: {SDK}")
    if not os.path.isfile(MOD):
        skip(f".mod proprietario ausente: {MOD}")

    layout = load_layout()
    print(f"IDisplay: {len(layout)} slots extraidos do SDK")

    bad = check(layout)
    for b in bad:
        print(f"  DIVERGENCIA: {b}")
    if bad:
        print("FAIL: layout do SDK nao explica os call-sites observados")
        return 1

    for off, (n_args, _u, va) in sorted(OBSERVED.items()):
        name, declared = layout[off]
        print(f"  OK  0x{off:02x} -> {name:<12} "
              f"{declared} args, confere com call-site 0x{va:08x}")

    # --- controle negativo: numeracao deslocada deve REPROVAR ---
    shifted = load_layout(shift=1)
    if not check(shifted):
        print("FAIL: layout deslocado em 1 slot tambem passou "
              "(a conferencia nao discrimina nada)")
        return 1
    print("  OK  controle negativo: layout deslocado em 1 slot REPROVA")

    print("PASS: identificacao de slots de IDisplay validada contra o binario")
    return 0


if __name__ == "__main__":
    sys.exit(main())
