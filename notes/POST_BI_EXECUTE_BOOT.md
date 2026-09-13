# Post-`bi_execute` boot sequence → AEECShell / BREW AppMgr

Grounding: OKL4 2.1.1-fix7 source under `refs/okl4-2.1.1-fix7/iguana/server/src/`
(byte-for-byte the code compiled into `1.1.2_APPS.bin`) cross-checked against ARM
disassembly of `nand/1.1.2_APPS.bin` (stripped ELF, entry `0x10000000`; the Iguana
root server lives in the `0xb000xxxx` LOAD segment). Disassembly via capstone
(`/tmp/dis.py` recipe, reproducible).

## 1. `main()` @ `0xb00033d0` — exact call order (disassembled)

Matches `iguana/server/src/main.c` line-for-line:

    0xb0003424  bl 0xb000d5b4   mempool_init            (utcb_init + L4_MapControl pools)
    0xb0003428  bl 0xb00055dc   mutex_init
    0xb000342c  bl 0xb000b1dc   space_init
    0xb0003430  bl 0xb0001e80   bootinfo_init  (pd_init/objtable_init/thread_init folded in 3434/3438/343c)
    0xb0003444  bl 0xb00001fc   bi_execute(__okl4_bootinfo)   <-- r0 = __okl4_bootinfo (0xb0d00000)
    0xb0003448  cmp r0, r4(=0)
    0xb000344c  beq 0xb000345c  ; r0==0 → continue
    0xb0003450/54 ldr r0,="PANIC: Bootinfo did not initialise correctly"; bl 0xb000c594; b .  (dead loop)
    0xb000345c  bl 0xb00017b8   extensions_init          <-- MILESTONE A
    0xb0003460  bl 0xb000aa94   iguana_server_loop       <-- MILESTONE B (never returns)

So the two requested addresses are precisely `extensions_init` and the server loop,
reached only when `bi_execute` returns 0.

## 2. What `bi_execute` already did (the "spawn initial tasks" step)

`bi_execute` (`bootinfo.c`) is the mechanism by which Iguana spawns every initial
task/server. It is NOT a separate "naming server / VFS server" fork step — OKL4/Iguana
has no runtime fork; the entire initial task set is *declared* in the `__okl4_bootinfo`
records and instantiated by the `bi_callbacks_t` table:

- `new_pd`      → creates each protection domain (address space) via `pd_create`.
- `new_pool` / `init_mem2` → virt/phys/direct memory pools (10 VIRT + 5 PHYS in the real copy).
- `new_ms`      → memsections (the ELF segments of each server binary).
- `map`         → `l4e_map` phys→virt into the root space.
- `new_thread`  → `pd_create_thread` + `L4_KDB_SetThreadName` (the human name, e.g. the
                  init task / BREW bootstrap thread).
- `register_stack` / `argv` → sets SP and pushes argc/argv/env frame.
- `grant` / `new_cap` / `grant_cap` / `export_object` → builds each PD's clist + env so a
  task can find its capabilities (this env is the Iguana equivalent of a "naming service":
  string keys → caps/threads/pools, closed by `close_obj_env` with the `OKL4_CLIST` key).
- `run_thread`  → **the actual launch**: `thread_start(td->thrd, td->ip, sp)` with the
  stack frame `obj_env_ptr / argc / argv / user_main`.

Thus the "initial servers" (device/timer/serial/vbus/vfs-style extensions and the BREW
bootstrap PD) are all brought up by iterated `new_thread`+`run_thread` records inside
`bi_execute`, before `extensions_init` runs.

## 3. `extensions_init` @ `0xb00017b8` (MILESTONE A)

Source (`extensions.c`): walks the env item list (`env_get_next`), and for every item of
type `ENV_ELF_FILE` whose `elf_file_type == ELF_TYPE_EXTENSION`, casts
`env_elf_file_entry()` to a function pointer and **calls it in-process** (same address
space as the Iguana server). Extensions are static constructors for kernel-adjacent
services, run once, synchronously, before the IPC loop.

Disasm: `0xb00017b8` is a 3-instruction thunk (`ldr r0,=envlist; mov r1,#0; b 0xb0006b88`)
into the shared `env_get_next` iterator at `0xb0006b88` (the loop that does the
`(item->type>>4)&0x3f` decode visible at `0xb0001810`+). No extension entry is invoked
unless an `ELF_TYPE_EXTENSION` env item exists.

## 4. `iguana_server_loop` @ `0xb000aa94` (MILESTONE B) — the IPC dispatcher

This is the root server's infinite `L4_ReplyWait` loop (`iguana_server.c`). Disassembly
confirms the classic Magpie/idl4 server skeleton:

- Reads own UTCB via `MyUTCB = *(0xff000ff0)` then `+0x40` (MR base) — the `0xff0` KIP
  UTCB pointer. `r5 = utcb+0x40` is the MR window.
- `0xb000aac4` label = top of loop: builds the reply tag, calls the L4 IPC syscall
  wrapper `0xb000c800` (this is `L4_Ipc`/ReplyWait; the `0x14` bit games at
  `0xb000aad8..ab04` compose the message tag / acceptor).
- On return, `r4 = tag`; `bl 0xb0009f2c` checks for exception/unknown-IPC.
- `0xb000ab38  cmn r3, #0x200000` tests the label against `L4_PAGEFAULT` (`-(2<<20)`) →
  branches to `iguana_ex_pagefault_impl` path at `0xb000ac1c` (the page-fault server:
  `objtable_lookup` + `try_attach` + `HANDLE_PAGE_FAULT`).
- `0xb000ab40  ldr r3,[r5,#4]; sub r3,#0x16; cmp r3,#9; ldrls pc,[pc,r3,lsl#2]` = a
  10-entry **jump table** for request method IDs `0x16..0x1f` (the iguana_* IDL methods:
  pd/thread/memsection/physpool/clist/mutex ops implemented in `iguana_server.c`).
- Every handler tail-branches back to `0xb000aac4` (reply-and-wait again).

The loop never returns; `main` lines after it (`0xb0003464+`) are unreachable (the
`assert(!"Should never reach here")`).

## 5. Required OKL4 syscalls (what the emulator MUST implement for this path)

From the source + the syscall dispatch already stubbed in `zeebo_lle_main.cpp`
(`c0_intr_hook`, FINDINGS L854-860). Firmware ABI is the hybrid OKL4 2.1.1-fix7 trap set:

| Syscall            | trap/id (fix7)  | Used by (post-bi_execute)                                  |
|--------------------|-----------------|------------------------------------------------------------|
| `L4_ThreadControl` | `0x08`/`0x1408` | thread_init, `pd_create_thread` under `new_thread`         |
| `L4_SpaceControl`  | `0x18`/`0x1418` | space_init, `pd_create` (each `new_pd`), main.c L115       |
| `L4_MapControl`    | `0x14`/`0x1414` | mempool_init, `map`/`l4e_map` records, memsection mapping  |
| `L4_ExchangeRegisters` | `0x0c`/`0x140c` | `thread_start`/`run_thread` (set IP+SP of init threads) |
| `L4_Ipc` (ReplyWait) | `0x00`/`0x1400` | the server loop @0xb000aa94 (wrapper 0xb000c800)          |

Supporting: `L4_CacheFlushAll` (main.c L118, ARMv6 path), `L4_SecurityControl`
(interrupt grants in bootinfo), `L4_KDB_SetThreadName` (naming, no-op OK). The current
LLE dispatcher returns fixed r0=0/1 — sufficient to *pass* the calls but NOT a real MMU
mapping (FINDINGS L1509: MAP_CONTROL must actually map for user-space to execute).

**Critical correctness note (already in skill):** `L4_MapControl` fpages with
`size_log2>=32` or `phys_base>=0x1_0000_0000` are whole-address-space control ops
(flush/grant), not physical page maps → treat as no-op success, never forward `size=2^32`
to Unicorn (overflow → `UC_ERR_NOMEM`).

## 6. How AEECShell / BREW AppMgr is located and launched

There is **no L4-level VFS lookup for AEECShell**. Layering:

1. Iguana runs the BREW bootstrap thread (one of the `run_thread` records, entry in the
   APPS `0x10000000` code region). This is the user-space side that the LLE currently
   *fakes* by hard-writing `core0_.entry = 0x1013a000` at cycle>=38
   (FINDINGS L1505-1508) — that jump is a printf-labelled shortcut ("Vectoring to BREW
   4.0.2 AEECShell"), NOT an organically reached entry. Real boot must reach it by
   executing the started init thread.
2. BREW brings up `AEECShell` (the AEE kernel/shell). AEECShell is **code embedded in the
   `0:APPS` ELF**, not a file loaded from EFS2 — same as ZeeboApp/AppMgr strings which the
   skill documents as embedded in the `0x1cc0000–0x3220000` system partition ELF.
3. Which applet AEECShell auto-launches is decided by `fs:/lctsys/flixfile.dat`
   `FIRSTAPP:` (skill): `3`=Z-Wheel (`ZeeboApp`, factory default), `0`=BREW AppMgr
   (`brewappmgr`), `1`=EMAPPLET.
4. BREW AppMgr resources live embedded in `0:APPS`:
   `fs:/mif/brewappmgr.mif` @ `0x2c285ee`, `fs:/mod/brewappmgr/appmgrls.bar` @ `0x2fe9ffc`.
   Installed game applets instead live in the EFS2 COW VFS on `0:EFS2APPS` (NAND
   `0x3220000`), resolved by IFILEMGR via the `(parent_inode, name)` dirent index
   (`0x69` marker records) — see `tools/cpp/zeebo_efs2_fs.h`.
5. AEECShell launches an applet by ClassID via `ISHELL_CreateInstance` →
   `AEEMod_Load` → `AEEClsCreateInstance` → applet `HandleEvent(EVT_APP_START=0x1f96)`.
   The `.mod` code comes from the `0:APPS` ELF (Z-Wheel/AppMgr) or from `0:EFS2APPS` /
   `fs:/mmc4/mod/<appid>/` (installed games).

## Bottom line for the LLE

The post-`bi_execute` chain is fully mapped and matches the OKL4 source. The blocker is
NOT understanding the sequence — it is that Core 0 does not yet execute the started init
thread under a *real* address space: `L4_MapControl` must perform genuine mappings and the
`iguana_server_loop` IPC (`0xb000c800`) must service real page faults so the BREW/AEECShell
thread runs organically instead of via the hard-coded `0x1013a000` PC write. Those two
(real MAP_CONTROL + real IPC/pagefault serving) are the gate to a non-faked AEECShell boot.
