#!/usr/bin/env python3
"""
QW12 — Live Python mempool / bi_execute probe for the Zeebo LLE emulator.

Observes the Core 0 Iguana boot path around a fixed, byte-anchored set of
targets using ONLY the existing ControlServer primitives exposed by
``ZeeboDebugClient`` (bp/bpclear, pause/cont/step, reg, peek/read_mem,
probe.list/probe.get) plus the client-side TraceDeduplicator (QW2). It adds no
ad-hoc C++ tracing, never forces/patches registers, and never validates by
instruction count — progress is proven only by real breakpoint hits and the
register/byte evidence captured at each hit.

The module is split into a *pure* core (ordering, dedup, milestone
classification, malformed/timeout handling) that is unit-testable with a fake
client and no socket, and a thin live driver that wires a real
``ZeeboDebugClient`` and emitted breakpoints to that core.

Targets (byte-anchored, from firmware RE — see zeebo-lle skill):
    mempool_init     0xb000d5b4
    decomposition    0xb000d4dc
    stuck_add        0xb000d6dc
    bi_execute       0xb00001fc
    extensions_init  0xb00017b8
    loop             0xb000aa94   (terminal server main loop)
"""

import json
import os
import struct
import sys
import time

BASE_DIR = os.path.dirname(os.path.abspath(__file__))
if BASE_DIR not in sys.path:
    sys.path.insert(0, BASE_DIR)

# Ordered milestone chain. Order encodes boot progression: reaching a later
# target implies the earlier ones were on the path. The classifier treats the
# furthest-reached target as the terminal milestone.
TARGETS = [
    ("mempool_init", 0xb000d5b4),
    ("decomposition", 0xb000d4dc),
    ("stuck_add", 0xb000d6dc),
    ("bi_execute", 0xb00001fc),
    ("extensions_init", 0xb00017b8),
    ("loop", 0xb000aa94),
]

# UTCB layout (Iguana/OKL4): message registers begin at UTCB+0x40. We capture
# MR0..MR7 (32 bytes) and the phys_desc/fpage pair used by L4_MapControl.
UTCB_PTR_ADDR = 0xff000ff0        # where the guest stores the current UTCB VA
DEFAULT_UTCB = 0xdff00000
MR_BASE_OFF = 0x40
MR_COUNT = 8


def is_error_response(resp):
    """A response is an error if it is not a dict, lacks ok, or ok is falsey
    with an error field. Used to reject malformed/timeout without fabricating
    progress."""
    if not isinstance(resp, dict):
        return True
    if resp.get("ok") is False:
        return True
    if "error" in resp and resp.get("ok") is not True:
        return True
    return False


def classify_terminal(hit_names, target_order=None):
    """Pure milestone classification.

    Given the ORDERED list of target names actually hit (byte/register-backed),
    return a verdict dict:

        status   : "reached_terminal" if the final target was hit,
                   "blocked" otherwise, "no_progress" if nothing hit.
        terminal : name of the furthest-reached target in target_order, or None.
        reached  : list of reached target names in chain order.
        missing  : list of not-yet-reached target names in chain order.

    Never returns PASS/terminal unless the terminal target genuinely appears in
    hit_names. Unknown names are ignored (cannot invent progress).
    """
    if target_order is None:
        target_order = [n for n, _ in TARGETS]
    order_index = {n: i for i, n in enumerate(target_order)}
    hit_set = {n for n in hit_names if n in order_index}
    reached = [n for n in target_order if n in hit_set]
    missing = [n for n in target_order if n not in hit_set]

    if not reached:
        return {"status": "no_progress", "terminal": None,
                "reached": [], "missing": list(target_order)}

    # Furthest reached = highest chain index among reached.
    terminal = max(reached, key=lambda n: order_index[n])
    final_name = target_order[-1]
    status = "reached_terminal" if final_name in hit_set else "blocked"
    return {"status": status, "terminal": terminal,
            "reached": reached, "missing": missing}


class EvidenceLog:
    """Ordered, dedup-aware evidence accumulator.

    Records are appended in hit order. Repeated hits on an already-recorded PC
    are counted (hit_counts) but, when a TraceDeduplicator is attached and
    suppresses the PC, they do not emit a duplicate full record — keeping the
    ordered log readable while preserving exact hit counts.
    """

    def __init__(self, dedup=None):
        self.records = []
        self.hit_counts = {}
        self.hit_order = []          # ordered names, one per emitted record
        self._dedup = dedup

    def add(self, name, addr, record):
        self.hit_counts[name] = self.hit_counts.get(name, 0) + 1
        emit = True
        if self._dedup is not None:
            emit = self._dedup.observe(addr)
        if emit:
            rec = dict(record)
            rec["name"] = name
            rec["addr"] = addr
            rec["seq"] = len(self.records)
            self.records.append(rec)
            self.hit_order.append(name)
        return emit

    def ordered_names(self):
        """Unique names in first-emission order (for classification)."""
        seen = set()
        out = []
        for n in self.hit_order:
            if n not in seen:
                seen.add(n)
                out.append(n)
        return out

    def to_json(self):
        return {
            "records": self.records,
            "hit_counts": self.hit_counts,
            "hit_order": self.hit_order,
        }


class MempoolProbe:
    """Live driver. Uses only existing primitives via ``client``.

    ``client`` must expose: bp, bpclear, pause, cont, state, reg, read_mem, rpc.
    A fake implementing that surface drives the pure logic in unit tests.
    """

    def __init__(self, client, dedup=None, targets=None, poll_interval=0.02,
                 max_hits=64, timeout_s=30.0):
        self.client = client
        self.targets = targets if targets is not None else list(TARGETS)
        self.addr_to_name = {addr: name for name, addr in self.targets}
        self.log = EvidenceLog(dedup=dedup)
        self.poll_interval = poll_interval
        self.max_hits = max_hits
        self.timeout_s = timeout_s
        self.errors = []

    # ── evidence capture (read-only) ───────────────────────────────────────
    def _read_regs(self, core=0):
        regs = {}
        for n in (0, 4, 7, 13, 14, 15):
            regs[f"r{n}"] = self.client.reg(core=core, n=n)
        return regs

    def _read_utcb_mrs(self, core=0):
        """Capture UTCB MR0..MR7 and the L4_MapControl phys_desc/fpage pair as
        raw bytes + decoded words. Purely read-only via read_mem."""
        out = {"utcb": None, "mrs": None, "mr_bytes": None,
               "phys_desc": None, "fpage": None}
        try:
            ptr_raw = self.client.read_mem(core=core, addr=UTCB_PTR_ADDR, length=4)
            utcb = struct.unpack("<I", ptr_raw)[0] if len(ptr_raw) == 4 else DEFAULT_UTCB
            if utcb == 0:
                utcb = DEFAULT_UTCB
            out["utcb"] = utcb
            mr_bytes = self.client.read_mem(core=core, addr=utcb + MR_BASE_OFF,
                                            length=MR_COUNT * 4)
            if len(mr_bytes) == MR_COUNT * 4:
                words = struct.unpack("<%dI" % MR_COUNT, mr_bytes)
                out["mrs"] = list(words)
                out["mr_bytes"] = mr_bytes.hex()
                # L4_MapControl ABI: MR[0]=phys_desc (+0x40), MR[1]=fpage (+0x44)
                out["phys_desc"] = words[0]
                out["fpage"] = words[1]
        except Exception as exc:  # read failure must not fabricate evidence
            self.errors.append(f"utcb_read: {exc}")
        return out

    def capture_record(self, core=0):
        return {"regs": self._read_regs(core=core),
                "utcb": self._read_utcb_mrs(core=core)}

    def snapshot_probes(self):
        """Read-only probe.list/probe.get snapshot via rpc. Malformed/timeout
        responses are recorded as errors, never as progress."""
        out = {}
        listing = self.client.rpc({"cmd": "probe.list"})
        if is_error_response(listing):
            self.errors.append("probe.list failed")
            return out
        # probe.list returns a JSON string/array of names or objects.
        names = _extract_probe_names(listing)
        for pname in names:
            resp = self.client.rpc({"cmd": "probe.get", "probe": pname})
            if is_error_response(resp):
                self.errors.append(f"probe.get {pname} failed")
                continue
            out[pname] = resp
        return out

    # ── live run ───────────────────────────────────────────────────────────
    def run(self, core=0):
        """Arm breakpoints on every target, resume, and record each hit in
        order with register/UTCB evidence until the terminal loop is reached, a
        timeout elapses, or max_hits is exceeded. Returns the full report."""
        for _name, addr in self.targets:
            self.client.bp(addr, core=core)

        deadline = time.time() + self.timeout_s
        loop_addr = self.targets[-1][1]
        while time.time() < deadline and len(self.log.records) < self.max_hits:
            self.client.cont()
            paused_pc = self._wait_paused(core=core, deadline=deadline)
            if paused_pc is None:
                self.errors.append("timeout_waiting_pause")
                break
            name = self.addr_to_name.get(paused_pc)
            if name is None:
                # Paused somewhere unexpected (external break); record and stop.
                self.errors.append(f"unexpected_pause@0x{paused_pc:08x}")
                break
            self.log.add(name, paused_pc, self.capture_record(core=core))
            if paused_pc == loop_addr:
                break
            # step off the breakpoint before continuing
            self.client.step(core=core, ticks=1)
        return self.build_report()

    def _wait_paused(self, core=0, deadline=None):
        while deadline is None or time.time() < deadline:
            st = self.client.state()
            if is_error_response(st):
                self.errors.append("state_error")
                return None
            if not st.get("running", True):
                pc = self.client.reg(core=core, n=15)
                return pc & 0xffffffff
            time.sleep(self.poll_interval)
        return None

    def build_report(self):
        verdict = classify_terminal(self.log.ordered_names(),
                                    [n for n, _ in self.targets])
        report = {
            "verdict": verdict,
            "evidence": self.log.to_json(),
            "errors": self.errors,
        }
        return report


def _extract_probe_names(listing):
    """Normalize probe.list output (dict, JSON string, or list) into names."""
    data = listing
    if isinstance(listing, str):
        try:
            data = json.loads(listing)
        except ValueError:
            return []
    if isinstance(data, dict):
        if "probes" in data and isinstance(data["probes"], list):
            data = data["probes"]
        else:
            return list(data.keys())
    if isinstance(data, list):
        names = []
        for item in data:
            if isinstance(item, str):
                names.append(item)
            elif isinstance(item, dict) and "name" in item:
                names.append(item["name"])
        return names
    return []


def _live_main():
    import subprocess
    from zeebo_debug_scripting import ZeeboDebugClient
    from zeebo_trace_dedup import TraceDeduplicator

    port = int(os.environ.get("QW12_PORT", "48993"))
    proc = subprocess.Popen(
        [os.path.join(BASE_DIR, "zeebo_lle_main"), "--headless",
         f"--control-port={port}"],
        cwd=BASE_DIR)
    dbg = ZeeboDebugClient(port=port)
    try:
        dbg.connect(timeout=8.0)
        probe = MempoolProbe(dbg, dedup=TraceDeduplicator(recent_depth=32))
        report = probe.run()
        report["probes"] = probe.snapshot_probes()
        print(json.dumps(report, indent=2, default=lambda o: o.hex()
                         if isinstance(o, bytes) else str(o)))
        dbg.quit()
    finally:
        dbg.close()
        try:
            proc.wait(timeout=3)
        except Exception:
            proc.terminate()
            proc.wait()


if __name__ == "__main__":
    _live_main()
