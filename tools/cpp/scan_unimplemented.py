#!/usr/bin/env python3
"""Varre uma imagem de firmware ARM procurando instrucoes A32 que o backend
recompilado (Dynarmic) NAO traduz.

Motivacao: o Dynarmic delega um punhado de instrucoes de modo privilegiado ao
interpretador (`InterpretThisInstruction`). Este projeto nao acopla
interpretador, entao cada uma dessas instrucoes PARA o Core0 sob --jit.
Descobri-las uma a uma esbarrando no boot e lento; melhor varrer o firmware.

Escaneia como ARM de 32 bits alinhado. Isso super-estima: dados e codigo Thumb
tambem sao lidos como palavras ARM. Por isso o script separa os achados por
regiao e relata contexto, em vez de afirmar que todo achado sera executado.

Uso: python3 scan_unimplemented.py <firmware.bin> [base_va]
"""
import sys
from collections import defaultdict

# Instrucoes que o Dynarmic despacha para o interpretador.
# Verificado em third_party/dynarmic/src/dynarmic/frontend/A32/translate/impl/:
#   status_register_access.cpp: arm_CPS, arm_RFE, arm_SRS
#   load_store.cpp:             arm_LDM_usr, arm_LDM_eret, arm_STM_usr
# (CPS ja foi implementado na nossa camada; continua listado para deteccao.)


def classify(w):
    """Devolve o nome da instrucao nao-traduzida, ou None."""
    cond = (w >> 28) & 0xF

    # CPS: 1111 0001 0000 ... (incondicional)
    if (w & 0xFFF1FE20) == 0xF1000000:
        return "CPS"

    # RFE: 1111 100x x0x1 nnnn 0000 1010 ...  (incondicional, L=1)
    if (w & 0xFE50FFFF) == 0xF8100A00:
        return "RFE"

    # SRS: 1111 100x x1x0 1101 0000 0101 000x xxxx (incondicional)
    if (w & 0xFE5FFFE0) == 0xF84D0500:
        return "SRS"

    # LDM/STM com banco de usuario ou retorno de excecao: bit S (22) = 1
    # Formato: cond 100 P U S W L Rn register_list
    if cond != 0xF and (w & 0x0E000000) == 0x08000000 and (w & 0x00400000):
        L = (w >> 20) & 1
        if L:
            # LDM (exception return) se R15 esta na lista; senao LDM (user)
            return "LDM_eret" if (w & 0x8000) else "LDM_usr"
        return "STM_usr"

    return None


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        return 2
    path = sys.argv[1]
    base = int(sys.argv[2], 0) if len(sys.argv) > 2 else 0

    data = open(path, "rb").read()
    hits = defaultdict(list)

    for off in range(0, len(data) - 3, 4):
        w = int.from_bytes(data[off:off + 4], "little")
        if w == 0 or w == 0xFFFFFFFF:
            continue
        name = classify(w)
        if name:
            hits[name].append((base + off, w))

    print(f"arquivo: {path}  ({len(data)} bytes, base=0x{base:08x})")
    print(f"palavras ARM escaneadas: {len(data)//4}\n")

    if not hits:
        print("nenhuma instrucao nao-traduzida encontrada")
        return 0

    total = sum(len(v) for v in hits.values())
    print(f"{'instrucao':<12} {'ocorrencias':>12}")
    print("-" * 26)
    for name in sorted(hits, key=lambda k: -len(hits[k])):
        print(f"{name:<12} {len(hits[name]):>12}")
    print(f"{'TOTAL':<12} {total:>12}\n")

    for name in sorted(hits, key=lambda k: -len(hits[k])):
        v = hits[name]
        print(f"== {name}: primeiras {min(8, len(v))} de {len(v)} ==")
        for va, w in v[:8]:
            print(f"   0x{va:08x}: {w:08x}")
        print()

    print("AVISO: varredura linear le dados e codigo Thumb como palavras ARM.")
    print("Um achado NAO prova que aquela instrucao sera executada; prova que")
    print("vale confirmar no traco de execucao antes de implementar.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
