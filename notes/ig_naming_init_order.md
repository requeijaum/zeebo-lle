# ig_naming Boot-Time IPC Init Order — WHO sends the first message, and WHY it never arrives

Static analysis of `nand/1.1.2_APPS.bin` (ELF, entry 0x10000000; Iguana root server in
`0xb000xxxx`; ig_naming in PT_LOAD va=0xb0100000 off=0x60000 fsz=0x702b). Disassembly via
capstone (ARM mode). Cross-checked against `zeebo_lle_main.cpp`, `zeebo_l4_ipc.h`,
`zeebo_l4_thread.h`, and prior notes. openzeobo corpus dir is **empty** (no zloader/mddi
evidence available). zeebulator HLE has no naming-server model (`core/brew/nid_table.cpp`
is BREW NID resolution, not L4 naming).

## 1. Registered "servers" in the LLE model (evidence: zeebo_lle_main.cpp:2328-2334)

Registration is NOT parsed from bootinfo. It is **inferred at runtime** inside the
`L4_ExchangeRegisters` (svc 0x140c) handler purely from the `new_ip` range:

| name           | VA range trigger        | tid          | iface | source line |
|----------------|-------------------------|--------------|-------|-------------|
| ig_naming      | 0xb0100000–0xb0120000   | dest (dynamic)| 1    | 2329        |
| quartz_servers | 0xb0300000–0xb0330000   | dest (dynamic)| 2    | 2331        |
| amss           | 0x10137000–0x10200000   | dest (dynamic)| 3    | 2333        |

The `tid` values 6/13/23 in `test_l4_ipc_dispatch.cpp` are **test fixtures**, not observed
firmware TIDs. Real tids are whatever `dest` the firmware passes to ExchangeRegisters.

**Boot order (from OKL4/Iguana, POST_BI_EXECUTE_BOOT.md):** all initial tasks are declared
in `__okl4_bootinfo` and instantiated by `bi_execute` (`new_pd`→`new_thread`→`run_thread`)
*before* `iguana_server_loop`. So the intended live-thread set at the stall is:
Iguana root server (0xb000xxxx) + ig_naming + quartz_servers + the BREW/AMSS bootstrap
thread. In the LLE only threads that actually execute an ExchangeRegisters get into
`thread_table_`, so the set is usually **just ig_naming** (see §5).

## 2. ig_naming is a ReplyWait server; the wait is at its thread entry

- Thread entry / wait wrapper: **0xb0105d88** (`push {r4,lr}` … `bl 0xb0102c10`). It has
  **zero BL callers** in the segment → it is the thread's top-level entry, invoked by the
  scheduler, not by a local call. It builds a receive descriptor (`mov r1,#0x200`,
  BR window) then calls the IPC wrapper.
- Main dispatch loop: **0xb01056f0** = `bl 0xb0105e04` (recv) → decode → `bl 0xb0102cb8`
  (reply-send). This is the classic idl4/Magpie **ReplyWait** skeleton.
- IPC wrappers (disassembled, corrects a prior note):
  - `0xb0102c10` = **svc #0x140c** = L4_ExchangeRegisters (NOT the IPC wait).
  - `0xb0102cb8` = **svc #0x1400** = L4_Ipc (the real send/recv). Loads UTCB from
    `[0xff000ff0]`, fills MR0..MR5 at `utcb+0x40..0x54`, traps, unpacks reply.
- Reply payload observed at 0xb0105708-0x571c: `MR0(utcb+0x40)=0x50006`,
  `MR1(utcb+0x44)=0x16` (label 0x16). So ig_naming **answers** requests with label 0x16.

## 3. The FIRST IPC that should hit ig_naming

ig_naming is the **name server**: peers REGISTER, then LOOK UP. On boot it enters
`recv` (0xb0105d88→svc 0x1400) and blocks until a **client sends a REGISTER/announce**.

- Expected first sender: the **first non-naming initial task started by `bi_execute`** —
  per OKL4 layering that is the Iguana root server publishing its own env caps, then the
  next `run_thread` task (the BREW/AMSS bootstrap PD, entry in APPS 0x10137000 region, or
  quartz_servers at 0xb0300000). It performs an `L4_Ipc` **Call** to ig_naming's TID with
  a name string + cap.
- Message shape (from the reply/handler side, best evidence we have): untyped word count
  in MR0, **label 0x16** in MR1 (the first case of ig_naming's method jump table — the same
  0x16 the root server's own loop uses at POST_BI_EXECUTE_BOOT §4). The LLE test corpus
  models the intended handshake as `msg.mr = {0x16, 0x10137000, 0, 1}` (register amss's
  entry) — **plausible but unconfirmed** as the literal first message.

> ⚠ **RECLASSIFICADO (ver `notes/core1_boot_estado_real.md`, 2026-09-10).**
> (A) T-bit no branch 0x00: **ja corrigido** (zeebo_lle_main.cpp:3063/3069).
> (B) "no real sender thread": nao e latente, e **inalcancavel** — medido ZERO
> L4_Ipc e ZERO ExchangeRegisters no boot (8 SVCs, todos 0x14 MapControl). Todo
> o `case 0x00`, incluindo a injecao sintetica MR1=0x16, e codigo morto aqui.

## 4. What the LLE is missing / doing wrong

Two distinct problems, in priority order:

**(A) [ROOT CAUSE, highest confidence] T-bit misclassification on the L4_Ipc SVC return.**
`apply_tbit()` (line 2432-2434) forces Thumb (`pc|1`) for any caller pc outside
0xb0000000–0xb0020000. ig_naming (0xb0100000-0xb0120000) is **pure ARM** but carries its
own *local copy* of the trap stubs (0xb0102c10/0xb0102cb8). QW42 already patched exactly
this for syscall 0x0c (line 2482-2483 `local_arm_copy` guard) but the **0x00 (L4_Ipc)
branch at line 2452 was NOT patched** — it still calls bare `apply_tbit(pc)`. When the
recv SVC at 0xb0102ce8 returns to 0xb0102cec (inside ig_naming's local ARM copy),
apply_tbit sets bit0 → ig_naming resumes as Thumb → decode drifts → PC lands 2 bytes
inside the `bl` at 0xb0103338 (the reported 0xb010333a). This is a **decode artifact, not
a scheduler deadlock** — ig_naming is genuinely running forward. Matches
`ig_naming_investigation.md` verdict.

**(B) [latent, would surface after A is fixed] No real sender thread is scheduled.**
The LLE never routes a real inter-thread IPC. Instead the 0x00 handler *fakes* the first
message: lines 2232-2237 inject `MR0=1, MR1=0x16` into the UTCB whenever the server waits
with `mr1<0x16`. And `pick_next_thread` (zeebo_l4_thread.h:101) round-robins only over
threads marked `active` — which only happens when an ExchangeRegisters with
HALTFLAG-resume (control≈0x11e) is observed. If ig_naming is the **only** started thread,
`pick_next_thread` returns ig_naming itself → cooperative handoff is a no-op → nothing ever
sends a genuine REGISTER. The synthetic MR injection papers over this for the first
message only; subsequent real lookups/registers have no backing sender.

## 5. WHY it blocks forever (concrete)

- Not a classic wait-forever at the svc site: the PC is deterministically mid-`bl` at
  0xb010333a, which is the (A) T-bit decode drift, confirmed by ROADMAP QW42 ("efeito
  colateral do modo errado").
- Underneath (B): the emulator models **server registration by IP-range sniffing**, not by
  actually creating and scheduling the peer threads that `bi_execute`'s `run_thread`
  records should have started. So even with correct decode, ig_naming's *second* IPC
  (a real lookup from BREW/AMSS) has no live sender — the missing boot step is the
  **instantiation + scheduling of the initial-task threads** (Iguana root + quartz_servers
  + AMSS/BREW bootstrap) that `bi_execute` declares. The LLE currently substitutes the
  hard-coded `core0_.entry=0x1013a000` jump instead of organically running those threads.

## 6. Most actionable NEXT STEP (minimal fix first)

**MINIMAL (unblocks the first IPC, ~2 lines):** apply the QW42 `local_arm_copy` guard to
the L4_Ipc branch. In `zeebo_lle_main.cpp` line ~2452 replace:
```cpp
} else {
    target_pc = apply_tbit(pc); // pc == svc+4 (0xb000c834)
```
with the range-guarded form used at 2482-2483:
```cpp
} else {
    bool local_arm_copy = (pc >= 0xb0100000u && pc < 0xb0120000u);
    target_pc = (caller_is_kernel_stub || local_arm_copy) ? (pc & ~1u) : (pc | 1u);
```
Add a TDD sibling of `test_exregs_ig_naming.cpp` exercising the SVC #0x1400 resume from the
ig_naming local ARM copy (RED with bare apply_tbit, GREEN with the guard).

**DURABLE (kills the whole regression class):** derive the SVC-return T-bit from the actual
call-site instruction, not a PC range — either restore the CPSR T-bit captured at SVC entry,
or decode `[pc-2]`: halfword `0xDFxx` ⇒ Thumb svc, else ARM. This removes "another local ARM
copy in another syscall case" one-offs permanently.

**FOLLOW-UP (problem B):** once decode is correct, model the initial-task threads properly —
have `bi_execute`/`run_thread` create ThreadTable entries (active=true) for the root server,
quartz_servers, and the AMSS/BREW bootstrap PD so `pick_next_thread` yields a *real* sender,
and retire the synthetic MR injection (2232-2237) + the hard-coded 0x1013a000 entry jump.

## Uncertainty labels
- Exact first-message MR payload (label 0x16 + which cap/name): inferred from the
  handler/reply side and the test fixture; **not** confirmed by tracing a real sender.
- Real firmware TIDs of ig_naming/quartz/amss: unknown (fixtures 6/13/23 are test-only).
- openzeobo/zeebulator provided no additional init-order evidence (dir empty / no L4 naming
  model).
