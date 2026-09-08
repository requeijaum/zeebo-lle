#!/usr/bin/env python3
"""
Testes TDD do QW2 — deduplicação de trace no cliente Python.

Prova determinística de que:
  - PCs repetidos dentro da janela recente são omitidos (loops/CTZ/NOP-slide);
  - o contador de omitidos reflete exatamente as supressões;
  - um `poke` que modifica código executável (SMC) invalida a máscara de PCs,
    tornando aquele PC elegível para reemissão;
  - a integração com ZeeboDebugClient.poke() dispara a invalidação sem quebrar
    o protocolo NDJSON (poke continua retornando a resposta do servidor).

Sem emulador: unidade pura + um fake de socket para a integração.

Uso:
    python3 test_zeebo_trace_dedup.py
Exit 0 = todos passaram.
"""

import json
import os
import sys

BASE_DIR = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, BASE_DIR)

from zeebo_trace_dedup import TraceDeduplicator  # noqa: E402

_RESULTS = []


def _check(name, cond, detail=""):
    ok = bool(cond)
    _RESULTS.append((name, ok, detail))
    print(f"[{'PASS' if ok else 'FAIL'}] {name}" + (f" — {detail}" if detail else ""),
          flush=True)
    return ok


def test_new_pc_always_emitted():
    d = TraceDeduplicator(recent_depth=4)
    emitted = [d.observe(pc) for pc in (0x100, 0x104, 0x108)]
    _check("PCs novos sempre emitidos", emitted == [True, True, True], str(emitted))
    _check("nada omitido em PCs novos", d.omitted == 0, str(d.omitted))


def test_tight_loop_omitted():
    """`b .` no mesmo PC: 1ª emissão, restante omitido; contador exato."""
    d = TraceDeduplicator(recent_depth=4)
    seq = [0x200] * 5
    emitted = [d.observe(pc) for pc in seq]
    _check("tight-loop: só a 1ª emite",
           emitted == [True, False, False, False, False], str(emitted))
    _check("tight-loop: omitidos=4", d.omitted == 4, str(d.omitted))


def test_repeat_within_window_omitted():
    """Loop de corpo pequeno cabe na janela recente e é totalmente suprimido."""
    d = TraceDeduplicator(recent_depth=8)
    body = [0x300, 0x304, 0x308]
    emitted = []
    for _ in range(4):
        for pc in body:
            emitted.append(d.observe(pc))
    # 3 emissões (primeira volta) + 9 omissões (3 voltas x 3).
    _check("loop na janela: 3 emitidos", sum(emitted) == 3, str(sum(emitted)))
    _check("loop na janela: 9 omitidos", d.omitted == 9, str(d.omitted))


def test_pc_falls_out_of_window_reemits():
    """PC visto há muito tempo (fora da janela) é reemitido, não omitido."""
    d = TraceDeduplicator(recent_depth=2)
    # 0xA emitido; janela=2, depois enchemos com 2 PCs distintos -> 0xA sai.
    _check("0xA emite", d.observe(0xA) is True)
    d.observe(0xB)
    d.observe(0xC)  # janela agora [0xB,0xC]; 0xA fora
    reemit = d.observe(0xA)
    _check("0xA reemite fora da janela", reemit is True, str(reemit))


def test_poke_invalidates_pc_makes_eligible():
    """SMC: invalidar o range do PC o torna elegível novamente (reemite)."""
    d = TraceDeduplicator(recent_depth=4)
    d.observe(0x400)
    _check("0x400 repetido é omitido", d.observe(0x400) is False)
    removed = d.invalidate(0x400, size=4)
    _check("invalidate reporta 0x400", 0x400 in removed, str(sorted(removed)))
    _check("0x400 elegível após poke de código", d.observe(0x400) is True)


def test_invalidate_range_only_affects_overlap():
    d = TraceDeduplicator(recent_depth=8)
    for pc in (0x500, 0x504, 0x508, 0x50c):
        d.observe(pc)
    removed = d.invalidate(0x504, size=8)  # cobre 0x504 e 0x508
    _check("invalidate range = {0x504,0x508}",
           removed == {0x504, 0x508}, str(sorted(removed)))
    _check("0x500 continua suprimido", d.observe(0x500) is False)
    _check("0x504 reemite", d.observe(0x504) is True)
    _check("0x508 reemite", d.observe(0x508) is True)


def test_stats_snapshot():
    d = TraceDeduplicator(recent_depth=4)
    for pc in (0x600, 0x600, 0x604, 0x600):
        d.observe(pc)
    s = d.stats()
    _check("stats.emitted", s["emitted"] == 2, str(s))
    _check("stats.omitted", s["omitted"] == 2, str(s))
    _check("stats.unique_pcs", s["unique_pcs"] == 2, str(s))


# ── Integração com ZeeboDebugClient sem emulador (fake socket) ──────────────
class _FakeSock:
    """Socket falso: responde toda RPC com {ok:true} + eco do addr."""
    def __init__(self):
        self._last = {}
    def sendall(self, payload):
        self._last = json.loads(payload.decode("utf-8").strip())
    def recv(self, n):
        resp = {"ok": True, "echo": self._last}
        return (json.dumps(resp) + "\n").encode("utf-8")
    def close(self):
        pass


def test_client_poke_triggers_invalidation():
    from zeebo_debug_scripting import ZeeboDebugClient
    d = TraceDeduplicator(recent_depth=4)
    cli = ZeeboDebugClient()
    cli.sock = _FakeSock()
    cli.attach_dedup(d)

    d.observe(0x700)
    _check("0x700 omitido (repeat) pré-poke", d.observe(0x700) is False)

    resp = cli.poke(0x700, 0x1234, size=4)  # SMC via protocolo real
    _check("poke não quebra protocolo", resp.get("ok") is True, str(resp))
    _check("poke invalidou 0x700", d.observe(0x700) is True)


class _FakeSockFail:
    """Socket falso: responde toda RPC com {ok:false} (poke rejeitado)."""
    def __init__(self):
        self._last = {}
    def sendall(self, payload):
        self._last = json.loads(payload.decode("utf-8").strip())
    def recv(self, n):
        resp = {"ok": False, "error": "rejected"}
        return (json.dumps(resp) + "\n").encode("utf-8")
    def close(self):
        pass


def test_client_poke_failed_does_not_invalidate():
    """poke com ok=false NÃO deve invalidar: PC permanece suprimido."""
    from zeebo_debug_scripting import ZeeboDebugClient
    d = TraceDeduplicator(recent_depth=4)
    cli = ZeeboDebugClient()
    cli.sock = _FakeSockFail()
    cli.attach_dedup(d)

    d.observe(0x900)
    _check("0x900 omitido (repeat) pré-poke", d.observe(0x900) is False)

    resp = cli.poke(0x900, 0x1234, size=4)  # SMC rejeitado pelo servidor
    _check("poke falho retorna ok=false", resp.get("ok") is False, str(resp))
    _check("poke falho NÃO invalida 0x900", d.observe(0x900) is False)


def test_client_without_dedup_is_noop():
    """Sem dedup anexado, poke funciona igual (compatibilidade)."""
    from zeebo_debug_scripting import ZeeboDebugClient
    cli = ZeeboDebugClient()
    cli.sock = _FakeSock()
    resp = cli.poke(0x800, 0x1, size=4)
    _check("poke sem dedup ok", resp.get("ok") is True, str(resp))


def main():
    tests = [
        test_new_pc_always_emitted,
        test_tight_loop_omitted,
        test_repeat_within_window_omitted,
        test_pc_falls_out_of_window_reemits,
        test_poke_invalidates_pc_makes_eligible,
        test_invalidate_range_only_affects_overlap,
        test_stats_snapshot,
        test_client_poke_triggers_invalidation,
        test_client_poke_failed_does_not_invalidate,
        test_client_without_dedup_is_noop,
    ]
    for t in tests:
        print(f"\n=== {t.__name__} ===", flush=True)
        try:
            t()
        except Exception:
            import traceback
            _check(t.__name__, False, "exceção não tratada")
            traceback.print_exc()
    passed = sum(1 for _, ok, _ in _RESULTS if ok)
    total = len(_RESULTS)
    print(f"\n==== RESUMO: {passed}/{total} asserções passaram ====", flush=True)
    return 0 if passed == total else 1


if __name__ == "__main__":
    sys.exit(main())
