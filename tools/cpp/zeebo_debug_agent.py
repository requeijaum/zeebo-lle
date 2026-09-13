#!/usr/bin/env python3
"""
Zeebo LLE Autonomous Debug Agent Harness
========================================

CLI/harness autônomo para um agente de IA orquestrar diagnósticos e testes de
boot em lote contra o emulador `zeebo_lle_main`, consumindo as primitivas TCP
NDJSON do ControlServer (`zeebo_control_server.h`):

  - peek(addr,size,core)  : inspeção de memória
  - poke(addr,val,size)   : escrita de memória (auto-invalida cache JIT)
  - backtrace(core)       : unwinding de PC/LR/SP/FP + walk de pilha
  - vram_stat()           : telemetria de VRAM (width/height/format/pixel_sum/center/blank)
  - state / step / cont   : controle de execução

Modos de uso (CLI):

  # Sobe um emulador de boot, drena telemetria e sai:
  zeebo_debug_agent.py --launch --vram
  zeebo_debug_agent.py --launch --trace --core 0
  zeebo_debug_agent.py --launch --peek 0xb0000000 --size 4
  zeebo_debug_agent.py --launch --poke 0x00a1d73c --val 0x0f --size 4

  # Conecta a um emulador já em execução (--control-port=PORTA):
  zeebo_debug_agent.py --host 127.0.0.1 --port 48998 --vram

  # Roda um único applet do catálogo end-to-end e emite relatório JSON:
  zeebo_debug_agent.py --run-app=274755
  zeebo_debug_agent.py --run-app=reksio.mod --steps 200

  # Roda TODO o catálogo de applets EFS2 em lote e emite relatório estruturado:
  zeebo_debug_agent.py --test-catalog --report /tmp/zeebo_catalog.json

O harness é headless-safe (SDL_VIDEODRIVER=dummy) e nunca trava: toda RPC tem
timeout e todo subprocesso tem watchdog. Projetado para ser dirigido por um
agente de IA sem intervenção humana.
"""

import argparse
import json
import os
import re
import socket
import subprocess
import sys
import threading
import time

BASE_DIR = os.path.dirname(os.path.abspath(__file__))
EMU_BIN = os.path.join(BASE_DIR, "zeebo_lle_main")

# Importa o cliente RPC síncrono já existente (camada NDJSON).
sys.path.insert(0, BASE_DIR)
from zeebo_debug_scripting import ZeeboDebugClient  # noqa: E402


# ─── Catálogo de applets EFS2 a testar ──────────────────────────────────────
# mode:
#   "lifecycle"   -> lifecycle-only probe. Current EFS2 catalog entries are
#                    rejection controls until inode→cluster provenance exists.
#   "interleaved" -> attempts injection and continues run_interleaved() so the
#                    agent can prove clean refusal, core progress and no forged
#                    execution via live ControlServer telemetry.
CATALOG = [
    {"id": "274755",     "mode": "lifecycle",   "desc": "Z-Wheel candidate; gnode provenance unresolved"},
    {"id": "reksio.mod", "mode": "interleaved", "desc": "Reksio candidate; old KnownIB was modem/NV data"},
    {"id": "tectoy.mod", "mode": "interleaved", "desc": "TecToy candidate; old KnownIB was non-code data"},
]


def _headless_env():
    env = dict(os.environ)
    env.setdefault("SDL_VIDEODRIVER", "dummy")
    return env


def _pick_free_port():
    s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    s.bind(("127.0.0.1", 0))
    port = s.getsockname()[1]
    s.close()
    return port


class _StreamCollector:
    """Coleta stdout de um subprocesso em uma thread sem bloquear."""

    def __init__(self, stream):
        self.lines = []
        self._stream = stream
        self._t = threading.Thread(target=self._run, daemon=True)
        self._t.start()

    def _run(self):
        try:
            for line in iter(self._stream.readline, ""):
                if line == "":
                    break
                self.lines.append(line.rstrip("\n"))
        except Exception:
            pass

    def text(self):
        return "\n".join(self.lines)

    def join(self, timeout=2.0):
        self._t.join(timeout)


class ZeeboAgent:
    """Fachada autônoma sobre ZeeboDebugClient + gestão de subprocessos."""

    def __init__(self, host="127.0.0.1", port=48998, verbose=True):
        self.host = host
        self.port = port
        self.verbose = verbose
        self.proc = None
        self.collector = None
        self.dbg = None

    def _log(self, msg):
        if self.verbose:
            print(msg, file=sys.stderr, flush=True)

    # ── Ciclo de vida do emulador ────────────────────────────────────────
    def launch(self, extra_args=None, connect_timeout=8.0):
        if not os.path.exists(EMU_BIN):
            raise FileNotFoundError(
                f"Emulador não compilado: {EMU_BIN} "
                f"(rode: make -C tools/cpp zeebo_lle_main)"
            )
        args = [EMU_BIN, "--headless", f"--control-port={self.port}"]
        if extra_args:
            args += extra_args
        self._log(f"[agent] launching: {' '.join(args)}")
        self.proc = subprocess.Popen(
            args, cwd=BASE_DIR, env=_headless_env(),
            stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, text=True,
        )
        self.collector = _StreamCollector(self.proc.stdout)
        self.dbg = ZeeboDebugClient(host=self.host, port=self.port)
        self.dbg.connect(timeout=connect_timeout)
        self.dbg.ping()
        return self.dbg

    def connect(self, connect_timeout=8.0):
        self.dbg = ZeeboDebugClient(host=self.host, port=self.port)
        self.dbg.connect(timeout=connect_timeout)
        self.dbg.ping()
        return self.dbg

    def shutdown(self, quit_emu=True):
        if self.dbg:
            if quit_emu:
                try:
                    self.dbg.quit()
                except Exception:
                    pass
            try:
                self.dbg.close()
            except Exception:
                pass
            self.dbg = None
        if self.proc:
            try:
                self.proc.wait(timeout=3)
            except Exception:
                self.proc.terminate()
                try:
                    self.proc.wait(timeout=2)
                except subprocess.TimeoutExpired:
                    # SIGTERM is the final escalation; do not SIGKILL because it
                    # bypasses emulator cleanup and can leave output/state corrupt.
                    self._log("[agent] emulator did not exit after SIGTERM")
        if self.collector:
            self.collector.join(timeout=2)

    # ── Primitivas de diagnóstico ────────────────────────────────────────
    def vram(self):
        return self.dbg.vram_stat()

    def trace(self, core=0):
        return self.dbg.backtrace(core=core)

    def peek(self, addr, size=4, core=0):
        return self.dbg.peek(addr, size=size, core=core)

    def poke(self, addr, val, size=4, core=0):
        return self.dbg.poke(addr, val, size=size, core=core)

    def status_regs(self, core=0):
        """Registradores de status (r0-r3, sp, lr, pc, cpsr) para diagnóstico."""
        names = {0: "r0", 1: "r1", 2: "r2", 3: "r3",
                 13: "sp", 14: "lr", 15: "pc", 16: "cpsr"}
        out = {}
        for n, nm in names.items():
            try:
                out[nm] = self.dbg.reg(core=core, n=n)
            except Exception:
                out[nm] = None
        return out


# ─── Runner de um único applet do catálogo ──────────────────────────────────
_FB_SUM_RE = re.compile(r"framebuffer soma antes=\d+ depois=(\d+)")
_LIFE_PASS_RE = re.compile(r"Z-Wheel/Life\] PASS")
_INJECT_RE = re.compile(r"injetado @?0x[0-9a-fA-F]+.*dispatch BREW armado|injetado em 0x[0-9a-fA-F]+")


def run_applet(entry, steps=200, verbose=True):
    """Executa um applet do catálogo e retorna um dict de status estruturado."""
    app_id = entry["id"]
    mode = entry["mode"]
    port = _pick_free_port()
    report = {
        "applet": app_id,
        "mode": mode,
        "desc": entry.get("desc", ""),
        "loaded": False,
        "executed": False,
        "core_progress": False,
        "vram": None,
        "vram_blank": None,
        "backtrace": None,
        "status_regs": None,
        "steps_advanced": 0,
        "status": "unknown",
        "notes": [],
    }

    if mode == "lifecycle":
        # Z-Wheel: roda o ciclo de vida (EVT_APP_START) por 1s real; a prova de
        # pixels vem do stdout estruturado do emulador (framebuffer soma > 0).
        args = [EMU_BIN, "--headless", f"--efs2-run={app_id}", "--seconds=1"]
        try:
            proc = subprocess.run(
                args, cwd=BASE_DIR, env=_headless_env(),
                capture_output=True, text=True, timeout=60,
            )
            out = proc.stdout
        except subprocess.TimeoutExpired as e:
            report["status"] = "timeout"
            report["notes"].append(f"subprocess timeout: {e}")
            return report

        report["loaded"] = "injetado" in out or "extraído" in out
        m = _FB_SUM_RE.search(out)
        life_pass = bool(_LIFE_PASS_RE.search(out))
        report["executed"] = life_pass or bool(m)
        if m:
            fb_sum = int(m.group(1))
            report["vram"] = {"pixel_sum": fb_sum, "source": "lifecycle_stdout"}
            report["vram_blank"] = (fb_sum == 0)
            report["notes"].append(f"framebuffer pixel_sum={fb_sum}")
        if life_pass:
            report["notes"].append("Z-Wheel/Life PASS (EVT_APP_START r0=1)")
        report["status"] = "pass" if (report["loaded"] and report["executed"]
                                      and report["vram_blank"] is False) else "fail"
        return report

    # mode == "interleaved": injeta o applet e drena o ControlServer ao vivo.
    agent = ZeeboAgent(port=port, verbose=verbose)
    try:
        # --cycles alto: com o ControlServer ativo o loop permanece vivo até
        # receber 'quit', então isso apenas garante que não saia sozinho.
        agent.launch(extra_args=[f"--efs2-run={app_id}", "--cycles=100000000"])
        # Verificação de carga via TCP (robusta): o applet é injetado em
        # 0x12000000 pelo BrewLoader. Poll até a memória lá ficar não-nula, para
        # tolerar a latência de injeção sob carga (não dependemos do stdout, que
        # é block-buffered quando redirecionado).
        loaded_via_mem = False
        deadline = time.time() + 8.0
        while time.time() < deadline and not loaded_via_mem:
            # Varre offsets do payload (0x0, 0x200, etc.) pois alguns applets têm cabeçalho nulo nos primeiros bytes
            for off in (0, 4, 8, 12, 16, 0x200, 0x400):
                try:
                    r = agent.peek(0x12000000 + off, size=4, core=0)
                except Exception:
                    r = {}
                if r.get("ok") and r.get("val", 0) != 0:
                    loaded_via_mem = True
                    break
            if not loaded_via_mem:
                time.sleep(0.25)
        out = agent.collector.text()
        report["loaded"] = loaded_via_mem or ("injetado" in out) or bool(_INJECT_RE.search(out))

        # Avança execução em passos determinísticos.
        st0 = agent.dbg.state()
        agent.dbg.cont()
        for _ in range(max(1, steps // 20)):
            agent.dbg.step(core=0, ticks=20)
        report["steps_advanced"] = max(1, steps // 20) * 20
        st1 = agent.dbg.state()
        report["core_progress"] = (st1.get("c0_insns", 0) > st0.get("c0_insns", 0))
        # Core 0 progressing only proves that Iguana/APPS ran. It does not prove
        # that the injected module's entry point executed. Keep this false until
        # an applet-specific PC/byte/pixel milestone is observed.
        report["executed"] = False

        vram = agent.vram()
        report["vram"] = vram
        report["vram_blank"] = vram.get("blank", None)
        report["backtrace"] = agent.trace(core=0)
        report["status_regs"] = agent.status_regs(core=0)

        # Injection is a verified loading milestone, not applet execution.
        if report["loaded"] and report["core_progress"]:
            report["status"] = "loaded_only"
            report["notes"].append(
                "payload injected; applet entry execution not yet proven")
            if report["vram_blank"]:
                report["notes"].append("VRAM blank (no applet render milestone)")
        else:
            report["status"] = "fail"
        return report
    except Exception as e:
        report["status"] = "error"
        report["notes"].append(f"{type(e).__name__}: {e}")
        return report
    finally:
        agent.shutdown()


def run_catalog(steps=200, report_path=None, verbose=True):
    results = [run_applet(e, steps=steps, verbose=verbose) for e in CATALOG]
    summary = {
        "tool": "zeebo_debug_agent",
        "timestamp": time.strftime("%Y-%m-%dT%H:%M:%S"),
        "total": len(results),
        "passed": sum(1 for r in results if r["status"] == "pass"),
        "loaded_only": sum(1 for r in results if r["status"] == "loaded_only"),
        "results": results,
    }
    if report_path:
        with open(report_path, "w") as f:
            json.dump(summary, f, indent=2)
    return summary


# ─── CLI ────────────────────────────────────────────────────────────────────
def _parse_int(s):
    return int(s, 0)


def build_parser():
    p = argparse.ArgumentParser(
        description="Harness autônomo de debug do Zeebo LLE para agentes de IA.")
    p.add_argument("--host", default="127.0.0.1")
    p.add_argument("--port", type=int, default=48998,
                   help="Porta do ControlServer (--control-port do emulador).")
    p.add_argument("--launch", action="store_true",
                   help="Sobe um emulador de boot e conecta (senão conecta a um já ativo).")
    p.add_argument("--steps", type=int, default=200,
                   help="Passos/ciclos a avançar por applet (catálogo/run-app).")

    p.add_argument("--peek", type=_parse_int, metavar="ADDR")
    p.add_argument("--poke", type=_parse_int, metavar="ADDR")
    p.add_argument("--val", type=_parse_int, default=0)
    p.add_argument("--size", type=int, default=4)
    p.add_argument("--core", type=int, default=0)
    p.add_argument("--trace", action="store_true")
    p.add_argument("--vram", action="store_true")

    p.add_argument("--run-app", dest="run_app", metavar="ID_OU_NOME")
    p.add_argument("--test-catalog", action="store_true")
    p.add_argument("--report", metavar="PATH", help="Salva relatório JSON.")
    p.add_argument("--quiet", action="store_true")
    return p


def main(argv=None):
    args = build_parser().parse_args(argv)
    verbose = not args.quiet

    # Modos que gerenciam o próprio subprocesso.
    if args.test_catalog:
        summary = run_catalog(steps=args.steps, report_path=args.report, verbose=verbose)
        print(json.dumps(summary, indent=2))
        return 0 if summary["passed"] == summary["total"] else 1

    if args.run_app:
        entry = next((e for e in CATALOG if e["id"] == args.run_app), None)
        if entry is None:
            entry = {"id": args.run_app, "mode": "interleaved", "desc": "custom"}
        rep = run_applet(entry, steps=args.steps, verbose=verbose)
        print(json.dumps(rep, indent=2))
        return 0 if rep["status"] == "pass" else 1

    # Modos de probe direto (peek/poke/trace/vram).
    agent = ZeeboAgent(host=args.host, port=args.port, verbose=verbose)
    launched = False
    try:
        if args.launch:
            agent.launch()
            agent.dbg.cont()
            time.sleep(0.4)
            launched = True
        else:
            agent.connect()

        out = {}
        if args.peek is not None:
            out["peek"] = agent.peek(args.peek, size=args.size, core=args.core)
        if args.poke is not None:
            out["poke"] = agent.poke(args.poke, args.val, size=args.size, core=args.core)
        if args.trace:
            out["backtrace"] = agent.trace(core=args.core)
        if args.vram:
            out["vram"] = agent.vram()
        if not out:
            out["state"] = agent.dbg.state()
        print(json.dumps(out, indent=2))
        return 0
    finally:
        agent.shutdown(quit_emu=launched)


if __name__ == "__main__":
    sys.exit(main())
