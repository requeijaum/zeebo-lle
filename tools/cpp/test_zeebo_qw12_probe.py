#!/usr/bin/env python3
"""
QW12 TDD — fake-client unit tests for the live mempool/bi_execute probe.

Proves, WITHOUT an emulator or socket:
  - ordered evidence capture follows real breakpoint-hit order;
  - dedup suppresses repeated PC records while hit_counts stay exact;
  - malformed / timeout responses are rejected (never fabricate progress);
  - milestone classification returns terminal only when the final target is
    genuinely hit, otherwise "blocked" with the furthest byte-backed milestone.

Run:  python3 test_zeebo_qw12_probe.py   (exit 0 = all passed)
"""

import os
import struct
import sys

BASE_DIR = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, BASE_DIR)

from zeebo_qw12_probe import (  # noqa: E402
    MempoolProbe, EvidenceLog, classify_terminal, is_error_response,
    TARGETS, UTCB_PTR_ADDR, DEFAULT_UTCB, MR_BASE_OFF,
)
from zeebo_trace_dedup import TraceDeduplicator  # noqa: E402

_RESULTS = []


def _check(name, cond, detail=""):
    ok = bool(cond)
    _RESULTS.append((name, ok))
    print(f"[{'PASS' if ok else 'FAIL'}] {name}" + (f" — {detail}" if detail else ""),
          flush=True)
    return ok


class FakeClient:
    """Scriptable stand-in for ZeeboDebugClient.

    ``hit_sequence`` is a list of target addresses the emulator will 'pause' at,
    one per cont(). Register/UTCB reads return deterministic per-address values.
    """

    def __init__(self, hit_sequence, regvals=None, utcb=DEFAULT_UTCB,
                 mrs=None, rpc_map=None, fail_state_after=None):
        self.hit_sequence = list(hit_sequence)
        self.idx = -1
        self.running = True
        self.bps = set()
        self.regvals = regvals or {}
        self.utcb = utcb
        self.mrs = mrs or [0x1111, 0x2222, 0, 0, 0, 0, 0, 0]
        self.rpc_map = rpc_map or {}
        self.fail_state_after = fail_state_after
        self._state_calls = 0

    def bp(self, addr, core=0):
        self.bps.add(addr)
        return {"ok": True}

    def bpclear(self, addr, core=0):
        self.bps.discard(addr)
        return {"ok": True}

    def cont(self):
        self.idx += 1
        self.running = False  # pauses immediately at the next scripted hit
        return {"ok": True}

    def step(self, core=0, ticks=1):
        self.running = True   # step off breakpoint
        return {"ok": True}

    def state(self):
        self._state_calls += 1
        if self.fail_state_after is not None and self._state_calls > self.fail_state_after:
            return {"ok": False, "error": "timeout"}
        return {"ok": True, "running": self.running,
                "cycle": self._state_calls}

    def reg(self, core=0, n=15):
        if n == 15:
            if 0 <= self.idx < len(self.hit_sequence):
                return self.hit_sequence[self.idx]
            return 0
        return self.regvals.get(n, 0x1000 + n)

    def read_mem(self, core=0, addr=0, length=16):
        if addr == UTCB_PTR_ADDR:
            return struct.pack("<I", self.utcb)
        if addr == self.utcb + MR_BASE_OFF:
            return struct.pack("<8I", *self.mrs[:8])
        return b"\x00" * length

    def rpc(self, cmd):
        key = cmd.get("cmd")
        if key == "probe.get":
            key = f"probe.get:{cmd.get('probe')}"
        return self.rpc_map.get(key, {"ok": False, "error": "not_scripted"})


# ── classification (pure) ──────────────────────────────────────────────────
def test_classify_no_progress():
    v = classify_terminal([])
    _check("no hits -> no_progress", v["status"] == "no_progress" and v["terminal"] is None)


def test_classify_blocked_partial():
    v = classify_terminal(["mempool_init", "decomposition"])
    _check("partial -> blocked", v["status"] == "blocked")
    _check("terminal = furthest byte-backed", v["terminal"] == "decomposition", v["terminal"])
    _check("missing lists remaining", "loop" in v["missing"])


def test_classify_reached_terminal():
    names = [n for n, _ in TARGETS]
    v = classify_terminal(names)
    _check("all hits -> reached_terminal", v["status"] == "reached_terminal")
    _check("terminal = loop", v["terminal"] == "loop", v["terminal"])


def test_classify_ignores_unknown():
    v = classify_terminal(["bogus", "mempool_init"])
    _check("unknown names ignored", v["reached"] == ["mempool_init"], str(v["reached"]))


def test_classify_out_of_order_hits():
    """Furthest reached is by chain index, not arrival order."""
    v = classify_terminal(["bi_execute", "mempool_init"])
    _check("furthest by chain index not arrival", v["terminal"] == "bi_execute", v["terminal"])


# ── malformed / timeout ─────────────────────────────────────────────────────
def test_is_error_response():
    _check("None is error", is_error_response(None))
    _check("list is error", is_error_response([1, 2]))
    _check("ok:false is error", is_error_response({"ok": False}))
    _check("error field is error", is_error_response({"error": "x"}))
    _check("ok:true not error", not is_error_response({"ok": True, "v": 1}))


def test_timeout_state_does_not_fabricate_progress():
    # state() fails after first call -> _wait_paused returns None, run aborts,
    # no records fabricated.
    fc = FakeClient([0xb000d5b4], fail_state_after=0)
    probe = MempoolProbe(fc, timeout_s=1.0, poll_interval=0.001)
    report = probe.run()
    _check("timeout -> no fabricated records",
           len(report["evidence"]["records"]) == 0, str(report["evidence"]["records"]))
    _check("timeout recorded as error", any("error" in e for e in report["errors"]),
           str(report["errors"]))


# ── ordering + dedup + evidence ─────────────────────────────────────────────
def test_ordered_evidence_capture():
    seq = [0xb000d5b4, 0xb000d4dc, 0xb00001fc, 0xb00017b8, 0xb000aa94]
    fc = FakeClient(seq, regvals={0: 0xdead, 4: 0xb0d00000, 7: 0x7},
                    mrs=[0xb0d00146, 0x00000014, 0, 0, 0, 0, 0, 0])
    probe = MempoolProbe(fc, timeout_s=2.0, poll_interval=0.001)
    report = probe.run()
    names = [r["name"] for r in report["evidence"]["records"]]
    _check("evidence in hit order",
           names == ["mempool_init", "decomposition", "bi_execute",
                     "extensions_init", "loop"], str(names))
    _check("terminal loop reached", report["verdict"]["status"] == "reached_terminal")
    rec0 = report["evidence"]["records"][0]
    _check("captures r0/r4/r7", rec0["regs"]["r4"] == 0xb0d00000 and rec0["regs"]["r7"] == 0x7)
    _check("captures fpage/phys_desc bytes",
           rec0["utcb"]["phys_desc"] == 0xb0d00146 and rec0["utcb"]["fpage"] == 0x14,
           str(rec0["utcb"]))
    _check("captures mr_bytes hex", rec0["utcb"]["mr_bytes"] is not None)


def test_dedup_suppresses_repeats_but_counts_exact():
    # stuck_add hit 3 times, then loop. Dedup should emit stuck_add once but
    # count it 3 times.
    seq = [0xb000d6dc, 0xb000d6dc, 0xb000d6dc, 0xb000aa94]
    fc = FakeClient(seq)
    probe = MempoolProbe(fc, dedup=TraceDeduplicator(recent_depth=8),
                         timeout_s=2.0, poll_interval=0.001)
    report = probe.run()
    ev = report["evidence"]
    stuck_records = [r for r in ev["records"] if r["name"] == "stuck_add"]
    _check("dedup emits stuck_add once", len(stuck_records) == 1, str(len(stuck_records)))
    _check("hit_counts exact (3)", ev["hit_counts"]["stuck_add"] == 3,
           str(ev["hit_counts"]))
    _check("loop still recorded", any(r["name"] == "loop" for r in ev["records"]))


def test_unexpected_pause_is_blocked_not_pass():
    seq = [0xb000d5b4, 0xdeadbeef]  # second pause is off-target
    fc = FakeClient(seq)
    probe = MempoolProbe(fc, timeout_s=2.0, poll_interval=0.001)
    report = probe.run()
    _check("unexpected pause -> not terminal",
           report["verdict"]["status"] == "blocked", report["verdict"]["status"])
    _check("unexpected pause logged as error",
           any("unexpected_pause" in e for e in report["errors"]), str(report["errors"]))


def test_evidencelog_ordered_names_unique():
    log = EvidenceLog()
    log.add("a", 1, {}); log.add("b", 2, {}); log.add("a", 1, {})
    _check("ordered_names unique first-seen", log.ordered_names() == ["a", "b"],
           str(log.ordered_names()))


def test_snapshot_probes_rejects_malformed():
    rpc_map = {
        "probe.list": {"ok": True, "probes": ["mmu", "bootinfo"]},
        "probe.get:mmu": {"ok": True, "min_page_log2": 12},
        "probe.get:bootinfo": {"ok": False, "error": "boom"},
    }
    fc = FakeClient([], rpc_map=rpc_map)
    probe = MempoolProbe(fc)
    snap = probe.snapshot_probes()
    _check("good probe captured", "mmu" in snap and snap["mmu"]["min_page_log2"] == 12)
    _check("malformed probe rejected", "bootinfo" not in snap, str(snap.keys()))
    _check("malformed probe logged", any("bootinfo" in e for e in probe.errors))


def main():
    for fn in list(globals().values()):
        if callable(fn) and getattr(fn, "__name__", "").startswith("test_"):
            fn()
    passed = sum(1 for _, ok in _RESULTS if ok)
    total = len(_RESULTS)
    print(f"\n{passed}/{total} checks passed")
    sys.exit(0 if passed == total else 1)


if __name__ == "__main__":
    main()
