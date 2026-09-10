# OKL4 microkernel source — a referência exata para o boot do LLE

**Data:** 2026-09-09 | **Fonte local:** ~/projects/zeebo-lle/refs/okl4-2.1.1-fix7/ (+ REX, kernel-ARM build)

---

## O que existe no corpus
- **OKL4 2.1.1 fix7** completo: `iguana/`, `pistachio/include/l4.h`, `arch/arm/iguana/`, `libs/`
- **REX**: `refs/rex.c`, `rexarm.s`, `rexcore.s`, `rextime.c`, `rex_qsc1110.h`
- **Kernel ARM build**: `refs/okl4-arm-build/arm-kernel.elf`
- **Docs**: `docs/l4e-syscall-abi.md` (ABI corrigida), `docs/rex-abstraction-layer.md`, `notes/boot-investigation/okl4-source-reference-map.md`

## Evidência que o firmware É OKL4
Firmware `1.1.2_APPS.bin` embute os paths de fonte OKL4:
- `iguana/server/src/iguana_server.c`
- `iguana/naming/src/naming_server.c`
- `libs/naming/src/naming.c`
- String `ig_naming` (x2)
- `k1Process: Created iguana PD=%x`

## ABI (docs/l4e-syscall-abi.md — CORRIGIDO)
- Kernel IS L4e/OKL4 + REX (strings AMSS/APPS).
- Syscall trigger: **`bl` para KIP link address** (~0xFE00..0xFE0F) — NÃO svc #imm na ABI L4e pura; mas o firmware usa um veneer `mvn sp,#0x4b; svc #0x14` (shim sobre os KIP-link). `svc #0x1400` = L4_Ipc no modelo do emulador.
- MR0-5 = **r3..r8**; sp/lr preservados.
- **UTCB/MyLocalId lido de 0xFF000FF0** ← EXATAMENTE o `ldr r2,[r3,#0xff0]` (r3=0xff000000) visto no disassembly do ig_naming.

## Ordem de boot de cada server (timer/main.c:177-203 — padrão OKL4 genérico)
```
main_tid = thread_l4tid(iguana_getenv("MAIN"));  // thread principal
obj = device_create_impl(main_tid, ...);          // cria objeto
naming_insert("timer", obj);                      // ← IPC que vai pro IG_NAMING
iguana_cb_handle = cb_attach(...);
L4_Accept(L4_AsynchItemsAcceptor);
L4_Set_NotifyMask(1UL<<31);
timer_server_loop();                              // espera IPC (reply/wait)
```

## Ordem esperada de inicialização
1. ig_naming (naming server base) — sobe primeiro
2. iguana_server (namespace raiz) → `naming_insert("iguana", ...)`
3. timer → `naming_insert("timer", obj)`
4. (etc. servers OKL4: memsection, trace, event, vbus...)
5. AEECShell / AppMgr → Z-Wheel applet

> ⚠ **STATUS DESATUALIZADO (ver `notes/core1_boot_estado_real.md`, 2026-09-10).**
> O fix 1 (T-bit) **JÁ FOI APLICADO** — o guard foi generalizado para dentro do
> proprio `apply_tbit` (zeebo_lle_main.cpp:3063/3069) e vale para TODOS os
> syscalls, inclusive o 0x00. O fix 2 (scheduler) e **inalcancavel**: medicao do
> hook de syscall mostra 8 SVCs no boot inteiro, todos 0x14 (MapControl), ZERO
> L4_Ipc. O boot morre ANTES de qualquer IPC, no laco de page-table walk.

## Causa raiz do stall (2 bloqueios, em ordem) — SUPERADO, ver aviso acima
1. **T-bit bug (físico, primeiro)**: branch `syscall==0x00` (linha 2452 em zeebo_lle_main.cpp) usa `apply_tbit(pc)` cru; força Thumb no retorno do L4_Ipc do ig_naming (cópia local ARM em 0xb010xxxx) → decode drift → stall em 0xb010333a. QW42 corrigiu SÓ o 0x0c, não o 0x00.
2. **Scheduler (lógico)**: só o ig_naming está agendado; nenhum server dispara `naming_insert` real → nenhum IPC chega.

## Fix em 2 partes
1. **T-bit**: copiar guard `local_arm_copy` (linha 2482-2483) para o branch 0x00 (linha 2452). Durável: derivar T-bit do return pela origem (`[pc-2]==0xDFxx`⇒Thumb) ou CPSR, não faixa de PC.
2. **Scheduler**: criar/logar os servers na ORDEM OKL4 (iguana → timer → ... → appmgr) para que o `naming_insert` real atinja o ig_naming. Validar ABI contra o source (UTCB/MR r3-r8).

## Vantagem
Agora o fix pode ser validado DIRETAMENTE contra o source OKL4 (não por tentativa): o naming ABI, os MRs, a ordem de init estão todos no código local.