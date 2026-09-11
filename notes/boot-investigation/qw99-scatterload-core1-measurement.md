# QW99 Part 3 — Scatterload divergence + Core1 boundary (MEASURED)

Worktree: `/tmp/zeebo-qw99-scatter` @ base `aa7a914` (detached).
Binary: `tools/cpp/zeebo_lle_main`, **pure interpreter** (no `--jit`).
NAND: `/home/rafaelfrequiao/projects/zeebo-lle/nand/{1.1.2.bin,1.1.2_APPS.bin,1.1.2_AMSS.bin}`.
Instrumentation is env-gated by `ZEEBO_QW99` and read-only (no PC/register writes).

## Controls (both interpreter-only, --seconds=45, exit 0)

- **Positive** (`ZEEBO_QW99=1`): emits QW99 lines; Core0 ends in the
  `0xb0400064..0xb040006c` copy loop.
- **Negative** (gate unset): **0** QW99 lines; identical Core0 stall in the same
  loop. ⇒ the instrumentation is non-perturbing; the divergence is intrinsic to
  the boot, not an artifact of the probe.

## Core0 root cause (measured, deterministic across runs)

Exactly **2** entries to `0xb0400000` (the Thumb scatterload/LZ decoder) in 45 s:

| entry | tid | tsid | active_sid | b04151a4 bytes | fnv32 | backing |
|------|------|------|-----------|----------------|-------|---------|
| #1 | 0x0 | 0x0 | 0x0 | `720165b4…` | 0x8c04405d | off=-1 host=nil |
| #2 | 0x8000c001 | 0x8000c001 | 0x8000c001 | `01000000…` | 0x2c9d0461 | off=-1 host=nil |

- Entry #1 (bootstrap, tsid=0) decodes the **correct** compressed source
  (`720165b4…`) and completes.
- Entry #2 runs under thread space **`0x8000c001`** and reads **`01000000…`**
  (wrong/garbage). The decoder length underflows (observed `r4=0xfff2a6f2`,
  `r1(dst)=0xb04eaab2` past `r2(end)=0xb04155b4`) ⇒ unbounded `ldrb/strb` copy ⇒
  the `0xb0400064..6c` infinite loop.

**Why the source is wrong — the address-space mismatch:**
The page covering `0xb04151a4` (VA `0xb0410000`, size `0x10000`, phys
`0x10040000`, host-backed) is registered **once**, under **`sid=0x80000100`**
(`[QW99/MC-record] … COVERS_b04151a4`). But the thread executing the second
scatterload lives in **`sid=0x8000c001`**, whose region set does **not** contain
`0xb04151a4` (`off=-1 host=(nil)` for tsid=0x8000c001). Scheduler activations
seen: `0x0 → 0x80008001 → 0x8000c001`; none is `0x80000100`. So `SpaceManager`
never populates the running space with the compressed source — the decoder reads
whatever the flat/zero backing yields.

This is a **SID-attribution defect**, not a decoder bug: MapControl records the
scatterload source against `0x80000100` while the consumer runs in `0x8000c001`.
The earlier `cd7da2f` per-SID switching was a real fix but insufficient here — it
switches address spaces correctly, yet the source page was registered to a space
the consumer thread never activates.

**No causal fix is proven.** Per the STOP rule, no PC/register/backing patch was
applied. The next honest step (future work) is to find where `0x80000100` vs
`0x8000c001` diverges: either the MapControl caller's SID is misattributed, or the
consumer thread should inherit/activate `0x80000100`'s regions. That requires
tracing the MapControl caller's true space and the ExchangeRegisters/thread-create
that spawns `0x8000c001` — out of scope for this measurement pass.

## Core1 boundary re-established — QW49 vs QW59 reconciled

Unfiltered Core1 trace over the same 45 s boot:

- Core1 advances continuously: insns counter climbs to **~272.5M**, cycling ~92
  distinct PCs, steady-state in the `0xf0003b–0xf0004xxx` scheduler/idle band.
- **Zero** hits at `0xf0016d14` (the *real* thread.cc:1273 panic target) and zero
  at `0xf0016bec` (the `beq` test that QW59 showed was a false-positive probe).

**Verdict: QW59 is correct; QW49's "TCB panic / infinite page-table walk" no
longer describes the current boot.** Core1 is not wedged at the TCB — it reaches
and cycles the scheduler band while doing address-space setup (consistent with
QW59's MapControl-progress observation). The QW49 page-table-walk root cause was
real at its HEAD but has since been resolved upstream; `core1_boot_estado_real.md`
and QW49's live-state text are stale and should be annotated as superseded by
QW59 + this measurement. The current single blocker on the boot's critical path is
the **Core0 scatterload SID mismatch above**, not Core1.

## Reproduce

```
cd tools/cpp && make zeebo_lle_main
N=/home/rafaelfrequiao/projects/zeebo-lle/nand
# positive
SDL_VIDEODRIVER=dummy ZEEBO_QW99=1 ./zeebo_lle_main --headless --seconds=45 \
  $N/1.1.2.bin $N/1.1.2_APPS.bin $N/1.1.2_AMSS.bin 2>qw99_pos.err
grep 'QW99] entry\|MC-record\|IPC-sched' qw99_pos.err
# negative (must print 0 QW99 lines, same stall)
SDL_VIDEODRIVER=dummy ./zeebo_lle_main --headless --seconds=45 \
  $N/1.1.2.bin $N/1.1.2_APPS.bin $N/1.1.2_AMSS.bin 2>qw99_neg.err
grep -c QW99 qw99_neg.err
```
