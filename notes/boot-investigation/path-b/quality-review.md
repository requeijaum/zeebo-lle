# Path B Fuzz Harness — Quality Review (commit 152334a)

**Verdict: APPROVED** (with non-blocking hardening recommendations)

Scope: read-only review of `tools/fuzz_pathb.py`, `fuzz_pathb_campaign.py`,
`fuzz_pathb_stepcmp.py` after spec PASS. Env: unicorn 2.1.2, capstone 5.0.7,
NAND 1.1.2_APPS.bin (22,151,168 bytes) present.

## Bounded verification actually run
- `fuzz_pathb.py` self-check: minpage(0x01111006) actual=12 == model=12;
  largest actual=0xb0d00000-derived == model. OK.
- minpage model-None inputs {0x0,0x1,0x200,0x3ff}: all actual → `TIMEOUT`, model → None.
- wrapper good/bad_stack/bad_reg exercised (see S3 finding below).

## The S1 concern — classification of the None model (VERIFIED SOUND, one caveat)
The stated risk ("timeout counted success = infinite-loop proof") does **not**
materialize as a correctness defect, because the two failure modes are kept
distinct:
- `run_minpage` (fuzz_pathb.py:111-117): count-exhaustion returns normally from
  `emu_start` → PC != RET_MAGIC → `('TIMEOUT',...)`. A genuine `UcError`
  (fault) is caught at :113-114 and, when PC != RET_MAGIC, returns `('ERR',...)`.
- campaign S1 (fuzz_pathb_campaign.py:34-37) increments `pass` **only** on
  `act[0]=='TIMEOUT'`; an `ERR` falls to `counterexamples`.

So a silent unexpected exception is NOT accepted as an expected hang — sentinel
non-reach AND the exception status are both checked. This is the important
property and it holds.

**Caveat (honest-labeling, not a bug):** line 35 comment "actual also hangs →
matches hazard contract prediction" and the `pass+=1` conflate *bounded
non-termination within 3000 insns* with the model's `None`. This only
establishes the routine did not reach the sentinel in the budget — not an
infinite loop, and not that it looped for the *modeled* reason (masked==0).
A routine that diverges into unrelated valid-but-wrong mapped code until the
count expires would also be scored `pass`. Recommend downgrading the semantic
claim in the report (e.g. count as `bounded_nonterm` rather than folding into
`pass`), and asserting the loop PC stays within the ctz loop body.

## S3 wrapper — MODELED, one conflation
- Good/null-ptr paths verify SP balance, r4/r5/r6 restoration, guarded writes. Solid.
- `bad_reg` (fuzz_pathb.py:165-166) clobbers r4=0xdeadbeef; empirically this
  faults `UC_ERR_WRITE_UNMAPPED` at 0xb000c740, so `check_wrapper` returns
  `('ERR',...)` and S3 records `property_violated=(ok is not True)` → True.
  The hypothesis is "confirmed" via a *memory fault*, not via the saved-register
  check it purports to exercise. Not a false pass (S3 `pass` is not incremented),
  but the counterexample's stated cause is imprecise. Recommend distinguishing
  `ERR` from a clean property violation in the recorded reason. Non-blocking;
  suite is explicitly labeled MODELED (Path A owns real integration).

## Robustness / hygiene
- ELF extraction bounded: phdr loop iterates `e_phnum` fixed 32-byte entries;
  `SEG_BYTES = D[0x30000:0x40000]` fixed 64K window; asserts PT_LOAD va+off match.
  No `len(D)` guard on the slice, but file (22 MB) >> window. Minor.
- Reset independence: every `run_*`/`stepped_largest` calls fresh `mk_uc()` with
  fresh `mem_map`/writes; no state bleeds across cases. Good.
- stepcmp correctly self-labels "same Unicorn frontend, not an independent CPU;
  verifies continuous vs count=1 determinism only." Honest framing.
- Hardcoded paths (NAND, OUT) are absolute into the live project; artifacts
  (`campaign_report.json`, `continuous_vs_stepped.json`) are written under
  `notes/boot-investigation/path-b/` — inside the repo, not a throwaway dir.
  Acceptable (notes tree) but not externalized; parametrize via env if reuse
  outside this tree is intended.

## Recommendations (non-blocking)
1. S1: separate `bounded_nonterm` count from `pass`; assert loop PC stays in the
   ctz body so TIMEOUT is attributable to the modeled hazard.
2. S3: record `ERR` distinctly from property violation in bad-hook counterexamples.
3. Add `len(D)` guard before the fixed 64K slice for defense-in-depth.

None of these block use; the differential core (real NAND bytes vs independent
math), mutation sensitivity, and ERR-vs-TIMEOUT separation are correct.
