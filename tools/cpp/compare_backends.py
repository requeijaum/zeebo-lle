#!/usr/bin/env python3
"""Compara a trajetoria do Core0 entre o backend interpretado e o recompilado
durante o boot, para localizar o primeiro ciclo em que divergem.

Uso: python3 compare_backends.py [ciclos]

Somente leitura: roda duas instancias independentes com --cycles e compara os
PCs relatados no log periodico. Nao altera firmware, ROMs nem estado do repo.
"""
import re
import subprocess
import sys
import os

HERE = os.path.dirname(os.path.abspath(__file__))
EMU = os.path.join(HERE, "zeebo_lle_main")
LINE = re.compile(r"\[Cycle (\d+)\] Core0\(ARM11\): pc=0x([0-9a-f]+) insns=(\d+)")


def trace(jit, cycles):
    args = [EMU, f"--cycles={cycles}", "--headless"]
    if jit:
        args.insert(1, "--jit")
    env = dict(os.environ, SDL_VIDEODRIVER="dummy", SDL_AUDIODRIVER="dummy")
    out = subprocess.run(args, capture_output=True, text=True, timeout=900, env=env).stdout
    return {int(c): (int(pc, 16), int(n)) for c, pc, n in LINE.findall(out)}


def main():
    cycles = int(sys.argv[1]) if len(sys.argv) > 1 else 1500
    a = trace(False, cycles)
    b = trace(True, cycles)

    common = sorted(set(a) & set(b))
    if not common:
        print("sem ciclos comparaveis nos dois backends")
        return 1

    print(f"{'ciclo':>6}  {'interpretado':>12}  {'recompilado':>12}  igual")
    first_div = None
    for c in common:
        pa, pb = a[c][0], b[c][0]
        same = pa == pb
        if not same and first_div is None:
            first_div = c
        if c <= 40 or not same:
            print(f"{c:>6}  0x{pa:08x}    0x{pb:08x}   {'sim' if same else 'NAO'}")
        if first_div is not None and c > first_div + 20:
            print("  ...")
            break

    print()
    if first_div is None:
        print("trajetorias identicas nos ciclos comparados")
    else:
        pa, pb = a[first_div][0], b[first_div][0]
        prev = [c for c in common if c < first_div]
        print(f"primeira divergencia: ciclo {first_div}")
        if prev:
            p = prev[-1]
            print(f"  ultimo ciclo igual : {p} pc=0x{a[p][0]:08x}")
        print(f"  interpretado       : pc=0x{pa:08x}")
        print(f"  recompilado        : pc=0x{pb:08x}")

    last = common[-1]
    print(f"\nfim (ciclo {last}): interpretado insns={a[last][1]} pc=0x{a[last][0]:08x}")
    print(f"                    recompilado  insns={b[last][1]} pc=0x{b[last][0]:08x}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
