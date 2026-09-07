# REX RTOS — abstraction-layer reference (Zeebo LLE)

Source of truth: real `rex.h` from QSC1110 AMSS (same Qualcomm era/family as the
Zeebo's MSM7201A), pulled from public AMSS trees (chinahby/1110, robcore/qsc1110)
— IDENTICAL 2624-line header. Kept git-ignored at `refs/rex_qsc1110.h` (clean-room
hygiene, lesson #7: read published header STRUCTURE, don't ship Qualcomm impl).

## Why REX is the right abstraction boundary
Our stages (APPS/AMSS) are REX tasks on L4e (Pistachio-embedded). Two intercept
levels exist:
- L4e level (svc #imm, SP-magic selector): what we SEE in execution, but it's the
  raw microkernel ABI (IPC/threads/mappings) — reimplementing it is large.
- REX level (rex_* C API): a SMALL, well-documented RTOS surface that sits just
  above L4e. The game/BREW stack talks to REX, not raw L4e. This matches lesson
  #1 "identify the real ABI boundary and intercept there". Reimplementing REX as
  an abstraction layer (a tiny cooperative scheduler + signals + timers) is
  tractable and is exactly what Rafael proposed.

## REX API surface actually used by the Zeebo firmware
(confirmed by strings in 1.1.2_APPS.bin / 1.1.2_AMSS.bin)
  rex_self()                      -> rex_tcb_type*      current task TCB
  rex_curr_task()                 -> rex_tcb_type*
  rex_wait(sigs)                  -> rex_sigs_type      block until any sig set
  rex_sleep(ms)                   -> void               timed block
  rex_set_timer(timer*, msecs)    -> cnt                arm a timer
  rex_get_timer(timer*)           -> cnt
  rex_clr_timer(timer*)           -> cnt
  rex_set_sigs(tcb*, sigs)        -> rex_sigs_type      set signals (may swap)
  rex_clr_sigs(tcb*, sigs)        -> rex_sigs_type
  rex_get_sigs(tcb*)              -> rex_sigs_type
  rex_is_in_irq_mode()            -> boolean
  rex_def_task(tcb,stack,siz,pri,func,arg) -> void      create task
  rex_kill_task(tcb*)             -> void
  rex_init(istack,isiz,tcb,stack,siz,pri) -> void       bring up first task
  rex_dispatch_event / rex_release_sync_event           (higher-level, on top)
Also referenced: oncrpc_rex.c, rpc_router_os_rex.c (PROC_COMM/ONCRPC over REX —
the ARM11<->ARM9 bridge), rextime.c (timer tick).

## Core types (from rex.h)
- rex_sigs_type: bitmask (unsigned long) of task signals. Wait/set/clr operate on
  it. This is the whole synchronization model — no mutex zoo, just sigs + crit
  sections (rex_crit_sect_type: lock_count + owner tcb + waiter list).
- rex_tcb_type (rex_tcb_struct): linked (next_ptr/prev_ptr) TCB with priority,
  stack, sigs, wait mask, timer. A REX task = one TCB + stack + priority + entry.
- rex_timer_type: per-task countdown, decremented on the timer tick; on expiry
  rex_set_sigs() is called on the owner (that's the whole timer->signal path).

### How our emulators handle REX (verified local)
zeebulator + zeemu are PURE HLE at the BREW/AEE level — they implement the
AEE C++ API surface and NEVER touch REX or L4e (grep hits are false positives
like the game "alpineracerex"). This is the lesson-#1 boundary: games talk
BREW/AEE, not REX. So existing emulators give no REX-reimplementation basis;
they skip it by being above it. Our LLE is the first work that must deal with
REX at all (because we boot real firmware that RUNS it).

## What the QSC1110 GitHub projects are (verified)
chinahby/1110 and robcore/qsc1110 are **Qualcomm QSC1110 baseband firmware
source leaks** (complete AMSS: core/kernel/modem/etc). They contain the FULL
REX RTOS implementation: rex.c (4396 ln), rexarm.s (2254 ln), rexcore.s,
rextime.c (911 ln), rexswm.c, rexcorelog.c, rextp.c, rextls.c, rex.h.
IMPORTANT distinction: this is the **native REX** (runs directly on ARM via its
own SVC trap into rexarm.s's context-switch; no L4e underneath). The Zeebo's
MSM7201A REX runs ON TOP OF L4e (strings prove both l4e_* and rex_* present).
So QSC1110 is a **behavioral/structural reference for REX semantics** — the
task/TCB/signal/timer/scheduler model — NOT the exact L4e syscall ABI. It tells
us WHAT REX does; L4e is the substrate beneath.

## REX core model (from rex.c / rex.h, confirmed)
- rex_tcb_type (struct rex_tcb_struct @428): sp, stack_limit, slices, sigs,
  wait, pri, + task linkage; FIFO/priority linked by next/prev. rex_self() =
  current TCB from rex_curr_task global.
- rex_wait(sigs): if (curr_task->sigs & p_sigs)==0 -> mark task not-ready, call
  rex_sched() (context switch, priority preemption). Returns the sigs you got.
  ASSERT !rex_is_in_irq_mode() && !TASKS_ARE_LOCKED().
- synchronization = a bitmapped sigs word per task + REX_INTLOCK() (interrupt
  lock) + rex_crit_sect_type (lock_count + owner + waiter list). No mutex zoo.
- timers (rextime.c): per-task countdown decremented on a tick; expiry ->
  rex_set_sigs() on owner -> may schedule.
- Context switch substrate: on QSC1110 it is rexarm.s (banked-register save +
  rex_sched). On Zeebo the same rex_sched() ultimately bottoms out in an L4e
  IPI/thread call (svc #0x14 sel ~0x4b).

## Consequences for the abstraction layer
1. Reimplementing REX = a small cooperative/priority scheduler + sigs + timers +
   TCB table (~a few hundred lines in Python or C), exactly Rafael's proposal.
2. The ELFs are STRIPPED (no symtab) so we must LOCATE each rex_* function by
   signature RE. Anchors: assertion strings (rex_self(), rex_wait(), rex.c,
   rextime.c, oncrpc_rex.c), the QSC1110 rex.c/rexarm.s control flow, and the
   svc-thunk (svc #0x14 sel ~0x4b). rex_self() is the easy first target (leaf,
   loads rex_curr_task global, called 21x in APPS).
3. We do NOT need L4e at all for the apps core if we hook rex_* at function
   entry: the shim implements rex semantics and the firmware mostly never sees
   raw L4e. (We must confirm REX-on-L4e firmware doesn't call L4e directly for
   IPC — some IPC may bypass REX.) ONCRPC (oncrpc_rex.c) is the ARM9 bridge and
   is the next wall after REX is shimmed.

# Reimplementation plan (the abstraction layer)
A REX shim is a cooperative round-robin over TCBs with:
  1. a signal word per task; rex_wait blocks the caller until (sigs & mask)!=0;
  2. a millisecond tick that decrements armed timers and set_sigs on expiry;
  3. rex_def_task registers a TCB+entry; a scheduler runs the highest-priority
     ready task; rex_self/curr_task return the running TCB.
This is a few hundred lines — NOT a microkernel. It replaces L4e entirely for the
apps core: instead of dispatching svc traps, we intercept at the rex_* functions.

## The hard prerequisite (honest): symbol resolution
The APPS/AMSS ELFs are STRIPPED (no section headers, no symtab). rex_* names
survive only inside assertion strings (rex_self(), rex_wait(), rex.c, rextime.c,
oncrpc_rex.c). So to intercept at the REX level we must first LOCATE each rex_*
function by signature RE, using:
  - the assertion strings as anchors (a function that references "rex_self()" in
    an assert is near rex_self usage),
  - the QSC1110 rex.c source as a structural template (control flow of rex_wait /
    rex_set_sigs / rex_def_task) to fingerprint the ARM code,
  - the svc-thunk we already found (svc #0x14 sel ~0x4b) which is one L4e call
    that rex_wait/rex_self bottoms out into.
Once located, we hook the function ENTRY (not the svc) and emulate the rex_* in
Python, returning proper rex_sigs_type/tcb pointers — which fixes the zero-return
NOP-slide (the return VALUES now mean something).

## Recommended next step
1. Fingerprint rex_self() first (simplest: returns the current TCB pointer, tiny
   leaf function, heavily called: 21x in APPS). Find it via the "rex_self()"
   assert string's referencing code + a leaf that loads a global and returns.
2. Then rex_wait/rex_set_sigs (the scheduler heart) and rextime tick.
3. Stand up a minimal TCB table + signal words + timer tick in the runner, hook
   those entries, and re-run APPS to see how far a REX-shimmed apps core gets
   before it needs ONCRPC (the ARM9 modem bridge) or a real device.

## Clean-room note
rex.h is a published Qualcomm API header (structure/prototypes), read for ABI
shape — same category as the BREW SDK headers we already use (lesson #7). We
reimplement behavior from the documented contract; we do NOT ship Qualcomm's
rex.c. The header stays git-ignored under refs/.
