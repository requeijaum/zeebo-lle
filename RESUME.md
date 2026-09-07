# Zeebo LLE — RESUMO / RESUME (2026-09-06)

Estado honesto ao parar. Tudo commitado em `~/projects/zeebo-lle/`; para retomar,
leia `ROADMAP.md` + `notes/FINDINGS.md` (log completo, sessões 2a–2m).

## Onde estamos
- **Objetivo**: bootar o firmware REAL do Zeebo (MSM7201A) em emulação LLE, via
  Unicorn ARMv6 (camada QEMU-free), a partir do dump NAND 1.1.2.
- **Kernel identificado**: L4e (NICTA Pistachio-embedded / OKL4 lineage) + REX RTOS
  por cima. AMSS/APPS são tasks REX.
- **ABI syscall ARM RECONCILIADA (2026-09-06)**: NÃO é bl->KIP (refman é genérico).
  O firmware Zeebo usa `mvn sp,#~mask; svc #IMM` com imm=syscall (0=ipc, 4=
  thread_switch, 8=thread_control, 0xc=exchange_regs, 0x10=schedule, 0x14=
  map_control, 0x18=space_control, 0x20=cache, 0x24=security, 0x28=lipc); SP
  magic é máscara separada. (Supera a leitura antiga "6 syscalls svc 0x14/1414";
  a 2o supercorrigiu — este é o entendimento final.)

## Conquistado (verificado)
1. **Mapa MMU real** ARM11 VA→PA (150 entradas) extraído de
   `~/.hermes/.../console__zeebo__mmu.txt` → `tools/arm11_mmu.py`
   (periféricos c0, RAM identity, coarse b0xxx→100a3xxx).
2. **Modelo NAND controller** `tools/nand_controller.py` (self-test passa:
   geometria 65536 págs, FETCH_ID=0x5580b1ad, PAGE_READ bate byte-a-byte).
3. **REX API + referência QSC1110** em `refs/` (rex.c/rexarm.s/rextime.c — REX
   nativo; referência comportamental, NÃO o ARM9 do MSM7201A — QSC1110 é chip
   discreto).
4. **Caminho de boot do APPSBL rastreado** ao handoff: `bl 0xf088` (flash read)
   → `0xf63c` (MMIO writer NAND) / `0xf508` (lookup de geometria, tabela em
   0xf8c0). Peripherals: VIC/GPT/DMOV/MDDI/GPIO/CLK todos confirmados do dump.

## Due obstáculos (becos documentados — NÃO re-perseguir)
- **Shim de valores L4e insuficiente**: MAP_CONTROL tem semântica de MMU real;
  retornar sucesso sem mapear = derail em NOP-slide. (shim v1 provado.)
- **Estágio isolado derrapa**: APPS em 0xb000fffc / AMSS — a RAM baixa que a
  runtime espera é construída pelo LOADER (relocação), não existe no ELF.
  Mirror de ELF não resolve.
- **APPSBL não emite NAND**: roda pleno com NAND conectada = 0 acessos a
  0xa0a00000. Pega o path de ID-baixo (0xf25c, lookup em RAM), nunca o MMIO.

## PRÓXIMO INCREMENTO (o túnel)
Bring-up da cadeia de boot = forçar o APPSBL ao path MMIO real do NAND:
1. Fazer `0xf088` tomar `0xf63c` (MMIO writer) em vez do `0xf25c` (fast path).
   Hipótese: o ID >8 ou uma chamada de tamanho (r1 grande) entra no path MMIO.
   Método: hook no `0xf088`, forçar o r0 do ramo, re-rodar `trace_mmu_on`/`probe_nand_emission`.
2. Com APPSBL emitindo NAND MMIO, alimentar via `NandController` (READ_ID,
   PAGE_READ) e ver ele carregar o estágio em RAM.
3. Depois: o elo que reloca a runtime baixa (loader do kernel L4e) — rastrear
   para onde o APPSBL entrega o controle após carregar.

Ferramentas-mãe: `tools/mm5_*`? Não — `probe_nand_emission.py`, `trace_mmu_on.py`,
`boot_chain_unuicorn.py`, `nand_controller.py`. Regra de ouro: verificar svc
`op>>24==0xEF` antes de declarar progresso (evidence, never insn-count).

## Comandos úteis
- Self-test NAND: `python3 tools/nand_controller.py`
- Boot APPSBL + contagem NAND: `python3 tools/probe_nand_emission.py`
- Trace handoff: `python3 tools/trace_mmu_on.py`
- MMU translate run APPS: `python3 tools/mmu_runner_v2.py APPS`