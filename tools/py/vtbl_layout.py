#!/usr/bin/env python3
"""
vtbl_layout.py — Extrai a ORDEM de slots de uma interface COM do BREW a partir
dos headers do SDK, expandindo as macros INHERIT_*.

Motivacao (ROADMAP DD4): `scan_deps.py` diz QUAIS offsets o jogo chama, mas nao
QUEM sao. Offset 0x18 pode ser qualquer coisa. Contar slots a mao atraves de
INHERIT_ encadeado e exatamente o tipo de trabalho que erra em silencio.

Este script faz a contagem mecanicamente: resolve a cadeia de heranca e emite
(slot, offset, tipo_retorno, nome, assinatura).

LEITURA APENAS. Nao copia codigo do SDK — extrai a ORDEM DECLARATIVA da ABI,
que e o contrato necessario para interoperar. Nenhuma linha de implementacao
da Qualcomm e reproduzida.
"""

import argparse
import glob
import os
import re
import sys

# INHERIT_IBase e a raiz: AddRef, Release.
# Registramos explicitamente porque a macro vive em AEEIBase.h com formatacao
# propria e e a unica que nao segue o padrao "um metodo por linha com \".
ROOTS = {
    "IBase": [("uint32", "AddRef", "iname*"),
              ("uint32", "Release", "iname*")],
}

METHOD_RE = re.compile(
    r"^\s*([A-Za-z_][A-Za-z0-9_ \t\*]*?)\s*\(\s*\*\s*([A-Za-z_][A-Za-z0-9_]*)\s*\)\s*\((.*?)\)\s*;?\s*\\?\s*$"
)


def find_macro(sdk_root, iface):
    """Localiza `#define INHERIT_<iface>(iname)` e devolve suas linhas."""
    pat = re.compile(r"#define\s+INHERIT_" + re.escape(iface) + r"\s*\(")
    for path in glob.glob(os.path.join(sdk_root, "**", "*.h"), recursive=True):
        try:
            with open(path, "r", errors="replace") as f:
                lines = f.read().replace("\r\n", "\n").split("\n")
        except OSError:
            continue
        for i, ln in enumerate(lines):
            if pat.search(ln):
                block = [ln]
                j = i
                while lines[j].rstrip().endswith("\\"):
                    j += 1
                    block.append(lines[j])
                return path, block
    return None, None


def parse(sdk_root, iface, depth=0, seen=None):
    """Devolve lista ordenada de (ret, nome, args) para a interface."""
    if seen is None:
        seen = set()
    if iface in seen:
        sys.exit(f"heranca circular em {iface}")
    seen.add(iface)

    if iface in ROOTS:
        return list(ROOTS[iface])

    path, block = find_macro(sdk_root, iface)
    if block is None:
        sys.exit(f"INHERIT_{iface} nao encontrado sob {sdk_root}")

    methods = []
    for ln in block:
        # heranca: INHERIT_IXxx(iname);
        m = re.search(r"INHERIT_([A-Za-z_][A-Za-z0-9_]*)\s*\(\s*iname\s*\)", ln)
        if m and m.group(1) != iface:
            methods.extend(parse(sdk_root, m.group(1), depth + 1, seen))
            continue
        m = METHOD_RE.match(ln.rstrip("\\").rstrip())
        if m:
            ret = " ".join(m.group(1).split())
            methods.append((ret, m.group(2), " ".join(m.group(3).split())))
    return methods


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("iface", help="nome da interface, ex.: IDisplay")
    ap.add_argument("--sdk", required=True, help="raiz do SDK extraido")
    ap.add_argument("--highlight", default="",
                    help="offsets a destacar, ex.: 0x10,0x14,0x18,0x1c")
    args = ap.parse_args()

    hi = set()
    for tok in args.highlight.split(","):
        tok = tok.strip()
        if tok:
            hi.add(int(tok, 0))

    methods = parse(args.sdk, args.iface)
    print(f"# Layout de vtable — {args.iface}  ({len(methods)} slots)")
    print(f"{'slot':>4} {'offset':>8}  {'':1} {'metodo':<20} assinatura")
    print("-" * 100)
    for i, (ret, name, arglist) in enumerate(methods):
        off = i * 4
        mark = "*" if off in hi else " "
        print(f"{i:>4} 0x{off:06x}  {mark} {name:<20} {ret} ({arglist})")
    if hi:
        print()
        print("* = offset efetivamente chamado pelo binario (scan_deps.py)")


if __name__ == "__main__":
    main()
