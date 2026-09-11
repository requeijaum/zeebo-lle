# Zeetris lifecycle probe — evidence (evidence-only, no boot)

Derived **only** from the real, proprietary bytes of `zeetris.mod` /
`zeetris.mif` (never committed; tests exit 77 when absent). No game boot is
claimed. Label of any execution: `hybrid/assisted`.

## Structural CLASS ID (from `zeetris.mif`)

`zeebo::brew::MifParser` walks the section-bounds table (magic `0x0011`,
`table_offset=0x48`, `section_count=6`). Section 5 is the 20-byte applet record
(`f4==0`, `fc==0`); its first word is the class ID:

    clsid = 0x12345678   (structural, section 5)

Authority is structural, not a raw 4-byte scan (the scan corroborates only). The
same bytes also appear at file offset 0x23860 — inside the applet record — so a
blind scan would agree, but validity is decided by the record position.

## Entry calling ABI (from `zeetris.mod`, 3,939,404 bytes)

First module word is a branch, so `resolve_mod_entry` returns `ENTRY_RAW_BRANCH`:

    0x12000000: ea000010   b 0x12000048        (first insn)
    0x12000048: e92d4017   push {r0,r1,r2,r4,lr}  (AEEMod_Load prologue)

Decoding the prologue (STMFD sp!/`push`, mask `0x4017`):

  - preserves **r0, r1, r2** (contiguous from r0) → **3 argument registers**
    (AAPCS: r0=pIShell, r1, r2=ppMod — the AEEMod_Load arity)
  - preserves **lr**
  - mode **ARM** (raw `.mod` entry is ARM code, *not* Thumb)

## Why v1 was invalid — refuted here

v1 called AEEMod_Load (0x12000048) as if it were a Thumb `HandleEvent` and got
`UC_ERR_INSN_INVALID`. The probe's v1-control forces `entry|1` (Thumb) and
observes the **same** `UC_ERR_INSN_INVALID`, proving the Thumb premise is wrong.
The mutant (`buggy` arg) pretends the Thumb ABI runs clean → the gate goes RED.

## Real execution (Unicorn interpreter, ARM, args r0..r2 = 0)

    entered_module = SIM   first_pc = 0x12000048   instr = 5038
    first missing dependency: READ_UNMAPPED @ 0x00000000

The raw entry executes 5038 real instructions inside the module, then stops
honestly at the first absent import/GOT/OEM dependency (a NULL-based read). No
success is fabricated; the probe stops at the first missing dependency, naming
the VA and fault type.

## Gate wiring

  - `make test_zeetris_lifecycle` — build
  - `make test-zeetris-lifecycle` — run (exit 0, or SKIP 77 without the ROM) +
    mutant that must fail (RED reproduced)
  - part of `check-fast` (Tier A). The ROM is proprietary and gitignored; the
    gate SKIPs (77) on a clean clone, never SKIP-as-PASS of real bytes.
