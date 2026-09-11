#!/usr/bin/env python3
"""
scan_deps.py — Inventario ESTATICO de dependencias de vtable/static-base de um .mod BREW.

Motivacao (ver ROADMAP DD3/DD4): ate aqui cada slot de vtable foi descoberto
REATIVAMENTE — roda, falha com ip=0, disassembla, instala stub, repete. Isso custa
uma janela inteira por slot e nao diz quantos faltam.

Este script faz o inverso: varre o binario procurando os idiomas de chamada
indireta do RVCT/ARM e emite a lista COMPLETA de (base, offset) exigidos.

Idiomas reconhecidos:

  (A) Chamada via vtable de objeto COM (this em r0):
        ldr rB, [rOBJ]        ; rB = vtable
        ldr rF, [rB, #imm]    ; rF = slot
        ...
        bx rF   |  blx rF  |  mov lr,pc ; bx rF

  (B) Chamada via static-base (AEEHelperFuncs), ROPI/RVCT:
        ldr rS, [rBASE, #-4]  ; rS = static base
        ldr rF, [rS, #imm]
        bx rF

Saida: tabela (offset, tipo, endereco_da_chamada, registrador) em texto e JSON.

USO SOMENTE-LEITURA. Nao modifica o .mod.
"""

import argparse
import json
import re
import sys
from collections import defaultdict

try:
    import capstone
    from capstone import arm as cs_arm
except ImportError:
    sys.exit("capstone ausente: pip install capstone")


# Janela (em instrucoes) dentro da qual um `ldr rF,[rB,#imm]` e um `bx rF`
# ainda sao considerados o mesmo idioma de chamada.
WINDOW = 8


def disasm_resilient(md, data, base_va):
    """capstone.disasm() para no primeiro word indecodificavel, e um .mod BREW
    intercala code e data livremente (literal pools, tabelas, .rodata).
    Varremos em passos de 4 bytes, reiniciando apos cada buraco, para cobrir
    o binario inteiro em vez dos primeiros 0.2%.

    Retorna a lista de instrucoes ordenada por endereco, sem duplicatas.
    """
    out = {}
    n = len(data)
    off = 0
    while off + 4 <= n:
        produced = 0
        for ins in md.disasm(data[off:], base_va + off):
            out[ins.address] = ins
            produced += 1
        if produced:
            # avanca ate logo depois da ultima instrucao decodificada nesta rodada
            last = max(a for a in out if a >= base_va + off)
            off = (last - base_va) + 4
        else:
            off += 4
    return [out[a] for a in sorted(out)]


def scan(data, base_va):
    md = capstone.Cs(capstone.CS_ARCH_ARM, capstone.CS_MODE_ARM)
    md.detail = True

    insns = disasm_resilient(md, data, base_va)
    # mapa VA -> indice, para exigir contiguidade real entre load e branch
    idx_of = {ins.address: i for i, ins in enumerate(insns)}
    hits = []

    for i, ins in enumerate(insns):
        if ins.mnemonic not in ("bx", "blx"):
            continue
        if not ins.operands or ins.operands[0].type != cs_arm.ARM_OP_REG:
            continue
        target_reg = ins.reg_name(ins.operands[0].reg)

        # Procura para tras o `ldr target_reg, [rB, #imm]` que alimentou o branch.
        load = None
        for j in range(i - 1, max(-1, i - 1 - WINDOW), -1):
            p = insns[j]
            # se houve buraco de decodificacao, nao e o mesmo fluxo
            if p.address + p.size != insns[j + 1].address:
                break
            if p.mnemonic != "ldr":
                continue
            if not p.operands or p.operands[0].type != cs_arm.ARM_OP_REG:
                continue
            if p.reg_name(p.operands[0].reg) != target_reg:
                continue
            mem = p.operands[1]
            if mem.type != cs_arm.ARM_OP_MEM:
                break
            if mem.mem.base == 0:
                break
            load = (j, p, p.reg_name(mem.mem.base), mem.mem.disp)
            break

        if load is None:
            continue

        j, lins, basereg, disp = load

        # Classifica: (B) static-base se a carga anterior for `ldr basereg,[rX,#-4]`
        kind = "vtable"
        for k in range(j - 1, max(-1, j - 1 - WINDOW), -1):
            q = insns[k]
            if q.mnemonic != "ldr":
                continue
            if not q.operands or q.operands[0].type != cs_arm.ARM_OP_REG:
                continue
            if q.reg_name(q.operands[0].reg) != basereg:
                continue
            m2 = q.operands[1]
            if m2.type == cs_arm.ARM_OP_MEM and m2.mem.disp == -4:
                kind = "static-base"
            break

        hits.append(
            {
                "call_va": ins.address,
                "load_va": lins.address,
                "kind": kind,
                "offset": disp,
                "reg": target_reg,
                "base_reg": basereg,
                "text": f"{lins.mnemonic} {lins.op_str} ; {ins.mnemonic} {ins.op_str}",
            }
        )

    return hits


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("mod", help="caminho do .mod")
    ap.add_argument("--base", default="0x12000000")
    ap.add_argument("--json", help="grava inventario JSON")
    args = ap.parse_args()

    base_va = int(args.base, 0)
    data = open(args.mod, "rb").read()
    hits = scan(data, base_va)

    by_kind = defaultdict(lambda: defaultdict(list))
    for h in hits:
        by_kind[h["kind"]][h["offset"]].append(h["call_va"])

    print(f"# Inventario de chamadas indiretas — {args.mod}")
    print(f"# base_va=0x{base_va:08x}  bytes={len(data)}  hits={len(hits)}")
    print()

    for kind in sorted(by_kind):
        offs = by_kind[kind]
        print(f"## {kind}  ({len(offs)} offsets distintos, "
              f"{sum(len(v) for v in offs.values())} chamadas)")
        for off in sorted(offs):
            sites = offs[off]
            slot = off // 4 if off >= 0 else None
            slot_s = f"slot {slot:>3}" if slot is not None else "     "
            shown = ", ".join(f"0x{a:08x}" for a in sites[:6])
            more = f" (+{len(sites)-6})" if len(sites) > 6 else ""
            print(f"  offset 0x{off:04x}  {slot_s}  x{len(sites):<4} {shown}{more}")
        print()

    if args.json:
        with open(args.json, "w") as f:
            json.dump(hits, f, indent=2)
        print(f"# JSON -> {args.json}")


if __name__ == "__main__":
    main()
