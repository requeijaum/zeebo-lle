#!/usr/bin/env python3
"""
Testes automatizados do harness autônomo `zeebo_debug_agent.py`.

Garante que um agente de IA consiga rodar o harness de ponta a ponta sem travar
ou falhar: sobe o emulador real (headless/dummy), exercita as primitivas TCP do
ControlServer (peek/poke/backtrace/vram_stat) e roda o catálogo EFS2 completo,
validando a estrutura do relatório JSON.

Uso:
    python3 test_zeebo_debug_agent.py            # roda todos os testes
Exit code 0 = todos passaram; !=0 = alguma falha.

Requer: emulador compilado (make -C tools/cpp zeebo_lle_main) e a NAND de
trabalho em ../../nand/1.1.2.bin.
"""

import json
import os
import sys
import time
import traceback

BASE_DIR = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, BASE_DIR)

import zeebo_debug_agent as agent_mod  # noqa: E402
from zeebo_debug_agent import ZeeboAgent, run_applet, run_catalog, CATALOG  # noqa: E402

_RESULTS = []


def _check(name, cond, detail=""):
    ok = bool(cond)
    _RESULTS.append((name, ok, detail))
    print(f"[{'PASS' if ok else 'FAIL'}] {name}" + (f" — {detail}" if detail else ""),
          flush=True)
    return ok


def test_binary_present():
    _check("emulador compilado", os.path.exists(agent_mod.EMU_BIN),
           agent_mod.EMU_BIN)


def test_primitives_live():
    """Sobe o emulador e exercita ping/state/vram/peek/poke/backtrace por TCP."""
    a = ZeeboAgent(port=agent_mod._pick_free_port(), verbose=False)
    try:
        dbg = a.launch()
        _check("ping responde", dbg.ping().get("ok") is True)
        dbg.cont()
        time.sleep(0.4)

        st = dbg.state()
        _check("state ok", st.get("ok") is True and "c0_pc" in st)

        vram = a.vram()
        _check("vram_stat estrutura",
               vram.get("ok") is True and vram.get("width") == 640
               and vram.get("height") == 480 and vram.get("format") == "RGB565",
               f"{vram.get('width')}x{vram.get('height')} {vram.get('format')}")

        pk = a.peek(0xb0000000, size=4, core=0)
        _check("peek ok", pk.get("ok") is True and "val" in pk,
               hex(pk.get("val", 0)))

        # poke em RAM segura + read-back.
        wr = a.poke(0x00a1d000, 0xdeadbeef, size=4, core=1)
        rb = a.peek(0x00a1d000, size=4, core=1)
        _check("poke+readback", wr.get("ok") is True and rb.get("val") == 0xdeadbeef,
               hex(rb.get("val", 0)))

        bt = a.trace(core=0)
        _check("backtrace frames",
               bt.get("ok") is True and isinstance(bt.get("frames"), list)
               and len(bt["frames"]) >= 1,
               f"{len(bt.get('frames', []))} frames")

        # Regression: stepping an intentional ARM self-loop must not forge PC+4.
        dbg.pause()
        loop_addr = 0x00a1d000
        a.poke(loop_addr, 0xeafffffe, size=4, core=1)  # b .
        dbg.setreg(core=1, n=15, val=loop_addr)
        step = dbg.step(core=1, ticks=1)
        loop_pc = dbg.reg(core=1, n=15)
        _check("step preserva self-loop real",
               step.get("ok") is True and loop_pc == loop_addr,
               hex(loop_pc))

        bad_reg = a.dbg.rpc({"cmd": "reg", "core": 0, "n": 99})
        bad_hex = a.dbg.rpc({"cmd": "write", "core": 0,
                             "addr": 0x10000000, "hex": "xyz"})
        _check("reg inválido rejeitado", bad_reg.get("ok") is False)
        _check("hex inválido rejeitado", bad_hex.get("ok") is False)
    finally:
        a.shutdown()


def test_run_app_zwheel():
    rep = run_applet(CATALOG[0], steps=50, verbose=False)
    _check("274755 status pass", rep["status"] == "pass", rep["status"])
    _check("274755 pixels gerados", rep["vram_blank"] is False,
           str(rep.get("vram")))


def test_run_app_interleaved():
    rep = run_applet(
        {"id": "reksio.mod", "mode": "interleaved", "desc": "test"},
        steps=40, verbose=False)
    _check("reksio.mod carregado", rep["loaded"] is True)
    _check("reksio.mod execução não é forjada", rep["executed"] is False)
    _check("reksio.mod core progrediu", rep["core_progress"] is True)
    _check("reksio.mod status loaded_only", rep["status"] == "loaded_only", rep["status"])
    _check("reksio.mod backtrace presente",
           rep["backtrace"] is not None and rep["backtrace"].get("ok") is True)


def test_catalog_report(tmp_report="/tmp/zeebo_agent_test_report.json"):
    summary = run_catalog(steps=40, report_path=tmp_report, verbose=False)
    _check("catálogo total=3", summary["total"] == 3, str(summary["total"]))
    _check("catálogo distingue execução de mera carga",
           summary["passed"] == 1 and summary["loaded_only"] == 2,
           f"pass={summary['passed']} loaded_only={summary['loaded_only']}")
    _check("relatório JSON gravado", os.path.exists(tmp_report))
    # Valida que o arquivo é JSON válido e reflete os resultados.
    with open(tmp_report) as f:
        loaded = json.load(f)
    _check("relatório JSON íntegro",
           loaded.get("total") == 3 and len(loaded.get("results", [])) == 3)


def test_no_hang_on_bad_app():
    """Applet inexistente NÃO deve travar; deve retornar status fail/error limpo."""
    rep = run_applet(
        {"id": "nao_existe_xyz.mod", "mode": "interleaved", "desc": "bad"},
        steps=20, verbose=False)
    _check("applet inválido não trava",
           rep["status"] in ("fail", "error"), rep["status"])


def main():
    tests = [
        test_binary_present,
        test_primitives_live,
        test_run_app_zwheel,
        test_run_app_interleaved,
        test_catalog_report,
        test_no_hang_on_bad_app,
    ]
    for t in tests:
        print(f"\n=== {t.__name__} ===", flush=True)
        try:
            t()
        except Exception:
            _check(t.__name__, False, "exceção não tratada")
            traceback.print_exc()

    passed = sum(1 for _, ok, _ in _RESULTS if ok)
    total = len(_RESULTS)
    print(f"\n==== RESUMO: {passed}/{total} asserções passaram ====", flush=True)
    return 0 if passed == total else 1


if __name__ == "__main__":
    sys.exit(main())
