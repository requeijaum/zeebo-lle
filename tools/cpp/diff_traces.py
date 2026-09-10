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


from collections import deque

# Janela de contexto mantida em memoria. O resto do traco NAO e materializado:
# com 3.000.000 de instrucoes por backend, carregar tudo faz o processo ser
# morto pelo OOM killer (exit 137) -- foi o que aconteceu na primeira tentativa
# de comparar tracos longos.
CONTEXTO = 10
DEPOIS = 8


def parse(line):
    p = line.split()
    if len(p) < 18:
        return None
    # (n, pc, flags/cpsr, registradores, opcode)
    return (int(p[0]), p[1], p[2], p[4:19], p[3])


def iter_rows(path):
    with open(path) as f:
        for line in f:
            row = parse(line)
            if row is not None:
                yield row


def main():
    if len(sys.argv) < 3:
        print(__doc__)
        return 2
    ia = iter_rows(sys.argv[1])
    ib = iter_rows(sys.argv[2])

    contexto = deque(maxlen=CONTEXTO)
    n = 0
    for ra_row, rb_row in zip(ia, ib):
        n += 1
        na, pca, fa, ra, opa = ra_row
        nb, pcb, fb, rb, opb = rb_row
        if pca == pcb and fa == fb and ra == rb:
            contexto.append(ra_row)
            continue

        print(f"\nPRIMEIRA DIVERGENCIA na instrucao #{na}")
        if contexto:
            ant = contexto[-1]
            print(f"  a instrucao ANTERIOR (#{ant[0]}) foi pc=0x{ant[1]} opcode=0x{ant[4]}")
        print(f"  esta instrucao: pc=0x{pca} opcode=0x{opa}")
        print(f"  pc     interpretado=0x{pca}  recompilado=0x{pcb}"
              f"{'' if pca == pcb else '   <== PC DIFERENTE'}")
        if fa != fb:
            print(f"  flags  interpretado=0x{fa}  recompilado=0x{fb}   <== FLAGS DIFERENTES")
        for k in range(15):
            if ra[k] != rb[k]:
                print(f"  r{k:<2}    0x{ra[k]}  !=  0x{rb[k]}")

        print(f"\n  contexto ({len(contexto)} instrucoes antes, ambos identicos):")
        for row in contexto:
            print(f"    #{row[0]:<8} pc=0x{row[1]} opcode=0x{row[4]}")

        print("\n  como cada motor segue depois:")
        print(f"    #{na:<8} interpretado=0x{pca}   recompilado=0x{pcb}")
        for _ in range(DEPOIS - 1):
            pa = next(ia, None)
            pb = next(ib, None)
            if pa is None or pb is None:
                break
            print(f"    #{pa[0]:<8} interpretado=0x{pa[1]}   recompilado=0x{pb[1]}")
        return 1

    print(f"\nsem divergencia nas {n} instrucoes comparadas")
    resto_a = sum(1 for _ in ia)
    resto_b = sum(1 for _ in ib)
    if resto_a or resto_b:
        print(f"(os tracos tem tamanhos diferentes: sobraram {resto_a} vs {resto_b} linhas)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
