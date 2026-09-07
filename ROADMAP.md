# Zeebo LLE Emulator — ROADMAP (rev 2026-09-06, after session 2o audit)

Low-level emulation of the Zeebo: boot the REAL firmware from the NAND dump on an
emulated Qualcomm MSM7201A (ARM11 apps core), no HLE of BREW. Decided by Rafael
2026-09-06 despite the emulation ROADMAP marking HLE-LLE "OUT". This revision is
grounded in what sessions 2a-2p actually PROVED, including the audited correction
of the L4e syscall ABI.

## What is now KNOWN (evidence, not guesses)
- **Kernel = L4e (NICTA Pistachio-embedded / OKL4 lineage) + REX RTOS on top.**
  AMSS/APPS are REX tasks. Strings `l4e_min_pagesize`, `L4_Restore fell through`,
  `rex_self`, `rex_*_tcb` prove it.
- **AUDITED: ARM L4e syscalls are `bl` to KIP link addresses**, not `svc #imm`+
  SP-magic (NICTA RefMan N1 rev2, ARM C.2; example `bl 0xFE0000B4`=KernelInterface;
  MR0-5=r3-r8; UTCB read from 0xFF000FF0; sp/lr preserved). The earlier
  "svc#0x14=MAP_CONTROL..." 6-syscall map was WRONG (misread of OKL4 user-side
  ipc.spp as the ABI); the firmware's `mvn sp,#0x4b; svc #0x14` matches neither
  SYSNUM/SWINUM. Real syscalls must be derived from the KIP link fields.
- **Real MSM7201A register map** is in openzeebo `zloader/arch_msm7k`+
  `include/msm7k/*.h` (VIC 0xC0000000, GPT 0xC0100000, DMOV/ADM 0xA9700000, MDDI
  0xAA600000, CLK 0xA8600000, UART1 0xA9A00000, NAND 0xA0A00000).
- **Real VA->PA MMU map (both cores)** in tripleoxygen `console__zeebo__mmu.txt`
  (ARM11: periph->c0 block, f0000000->10000000, coarse b0xxx->100a3xxx).
- **APPSBL does ONLY peripheral bring-up** (VIC/GPT/DMOV/GPIO/MDDI + MMU-enable),
  then slips off the end. It does NOT read NAND (0 accesses to 0xa0a00000 in a
  full 20M-insn boot, even with r0 forced on the flash path). The element that
  reads NAND + relocates the OS image is LATER in the boot chain.
- **APPS isolated cannot boot**: entry 0x10000000 is not a valid ARM11 MMU section
  (loader re-maps it), and it derails into 0xb000fffc (loader-built RAM).
- NAND ID 0x5580b1ad confirmed (openzeebo nandread.py/nandwrite.py/flash.c).

## Honest position
The goal post ("boot real firmware end-to-end, no BREW HLE") requires a **full
boot-chain bring-up**: APPSBL -> [the flash-reading loader element] -> load L4e
kernel -> L4e loads AMSS/APPS as REX tasks. That is a multi-session project with
the L4e-on-ARM ABI as the deepest uncertainty (now corrected/known). We are NOT
close to a booted system; we have the register map, MMU map, flash model, and the
kernel identity — the "what" — but not the "how" of the loader handoff.

## Staged plan (re-grounded)

### Phase 0 — DONE. MSM7201A peripheral map recovered + APPSBL bring-up traced
41 regs modeled empirically then corrected against arch_msm7k. Inject:
- VIC 0xC0000000, GPT 0xC0100000 (free-running COUNT_VAL@+0x04), DMOV 0xA9700000
  (STATUS=RSLT_VALID|CMD_PTR_RDY, RSLT=DONE), MDDI 0xAA600000, GPIO, CLK.
- The 0x8e0 software udelay must be short-circuited (force r0=1) or it eats the
  whole instruction budget.

### Phase 1 — NAND controller + full-chain loader element  [NEXT, the real work]
- `tools/nand_controller.py` exists and passes self-tests (FETCH_ID=0x5580b1ad,
  PAGE_READ byte-exact). 
- The MISSING LINK: find and run the boot element that reads NAND + relocates the
  OS image. It is LATER than APPSBL's reachable code (real 0xa0a00000 literals in
  APPSBL funcs 0x7100-0x8334, 0xc6ac, 0xe2b4...). Trace how APPSBL hands off and
  what it targets, then load/run that element with the NAND model backed.

### Phase 2 — L4e kernel boot
- Load the OKL4 L4e kernel (ELF wanted; kernel has arm1176jz dir in the OKL4 tree
  we pulled). Bring up its own MMU/KIP per the real map.
- **Derive the ACTUAL syscall set from the KIP link fields** (find KIP, read the
  `bl` targets the guest branches to), now that svc-immediates are ruled out.

### Phase 3 — AMSS/APPS as REX tasks
- L4e loads AMSS/APPS; REX scheduler runs them. REX API is documented
  (rex_self/rex_wait/rex_set_sigs/timers; QSC1110 rex.c is behavioral reference;
  QSC1110 is a DISCRETE chip, not the MSM7201A ARM9). A REX shim services a small
  IPC/thread surface located via KIP links — no full kernel needed IF we stay at
  the REX boundary.

### Phase 4 — The two big undocumented blocks (unchanged concerns)
- ARM9 modem (AMSS): ONCRPC / PROC_COMM RPC between cores; stub responses
  (HLE-of-modem inside an otherwise-LLE apps core) vs full ARM9 — decide with data.
- Adreno 130: tile renderer + ring-buffer; likely weigh pure-LLE vs redirecting GS
  to host GLES.

## Verification rule (hard-won, obey always)
Repeated UC_HOOK_INTR + growing insn count is NOT proof of life — a derail into
empty memory spins the INTR hook thousands of times. ALWAYS read the svc bytes and
require `op>>24==0xEF` (real SVC) before declaring a syscall/idle/pass.

## Clean-room note
arch_msm7k is BSD/Apache (Google little-kernel / zloader derivative) — usable as
reference and portable with attribution; it is boot code, not game/BREW code, so
it does not touch the BREW clean-room. a1Sim stays black-box-only.