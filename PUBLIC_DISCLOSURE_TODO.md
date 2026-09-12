# PUBLIC_DISCLOSURE_TODO — Pré-requisitos para publicar o zeebo-lle no GitHub

Health check em 2026-09-08. Este documento lista o que precisa ser resolvido
ANTES de tornar o repositório público. Nada foi alterado no repo — só auditado.

## Resumo do veredito

Build e clean-room OK. Publicação **bloqueada** por 3 itens (2 jurídicos, 1 de higiene).

- Build (clone limpo): `git clone . /tmp/verify && make -C tools/cpp zeebo_lle_main` → EXIT 0.
  HEAD é autocontido; apenas warnings, nenhum erro. Bug histórico de `#include`
  não-commitado está resolvido.
- Clean-room íntegro: `refs/tainted/a1Sim.exe` e `a1Host.dll` corretamente ignorados
  e **nunca** entraram no histórico git. NAND (`nand/1.1.2*.bin`, 303MB) ignorado e
  nunca commitado. Nenhum arquivo `a1sim/tainted/decomp` rastreado.

---

## BLOQUEADORES (resolver antes do push)

### 1. Binários compilados rastreados no git  [higiene / CONCLUÍDO `2237b8b`/`aab47db`]

6 ELFs de build estavam versionados em `tools/cpp/` (~212K total):

- `tools/cpp/zeebo_boot`
- `tools/cpp/zeebo_harness`
- `tools/cpp/zeebo_partition`
- `tools/cpp/zeebo_elf`
- `tools/cpp/zeebo_kernel_boot`
- `tools/cpp/zeebo_devices_test`

São artefatos de build (o `.gitignore` já ignora os DEMAIS binários de `tools/cpp/`,
mas estes escaparam). Também rastreado: `refs/okl4-arm-build/arm-kernel.elf` (216K) —
ver item 2.

Ação sugerida:
```
git rm --cached tools/cpp/zeebo_boot tools/cpp/zeebo_harness tools/cpp/zeebo_partition \
                tools/cpp/zeebo_elf tools/cpp/zeebo_kernel_boot tools/cpp/zeebo_devices_test
# adicionar essas 6 entradas ao .gitignore
```

### 2. Fontes de terceiros com copyright em `refs/`  [JURÍDICO / bloqueia]

663 arquivos rastreados em `refs/`, incluindo código copyrighted de terceiros:

- Árvore OKL4 / Pistachio completa (`refs/okl4-2.1.1-fix7/...`) — kernel L4 OKL4/NICTA.
- `refs/rex.c` (144K), `refs/rexarm.s` (80K) — REX RTOS (Qualcomm).
- `refs/msm_nand-kernel-driver.c` (200K) — driver Qualcomm MSM.
- `refs/okl4-arm-build/arm-kernel.elf` (216K) — binário buildado do OKL4 (desindexado em `d41350f` e ignorado em `902fae0`).

Publicar código Qualcomm/OKL4 é risco jurídico real e **contradiz o objetivo
distributável do clean-room** — a distribuibilidade que o projeto protege ao não
descompilar o a1Sim é anulada se o repo público carrega fonte de terceiros.

Ação sugerida (escolher uma):
- Remover `refs/` de terceiros do versionamento e substituir por um `refs/MANIFEST.md`
  que aponta URLs/commits upstream + script de fetch; OU
- Manter `refs/` apenas localmente (adicionar ao `.gitignore`), fora do repo público.

Nota: apenas material observado/RE próprio (notas em `notes/`, disassembly derivado)
pode permanecer. Fonte upstream copiada, não.

### 3. Firmware de terceiros compilado sem atribuição  [JURÍDICO leve / RESOLVIDO `92ac14a`+`3f433ab`]

`firmware/openzeebo-zloader.bin` e `openzeebo-zloader-debug.bin` eram binários
compilados do projeto **OpenZeebo** (`github.com/tripleoxygen/openzeebo`).
Foram removidos do índice git (delete mode 100755) e `firmware/*.bin` adicionado ao `.gitignore`,
mantendo os arquivos físicos intactos no workspace local para testes sem versioná-los.

### 4. Sem LICENSE nem README  [JURÍDICO + apresentação / bloqueia]

Não existem `LICENSE*` nem `README*` na raiz. Sem LICENSE o default legal é
"todos os direitos reservados" — o oposto de open-source. Um projeto que se declara
"open-source distributável" precisa de ambos.

Ação sugerida:
- Adicionar `LICENSE` (escolher: MIT / Apache-2.0 / GPL — decidir).
- Adicionar `README.md`: o que é, objetivo (LLE do Zeebo), status, política clean-room,
  como buildar, aviso de que NAND e a1Sim NÃO são distribuídos.

---

## PENDÊNCIAS MENORES (não bloqueiam)

- 7 arquivos untracked no working tree — decidir commitar ou ignorar:
  `tools/dis2.py`, `tools/dis3.py`, `tools/dis_f0017448.py`, `tools/probe_f0017448.py`,
  `tools/probe_reloc.py`, `tools/cpp/probe_map_ptr`, `tools/cpp/test_l4_mmu_uc`.
  (Os dois últimos são binários — ignorar.)
- Warnings de compilação (`-Wunused-parameter`, `-Wunused-function` em
  `zeebo_lle_main.cpp` / `zeebo_devices.h`) — cosmético.
- E-mail de terceiro em doc rastreado: redigido para `<redacted>` em `notes/FINDINGS.md` (concluído).

## Verificações feitas que passaram (não precisam ação)

- Sem segredos/chaves nossos em arquivos rastreados (grep de private key / api key /
  token / password — só matches inócuos em comentários).
- Sem submódulos git; sem `.gitmodules`.
- Nenhum dado pessoal do Rafael (e-mail/telefone/IP) em arquivos rastreados fora de `refs/`.
- `refs/tainted/` (a1Sim.exe, a1Host.dll) e `nand/*.bin` confirmados ignorados E
  ausentes de todo o histórico git.

---

## Checklist final (marcar antes do push)

- [x] 6 ELFs removidos do índice + `.gitignore` atualizado (concluído em `2237b8b`/`aab47db`)
- [ ] `refs/` de terceiros removido do versionamento (manifest ou gitignore)
- [x] `arm-kernel.elf` removido do índice
- [x] Firmware OpenZeebo: licença/atribuição adicionada OU `.bin` removidos + doc de build (`firmware/*.bin` ignorado e desindexado)
- [x] `notes/FINDINGS.md`: e-mail de terceiro redigido
- [ ] `LICENSE` adicionado
- [ ] `README.md` adicionado
- [ ] Arquivos untracked triados (commit ou ignore)
- [ ] Re-verificar build em clone limpo após remoções: `git clone . /tmp/v && make -C /tmp/v/tools/cpp zeebo_lle_main`
- [ ] Confirmar que `refs/tainted/` e `nand/*.bin` continuam ignorados e fora do histórico
