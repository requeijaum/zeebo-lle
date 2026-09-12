# Path D — Rescue Report: Firmware Rehosting Techniques + OKL4 L4-e ABI Match

Generated: 2026-09-08 · Clean-room: a1Sim black-box only, qdsp5 frozen, no firmware
copied into distributable code, no verbatim upstream source in project source tree.
Upstream files below are **reference evidence only**, held outside production under
`notes/boot-investigation/path-d/upstream/`.

## Verified handles (pinned)

- **Upstream mirror:** `rochus-keller/OKL4` (public, BSD-style OzPLB/OKL4 header).
- **PINNED revision:** commit `2f80ba36b59870ade7dbb41a0e16c3f3b7e70e43`
  (default branch `master`, committed 2025-10-20T14:51:47Z, "updated readme").
  Repo has **no git tags**; pin by SHA, not by version string.
- **Version provenance (from repo README, not a release artifact):** mirror author
  states the tree is `okl4-2.1.1-fix7`, cross-checked against a SourceForge
  `okl4-2.1.1-emboslab` tree and an archive.org snapshot, identical except one file.
  Treat "2.1.1-fix7" as the mirror's *claim*; the hard evidence is the commit SHA +
  file hashes below, not a signed release tag.

## Remote facts — primary-source technique matrix (2-3 techniques)

| Technique | Primary source | Core mechanism | Relevance to Zeebo/QDSP LLE | Caveat |
|---|---|---|---|---|
| **Fuzzware** (MMIO modeling) | Scharnowski et al., *USENIX Security '22*, pp.1239-1256, ISBN 978-1-939133-31-1; repo `github.com/fuzzware-fuzzer/fuzzware`, Apache-2.0, Zenodo DOI 10.5281/zenodo.6499215 | Unicorn + AFL/AFL++; symbolic-execution (angr) derives per-access MMIO models so the fuzzer only mutates values that affect firmware logic; auto-detects "booted" states for continued fuzzing | Snapshot/boot-state detection is directly applicable to isolating a booted QDSP/L4 state; MMIO modeling could stub a1Sim-observed peripheral reads | **Cortex-M (M3/M4) monolithic scope.** Zeebo target is ARM/L4 with an MMU — model transfer is conceptual, not turnkey |
| **UnicornAFL** (harness substrate) | Same paper §design; Unicorn Engine as the ISA emulator glued to AFL | Emulate unmodified firmware; serve every MMIO/hardware read from fuzz input | Baseline substrate we can reuse for differential replay of mapped guest routines | Needs deterministic re-execution (identical runs) — matches our incomplete-snapshot problem |
| **avatar2 / HALucinator** (hardware-in-the-loop / HAL abstraction) | Referenced as state-of-the-art re-hosting lineage in the Fuzzware paper's related work | avatar2: orchestrate emulator↔real-device; HALucinator: replace HAL functions with high-level handlers | Alternative when black-box a1Sim must stay in the loop for peripherals we cannot model | No standalone primary retrieved this pass — cited at lineage level only |

### Conference talks (bounded check)
No confirmed dedicated CCC / DEF CON / Black Hat talk was located for the OKL4-on-Zeebo
/ QDSP5 rehosting angle in this bounded pass. State plainly: **no confirmed talk**;
the load-bearing primary is the Fuzzware USENIX '22 paper + repo.

## Local facts — L4-e ABI comparison (differential)

- **Local refs tree** `refs/okl4-2.1.1-fix7/` contains `arch/.../kernelinterface.spp`
  but is **missing `libs/l4e/`**; the l4e sources were recovered from the pinned
  upstream commit as reference evidence.
- **PRIMARY ABI evidence — byte-identical:**
  `arch/arm/libs/l4/src/kernelinterface.spp`
  - local  sha256 `e4626e56a681db3436f2341d82b944939532fb901983db42fb9df22d401a81fb`, 4159 B
  - upstream (pinned SHA) sha256 **identical**, 4159 B → `BYTE_IDENTICAL: True`.
  - Confirms the `L4_KernelInterface` ARM stub `stmfd sp!, {r4-r6, lr}` /
    `ldmfd sp!, {r4-r6, pc}` saved-frame layout (r4/r5/r6 = ApiVersion/ApiFlags/KernelId,
    `swi 0x14`) matching the firmware's observed syscall frame — the r4/r5/r6 saved-frame bug anchor.
- **l4e algorithms — semantic match to mapped guest routines (differential passes):**
  - `l4e_min_pagebits()`: CTZ over `L4_GetPageMask()` (loop shifting until low bit set)
    → matches local `l4e_min_pagesize()` usage at `iguana/server/src/pd.c:732`.
  - `l4e_biggest_fpage(addr,base,end)`: grow `bits` while the `1<<(bits+1)` aligned page
    stays within `[base,end]`, return `L4_FpageLog2(addr>>bits<<bits, bits)`
    → matches local `mempool_init` largest-aligned-fpage shift-by-bits loop at `0xb000d4dc`.
  - These are **semantic/algorithmic** matches (guest is compiled binary), not byte matches.
- **Snapshots: incomplete** — booted-state capture is partial; this is the current gap,
  and the Fuzzware booted-state-detection idea is the most transferable mitigation.

## Evidence artifacts (this pass)

`notes/boot-investigation/path-d/upstream/`:
- `MANIFEST.json` — pinned SHA, per-file repo path, HTTP 200, byte length, sha256,
  and the byte/semantic identity result for kernelinterface.spp.
- `kernelinterface.spp` (4159 B), `min_pagebits.c`, `biggest_fpage.c`, `map.c`,
  `map.h`, `misc.h` — recovered l4e reference sources, all HTTP 200 from pinned commit.

## Method notes / reliability
- `curl` blocked (user denial) and `urllib` hung (300s timeout, as previously seen);
  raw fetches succeeded via browser `fetch()`. All URLs pinned to the commit SHA — no
  guessed paths, no version string in fetch URLs.
- Fuzzware retrieved via search abstract + official repo README (primary), not the full PDF.
