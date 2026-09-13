# ig_naming Stall Investigation — VA 0xb010333a

Static analysis only (no emulator run). Firmware: `nand/1.1.2_APPS.bin` (ELF, MD5 preserved).

## 1. ELF layout for the ig_naming range
- `ig_naming` lives in PT_LOAD segment: `p_vaddr=0xb0100000, p_offset=0x60000, filesz=memsz=0x702b`.
- The scattered-page assumption is wrong for THIS segment: it is contiguous. File offset = `VA - 0xb0100000 + 0x60000`.
- File offset of `0xb010333a` = **`0x6333a`**.

## 2. Actual bytes at the stall point (ARM word-aligned)
```
b0103330: e1a00008   mov  r0, r8
b0103334: e58dc004   str  ip, [sp, #4]
b0103338: ebfffe5e   bl   0xb0102cb8      <-- the real instruction
b010333c: e5942ff0   ldr  r2, [r4, #0xff0]
```
`0xb010333a` is **2 bytes INSIDE the 4-byte `bl` at 0xb0103338** — an odd/Thumb-granularity
PC landing in the middle of a legitimate ARM instruction. It is NOT a real instruction boundary.

## 3. What the BL target does (0xb0102cb8)
```
b0102cb8: e92d4ffa   push {...,lr}       ; L4_Ipc wrapper prologue
...
b0102ce8: ef001400   SVC #0x1400         ; imm&0xff = 0x00 = L4_Ipc  (RECEIVE/wait)
b0102cec: e8bd0006   pop  {r1, r2}       ; pc = svc+4 lands here on return
```
So `ig_naming` at `0xb0103338` calls its own **local ARM L4_Ipc wrapper**, which issues
`SVC #0x1400` = **syscall 0x00 (L4_Ipc wait)**. This is legacy ARM code embedded inside the
`0xb0100000-0xb0120000` range — the same "local ARM copy" situation QW42 documented, but for
a DIFFERENT syscall.

## 4. Instruction at the stall
There is no instruction at `0xb010333a`. The PC is corrupted: the emulator resumed the block
in **Thumb** mode (T-bit=1) when it should be **ARM** (T-bit=0), so decode/PC tracking drifted
by 2 bytes and stopped mid-`bl`.

## 5. Root cause — T-bit misclassification, NOT a deadlock
QW42 fixed exactly this class of bug, but only in the `syscall == 0x0c` (L4_ExchangeRegisters)
branch of the intr hook (`zeebo_lle_main.cpp:2482-2483`):
```cpp
bool local_arm_copy = (pc >= 0xb0100000u && pc < 0xb0120000u);
target_pc = (caller_is_kernel_stub || local_arm_copy) ? (pc & ~1u) : (pc | 1u);
```
The **L4_Ipc branch (`syscall == 0x00`, non-handoff `else`, line ~2452) was NOT patched**:
```cpp
} else {
    target_pc = apply_tbit(pc);   // <-- forces Thumb (pc|1) for any pc outside 0xb000_0000-0xb002_0000
    ...
}
```
`apply_tbit()` (line 2432) treats every caller outside `0xb0000000-0xb0020000` as Thumb. When the
L4_Ipc SVC returns to `0xb0102cec` (pc = svc+4, inside the ig_naming local ARM copy), `apply_tbit`
sets bit0 → the ARM wrapper is resumed as Thumb → decode drifts → PC lands at `0xb010333a`
(2 bytes into the `bl` at 0xb0103338). This is the identical failure mode QW42 fixed for
ExchangeRegisters, now reappearing on the L4_Ipc return path.

**This is an emulator T-bit classification bug, not an L4 scheduler bug and not an IPC-model
deadlock.** ig_naming is genuinely executing forward (it reaches its own L4_Ipc wrapper call);
the stall is purely mis-decoding of the return, not a thread that never gets scheduled.

## 6. Deadlock hypothesis — assessed and rejected
The task's deadlock scenario (ig_naming waits on an IPC whose sender is never scheduled) is not
supported by the evidence:
- The stall PC is deterministically 2 bytes inside a known ARM `bl`, which is a decode artifact,
  not a wait state. A real IPC-wait deadlock would park the core at the `svc #0x1400` site
  (`0xb0102ce8`) or at the fixed server-loop wait `0xb000c834`, not mid-instruction at `0xb010333a`.
- The ROADMAP itself (QW42, line 512) already attributes `0xb010333a` to "efeito colateral do
  modo errado" (T-bit side effect), consistent with this finding.
- Registry model (test_l4_ipc_dispatch.cpp): ig_naming=tid6, quartz_servers=tid13, amss=tid23;
  the intended first handshake is ig_naming → amss (`msg.mr[1]=0x10137000`). That path is only
  reached AFTER ig_naming finishes its own name registrations, which requires this L4_Ipc return
  to decode correctly first. So the T-bit bug blocks reaching any real IPC/scheduling question.

## 7. Next step to resolve
Apply the QW42 `local_arm_copy` guard to the **L4_Ipc (`syscall == 0x00`) non-handoff branch** in
`zeebo_lle_main.cpp` (~line 2452), mirroring lines 2482-2483:
```cpp
} else {
    bool local_arm_copy = (pc >= 0xb0100000u && pc < 0xb0120000u);
    target_pc = (caller_is_kernel_stub || local_arm_copy) ? (pc & ~1u) : (pc | 1u);
    if (ip) uc_reg_write(uc, UC_ARM_REG_SP, &ip);
    uc_reg_write(uc, UC_ARM_REG_PC, &target_pc);
    uc_ctl_remove_cache(uc, 0xb000c800, 0x100);
}
```
Add a TDD sibling of `test_exregs_ig_naming.cpp` for the L4_Ipc resume (SVC #0x1400 from the
ig_naming local ARM copy) — RED with generic `apply_tbit`, GREEN with the range guard.

**Better/durable fix (recommended over per-case patches):** the T-bit for an SVC return should be
derived from the *actual instruction format at the call site*, not from a PC range heuristic. Two
robust options:
1. Read the saved CPSR T-bit that the hardware/Unicorn had at SVC entry and restore exactly that,
   instead of guessing per range (the range table will keep leaking, one syscall case at a time).
2. Classify by decoding: if the halfword at `pc-2` is `0xDFxx` it was a Thumb `svc`, else ARM.
Either removes the whole class of "another local ARM copy in another syscall case" regressions.

## 8. What QW42's fix covers vs. not
- Covers: `syscall == 0x0c` (L4_ExchangeRegisters) resume from the ig_naming local ARM copy.
- Does NOT cover: `syscall == 0x00` (L4_Ipc) — the branch that actually fires here — nor 0x08/0x14/
  0x18, all of which route through `apply_tbit(pc)` and would mis-flag any local ARM copy in
  `0xb0100000-0xb0120000` the same way. `test_exregs_ig_naming.cpp` only exercises the 0x0c path
  (SVC #0x140c), so it cannot catch this L4_Ipc regression.

## Verdict
**Emulator T-bit classification bug** (mode-restore heuristic), a direct sibling of the QW42
finding, in the L4_Ipc (syscall 0x00) SVC-return branch. Not a scheduler bug, not an IPC-model
deadlock.
