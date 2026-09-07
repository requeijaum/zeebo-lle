# L4e (OKL4 2.1.1 / Pistachio-embedded) ARM syscall ABI — Zeebo mapping

Source of truth: OKL4 2.1.1 kernel source from userlandkernel/baseband-research
(NICTA/Open Kernel Labs). The Zeebo's `l4e_min_pagesize`/`L4_Restore` strings
match this kernel family exactly.

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

## Zeebo firmware thunks -> L4e syscall (by immediate)
Byte scan of 1.1.2_APPS.bin (pattern EF00mmii, little-endian ii mm 00 ef):
  svc #0x14             -> syscall #0x14           = MAP_CONTROL         (x7)  [nb: 
                         canonical SWINUM would be 0x1414; the raw 0x14 bases
                         likely on a variant where the guest kernel masks]
  svc #0x1404 (SWINUM)  -> low 0x04 = THREAD_SWITCH                      (x9)
  svc #0x1408 (SWINUM)  -> low 0x08 = THREAD_CONTROL                     (x5)
  svc #0x140c (SWINUM)  -> low 0x0c = EXCHANGE_REGISTERS                 (x5)
  svc #0x1410 (SWINUM)  -> low 0x10 = SCHEDULE                           (x5)
  svc #0x1414 (SWINUM)  -> low 0x14 = MAP_CONTROL                        (x5)
The dominant thunk at runtime was svc #0x14 (sel ~0x4b via mvn sp) — the one our
first trap hit. Both #0x14 and #0x1414 decode to MAP_CONTROL; the same syscall
appears with two encodings (raw vs SWIBASE-based), likely a fast/slow path or a
version skew in the guest library. Confirm by disassembling the guest kernel's
own SVC dispatcher if needed.

## Protocol-level note
REX on L4e implements its wait/signal/scheduler by issuing these L4e syscalls
(thread_switch, schedule, exchange_registers for context switch; lipc for
inter-task signals). A REX shim therefore needs to service a SMALL IPC/thread
surface — not a full microkernel. The 6 syscalls here are the tractable set.