# PC14 — separação causal do "ldr=5 vs uc_mem_read=0" em 0xb04241a8

Gate: `ZEEBO_PC14_ALIAS=1` (read-only, no-op sem a env var). Fontes:
`c0_code_hook` (instrumentação no EXATO ldr/mov/bx `b04001d4/d8/e0`) e
`SpaceManager::activate` (rastreio de remap da página `0xb0424000`) em
`tools/cpp/zeebo_lle_main.cpp` / `tools/cpp/zeebo_l4_mmu.h`. Worktree
isolado `/tmp/zeebo-qw99-pc14-alias` @ `ba3dc47`, **interpretador puro**
(sem Dynarmic/JIT), run `--headless --seconds=40`, exit 0.

## Pergunta
A discrepância medida antes (guest `ldr r1,[0xb04241a8]`=5, mas host
`uc_mem_read` do MESMO VA=0 antes do `bx r1` → PC4/14) é:
- (A) escrita/estado/timing legítimo do guest, ou
- (B) desacordo de mapeamento/alias do Unicorn (guest lê de um alias
  enquanto o backing real é outro)?

## Medição na fronteira exata (não no PC=4 posterior)
Instrumentei r1 arquitetural APÓS o ldr, `uc_mem_read` do VA-alvo no
mesmo instante, tradução VTLB, `SpaceManager::resolve_host` (host
efetivo + bytes de backing) para o SID corrente E para `0x80000100`
(onde a fonte foi registrada, per QW99), TID/SID/active_sid, e controle
negativo `0xb0424000` (mesma página, offset diferente).

```
@LDR   pc=b04001d4 va=0xb0041268 uc_read=0xb000c3fc  -> @postLDR r1(arch)=0xb000c3fc   [AGREEM]
@LDR   pc=b04001d4 va=0xb0046fa8 uc_read=0x00000005  -> @postLDR r1(arch)=0x00000005   [AGREEM]
@postLDR/@BX  va(sonda fixa)=0xb04241a8  uc_read=0  ;  sm(0x8000c001).host=0x..53010 val=0
                                                   ;  sm(0x80000100).host=0x..53010 val=0
neg(0xb0424000) uc=0 em todos os pontos
[ALIAS/activate] sid=0x80008001 e sid=0x8000c001 remapeiam 0xb0424000 -> host=0x..53010 (MESMO ponteiro)
```

## Veredito: hipótese B REFUTADA; hipótese A confirmada
1. **Guest e host CONCORDAM no endereço efetivo real.** Em ambas as
   passagens pelo `ldr r1,[r4]`, o `r1` arquitetural do guest é
   **byte-idêntico** ao `uc_mem_read` no VA que `r4` realmente aponta
   (`0xb0041268→0xb000c3fc`; `0xb0046fa8→5`). Não há divergência de
   visão de memória entre o load do guest e o read do host.
2. **A "discrepância 5 vs 0" era erro de ATRIBUIÇÃO DE ENDEREÇO**, não
   de alias. O dump one-shot anterior (em PC=4) leu `0xb04241a8`
   assumindo `r4=0xb04241a8`; mas no `ldr` real `r4=0xb0046fa8`.
   `*(0xb0046fa8)=5` para guest E host; `*(0xb04241a8)=0` para guest E
   host. São **dois endereços diferentes** — o "5" e o "0" nunca foram
   do mesmo VA. O dump ocorreu **depois** dos remaps de escalonamento e
   **no endereço errado** — dupla causa do fantasma.
3. **Backing coerente através de SIDs/remaps.** `resolve_host` para
   `0x8000c001` e `0x80000100` devolve o MESMO host_ptr (`0x..53010`) e
   o MESMO valor (0). Os dois `[ALIAS/activate]` que religam a página
   `0xb0424000` usam o MESMO `host=0x..53010` — nenhum remap troca o
   backing por um alias divergente.
4. Controle positivo (o próprio VA) reflete os dados esperados;
   controle negativo (`0xb0424000`) permanece 0 e não-perturbado.

Portanto **não há defeito de aliasing/mapeamento do Unicorn nesta
fronteira**. O estado observado é legitimamente visível ao guest: `r4` aponta para
`0xb0046fa8`, cujo slot 0 contém o inteiro pequeno `5` onde o consumidor em
`b04001e0` espera um destino chamável. O `bx 5` é fielmente executado. Isso prova o
valor inválido no ponto de consumo, mas ainda não prova se `r4` está incorreto, se a
estrutura está incompleta ou qual produtor deixou `5` nesse slot.

## Origem a montante ainda aberta
O defeito anterior de atribuição de SID no scatterload foi corrigido antes desta
fronteira e o avanço até `b04001e0` depende dessa correção. Portanto não é válido
atribuir automaticamente o `5` ao estado produzido pelo bug antigo. O próximo passo
causal é rastrear a procedência de `r4=0xb0046fa8` e da última escrita em
`[0xb0046fa8]`, com controles positivo e negativo, até identificar o produtor ou uma
lacuna de inicialização.

## Consequência de protocolo
Como **NÃO** se provou bug de alias do emulador no caminho de
produção/activate/mapping, **não** foi escrito teste RED contra esse
caminho nem aplicado qualquer fix (o mapping está correto: guest==host).
Apenas instrumentação read-only env-gated (`ZEEBO_PC14_ALIAS`) + esta
nota foram commitadas. Ponteiro/PC/memória do guest **não** patcheados.
O gancho medido para a próxima iteração é a procedência de `r4` e da escrita do valor
`5`, não o SID já corrigido nem a memória do Unicorn.

## PC14W — procedência de `r4` (do spill de pilha ao dispatch de objeto)
Gate: `ZEEBO_PC14_WRITER=1` (read-only, no-op sem a env var). Rastreador
causal da ÚLTIMA escrita em `0xb0046fa8` no `c0_mem_hook`, com controle
positivo (VA alvo) e negativo (`0xb0055000`), + dump de código dos sítios
produtor/caller/consumer/consumer_pre. Interpretador puro, run bounded
`--seconds<=25`, exit 0.

### Escrita única de `5` (produtor confirmado)
```
[PC14W] TGT va=0xb0046fa8 value=0x00000005 pc=0xb000c3d4 lr=0xb0006c18
        sp=0xb0046fc4 r0=b0400000 r5=b0e00008     (controle negativo: 0 hits)
```
Produtor `@0xb000c3d4` = `push {r0-r8,sb,sl,fp,ip}` seguido de
`ldr lr,[pc,#..]; str lr,[ip]; str sp,[ip,#4]; stmdb sp!,{ip}` — um
**salvamento de contexto estilo setjmp**. `0xb0046fa8 = sp-0x1c` = o slot
do **r6 spillado** (um inteiro pequeno transitório), não um campo de struct.
Logo `5` é um registrador salvo legítimo, não storage de objeto.

### Consumidor: `r4` é tratado como ponteiro de objeto
```
consumer_pre @b0400180: str r6,[r4,#0x10]; str r7,[r4,#0xc];
                        ldr r1,[r4]; mov r0,r4; blx r1
consumer     @b04001c8: str r6,[r4,#0x10]; str r0,[r5,#4]; str r7,[r4,#0xc];
                        ldr r1,[r4]; mov r0,r4; pop {r4-r8,lr}; bx r1
```
`r4` funciona como `this`: o consumidor escreve campos em `[r4+0x10]` e
`[r4+0xc]`, depois chama `[r4]` como ponteiro de função. O
`pop {r4-r8,lr}` ocorre **depois** de `ldr r1,[r4]` e apenas restaura o `r4` do
chamador antes do `bx r1`; portanto ele não explica a procedência do `r4` usado
pelo `ldr`. Essa procedência precisa ser rastreada desde a entrada da função e
seu caller.

### Fronteira do `ldr r1,[r4]` (ALIAS, duas passagens)
```
passagem 1: r4=0xb0041268  -> *r4=0xb000c3fc (ponteiro de código VÁLIDO)  tid=0x0
passagem 2: r4=0xb0046fa8  -> *r4=0x00000005 (inteiro pequeno, slot de spill) tid=0x8000c001
```
Entre as duas passagens há **troca de contexto do escalonador**
(`tid 0x0 → 0x8000c001`, dois `[ALIAS/activate]` remapeando páginas). Na
segunda passagem `r4` passou a apontar para **dentro da pilha** (região
`0xb0046fxx`, o próprio slot de spill do produtor setjmp), em vez de um
objeto de heap.

### Veredito da procedência
1. **Não é bug de memória/alias do Unicorn** — guest `r1(arch)` ==
   `uc_mem_read` byte-idêntico em ambas as passagens (já refutado acima;
   reconfirmado com o valor `5`/`0xb000c3fc`).
2. **`5` tem produtor único e legítimo**: r6 spillado no context-save
   `@b000c3d4`. Nenhum outro escritor toca o slot (controle negativo limpo).
3. **`r4=0xb0046fa8` aponta para um ENDEREÇO DE PILHA**, não para uma vtable:
   o consumidor faz `bx *(stack_slot)` = `bx 5`.
4. A troca de contexto entre as duas passagens é correlação temporal, não causa
   demonstrada. Como o `pop {r4-r8,lr}` vem depois do `ldr`, esta captura ainda não
   prova restauração incorreta de `r4`, SP ou frame pelo emulador.

### Consequência de protocolo (não-fix)
O `bx` é **fielmente executado** pelo interpretador: a memória guest==host e o
`5` tem produtor conhecido. Ainda não foi isolado um defeito específico do
emulador. Por protocolo, **sem bug provado → nenhum teste RED nem fix**; apenas
o rastreador read-only env-gated (`ZEEBO_PC14_WRITER`) + esta nota. Nada de
PC/registrador/memória do guest foi patcheado. O próximo gancho é rastrear o
`r4` usado pelo `ldr` desde a entrada de `b0400180` e seu caller; somente então,
se a evidência apontar para save/resume, instrumentar a fronteira do scheduler.
