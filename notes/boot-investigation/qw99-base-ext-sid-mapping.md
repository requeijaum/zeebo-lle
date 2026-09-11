# QW99 — OKL4 base↔extension PD mapping fix (SID attribution)

## Summary
The ARM11 scatterload of the second image read the wrong source bytes because an
OKL4 **extension** protection domain did not see the mappings owned by its
**base** PD. When ThreadControl creates a thread in its own SpaceSpecifier but
names a *different* space as its Pager, that pager is the base PD whose page
window / shared domain the extension inherits. Without linking the two, the
extension's `activate()` bound the wrong (stale/flat) host backing at the shared
VA and the second scatterload hash was computed over garbage.

## Fix (generic, no hardcoded IDs)
- `SpaceManager::link_base(ext, base)` records `ext -> base` purely from the
  observed ThreadControl `SpaceSpecifier`/`Pager` relation. A space is never its
  own base; a nil base clears the link. No SID or page address is hardcoded.
- `resolve_host()` / `maps()` walk the base chain (cycle-guarded, ≤8 hops); the
  extension's own region shadows the base's at the same VA.
- `activate()` builds an **effective region set** = the space's own regions plus
  base-chain regions not already present. Both the unmap pass (prior space) and
  the map pass (target space) use this effective set, so no stale base mapping is
  left behind and none is missing.
- Shared VAs often collide with a pre-existing static flat mapping, so
  `uc_mem_map_ptr` returns `UC_ERR_MAP` and a bare reprotect never rebinds the
  host pointer. When we own a real host backing, `activate()` forces a rebind
  (`uc_mem_unmap` → `uc_mem_map_ptr`), falling back to reprotect on failure.
- Wired in `zeebo_lle_main.cpp` at the L4 ThreadControl handler:
  `if (tc_space && tc_pager && tc_pager != tc_space) link_base(tc_space, tc_pager)`.

## Evidence — positive interpreter run (fix on)
- Second scatterload entry now reads the **correct** source
  (`b0=720165b4 fnv=0x8c04405d`, matching entry #1); the underflow is gone.
- Core0 escapes the old `0xb0400064..0xb040006c` scatter loop and advances
  through the decompressed code: `0xb0100000 → 0xb034ef74 → 0xb03b09f4`,
  kernel bands `0xf00xxxxx`, and `0x100161c8`.
- New (later) failure: Core0 faults at `0x00000014` (exception vector) — a fresh,
  strictly later boot stage than the previous stall.

## Evidence — negative control
- Unit test `test_l4_base_ext_share` (via `make test_l4_base_ext_share`):
  positive PASSES; `buggy` mutant (no `link_base`) FAILS as expected — the
  extension cannot resolve the base-only source page.

## Limitation
- The firmware-level negative interpreter run (a `ZEEBO_NOLINK`-gated boot to
  prove causality end-to-end in the guest) was **denied/blocked by the user** and
  was NOT executed; production code was restored to the clean, ungated state.
  Causality is therefore established at the unit-test level (mutant) plus the
  strong positive frontier advance, not by a firmware-level A/B.
- This advances the ARM11 boot past the scatterload underflow; it does **not**
  claim AppMgr reached or any later subsystem functional. The `0x00000014`
  fault is the next unsolved frontier.
