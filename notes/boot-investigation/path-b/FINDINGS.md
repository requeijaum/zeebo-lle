# Path B — Bounded Seeded Property/Differential Fuzzing (firmware ABI + fpage routines)

Worktree `/tmp/zeebo-boot-fuzz` base `ea1b48f`. Scripts/tests only, own worktree.

## Runtime provenance (recorded in artifacts)
- Firmware SHA256 `be45dae7c6fd71d615a6247f7e56e9c64bf5ac40d164df99eded475e85f9c909` (matches parent baseline)
- Unicorn py 2.1.2, Capstone py 5.0.7, libunicorn.so.2 `uc_version` raw `0x20102ff`
- Seed `0xB007` (deterministic)

## Extraction (no full-machine emulator)
Routines pulled from the real ELF PT_LOAD RX segment (vaddr `0xb0000000`, fileoff `0x30000`, R-X)
into a bounded Unicorn ARM harness. NAND opened read-only. Routines covered:
- KernelInterface wrapper `0xb000c720..c758`
- minpage / `l4e_min_pagesize` `0xb000d464..d4dc`
- fpage `largest_aligned_fpage` `0xb000d4dc..d570`
- mempool mapping `0xb000d5b4..d77c` (callers exercised via the two leaf routines)

## Method
Actual firmware bytes executed under Unicorn are differentially compared against an INDEPENDENT
mathematical contract re-derived from ARM32 semantics (CTZ for minpage; alignment/encoding for
fpage) — not "reached-PC = pass", no forced branches, no global-size<32 assumption. minpage's
global cache word is deterministically pointed at a mapped scratch page (literal rewritten) and
KernelInterface stubbed to return a seeded KIP so the routine runs organically without the full OS.

## Results (`campaign_report.json`)
| Suite | n | pass | genuine counterexamples |
|---|---|---|---|
| minpage CTZ differential | 210 | 210 | 0 |
| largest_fpage differential + boundary | 409 | 409 | 0 |
| wrapper ABI saved-regs (MODELED) | 36 | 34 | 2 (MODELED, expected) |

Self-check: `minpage(0x01111006)` actual==model==12; `largest(0xb0d00000,0xb0d00000,0xb6d00000)`
actual==model==`0xb0d00146` (1MB fpage, size_log2=20). Zero genuine divergences → minimization
produced no genuine counterexamples to minimize for the differential suites.

## Continuous vs stepped (`continuous_vs_stepped.json`)
Same routine run continuously vs `count=1` single-stepping on the SAME Unicorn frontend: all 4
cases identical r0. This verifies stepping/continuous determinism only — NOT an independent second
CPU (explicitly labeled; no independent-frontend claim).

## KernelInterface wrapper — primary hypothesis (MODELED, Path A owns real integration)
Wrapper contract (from disassembly): `push {r4,r5,r6,lr}`; copies out-ptrs r0/r1/r2→r4/r5/r6;
`mov ip,sp` (ip = post-push frame top) then `mvn sp,#0x4b`; `svc #0x14`; on return
`cmp rN,#0; strne` writes rets to the (nonzero) out-ptrs; `pop {r4,r5,r6,pc}`.

Property enforced: after the call, caller sentinels in r4/r5/r6 are restored, SP is balanced, and
the only writes are to nonzero out-ptrs.

MODELED hook cases (labeled, not production):
- `good` hook (restores sp=ip only): PASS across 34 cases incl. all null-out-ptr `strne`-skip combos.
- `bad_stack` (writes ip+0/4/8): property VIOLATED → after `pop`, r4/r5/r6 load `0xbad0bad0/4/8`.
  ip+0/4/8 ARE the pushed {r4,r5,r6} slots (ip+12 is the return-PC slot, untouched). This is the
  exact mechanism of the primary hypothesis: a host SVC hook writing IP+0/4/8 corrupts saved r4 —
  the page-cache pointer later consumed at `0xb0041284` — feeding garbage into `largest_aligned_fpage`,
  which then returns 0 and drives the whole-space (size_log2=32) fallback → `1<<32==0` → infinite pool loop.
- `bad_reg` (clobbers r4 mid-call): `strne r1,[r4]` faults at `0xb000c740` — clobbering r4 also
  breaks the guarded output-ptr write.

## Counterexample causal limits
- Differential suites prove the extracted routine BYTES compute the ABI/fpage contract correctly in
  isolation — they do NOT prove the boot stall's absence, since the stall arises from cross-routine
  state (the saved-register corruption above) driven by host integration, not from these leaf routines.
- The wrapper "counterexamples" are MODELED demonstrations of a hypothesized hook, not observations of
  production behavior. They show the hypothesis is *mechanically sufficient* to cause the stall; they do
  NOT establish that production actually performs such a write (Path A verifies real integration).
- Single-frontend stepping match is not independent-CPU corroboration.

## Files
- `tools/fuzz_pathb.py` — extraction + harness (models + actual execution)
- `tools/fuzz_pathb_campaign.py` — seeded campaign + minimizer
- `tools/fuzz_pathb_stepcmp.py` — continuous vs stepped
- Artifacts: `campaign_report.json`, `continuous_vs_stepped.json`
