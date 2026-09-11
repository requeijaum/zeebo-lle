# PC=0x14 — salto indireto inválido pós-scatterload (`bx r1`, r1=5)

Gate de instrumentação: `ZEEBO_PC14=1` (read-only, no-op sem a env var).
Fonte: `c0_code_hook` em `tools/cpp/zeebo_lle_main.cpp` (ring buffer + dump one-shot).

## Sintoma
Core0 cai na página de vetores baixos com `PC=0x00000004`, T=1, e o
decode Thumb do lixo `0x0014` produz INSN_INVALID → boot morre.

## Cadeia causal (medida, não inferida)
- Último salto real: `b04001e0: e12fff11  bx r1` com **r1 = 0x00000005**.
- r1 carregado em `b04001d4: e5941000  ldr r1,[r4]`, com **r4 = 0xb04241a8**.
- `bx 5` → bit0=1 ⇒ T=1, PC = 5 & ~1 = **0x4**. Depois o fetch em 0x4 lê o
  conteúdo do vetor `04:00000014` e o Thumb-decodifica como lixo.
- CPSR=0x80000030 → **modo 0x10 = User**, SPSR=0. **Não** é entrada de
  exceção, **não** é reset vector, **não** é misclassificação de T-bit
  (T foi corretamente derivado do bit0 de r1). O emulador executou o
  `bx r1` fielmente; o valor 5 é o defeito, não o branch.

## Proveniência de b04001bc..1e0
Trecho é ARM (não Thumb) em RAM decodificada pelo scatterload. É um
epílogo de dispatch por ponteiro de objeto:
```
b04001bc  ldr  r0,[r4,#0x18]
b04001c0  blx  #0xb040d266
b04001c4  mov  r0,#1
b04001c8  str  r6,[r4,#0x10]     ; r6=0x150
b04001cc  str  r0,[r5,#4]
b04001d0  str  r7,[r4,#0xc]      ; r7=0
b04001d4  ldr  r1,[r4]           ; r1 = *(objeto) — slot 0 = vtable/fn-ptr
b04001d8  mov  r0,r4             ; this = r4
b04001dc  pop  {r4-r8,lr}
b04001e0  bx   r1                ; dispatch: *this()
```
Chamador Thumb: `b040d29e/b040d2a0 ...` e `b040e164..e18a` (mesma unidade
decomprimida). r10=0xb04151a4 (região do hash QW99), r12=0xb040d219,
LR=0xb040d225 → tudo dentro do mesmo componente decomprimido em
0xb040xxxx (o "kernel/loader" pós-scatterload que também contém o probe
QW99). O objeto em r4 tem no slot 0 um **inteiro pequeno (5)** onde
deveria haver um ponteiro de código — assinatura clássica de objeto
lido antes de sua vtable/primeiro-campo ser inicializada, ou de r4
apontando para o meio de um bloco de dados errado.

## Descoberta aberta (próxima pista, NÃO patcheada)
O dump one-shot em PC=4 mostra **inconsistência de visão de memória**:
```
obj[r4] @0xb04241a0: 00 00 00 00 b04242d8 00 00 00000001
```
Ou seja, no dump `*(0xb04241a8)=0`, mas o `ldr r1,[r4]` momentos antes
leu **5**; e `str r6,[r4,#0x10]` (r6=0x150) também não aparece em
0xb04241b8 (mostra 0). O `ldr/str` do guest e o `uc_mem_read` do dump
enxergam conteúdos diferentes para o MESMO endereço, mesmo modo/SID
(User, SID 0x8000c001) inalterados entre os dois pontos.

Isso aponta para uma das duas hipóteses, a serem separadas com controle
positivo/negativo em sessão futura (bounded):
1. **Dado de guest genuíno**: o objeto foi escrito com 5 upstream (r4
   errado / inicialização faltante do produtor do objeto) — defeito de
   *guest state* que sobe de um passo anterior de boot; sem defeito de
   emulador nesta fronteira.
2. **Aliasing/mapeamento de memória do emulador**: a visão do `ldr`
   diverge do `uc_mem_read` → possível defeito de mapeamento/espelho
   (MMU/SID) que faria o guest ler `5` de um alias enquanto o backing
   real é `0`. Se comprovado, seria defeito geral do emulador.

Não há prova de defeito de emulador **nesta instrução** (o `bx` está
correto). Portanto, conforme protocolo: **somente instrumentação
read-only env-gated + esta nota** foram commitadas; ponteiro e PC **não**
foram patcheados. A discrepância de visão de memória é o gancho medido
para a próxima iteração.
