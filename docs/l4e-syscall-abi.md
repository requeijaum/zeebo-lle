# L4e (OKL4 2.1.1 / Pistachio-embedded) ARM syscall ABI — CORRECTED 2026-09-06

Source of truth: the NICTA **L4-embedded Reference Manual (N1 rev2)** — the
authoritative L4e ABI spec (downloaded from cecs.pdx.edu). The Zeebo's
`l4e_min_pagesize`/`L4_Restore` strings match this kernel family.

## AUDIT CORRECTION — the earlier svc-immediate map was WRONG
An audit (2026-09-06, per Rafael's standing request) against the primary source
found my prior claim "svc #0x14 => MAP_CONTROL, ...6 syscalls" was INVALID.
On ARM, per RefMan N1 rev2 Appendix C.2 (verbatim):

> "The system-calls, which are invoked by the `bl` instruction, take the target
> of the calls from the system call link fields in the kernel interface page...
> One may invoke the system calls with any instruction that branches to the
> appropriate target, as long as the return-address is contained in r14."

Example: `bl 0xFE0000B4` = KernelInterface. Consequences:
- The syscall trigger is a **`bl` to a KIP (Kernel Interface Page) link address**,
  NOT `svc #imm` / SP-magic.
- MR0-5 (the first six message registers) map to **r3-r8**; sp and lr preserved.
- UTCB / MyLocalId for ARM is read from a 32-bit load at `0xFF000FF0`.

The OKL4 user-side library (`ipc.spp`: `mov sp,#SYSNUM; swi SWINUM`) is a
LIBRARY CHOICE, not the L4e ABI; and the firmware's `mvn sp,#0x4b; svc #0x14`
matches neither (sp=0xffffffb4 != SYSBASE(=0xffffff00)+num; imm 0x14 not in the
SWINUM=0x1400+num set). The svc#0x14 thunk we trapped in the firmware is likely a
shim/veneer over the real KIP-link syscalls.

## What still holds (verified)
- Kernel IS L4e/OKL4 + REX on top (string evidence in AMSS/APPS).
- Message registers MR0-5 = r3..r8.
- UTCB pointer readable from 0xFF000FF0; KIP syscall-link base ~0xFE00..-0xFE0F.
- Registers r8..r12 are clobbered (undefined) after most syscalls.

## Correct next step
Derive the REAL syscall set from the firmware's KIP link fields (find the KIP,
read the `bl` targets the guest actually branches to), NOT from svc immediates.

## ARM syscall convention (from install-time ipc.spp / lipc.spp)
User-side thunk pattern (identical to our firmware's):
    mov  ip, sp
    mov  sp, #SYSNUM(name)     ; sp = 0xffffff00 + syscall_number
    swi  SWINUM(name)          ; svc immediate = 0x1400 + syscall_number
    ...store out-regs through caller pointers...
The syscall NUMBER is encoded in BOTH the SWI/SVC immediate (low bits, via
SWIBASE=0x1400) and the SP magic (via SYSBASE=0xffffff00). The kernel dispatches
on the immediate; SP carries a magic check.

src/ipc.spp actually does `mov sp, #SYSNUM(ipc)` then `swi SWINUM(ipc)`.
Our firmware builds the same by `mvn sp,#imm` (gives 0xffffffxx) + `svc #imm`.

## Syscall numbers (arch/arm/libs/l4/include/syscalls_asm.h)
#define SYSCALL_ipc                0x0
#define SYSCALL_thread_switch      0x4
#define SYSCALL_thread_control     0x8
#define SYSCALL_exchange_registers 0xc
#define SYSCALL_schedule           0x10
#define SYSCALL_map_control        0x14
#define SYSCALL_space_control      0x18
#define SYSCALL_cache_control      0x20
#define SYSCALL_security_control   0x24
#define SYSCALL_lipc               0x28
#define SYSCALL_platform_control   0x2c
#define SYSCALL_space_switch       0x30
#define SYSCALL_mutex              0x34
#define SYSCALL_mutex_control      0x38
#define SYSCALL_interrupt_control  0x3c
#define SYSCALL_cap_control        0x40
#define SYSCALL_memory_copy        0x44
#define SYSCALL_last               0x44
#define SYSBASE                    0xffffff00
#define SWIBASE                    0x1400
#define SYSNUM(name)  (SYSBASE + SYSCALL_##name)
#define SWINUM(name)  (SWIBASE + SYSCALL_##name)

## Zeebo firmware thunks — INVALIDATED map (do not use)
The same-immediates scan above (svc 0x14/0x1404/.../0x1414) was used to claim a
6-syscall map, but the audit (session 2o) proved the svc immediate is NOT the arm
L4e trigger — it is a `bl` to a KIP link. The `mvn sp,#0x4b; svc #0x14` thunk is a
shim; its SP-magic (0xffffffb4) and immediate (0x14) match neither SYSNUM/SWINUM.
Treat the old "MAP_CONTROL/THREAD_*" mapping as INVALID. (Kept here only as a
record of the wrong turn; do not derive syscall semantics from svc immediates on
ARM L4e.)

## Protocol-level note (still valid)
REX on L4e implements its wait/signal/scheduler by issuing L4e syscalls
(thread_switch, schedule, exchange_registers for context switch; lipc for
inter-task signals). A REX shim therefore services a SMALL IPC/thread surface,
not a full microkernel — but the syscalls must be located via the KIP link
fields, not svc immediates.