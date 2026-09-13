# OKL4 2.1.1 — Mapa de referência para decomps do firmware (2026-09-08)

Fonte local: `~/projects/zeebo-lle/refs/okl4-2.1.1-fix7/` (OKL4 2.1.1 fix7: `iguana/`,
`pistachio/`, `arch/arm/`). Licença OKL4 (redistribuição permitida). USO: referência de
ABI/comportamento para casar com o firmware 1.1.2_APPS.bin — NÃO copiar código para o
emulador (clean-room: referência, não derivação).
SDK: `~/projects/zeebo-emulator/research/docs/sdk-extract/` (Zeebo SDK + BREW 4.0.2).
O tree do OKL4 é PARCIAL (faltam libs do user: bootinfo/bootinfo.h, kip.h, idl4).

## Correspondências firmware (1.1.2_APPS.bin, VA 0xb0000000 = fileoff 0x30000) x fonte

| Firmware | Fonte (iguana/server/src) | Notas |
|---|---|---|
| main `0xb00033d0` | `main.c` | Sequência casa 1:1: 7 bl de init, `bi_execute` (0xb0003444), `cmp r0,#0; beq` senão PANIC `while(1)` (0xb0003458), `extensions_init` (0xb000345c→0xb00017b8), `iguana_server_loop()` (0xb0003460→0xb000aa94), pós-loop `assert(!"Should never reach here")` → panic `0xb000ad3c` → hang `0xb000b1d4` |
| panic/assert handler | (util/debug do Iguana) | `0xb000ad3c`: loga + `bl 0xb000b1d4` (hang). 98 callers no kernel. `r3` = número da linha do fonte no assert (ex.: 0x86=134) |
| hang fatal | `0xb000b1d4: b .` | self-loop deliberado ("never return"). 3 callers: 0xb00012b4, 0xb000ad6c, self |
| `thread_init()` | `thread.c:98` | **`0xb00070c8`** (7ª chamada do main, ANTES do bi_execute). `1<<KIP[0xc4]` = `1<<L4_GetThreadBits()` = max_threadno; `(utcb[0]>>14)+2` = `L4_ThreadNo(L4_rootserver)+2` = min_threadno; `0xb000dd98`=rfl_new; `0xb000dd9c`=rfl_insert_range; `0xb000db64`=hash_init(0x400); panics = asserts |
| `bi_execute` | `bootinfo.c:1511` | `0xb00001fc`: chama `bootinfo_parse(bootinfo, &bi_callbacks, ...)` — callbacks: init_mem, new_pd, new_ms, add_virt_mem, add_phys_mem, new_thread, run_thread, map, attach, grant, argv, register_*, new_pool, kernel_info. 1 caller: main 0xb0003444 |
| server loop | `main.c` (extern `iguana_server_loop`) | `0xb000aa94`: loop infinito; `bl 0xb000c800` (L4_Ipc wait) + dispatch de opcodes 0x16..0x1e (jump table em ~0xab54, `sub r3,#0x16; cmp r3,#9`). 1 caller: main 0xb0003460. Definição não está no tree parcial (idl4/lib externa) |
| stubs syscall ARM | `arch/arm/libs/l4/src/*.spp` | mapcontrol.spp/exchangeregisters.spp/etc = firmware byte a byte (ver abaixo) |
| syscall numbers | `arch/arm/libs/l4/include/syscalls_asm.h` | decode confirmado (ver abaixo) |

## Mecanismos confirmados (syscalls ARM OKL4)

- `SYSBASE=0xffffff00`, `SYSNUM(name)=SYSBASE+SYSCALL_name`; o stub faz `mov ip,sp` +
  `mov sp,#SYSNUM` (compilador otimiza p/ `mvn sp,#~SYSNUM`) + `swi SWINUM` (`0x1400+n`).
  O KERNEL restaura `sp=ip` ao retornar do swi → epílogos com `pop {r4-r11,pc}` funcionam.
  **Handler host: identidade = `sp&0xFF`** (0x00=Ipc, 0x04=ThreadSwitch, 0x08=ThreadControl,
  0x0c=ExchangeRegisters, 0x10=Schedule, 0x14=MapControl, 0x18=SpaceControl, 0xb0=GetUtcb,
  0xb4=KIP). TRAPNUM: KIP era 0xb4.
- `L4_MapControl` stub (firmware 0xb000c930 = mapcontrol.spp): push {r4-r11,lr}; mov ip,sp;
  sp=0xffffff14 (mvn #0xeb); swi #0x1414; pop {r4-r11,pc}. Retomada correta: `pc` (=pop) + `sp=ip`.
- `L4_ExchangeRegisters` stub (firmware 0xb000c758 = exchangeregisters.spp): push {r4-r11,lr};
  ldr r4,r5,r6 de [sp+0x24/28/2c] (flags/UserDefHandle/pager do caller); mov ip,sp; sp=0xffffff0c
  (mvn #0xf3); swi #0x140c; `add lr,sp,#0x30; ldm lr,{r7-r12}` (lê 6 outputs que o kernel
  escreveu em [ip+0x30..ip+0x48]); str r1,[r7](old_control) r2,[r8](old_sp) r3,[r9](old_ip)
  r4,[r10](old_flags) r5,[r11](old_handle) r6,[r12](old_pager); pop {r4-r11,pc}.
  **Handler host (case 0x0c) precisa: retomar em `pc` COM `sp=ip` + escrever os 6 outputs em
  [ip+0x30..ip+0x48]** (atualmente NÃO restaura sp nem escreve outputs — bug análogo ao QW19).
- threadid global ARM: `threadno = raw >> 14`, `version` nos 14 bits baixos. L4_ThreadNo(tid)=raw>>14.
  Rootserver: tid com threadno pequeno → raw ≈ threadno<<14 (ex.: threadno 1 → 0x4000).

## Estado do boot real pós-QW19 (observado 2026-09-08)

1. main → `0xb000d5b4` (96 MapControl de 1 MiB atravessados: 0xb0d00000..0xb6c00000) ✓
2. main → inits ... → `thread_init` (0xb00070c8) → **PANIC no assert (linha 134, 0xb0007184)**:
   `rfl_insert_range(min_threadno=131075, max_threadno=1)` falha.
   Causa: **KIP[0xc4] (thread_bits) = 0** no KIP sintético (build_kip) → max_threadno = 1<<0 = 1;
   e utcb[0]=0x80010000 (handle? lixo) → min_threadno=(0x80010000>>14)+2=131075. min>max → assert.
3. `bi_execute` (0xb00001fc) NUNCA é alcançado (bps confirmam; o fluxo panica antes).
4. Config ARM (`arch/arm/pistachio/include/config.h`): "256 MB de KTCBs = 18 valid bits for
   thread IDs" → thread_bits provável = 18 (1<<18 = 262144 ≥ min). Valor exato do Zeebo a
   confirmar por RE (KIP do hardware ou aceitação do firmware).

## EXPERIMENTO KIP[0xc4]=18 (2026-09-08, poke via control-port, boot orgânico) — DESTRAVA

Com KIP[0xc4]=18: o panic do thread_init NÃO ocorre; **bi_execute (0xb00001fc) é alcançado**
(lr=0xb0003448 do main, r0=0xb0d00000 = __okl4_bootinfo) e o boot avança 13k+ ciclos executando
código de SERVER (0xb0400000+). NOVO BLOQUEIO (2ª causa raiz):

- O bi_execute mapeia páginas dos servers: ex. va=0xb0425000 ← MR[0]=0x10081000 (phys_desc cru),
  MR[1]=0xb04250c7 (fpage: rwx=7, size_log2=12). O decode atual `PhysDesc::phys_base() =
  (raw>>6)<<10` (gran 1KB) produz **0x100810000** (>4GB) → o handle_map_control classifica como
  "whole-space/controle" e NÃO mapeia (no-op) → a página fica com rwx do passo anterior (0) →
  o memset do BSS (pc=0xb000afe0) falha com UC_ERR_WRITE_PROT em loop.
- Evidência do gran correto: o raw 0x10081000 decodifica para RAM real 0x10081000 (dentro do pool
  0x10000000-0x16000000) SÓ com `(raw>>6)<<6` (gran 64B). Como o hardware real funciona, o kernel
  do Zeebo usa gran 64B para o phys_desc (o tree fix7 `pistachio/include/map.h` usa <<10 — o Zeebo
  diverge). FIX CANDIDATO: `phys_base() = (u64)(raw>>6) << 6` (validar com TDD + RE).
- Nota: a heurística "phys_base >= 0x100000000 = controle" do handle_map_control pode ter sido
  construída SOBRE esse decode errado (mascarava os maps reais como whole-space). Revisar junto.

## Pendências abertas (referência)

- QW21 (2026-09-08): o stub ExchangeRegisters (0xb000c758) NÃO é código morto — há **8 callers reais**
  (0xb00015fc, 0xb0002928, 0xb0002ac8, 0xb0002b90, 0xb0005c94, 0xb0007700, 0xb0007780, 0xb000780c;
  funções de thread/pd/space do kernel, verificadas com find_callers_p12.py CORRIGIDO). O fechamento
  original do QW21 ("0 call sites") usou a ferramenta com bug. Porém, bp vivo no stub durante o cold
  boot NÃO hitou: nenhum caller roda no caminho main→thread_init (o boot panica no thread_init antes).
  → case 0x0c do handler (sp=ip + outputs em ip+0x30) foi implementado e integrado no QW27 (`fc4a811`), eliminando o PC=0x00000000. No QW29 (`93503d5`), a tabela de threads L4 (`zeebo_l4_thread.h`) passou a capturar os descritores de thread (SP, IP, ThreadID) das chamadas ExchangeRegisters para comutação no QW28.
  destravar o bi_execute (threads dos servers usam ExchangeRegisters).

- Valor real de KIP[0xc4] (thread_bits) do Zeebo — candidato 18; validar contra o que o
  firmware aceita (insert_range(min=0x20003, max) precisa max ≥ 0x20003).
- utcb[0] deve conter o tid global do rootserver (raw ≈ 0x4000 p/ threadno 1?) — quem escreve?
  (kernel init OKL4 do Zeebo roda em 0xf0000000+/0x10000000 antes do Iguana; pode ser o
  responsável legítimo por KIP[0xc4] e utcb[0] — verificar se as escritas acontecem e por que
  não refletem.)
- bootinfo.h (tags BI_TAG_*, struct bootinfo) NÃO está no tree parcial — lib externa
  (`<bootinfo/bootinfo.h>`); obter para casar as tags do parser QW1.
- `find_callers_p12.py` CORRIGIDO (2026-09-08): filtro `v>=0x10000000` pulava o kernel inteiro
  (0xb0000000 > 256MB) e `op.imm` int32 assinado quebrava p/ endereços ≥ 0x80000000. Agora pula
  só a faixa BREW (0x10000000 ≤ v < 0xb0000000) e mascara imm com 0xffffffff. Validado:
  0xb000b1d4 → 3 callers; 0xb000aa94 → 1; 0xb000d5b4 → 11.
