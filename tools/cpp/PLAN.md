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
- [ ] **M2. Loader de ELF**: parsear o ELF da partição lida (AMSS entry 0xa00000,
      APPS entry 0x10000000), mapear os PT_LOAD nos endereços físicos corretos
      (mapa MMU ARM11: f0000000->10000000, b0xxx->100a3xxx, 10xxxx identity).
- [ ] **M3. Boot do AMSS/APPS em RAM**: carregar a imagem nos PAs, rodar do entry
      com tradução VA->PA, observar a PRIMEIRA syscall (bl->KIP) e o que ela
      espera — refinando o conhecimento do boundary L4e/REX.
- [ ] **M4. syscall L4e via KIP**: localizar os links da KIP no firmware e
      relacionar os bl-targets (corrigindo a ABI, que é bl->KIP, não svc#imm).
- [ ] **M5. Boot de chain**: juntar todo o provado — DMOV->NAND->partição->RAM
      ->entry->syscalls — num caminho contínuo inspecionável no harness.

## Ordem: M1 -> M2 -> M3 (cada um verificado com dados reais), M4/M5 conforme.