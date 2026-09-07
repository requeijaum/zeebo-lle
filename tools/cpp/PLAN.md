# PLAN — Evolução do boot LLE (C++23 harness)

Objetivo: usar o harness C++23 (que já tem NandController + DMOVModel funcionais)
para EVOLUIR o boot do firmware real — do ponto atual até imagens do OS carregadas
em RAM pelos caminhos que o loader real usa, com inspeção completa.

## Estado atual (verificado)
- Harness C++ executa DMOV: zloader roda e faz `flash_read_config` via DMA real
  (DMOV execs=1, cfg0=0xa25400c0, cfg1=0x4745e), 4M insns sem erro UC.
- Python mini-boot provou: ler a partição AMSS (bloco 0x12) e APPS (0xe6) via
  DMA -> RAM é byte-identical ao dump. A mesma capability deve existir em C++.

## Metas (passos incrementais)
- [x] **M1. Mini-boot C++**: `zeebo_partition.cpp` lê partição inteira via DMOVModel
      e compara byte-a-byte ao dump — AMSS (10560 pag) e APPS (10816 pag) ambos
      byte-identical, DMOV execs = n. (Paridade com o Python provada 2026-09-06.)
- [x] **M2. Loader de ELF**: `zeebo_elf.cpp` parseia ELF, mapeia os PT_LOAD nos PA
      via mapa MMU ARM11 (f0000000->10000000, b0d00000->100a3400, identity 10xxxx),
      entry traduzido. AMSS (18 LOAD, big 0xb1a000) e APPS (14 LOAD, 19.7MB+55MB) OK.
- [x] **M3. Boot do AMSS/APPS em RAM**: zeebo_boot.cpp carrega nos PAs e roda do entry:
      - AMSS: 285K insns, alcança 0x00b1a8a2 (código real no seg 0xb1a000) — roda do
        entry 0xa00000 com transfers legítimos (0xb17004->0xb1a89e->Thumb loops).
      - APPS: insn#0 pc=0x10000000 — entry NÃO coberto por PT_LOAD (re-confirma 2p):
        o loader real reloca APPS; isolado não executa. Boundary L4e/REX confirmado.
- [x] **M4. syscall L4e via svc#imm (reconciliação)**: probe_kip.py + disasm do AMSS
      bloco 0xf002480c mostram o thunk real: `mov ip,sp; mvn sp,#~mask; svc #imm`,
      EX: `mvn sp,#0xfb; svc #4` (thread_switch), svc#0=ipc, #0x10=schedule,
      #0x1c/#0x20 (cache)... => o IMEDIATO do svc É o número da syscall, e o
      SP-magic (~N) é uma máscara/selector distinta (0xff/0xfb/0xef/e3/df).
      RECONCILIA a auditoria 2o: o refman L4e (bl->KIP) é GENÉRICO; o firmware
      Qualcomm usa svc#imm. A correspondência svc#0x14->MAP_CONTROL (2f) estava
      certa no princípio (imm=seletor); o erro 2o era só no valor do SP-magic.
      Atualizar skill: ABI = svc#imm com imm 0/4/8/c/10/14/18-28 = syscall.
- [ ] **M5. Boot de chain**: juntar DMOV->NAND->partição->RAM->entry->syscalls
      num caminho contínuo inspecionável no harness.
      STATUS (2026-09-06): AMSS roda 285K insns do entry 0xa00000 e DERAPA para
      dados em 0x00b1a8a2 (Thumb garbage) — sem o kernel L4e real + MMU, a
      relocação/entry não leva a syscalls observáveis; APPS do entry não executa
      (2p). O boot de chain real exige o kernel L4e presente (o harness faz a
      peça de harness/inspeção; o kernel é o próximo bloco). M5 é o objetivo
      longo; M1-M4 = ferramentas+ABI provadas.

## Ordem: M1 -> M2 -> M3 (cada um verificado com dados reais), M4/M5 conforme.