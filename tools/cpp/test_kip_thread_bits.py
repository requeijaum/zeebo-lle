#!/usr/bin/env python3
"""
Teste de integração QW23 — KIP[0xc4] (thread_bits) inicializado no build_kip.

Contexto (Passo 13, notes/boot-investigation/okl4-source-reference-map.md):
  O main do Iguana (0xb00033d0) chama thread_init() (0xb00070c8) ANTES do
  bi_execute. thread_init faz:
      min_threadno = (utcb[0] >> 14) + 2         (~131074 com o dummy utcb)
      max_threadno = 1 << KIP[0xc4]              (ldrb — 1 byte: thread_bits)
      rfl_insert_range(min, max)  -> se min > max => ASSERT/panic
  No KIP sintético sem thread_bits, KIP[0xc4]=0 => max = 1<<0 = 1 < min =>
  panic em 0xb0007184 -> hang em 0xb000b1d4. Com KIP[0xc4]=18, 1<<18=262144
  >= min, o panic some e o boot atravessa thread_init rumo ao bi_execute.

  Escopo do QW23 é SÓ o campo do KIP. O próximo bloqueio conhecido (WRITE_PROT
  do memset em 0xb000afe0) é aceito como GREEN — chegar ao thread_init->além
  sem tocar no panic/hang é sucesso.

Regras deste teste:
  - Só observa: bp (panic/hang) + peek de KIP[0xc4]. NÃO faz poke de KIP nem
    força registradores — o campo DEVE vir do build_kip.
  - GREEN = boot passou do thread_init sem hitar panic (0xb0007184) nem hang
    (0xb000b1d4) dentro do timeout, com KIP[0xc4] == 18.

Requer: emulador compilado e a NAND de trabalho em ../../nand/1.1.2.bin.
Exit 0 = PASS.
"""

import os
import sys
import time

BASE_DIR = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, BASE_DIR)

from zeebo_debug_agent import _headless_env, _pick_free_port, EMU_BIN  # noqa: E402
from zeebo_debug_scripting import ZeeboDebugClient  # noqa: E402
import subprocess  # noqa: E402

PANIC_ADDR = 0xb0007184   # ASSERT do thread_init (min>max)
HANG_ADDR = 0xb000b1d4    # loop de hang pós-panic
KIP_BASE = 0xf0f00000
THREAD_BITS_OFF = 0xc4
EXPECTED_BITS = 18
TIMEOUT_S = 22.0

# NOTE (spec-review 2026-09-08): NÃO incluir 0xb000afe0 (memset) nos bps de
# parada. A rotina de memset (0xaf88+) roda nas inits ANTES do thread_init
# (0xb00070c8); um bp ali pausaria o boot cedo e mascararia o panic do
# thread_init em ambos os casos (KIP=0 e KIP=18), tornando o RED fraco. Os
# marcos de RED são o panic (0xb0007184) e o hang (0xb000b1d4); GREEN =
# atravessou o thread_init sem hitar ambos dentro do timeout (timeout
# silencioso = passou além).
_PANIC_HANG = (PANIC_ADDR, HANG_ADDR)

_RESULTS = []


def _check(name, cond, detail=""):
    ok = bool(cond)
    _RESULTS.append((name, ok, detail))
    print(f"[{'PASS' if ok else 'FAIL'}] {name}" + (f" — {detail}" if detail else ""),
          flush=True)
    return ok


def run_boot():
    port = _pick_free_port()
    proc = subprocess.Popen(
        [EMU_BIN, "--headless", f"--control-port={port}"],
        cwd=BASE_DIR, env=_headless_env(),
        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, text=True,
    )
    dbg = ZeeboDebugClient(port=port)
    result = {
        "kip_bits": None, "panic_hit": False, "hang_hit": False,
        "stopped_pc": None, "timed_out": False,
    }
    try:
        dbg.connect(timeout=8.0)
        dbg.ping()

        # Deixa o KIP ser construído; lê o byte thread_bits do build_kip.
        pk = dbg.peek(KIP_BASE + THREAD_BITS_OFF, size=1, core=0)
        result["kip_bits"] = pk.get("val")

        for a in _PANIC_HANG:
            dbg.bp(a, core=0)

        dbg.cont()

        deadline = time.time() + TIMEOUT_S
        stopped_pc = None
        while time.time() < deadline:
            time.sleep(0.1)
            st = dbg.state()
            if not st.get("running"):
                stopped_pc = dbg.reg(core=0, n=15)
                break
        else:
            result["timed_out"] = True

        result["stopped_pc"] = stopped_pc
        if stopped_pc is not None:
            result["panic_hit"] = (stopped_pc == PANIC_ADDR)
            result["hang_hit"] = (stopped_pc == HANG_ADDR)
        return result
    finally:
        try:
            dbg.quit()
        except Exception:
            pass
        try:
            dbg.close()
        except Exception:
            pass
        try:
            proc.wait(timeout=3)
        except Exception:
            proc.terminate()
            try:
                proc.wait(timeout=2)
            except Exception:
                pass


def main():
    _check("emulador compilado", os.path.exists(EMU_BIN), EMU_BIN)
    if not os.path.exists(EMU_BIN):
        return 1

    r = run_boot()
    pc = r["stopped_pc"]
    pc_hex = "None" if pc is None else f"0x{pc:08x}"
    print(f"\n[info] KIP[0xc4]={r['kip_bits']} stopped_pc={pc_hex} "
          f"panic={r['panic_hit']} hang={r['hang_hit']} "
          f"timed_out={r['timed_out']}", flush=True)

    _check("KIP[0xc4] == 18 (thread_bits do build_kip)",
           r["kip_bits"] == EXPECTED_BITS, str(r["kip_bits"]))
    _check("boot NÃO hitou panic do thread_init (0xb0007184)",
           r["panic_hit"] is False, pc_hex)
    _check("boot NÃO hitou hang pós-panic (0xb000b1d4)",
           r["hang_hit"] is False, pc_hex)
    _check("boot atravessou thread_init (timeout sem panic/hang; próximo bloqueio 0xb000afe0 está além)",
           r["panic_hit"] is False and r["hang_hit"] is False, pc_hex)

    passed = sum(1 for _, ok, _ in _RESULTS if ok)
    total = len(_RESULTS)
    print(f"\n==== RESUMO: {passed}/{total} asserções passaram ====", flush=True)
    return 0 if passed == total else 1


if __name__ == "__main__":
    sys.exit(main())
