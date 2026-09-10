#!/usr/bin/env python3
"""Acha a primeira instrucao em que os tracos de execucao do Core0 divergem
entre o backend interpretado e o recompilado.

Uso:
  ./zeebo_lle_main --cycles=N --headless --trace-core0=/tmp/tr_uni.txt --trace-limit=L
  ./zeebo_lle_main --jit --cycles=N --headless --trace-core0=/tmp/tr_jit.txt --trace-limit=L
  python3 diff_traces.py /tmp/tr_uni.txt /tmp/tr_jit.txt

Formato de cada linha: <n> <pc> <nzcv> <opcode> <r0..r14>

Compara PC, flags de condicao e os 15 registradores gerais. Ao contrario do log
periodico do emulador (que amostra a cada 10 mil instrucoes), aqui a primeira
divergencia e exata.
"""
import sys


def load(path, limit=None):
    rows = []
    with open(path) as f:
        for line in f:
            p = line.split()
            if len(p) < 18:
                continue
            rows.append((int(p[0]), p[1], p[2], p[4:19], p[3]))
            if limit and len(rows) >= limit:
                break
    return rows


def main():
    if len(sys.argv) < 3:
        print(__doc__)
        return 2
    a = load(sys.argv[1])
    b = load(sys.argv[2])
    print(f"interpretado: {len(a)} instrucoes")
    print(f"recompilado : {len(b)} instrucoes")

    n = min(len(a), len(b))
    for i in range(n):
        na, pca, fa, ra, opa = a[i]
        nb, pcb, fb, rb, opb = b[i]
        if pca == pcb and fa == fb and ra == rb:
            continue

        print(f"\nPRIMEIRA DIVERGENCIA na instrucao #{na}")
        print(f"  a instrucao ANTERIOR (#{a[i-1][0]}) foi pc=0x{a[i-1][1]} opcode=0x{a[i-1][4]}")
        print(f"  esta instrucao: pc=0x{pca} opcode=0x{opa}")
        print(f"  pc     interpretado=0x{pca}  recompilado=0x{pcb}"
              f"{'' if pca == pcb else '   <== PC DIFERENTE'}")
        if fa != fb:
            print(f"  flags  interpretado=0x{fa}  recompilado=0x{fb}   <== FLAGS DIFERENTES")
        for k in range(15):
            if ra[k] != rb[k]:
                print(f"  r{k:<2}    0x{ra[k]}  !=  0x{rb[k]}")

        print("\n  contexto (10 instrucoes antes, ambos identicos):")
        for j in range(max(0, i - 10), i):
            print(f"    #{a[j][0]:<8} pc=0x{a[j][1]} opcode=0x{a[j][4]}")

        print("\n  como cada motor segue depois:")
        for j in range(i, min(i + 8, n)):
            print(f"    #{a[j][0]:<8} interpretado=0x{a[j][1]}   recompilado=0x{b[j][1]}")
        return 1

    print(f"\nsem divergencia nas {n} instrucoes comparadas")
    if len(a) != len(b):
        print(f"(os tracos tem tamanhos diferentes: {len(a)} vs {len(b)})")
    return 0


if __name__ == "__main__":
    sys.exit(main())
