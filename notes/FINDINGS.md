# Zeebo LLE — Findings Log

## Goal
Low-level emulation of the Zeebo (Qualcomm MSM7201A: ARM11 apps + ARM9 modem,
Adreno 130, BREW 4.0.2) by booting the REAL firmware from the NAND dump under a
full-SoC emulator (QEMU board or Unicorn-based core), no HLE of the BREW API.

Decision: Rafael chose to build the LLE project from scratch, accepting the cost
of reverse-engineering closed MSM7201A hardware (2026-09-06).

## Ground truth assets (local, verified)
- NAND copy at `~/projects/zeebo-lle/nand/` (sha256-identical to the original in
  zeebo-emulator/research/docs/nand-dump/, which is now chmod a-w read-only).
  Blobs: 1.1.2.bin (128MB full), 1.1.2_spare.bin (ECC), and the three split
  ELF/bootloader images below.
- Firmware 1.1.2. a1Sim is NOT used here — it is x86/HLE and irrelevant to LLE.

## Boot-chain structure (verified via ELF phdrs + capstone)
- **APPSBL** (393216 B): ARM11 bootloader. Starts with the classic 8-entry ARM
  vector table (`ldr pc,[pc,#0x18]` ×8). Handler targets:
  reset=0x8f8 undef=0x9cc swi=0x1028 pabt=0x9d4 dabt=0x9dc irq=0xcac fiq=0xc38.
  Reset handler sets FIQ/SVC modes (cpsr 0xd1/0xd3), SP=0x100000, `bl 0x6bc`.
- **AMSS** (ELF, EM_ARM): entry 0x00a00000 (matches zloader load addr from KB).
  18 LOAD segs; huge segments at 0x00b1a000 (9.7MB), 0x16e00000 (7.5MB) = modem.
- **APPS** (ELF, EM_ARM): entry 0x10000000. 14 LOAD segs; f0000000 = low code,
  b0xxxxxx = BREW/AEE region, 0x1013a000 (19.7MB) + 0x1140c000 = app/asset heap.

## First LLE probe result (tools/peripheral_probe.py, Unicorn ARMv6/ARM1176)
Booted APPSBL from reset with ONLY RAM mapped; trapped all peripheral access.
- Touched only **3 MMIO regs** before halting:
  - `0xc0000010` W=0  @pc 0xb6c  (early init)
  - `0xc0100100` R    @pc 0xc384 (a clock/PLL-ish reg: code does read, uxth,
    orr #0x640000, write-back — a read-modify-write of a control reg)
  - `0xa9700e10` R    @pc 0x169c (0xa9xxxxxx = Qualcomm periph band; UART/clock
    family per KB: UART1 base 0xa9a00000, so 0xa970 is a sibling block)
- Then the CPU falls into `b 0xc30` — an **infinite halt/trap**, reached at the
  end of a word-copy routine (0xc08-0xc2c). Interpretation: the bootloader took
  an ERROR/abort path because a stubbed MMIO read returned 0 instead of a real
  hardware value. This is expected: with no device models, the boot cannot pass
  its first hardware sanity check.

## What this measures (the point of the probe)
The "hole" to fill for a working LLE is not yet even visible — the boot dies at
the FIRST three peripheral touches. To get past 0xc30 we must model, at minimum,
the clock/PLL block at 0xc010xxxx and the 0xa970xxxx block with plausible return
values, then re-probe to reveal the NEXT wave of registers. Each iteration
uncovers more of the (undocumented) MSM7201A register map. This is the honest
shape of the LLE effort: an iterative RE loop, one register wall at a time.

## Boot progression (measured, halt pushed later each iteration)
- v1 (all MMIO reads=0):          3 regs -> dies at first clock poll (0xc30)
- v2 (sticky regs + a9700 ready): 12 regs -> passes clock, dies at a9700 ch1
- v2 (a9700 status band=ready):   41 regs -> passes ALL a9700 channels (24 regs
  = clock-gate init), reaches TWO new blocks:
    * 0xaa600000 : rich config seq (0x400/0x3c00/0xa8500/0x60006...) + poll
      +0x028 -> likely EBI/SDRAM memory controller or a UART. Boot now polls
      0xaa600028 for a status bit.
    * 0xa9200808 : GPIO/pinmux (writes 0x100000 = pin set).
  Register map so far by region:
    0xc0000000(4) interrupt ctrl · 0xc0100000(3) clock/PLL ·
    0xa9700000(24) clock-gate channels · 0xa9200000(1) GPIO ·
    0xaa600000(9) mem-ctrl/UART

## Honest assessment of the loop
Each ~5-min RE iteration (model a register from the code that consumes it,
re-probe, watch the halt move) uncovers one more hardware block. This is the
core LLE grind and it is WORKING — we went 3 -> 41 registers in three iterations.
The backlog ahead is large but now concrete and ordered: memory controller,
GPIO, then whatever the APPSBL touches to load+jump the AMSS/APPS ELF. Only after
APPSBL hands off do we hit the modem (ARM9) and the Adreno 130 GPU — the two
biggest undocumented blocks.

## MILESTONE — APPSBL phase-1 hardware bring-up COMPLETE (2026-09-06)
With the device model (sticky config regs + a9700/aa600028 status = ready) the
APPSBL runs its ENTIRE peripheral-init phase with no error halt:
  interrupt ctrl -> clock/PLL programming -> 24 clock-gate channels -> GPIO ->
  UART (config + status poll) -> ~54.6M instructions of init/delay loops.
Then execution derails into a zero/0xFF region (traced: last real branch target
is a zeroed data area at ~0xfa74, then it plows through NOP-like 0x00 words).

ROOT CAUSE (structural, not a model bug): after peripheral init the APPSBL LOADS
THE NEXT STAGE FROM NAND and jumps to it. Our standalone probe (a) does not model
the NAND controller (base 0xa0a00000 per KB) so the read returns 0, and (b) never
placed AMSS/APPS in RAM, so the jump target is empty. The bootloader cannot
proceed past hand-off without those two pieces. This is the natural ceiling of
the "run APPSBL alone" approach — and the exact next milestone.

## SESSION 2 — root cause of the "fall off the end", corrected (2026-09-06)
Armed with the arch_msm7k register map, re-diagnosed the standalone APPSBL end.
Session-1's "NAND hand-off" guess was WRONG. Real sequence:
1. Peripheral init REAL and complete: VIC(0xC0000000)+GPT(0xC0100000)+
   DMOV/ADM(0xA9700000 — the "24 channels" are DMA channels, NOT clock-gates)+
   GPIO1(0xA9200000)+MDDI(0xAA600000, NOT UART). 41 MMIO regs, no error halt.
2. A calibrated software udelay at 0x8e0-0x8f0 (r0=r0*11>>5; subs;bgt) burned ALL
   54M instructions. Forcing r0=1 at 0x8ec cut the run to 2.4M insns with the
   IDENTICAL endpoint -> the delay masks nothing; emulator must special-case it.
3. True transition: after last MDDI write (pc 0x8d78), boot returns and calls
   0x730 = CP15 MMU bring-up: TTBR(c2,c0,0)=0x00028000 (page table built by
   bl 0x142c), SCTLR(c1,c0,0)|=1 MMU ENABLE at 0x77c (SCTLR literal 0x00c50070),
   peripheral-port remap mcr c15,c2,4.
4. After MMU-on the boot jumps to its virtual entry, which should hold the NEXT
   stage (NAND-loaded) we never populated -> execution slides through zeroed RAM.
CONCLUSION: real boundary = MMU/TTBR + NAND-loaded next stage (ROADMAP Phase2/3),
now with exact CP15 values. Standalone-APPSBL RE essentially DONE.
Constants: TTBR=0x28000, SCTLR=0x00c50070, udelay@0x8e0, mmu@0x730, ptbuild@0x142c.

## SESSION 2b — Phase 2 NAND controller DONE + MMU boundary characterized
MMU dead-end (do not re-chase): the standalone APPSBL does ZERO writes to the
page table at TTBR=0x28000 — it assumes a prior PBL/boot-ROM built it AND that
the next stage is already NAND-loaded. Unicorn doesn't model ARMv6 FCSE/PID, so
chasing MMU-on semantics in Unicorn is a blind alley. The real unblock is the
NAND path + loading the next stage, then re-evaluating on a QEMU board.

NAND controller model DELIVERED: tools/nand_controller.py (0xA0A00000), a
behavioral EBI2 controller transcribed from zloader nand.h, backed by the dump.
Self-tests (run `python3 tools/nand_controller.py`) PASS:
- geometry verified from the real images: 1.1.2.bin = 65536 pages @2048;
  1.1.2_spare.bin = 65536 pages @2112 (2048 data + 64 spare).
- FETCH_ID returns the real 0x5580b1ad.
- PAGE_READ returns dump bytes verbatim (page 0 head matches).
- APPSBL is page-aligned inside NAND at 0x16e0000 (page 11712), first word
  18f09fe5 (ARM vector table). (Offsets 0x199c2c/0x1f9c2c are unaligned false
  hits — ignore.)
Next: wire NandController into the probe as the 0xA0A00000 handler and let a
NAND-aware boot flow (with the DMA/ADM command path from nand.c) actually read
pages; then decide QEMU board vs continue Unicorn for the MMU/stage hand-off.

## SESSION 2c — Phase 3: stages run on a MICROKERNEL (major finding)
tools/stage_runner.py loads APPS/AMSS ELF PT_LOAD segments directly, models
peripherals + NAND, jumps to e_entry, measures reach:
- APPS  (entry 0x10000000): ran 318K insns, then EXCEPTION at 0x103dcd14 =
  `svc #0x14` (preceded by `mvn sp,#0x4b`). Touched NO MMIO before it.
- AMSS  (entry 0x00a00000): ran 285K insns, then EXCEPTION at 0x16e9aa58 =
  the IDENTICAL `svc #0x14` thunk. Also no MMIO.
INTERPRETATION: both stages are microkernel TASKS, not bare-metal. The shared
`mvn sp,#0x4b; svc #0x14` thunk is a syscall into Qualcomm's REX-on-L4/Iguana
microkernel. Running a stage standalone hits its first kernel syscall almost
immediately (task/thread setup). This means a pure-LLE path must EITHER emulate
the L4 microkernel syscalls (svc handlers) OR boot the whole chain so the kernel
is present. Neither stage touches the NAND controller in this early window
(confirming the NAND model is for the boot/loader phase, not the running stage).
CONSEQUENCE for ROADMAP: Phase 4 is really "L4 microkernel boundary", bigger and
earlier than "modem RPC + Adreno". The tractable next step is an SVC hook that
decodes svc #0x14 and the following calls to map the microkernel ABI — OR pivot
to a QEMU board and let a full-chain boot bring the kernel up. Recommend the SVC
ABI mapping first (cheap, in Unicorn) to learn the syscall surface before
committing to full kernel emulation.

## SESSION 2d — L4 SVC ABI recon (Phase 4 recon, tool delivered)
tools/l4_svc_recon.py installs a UC_HOOK_INTR handler that decodes each L4
syscall (svc immediate + SP selector magic + r0-r7), returns benignly, and lets
the task advance. Findings on APPS:
- First syscall: `svc #0x14`, selector sp=0xffffffb4 (~0x4b), args r0-r2 are
  OUTPUT pointers on the stack (0xffeffe0/dc/d8) — classic L4 ipc/thread call
  returning values through caller-supplied pointers. (Unicorn advances PC past
  the svc, so decode reads PC-4; high stack 0xffe00000 must be mapped or the
  post-svc `strne r1,[r4]` faults.)
- After a benign return (r0-r3=0), APPS runs 8M more instructions with NO further
  syscall and settles into a loop at 0x01d4d788 in LOW RAM — i.e. it copied/
  relocated code into low RAM and runs there (runtime image not backed by the
  ELF file). Progress is real; the single early syscall was a setup/probe call.
INTERPRETATION: the L4 syscall surface reachable early is tiny (one call), then
the task self-relocates and runs. A minimal L4 shim may only need a handful of
syscalls to get much further. Next recon: map the high stack properly, keep the
run going past the relocation loop (raise insn budget / detect the loop head and
let it settle), and catalog the FULL syscall set before deciding on a shim vs a
QEMU full-chain boot.
Constants: L4 syscall thunk `mov ip,sp; mvn sp,#N; svc #imm`; first call
svc#0x14 sel ~0x4b; APPS relocates into low RAM (~0x01d40000 region).

## SESSION 2e — KERNEL IDENTIFIED: L4e (Pistachio-embedded) + REX RTOS
Strings in AMSS/APPS pin the microkernel exactly:
- `l4e_min_pagesize()`, `L4_Restore fell through!!!`, `base % l4e_min_pagesize()
  == 0` -> NICTA L4-embedded (L4e / Pistachio-embedded, the OKL4 lineage).
- `rex_self()`, `rex_is_in_irq_mode()`, `rex_get_timer()`, `&*_tcb` -> Qualcomm's
  REX RTOS running as tasks ON TOP of L4e. This is the classic AMSS architecture.
So the `svc #0x14` sel ~0x4b is an L4e system call (SP-magic ABI). Benign all-zero
returns are WRONG: after the svc, the stub stores r1/r2/r3 through caller output
pointers and computes addresses from them; returning 0 makes APPS derail into a
7M-instruction NOP-slide through zeroed RAM (0x01d4d788 -> 0x03801688, purely
sequential, no syscalls/MMIO) — proof that the syscall's RESULT semantics matter.
CONCLUSION (honest): the real Phase-4 boundary is implementing L4e syscall
semantics, not just cataloging them. The recon did its job — it identified the
exact kernel (so we can use the public L4e/Pistachio-embedded syscall ABI as
reference) and proved a zero-stub is insufficient. Decision point for next
session: (a) build a minimal L4e syscall shim keyed to sel ~0x4b using the
Pistachio-embedded ABI, or (b) pivot to a QEMU full-chain boot so the real L4e
kernel is present and dispatches its own syscalls. Given a real kernel with TCBs/
IPC/timers is involved, (b) is now the honest higher-probability path to a
running system; (a) remains useful to learn which syscalls REX actually needs.
Refs to pull next session: Pistachio-embedded / OKL4 ARM syscall ABI (SP-selector
magic values), REX task model.

## SESSION 2f — L4e syscall thunk set DEFINED (fingerprint, byte-verified)
Located the L4e syscall stub block in APPS ELF (little-endian word EF00mmii, so
search bytes = ii mm 00 ef):
  svc #0x14  x7   (sel ~0x4b)  - 3-out stub (r1,r2,r3 via r4/r5/r6)
  svc #0x1404 x9  (sel ~0xfb)
  svc #0x1408 x5  (sel ~0xf7)
  svc #0x140c x5  (sel ~0xf3)  - 6-out stub
  svc #0x1410 x5  (sel ~0xef)  - 2-out stub (r1,r2 via r7/r8)
  svc #0x1414 x5
Total ~36 thunks across ~6 distinct syscalls. The stub block at 0x103dcc00 is
byte-verified: `mov ip,sp; mvn sp,#~sel; svc #imm; <store outs>; pop`.
CONFIRMED the sel ~0x4b/svc#0x14 we first trapped is 3-output syscall — its
result values (r1/r2/r3 through caller output pointers) are what our zero-return
probe corrupted. This scopes the L4e shim: SIX syscalls, not an unknown storm.
PITFALL (matter): naive "scan every word for SVC" floods with false positives
(data decodes as svc); and endianness must be little (EF00mmii -> bytes ii mm 00
ef). Use the byte pattern, not a capstone word pass.

## SESSION 2g — shim v1 result: MAP_CONTROL boundary (evidence, not guesswork)
tools/l4e_shim_runner.py intercepts the 6 L4e syscalls, returns success, and
serves a UTCB (pointer at 0xff000ff0 -> 0xff0f0000, MRs at +64). Result:
- The FIRST syscall is svc#0x14 = MAP_CONTROL (sel ~0x4b), 3-output, args
  r0-r2 = output pointers (0xffeffe0/dc/d8) on the (mapped) high stack.
- After returning success, APPS runs ~6M insns, NO further syscalls, then
  derails: classify_post_shim.py proves it is a 1999/1999-sequential NOP-slide
  through zeroed RAM (andeq r0,r0,r0 = 0x00000000 words), final pc 0x481d08.
- root cause: we did NOT relocate the task or map low RAM. MAP_CONTROL's real
  semantic is SPACE/MAPPING (maptns pages); the task expects it to make the
  low-RAM region it jumps to contain VALID code. Zero return = derail into the
  zero-memory trap (lessons/other-emulators.md lesson #5), now proven again.
CONCLUSION (honest): a value-level shim is NOT enough because MAP_CONTROL +
the kernel's OWN page setup is what maps the task's low-RAM image. Options:
  (a) implement MAP_CONTROL as REAL mapping in Unicorn's address space: the task
      tells the kernel which physical pages map to which vaddrs; emulate that so
      low RAM (0x0048...) holds the relocated image. This needs decoding the
      MAP_CONTROL args -> non-trivial, and is essentially re-implementing the
      kernel MMU.
  (b) pivot to QEMU full-chain (PBL->APPSBL->L4e->AMSS/APPS) where the REAL
      OKL4 kernel does its own mapping. This is now clearly the higher-probability
      path: the shim has proven the APP is a real REX/L4e task, but "reimplement
      enough kernel MMU to make MAP_CONTROL work" is most of a kernel anyway.
Recommendation: STOP shim-only. Commit to the QEMU full-chain boot. The shim
served its purpose: it proved (1) kernel is OKL4/L4e + REX, (2) syscall set is
small (6), (3) the early stall is MAP_CONTROL mapping, all with hard evidence.
Next phase = QEMU board with real L4e; bring up PBL->APPSBL->stages.

## SESSION 2h — REAL MMU map recovered (breakthrough for QEMU-free path)
The tripleoxygen dump console__zeebo__mmu.txt has the REAL VA->PA maps of BOTH
cores taken while running. For the ARM11 (BREW) — the one that matters:
- periphery re-mapped to c0 block: c5300000->c0000000 (VIC), c5400000->c0100000
  (GPT), c1d00000->aa600000 (MDDI), c2a00000->a9700000 (DMOV), c2f00000->a9200000
  (GPIO1), c3200000->a8600000 (CLK). All the blocks our probe found.
- RAM: 0x00100000..0x0080000 are section identity; 0x10xxxxxx identity;
  f0000000->10000000, f0100000->10100000 (ELF f0000000 maps to PA 0x10000000).
- APPS ELF vaddrs map into RAM at 0x10100000..0x11400000 (identity section) and
  b0000000/b0100000/b0d00000/b0e00000 -> 0x100a3xxx (COARSE pages in RAM).
- UART1 A9A0.. not shown; check next stage.
KEY REALIZATION: the APPS derail to 0x0048xxxx is into VALID mapped RAM (ARM11
00400000..00800000 are identity sections), NOT an unmapped fault. So the task is
not "sliding" — it is EXECUTING a page that is mapped but EMPTY (we never loaded
the loader's low-RAM image there). The loader (or the kernel's MAP_CONTROL)
maps/loads the task text & data into low RAM (0x0048..., 0x00a7xxxx by the ARM9
map side) and the ELF vaddrs are ALIASED there via the f0000000/b0/10x MMU page
mapping. Our Unicorn loaded ELF at its vaddrs but DID NOT populate the low-RAM
pages, so control into 0x0048 hits zeros.
QEMU-FREE ALTERNATIVE (new): model the real MMU map as a TRANSLATION layer in the
Unicorn hooks — intercept each load/store/fetch and translate vaddr->physical
using this dump's tables, backing the physical side with the loader layout. Then
the APPS sees the same aliasing the hardware does: code writable at low RAM and
readable at its f0000000/b0 vaddrs. This is MORE tractable than a full kernel:
we have the exact tables. Implement as: map physical low RAM 0x0040, 0x00a7, 0x10a0
in Unicorn; on the 6 L4e syscalls do REAl mapping like the kernel (set up the
vaddr->pa alias by copying/mirroring). At minimum, pre-populate the low-RAM
regions the boot produces so 0x0048 has the loader's image.
NEXT: build the translation layer from this dump and re-run APPS.

## SESSION 2i — MMU translation layer BUILT; APPS advances to REX idle (big progress)
tools/build_mmu.py + arm11_mmu.py: parse the real ARM11 VA->PA map (150 entries:
sections + 4K coarse pages). tools/mmu_runner.py translates vaddr->pa in the
fetch/data hooks and loads the APPS ELF at PA. Result (vs shim-only run):
- INSNS 131K, SVCs 2000 (was 1), NO derail to 0x0048 (translation picked it up).
- The APPS climbs the REX bootstrap: MAP_CONTROL succeeded (we pre-mapped +
  returned success), task created its context, entered a wait loop.
- It idles in L4_Ipc/L4_WaitNotify with to=0 from=0 tag=0 (L4 nil-thread wait),
  repeatedly — the REX scheduler idle spin awaiting notifications/timers.
CAVEAT / next refinement (honest): the `b000fffc` site decodes as zeros (andeq
r0,r0,r0) = the ELF segment at vaddr 0xb0000000 is loaded at identity PA, but in
the real map `b0100000 -> 100a3800` (coarse PAGE in high RAM). So the b0xxx
segment must be MIRRORED to its coarse PA (0x100a3xxx) for the alias to carry the
relocated code; right now the task jumps to empty b0xxx and we get a false svc.
NEXT: implement coarse-page mirroring (vaddr b0xxx <-> pa 100a3xxx) in mmu_runner
so the b0xxx runtime image is populated; then the REX wait loop should receive a
timer/interrupt notification (implement L4 timer tick -> notify) to break idle.

## SESSION 2j — CORRECTION: the "REX idle" claim was WRONG (verified derail)
Honest reversal of session-2i. verify_idle.py + mmu_runner_v2.py show the site
`0xb000fffc` has op=None (instruction UNREADABLE), imm=0, r0-3=0, repeating
thousands of times — this is NOT a legitimate L4_Ipc idle. It is another derail:
after MAP_CONTROL the task jumps to 0xb000fffc expecting the RELOCATED runtime a
root-of-truth-LLM would not have = b000fffc is a RawBinary? No: the segment
b0000000 (filesz=memsz=0xf207) ends at 0x0xb000f207; 0xb000fffc is BEYOND it,
and no loader-relocation data exists there. The task derails into a region that
only the real loader/APPSBL would have populated. Zero-filling/mirroring does not
help: mmu_runner_v2 (fills all identity + coarse) gives the SAME derail.
CONCLUSION (hard): a standalone stage without the boot chain CANNOT get past
this, because the low/b0 RAM region the task jumps into is built by the loader's
RELOCATION of the OS image — work we do not have the artifacts for. Any shim or
MMU-mirror of the ELF alone fundamentally misses it. The legitimate, QEMU-free
path is to BOOT THE CHAIN in Unicorn: run APPSBL fully (it already does all
peripheral init + MMU enable) and let IT load the OS/stages through our NAND
model — the real boot code does the relocation itself, no kernel repo needed.
This is the honest next step that actually has a chance.
PITFALL: do not trust insn-count / repeated-INTR as "progress" — a derail reading
unmapped memory can spin the INTR hook thousands of times. Always verify the svc
instruction bytes (op must decode as a real EFxxxxxx SVC) before claiming the task
is alive.

## SESSION 2k — Boot APPSBL with NAND wired: does NOT advance (honest result)
tools/boot_chain_unuicorn.py runs APPSBL fully with the NAND controller model
hooked at 0xa0a00000 (writes->nand.write, reads->nand.read). Result:
- insns 2.4M (udelay short-circuited), final pc 0x5fffc, last real MMIO the MDDI
  final write at pc 0x8d78 — EXACTLY where standalone APPSBL always stopped.
- **ZERO NAND controller touches** (0 a0a00000 accesses). APPSBL does hardware
  bring-up ONLY (VIC/GPT/DMOV/GPIO/MDDI + MMU-enable) and never reads NAND.
CONCLUSION (corrected): the NEXT boot component — the one that reads NAND and
relocates the OS image for AMSS/APPS — is NOT the APPSBL. It is a later element
in the chain (the L4e/microkernel bootstrapper or a following loader). APPSBL
handsoff to it; it is the missing link that performs the relocation the standalone
APPS expected (derail to 0xb000fffc). To boot the chain in Unicorn we must find
and run THAT component, which requires knowing how APPSBL handsoff (where it
jumps/branches beyond pc 0x8d78 and what address it targets). Next: trace APPSBL
past pc 0x8d78 to capture its handoff target and the load map it passes on.

## SESSION 2l — APPSBL NAND path traced: real flash accesses identified
tools/trace_mmu_on.py traces APPSBL control flow past MMU-enable. Findings:
- 0xd38 -> bl 0xf088 (flash/NAND read). 0xf088 -> 0xf25c (if r0<8), 0xf508 (ID
  lookup), 0xf63c (actual NAND reg writes). 0xd58 chain calls 0xf63c.
- 0xf63c is the REAL NAND accessor: `str r0,[r5,#4]` (NAND_ADDR1) and
  `str r0,[r5,#0x10]` (NAND_EXEC_CMD) where r5=0xa0a00000 base. Our NandController
  is wired for these.
- BUT the flash accessor first calls 0xf508: a TABLE LOOKUP (scans a config table
  in ROM comparing bytes, sums an offset) BEFORE touching NAND regs. The boot
  stalls in that lookup when the table doesn't match, never reaching the MMIO.
- 0xbe2c is a NOP gate (bx lr), probably a weak/empty hook.
NEXT: model the flash config table that 0xf508 scans (it encodes the NAND geometry
per device ID) so 0xf088 completes a real READ and APPSBL loads the stage. Then
the stage-loading path becomes fully reachable in Unicorn.

## SESSION 2m — Honest ceiling: APPSBL reach confirmed, NAND handoff NOT reached
probe_nand_emission.py (clean, 20M insn, udelay short-circuited): APPSBL reaches
the handoff code (we see pc 0xdc0 -> bl 0xf088 in trace_mmu_on), BUT:
- **ZERO NAND MMIO touches** (0 a0a00000) and final pc still 0x5fffc (slips off
  the padding after the handoff routine).
- Reason: 0xf088 with r0<8 takes the LOW-ID fast path (bls 0xf25c) that only does
  an in-RAM geometry lookup (r4=0x1f00000) — no NAND register I/O. The MMIO
  writer 0xf63c is only reached on a bigger-id path that never triggers here.
HONEST ASSESSMENT: the remaining work is genuine full boot-chain bring-up
(handoff reliability, flash geometry model, then load the L4e/AMSS/APPS stage) —
a multi-session project, no longer a quick iterative probe. We have proven the
bootable path EXISTS (VMU via real MMU map, NAND accessor at 0xf088/0xf63c,
device-ID table at 0xf8c0, all 6 syscalls mappped). The next increment would be
forcing the flash geometry so 0xf088 takes the real MMIO path, then tracing the
stage load to completion — but each increment is now hours, not minutes.
Milestones banked: real MMU map, NAND controller model (self-tested), L4e syscall
set identified (OKL4), REX API + QSC1110 reference, boot path traced to handoff.

## SESSION 2n — increment (a) tested: NAND forced-path does NOT emit MMIO either
tools/incr_force_nand.py forces r0>=8 at 0xf088 (escape the low-id fast-path) +
hooks 0xa0a00000. Result: STILL 0 NAND MMIO touches; APPSBL again slips to 0x5fffc.
CRUCIAL correction: 0xf63c is NOT the NAND MMIO writer. Its r5 literal = 0xe8bd8070
(RAM), and its `str [r5,#4/#0x10/#0xc/#0x14]` write to a RAM descriptor at
0x1f00000 (block geometry/timing table), not the controller. The REAL 0xa0a00000
literals live in LATER functions (0x7100-0x8334, 0xc6ac, 0xe2b4, ...) that the
boot only reaches after much more of the chain. CONCLUSION: neither the un-forced
nor the forced-flash boot reaches the NAND controller in our emulation; the path
is genuinely full boot-chain bring-up (the element that reads NAND + relocates the
OS is later than APPSBL's reachable code). Increment (a) is exhausted. RESUME.md
documents the honest state and next steps for a dedicated bring-up session.

## Next milestone (bigger piece of work)
1. Model the NAND controller at 0xa0a00000 (+ MPU 0xa0b00000): page 2048B,
   64 pages/block, spare 64B, ID 0x5580b1ad (all in KB). Feed it from 1.1.2.bin.
2. Load AMSS (entry 0x00a00000) and/or APPS (entry 0x10000000) ELF segments into
   RAM at their phdr vaddrs so the hand-off jump lands on real code.
3. Re-probe: watch the NEXT wave of registers the loaded stage touches. Only
   after this do we reach the modem (ARM9 / ONCRPC-PROC_COMM) and Adreno 130.

## Register map (final for this session): see docs/register-map.md
41 MSM7201A registers modeled across 5 blocks; error halt eliminated.
1. Decode `bl 0x6bc` (pre-MMIO early init) and the RMW at 0xc384 / read at 0x169c
   to guess what each reg represents (status vs config vs ID).
2. Give those regs sticky "sane" return values, re-run, and watch how much
   further the boot gets — building the register map empirically.
3. Only once the register set stabilizes, decide QEMU board vs staying on the
   Unicorn harness (Unicorn is faster to iterate for pure register discovery;
   QEMU matters later for interrupts/DMA/timing fidelity).
