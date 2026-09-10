#!/usr/bin/env python3
"""Compara o caminho de LEITURA de memoria dos dois backends para um endereco.

Contexto: a primeira divergencia do boot (#23726) e um `ldreq r3,[r5]` com
r5 = 0xf401ffc0. Os dois motores executam a MESMA instrucao, no mesmo PC, com
as mesmas flags, e leem o MESMO endereco — mas obtem valores diferentes
(0x10090001 no interpretado, 0 no recompilado). Logo divergem no CONTEUDO da
memoria, nao na semantica da instrucao.

Este script nao adivinha: instrumenta o emulador com a sonda `peek` do servidor
de controle e compara o que cada backend enxerga no mesmo endereco.
"""

import argparse
import json
import socket
import subprocess
import sys
import time


def peek(sock_file, addr, size=4, core=0):
    req = {"cmd": "peek", "core": core, "i0": addr, "i1": size}
    sock_file.write(json.dumps(req) + "\n")
    sock_file.flush()
    return json.loads(sock_file.readline())


def run_backend(binary, jit, addr, cycles, port):
    """Sobe o emulador, roda ate `cycles` e le `addr`. Devolve (valor, erro)."""
    cmd = [binary, "--headless", f"--cycles={cycles}", f"--control-port={port}"]
    if jit:
        cmd.append("--jit")
    proc = subprocess.Popen(cmd, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    try:
        # Espera o servidor de controle subir.
        deadline = time.time() + 30
        sock = None
        while time.time() < deadline:
            try:
                sock = socket.create_connection(("127.0.0.1", port), timeout=3)
                break
            except OSError:
                time.sleep(0.5)
        if sock is None:
            return None, "servidor de controle nao subiu"

        with sock:
            f = sock.makefile("rw")
            resp = peek(f, addr)
            return resp.get("val"), resp.get("err")
    finally:
        proc.terminate()
        try:
            proc.wait(timeout=10)
        except subprocess.TimeoutExpired:
            proc.kill()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--binary", default="./zeebo_lle_main")
    ap.add_argument("--addr", default="0xf401ffc0")
    ap.add_argument("--cycles", type=int, default=300)
    ap.add_argument("--port", type=int, default=46100)
    args = ap.parse_args()

    addr = int(args.addr, 0)
    print(f"endereco sob investigacao: 0x{addr:08x}")
    print(f"(divergencia #23726: ldreq r3,[r5] com r5 = 0x{addr:08x})\n")

    uni_val, uni_err = run_backend(args.binary, False, addr, args.cycles, args.port)
    jit_val, jit_err = run_backend(args.binary, True, addr, args.cycles, args.port + 1)

    def fmt(v, e):
        if v is None:
            return f"<sem valor: {e}>"
        return f"0x{v:08x}" + (f"  (err={e})" if e else "")

    print(f"  interpretado: {fmt(uni_val, uni_err)}")
    print(f"  recompilado : {fmt(jit_val, jit_err)}")
    print()

    if uni_val is None or jit_val is None:
        print("RESULTADO: leitura indisponivel em pelo menos um backend.")
        return 2
    if uni_val == jit_val:
        print("RESULTADO: os dois backends enxergam o MESMO conteudo neste endereco.")
        print("A divergencia entao NAO esta no conteudo estatico da memoria —")
        print("investigar o momento da leitura (quem escreve antes, e quando).")
        return 0
    print("RESULTADO: os backends enxergam CONTEUDOS DIFERENTES no mesmo endereco.")
    print("Confirma divergencia de estado de memoria, nao de semantica da instrucao.")
    return 1


if __name__ == "__main__":
    sys.exit(main())
