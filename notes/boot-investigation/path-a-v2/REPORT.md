# Path A v2 — Relatório de correção (commit 690a6ef)

Escopo desta rodada: **corrigir apenas documentação e mensagem de commit**; o fix
C++ e o teste TDD aprovados na spec-review foram **preservados sem edição**. Logs
brutos (`coldboot_after_fix.log`, `*_result.json`) mantidos intactos.

## O que o fix faz (fato)

O handler de `L4_KernelInterface` em `c738` gravava as saídas do kernel
(ApiVersion/ApiFlags/KernelId) nos slots salvos `ip+0/4/8`, que são os
`r4,r5,r6` do chamador empilhados por `stmfd sp!,{r4-r6,lr}` em `0xb000c720`.
O epílogo `ldmfd sp!,{r4-r6,pc}` (`0xb000c754`) então restaurava `r4=0x0c` em
vez do ponteiro de cache de páginas `0xb0041284`.

Correção: remover as escritas nos slots salvos. As saídas seguem **apenas**
pelos ponteiros do chamador `[r4]/[r5]/[r6]` (já escritos logo acima),
conforme a ABI de `okl4-2.1.1-fix7 kernelinterface.spp`.

## Evidência de preservação (RED/GREEN, r4-r6)

Verificada programaticamente nos JSONs brutos:

- **RED** (`RED_...json`, `pass:false`): com as escritas nocivas, o frame salvo
  é sobrescrito — epílogo devolve `r4=0xb0046ff0, r5=0xb0046fec, r6=0xb0046fe8`
  ≠ entrada.
- **GREEN** (`GREEN_...json` / `kernelinterface_stack_result.json`, `pass:true`):
  com o fix, `r4=r5=0xb0057000, r6=0x0` sobrevivem `c720→c754` iguais à entrada.
- Ambos confirmam `live_c738=ef000014...` (APPS, não AMSS). Teste é
  load-bearing: reinstalar as escritas torna-o vermelho.

## Correção de causalidade (motivo do REQUEST_CHANGES original)

A mensagem/narrativa anterior atribuía o stall a um "CTZ preso em 2048" que
"agora resolve em ≤8 iterações e avança". Isso é **overreach causal** e foi
removido:

- A rotina em `0xb000d4a8` é `l4e_min_pagesize()`/CTZ que varre
  `KIP[0xc8]=0x01111006`. O frame corrompido afeta o **alvo de store do
  ponteiro de cache de páginas** (`0xb0041284`), não a condição de varredura do
  CTZ. Ligar o hang ao "CTZ 2048→8" confunde os dois mecanismos.
- Contagens (`≤8 iters`) não são milestone de progresso.

## Estado real pós-fix (limitações honestas)

Verificado em `coldboot_after_fix.log`:

- Core0 amostrado **estaciona em `pc=0xb000d6b8`** (257/259 amostras) — o
  `stuck_add` do `mempool_init` (avanço nulo por fpage whole-space, `LSL` por 32).
  `d6b8` é um **PC amostrado**, não necessariamente a instrução-raiz do stall.
- `L4_MapControl`: **96** ops de 1 MB (`size=1048576`) e **2477** whole-space
  (`size=4294967296`). São **contagens, não milestone**.
- **Nunca** atinge `bi_execute` (`0xb00001fc`), `main` (`0xb00033d0`) nem PANIC;
  ausência desses logs **não prova** não-execução, mas **nenhum boot completo
  foi provado**.

## Comparabilidade com Path C

A baseline `path-c-corrected` (em `~/projects/zeebo-lle/notes/boot-investigation/`)
está **fora deste worktree** e sua proveniência/scripts não foram reconciliáveis
a partir daqui. **Comparabilidade não estabelecida**; nenhuma alegação de avanço
relativo a Path C é feita.
