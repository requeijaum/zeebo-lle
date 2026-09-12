# Control / Resume Audit — debugger noninterference, syscall PC-resume, checkpoint atomicity

Scope: read-only audit at HEAD `ea1b48f`. Files read exactly:
`tools/cpp/zeebo_lle_main.cpp` (run loop 1113–1220; control dispatch 1410–1670;
`c0_intr_hook` 2106–2283; `c0_code_hook` 2285–2439; `c1_code_hook` 2563–2734),
`tools/cpp/zeebo_save_state.h`, `tools/cpp/test_save_state.cpp`.
No processes/builds run (RAM-constrained). Classification tags below:
**[DEFECT]** = provable from source; **[HYP]** = hypothesis needing a live check;
**[OK]** = verified correct in source.

---

## 1. Syscall PC-resume in `c0_intr_hook` — the double-advance hazard  **[HYP, high priority]**

The hook reads the SVC imm at `pc-4` (line 2118–2120) with the comment that the
imm is only diagnostic. That `pc-4` read **assumes Unicorn's INTR hook fires with
PC already pointing AFTER the SVC** (i.e. PC = svc_addr + 4).

Resume logic then splits:
- syscalls `0xb4`, `0x00`, `0x0c`: `target_pc = pc + 4` (lines 2250, 2257, 2262).
- default (LR-returning syscalls, e.g. MapControl `0x14`): `target_pc = lr & ~1`
  with Thumb bit folded into CPSR (2266–2274).

**The contradiction:** if PC is already `svc+4` (as the `pc-4` read presumes),
then `pc + 4` resumes at `svc + 8` — **skipping the one instruction immediately
after the SVC**. For `0x00` (L4_Ipc) the code even comments the target is
`0xb000c834: pop {r1,r2}` — i.e. it *intends* to land exactly one insn past the
SVC, which is only correct if PC==svc (NOT svc+4). So exactly one of the two code
paths is built on the wrong PC-origin assumption; they cannot both be right.

Consequence for the parent: any boot that traverses L4_Ipc / KernelInterface /
ExchangeRegisters (all use `pc+4`) may be silently skipping a real instruction
every syscall. A "boot" captured or fuzzed on top of this could encode a
skipped-instruction artifact, not true firmware behavior. This is the single
most important thing to resolve before trusting snapshot/fuzz results.

Secondary in same hook:
- **[HYP]** Thumb callers: `pc + 4` is wrong for a Thumb SVC (should be +2). L4
  stubs are ARM so likely unaffected, but the code makes no width check.
- **[DEFECT, minor]** default branch: if `lr == 0`, `target_pc` stays 0 → no PC
  write, no `uc_emu_stop`, `core0_.entry` not updated; execution silently
  continues from an undefined point with only r0 clobbered.
- **[OK]** No banked-register/SPSR handling is *needed*: the INTR hook substitutes
  for the exception vector, so CPSR mode is still the caller's — the syscall is
  emulated as a plain function, not a real mode switch.

### Low-cost discriminating test (host-only, no NAND, <1s)
Open a bare `uc` (`UC_ARCH_ARM/UC_MODE_ARM`), map one page, write
`svc #0x14` at 0x1000 and a sentinel (`mov r1,#0xAA`) at 0x1004. Register a
`UC_HOOK_INTR` that reads `UC_ARM_REG_PC` and stops. Run `uc_emu_start(0x1000..)`.
- If the hook sees **PC == 0x1004**, the `pc-4` imm read is correct AND the
  `pc+4` resume double-advances → **DEFECT confirmed** for `0xb4/0x00/0x0c`.
- If PC == 0x1000, the `pc-4` read is wrong (reads before the SVC) but the `pc+4`
  resume is correct.
Either way one 20-line test fixes the interpretation for every syscall path.

---

## 2. `pause` / `step` / `cont` and breakpoints

- **[OK] step** (1457–1487): runs exactly one Unicorn insn, propagates `uc_err`
  to the client, never fabricates `PC+4`, accepts a legitimate self-loop
  (PC unchanged). Matches the skill's required semantics.
- **[OK] cont-over-breakpoint** (1425–1455): single-steps off the current BP with
  `stepping_=true` so the code-hook BP re-trip is suppressed (2432 / 2700 gate on
  `!stepping_`), invalidates the JIT cache first. Correct.
- **[DEFECT, containment] pause does NOT freeze host devices.** `paused_` only
  gates the two `uc_emu_start` calls in the interleave loop (1142–1150). The SDL
  audio callback runs on the audio driver's own thread and keeps pulling/mutating
  the sink; the control-server thread also keeps running. So "pause" freezes both
  CPU cores (they share one thread) but **not** the host audio device or any
  async producer. A snapshot taken "while paused" is therefore NOT a quiescent
  point for device state.
- **[HYP] pause vs. mid-slice hook writes:** `pause` sets `paused_` from the
  control thread while the main thread may be inside `uc_emu_start`. The pause
  takes effect only at the next loop top, so the in-flight slice completes — fine
  for CPU, but confirms pause is coarse (slice-granular), not instruction-exact.

---

## 3. Trace / cached-entry PC reuse  **[OK with one caveat]**

`core0_.entry` / `core1_.entry` are the cached resume PCs. They are written from
two places: (a) the run loop reads `UC_ARM_REG_PC` after each slice (1154, 1163);
(b) hooks write `core*_.entry` directly before `uc_emu_stop` (2277, 2306, 2407,
2434, 2664…). Because a hook writes PC *and* stops, the loop's post-slice PC read
returns the same value → consistent. **Caveat [HYP]:** the `cont` fast-path
(1429–1451) trusts `core*_.entry` as "the PC we're stopped at"; if a prior slice
ended via an error path that left `entry` stale, the cont-step would resume at the
wrong address. Low risk but not proven safe.

---

## 4. Checkpoint / snapshot atomicity  **[DEFECT cluster]**

`ZeeboSaveStateManager` (zeebo_save_state.h) is **compile/unit-test-only**: no
caller exists in `zeebo_lle_main.cpp` and the control server exposes no
`save`/`load`/`snapshot` command (dispatch verbs: state, pause, cont, step, bp,
bpclear, peek, poke, hook, strict_unmapped, quit). **There is no snapshot RPC.**
The parent must not assume one.

If/when it is wired up, these are structural hazards:

- **[DEFECT] Aliased physical memory collapses on restore.** Save walks
  `uc_mem_regions` + `uc_mem_read`; restore re-creates each region with
  `uc_mem_map` (anonymous), NOT `uc_mem_map_ptr` (save_state.h:152). The
  APPS_RAM VTLB/PhysPool design maps multiple VAs to one host `PhysPool`. After a
  save→load, those aliases become **independent copies** — a write through one VA
  no longer shows through its alias. Shared-backing preservation is exactly the
  property the skill warns to test, and this restore path breaks it.
- **[DEFECT] Device / host state is not serialized.** Only Unicorn CPU context +
  mapped guest RAM are saved. NOT captured: GPU/rasterizer, input, qdsp5
  dispatcher, `rex_heap_shadow_` + `rex_heap_dirty_` (the Split-I/D shadow that
  keeps AMSS `.text` pristine), poll counters, brew/igl bridge state, control
  server queue, IRQ/scheduler interleave phase. Restoring RAM without the REX
  shadow set will desync code-vs-data in the `0xf0000000` window.
- **[HYP] `uc_context` cross-process portability** is assumed but unverified; the
  format also has no host/arch stamp beyond version=2.
- **[OK]** Header validation is solid: magic/version/context-size checks, region
  count caps, `end>=begin`, size==end-begin+1, `MAX_REGION_SIZE`, chunked I/O.
  The unit test covers save→mutate→restore equality and a corrupted-context-size
  rejection — but tests **only two disjoint non-aliased regions**, so it cannot
  catch the alias-collapse defect above (no load-bearing alias test).

---

## Dangerous paths, ranked (what can produce false snapshot/fuzz results)

1. **`c0_intr_hook` `pc+4` resume** — may skip one insn per L4_Ipc/KIP/ExchReg
   syscall across the whole boot. Run the §1 test before trusting any capture.
2. **Snapshot alias-collapse** — if a snapshot RPC is added on top of
   `save_state.h`, restored state silently loses PhysPool aliasing → fuzz replays
   diverge from live boot. Needs `uc_mem_map_ptr` restore + device serialization.
3. **pause ≠ quiescent** — audio/control threads keep mutating; any "atomic
   checkpoint while paused" is not atomic w.r.t. host devices.
4. **`lr==0` default-syscall path** — undefined resume, low frequency.

## Not defects (verified correct)
step semantics; cont-over-BP single-step + cache flush; breakpoint gating on
`!stepping_`; save/load header hardening; INTR-hook not needing SPSR unbanking.
