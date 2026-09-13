# KERNELS_TRICKS — técnicas de kernels reais (Linux, FreeBSD/NetBSD/OpenBSD, Darwin/XNU-Mach, L4/seL4) aplicadas ao Zeebo LLE

Data: 2026-09-11 · Base do repositório: commit `ecc629a` · Alvo: `tools/cpp/zeebo_lle_main.cpp`

## Por que ler isto

O `zeebo-lle` emula as syscalls do OKL4 em C++ dentro de um `UC_HOOK_INTR`. Não há exceção de
hardware: ninguém salva o `SPSR`, ninguém restaura `PC` e `CPSR` como par. Os quatro kernels
estudados aqui resolveram exatamente esse problema, cada um do seu jeito, e **convergem para as
mesmas três regras**:

1. **O modo (ARM/Thumb) vem do estado salvo, nunca de heurística de endereço.**
2. **`PC` e `CPSR` são restaurados como uma unidade indivisível** (`movs pc,lr`, `rfe`, `eret`).
3. **Quando há troca de thread, o epílogo de retorno de syscall não roda** (continuations).

## Como este documento está organizado

| Seção | Conteúdo | Fonte |
|---|---|---|
| `00` | Estado real do `zeebo-lle`, os problemas P1–P6, o caso `PC=0x14`, e uma auditoria que achou um defeito ativo | este repositório |
| `10` | Linux `arch/arm` / `arch/arm64` (v6.12) | GPL-2.0 |
| `20` | FreeBSD 13.2 `sys/arm` + NetBSD + OpenBSD | BSD-2 / BSD-4 / ISC |
| `30` | Darwin/XNU (Mach) + família L4 (OKL4 2.1.1 local, seL4) | APSL-2.0 / GPL-2.0 / BSD-2 |

**Licenças — regra única para todo o documento:** os padrões e a arquitetura descritos podem ser
reimplementados no `zeebo-lle`. **Código não pode ser copiado literalmente** de nenhuma das
fontes. Cada seção repete a licença específica que se aplica a ela.

**Convenção de evidência:** `[FATO VERIFICADO]` = o arquivo/linha foi lido; `[INFERÊNCIA]` =
dedução a partir de fatos verificados; `NÃO VERIFICADO` = declarado como lacuna, não preenchido
com suposição. Cada técnica termina com **APLICAÇÃO NO ZEEBO-LLE** (ligada a P1–P6) e um
**TESTE QUE PODE FALHAR** — a regra do repositório: *um controle só vale se puder dar negativo.*

---

> **Base de código citada: commit `ecc629a` (HEAD em 2026-09-11).** Todos os números de linha
> deste documento referem-se ao **blob do HEAD** (`git show HEAD:tools/cpp/zeebo_lle_main.cpp`),
> não à árvore de trabalho. Na data em que este documento foi escrito havia edições **não
> commitadas** em `tools/cpp/zeebo_lle_main.cpp` feitas por outra sessão em andamento
> (QW100/QW101/QW102: faixas ARM extras para `quartz_servers` e AMSS, remoção de
> `is_amss_thread` do `want_thumb`). Essas edições **não são estado do projeto** e estão
> marcadas como `[WIP não commitado]` onde aparecem.

## 0. Por que este documento existe

O `zeebo-lle` emula syscalls do OKL4 **em C++**, dentro de um `UC_HOOK_INTR` do Unicorn
(`c0_intr_hook`, linha 3024). Isto significa que **não existe exceção de hardware de verdade**
no caminho de uma SVC do guest:

| O que o ARM1136EJ-S (ARMv6) faz numa SVC | O que o `zeebo-lle` faz hoje |
|---|---|
| `SPSR_svc <- CPSR` (salva modo **e** bit T) | nada: ninguém salva o CPSR do chamador |
| `LR_svc <- endereço da próxima instrução` (`+4` em ARM, `+2` em Thumb) | o hook lê o `LR` do chamador, preenchido pelo `bl` do stub — não pela exceção |
| `CPSR.T <- 0`, `CPSR.M <- SVC(0x13)`, `CPSR.I <- 1` | nada: o CPSR continua o do chamador |
| `PC <- VecBase + 0x08`, com `VecBase = SCTLR.V ? 0xFFFF0000 : 0x00000000` | o hook despacha em C++ por `sp & 0xFF` (ABI OKL4: o stub faz `mvn sp,#cmd`) |
| `movs pc, lr` / `subs pc, lr, #0` restaura **PC e CPSR como par indivisível** | o hook escreve **só o PC**, com o bit T **adivinhado por faixa de endereço** |

> Nota de arquitetura: o ARM1136EJ-S é ARMv6 **sem** Security Extensions, portanto **não tem
> `VBAR`**. A base dos vetores só pode ser `0x00000000` ou `0xFFFF0000`, selecionada por
> `SCTLR.V` (CP15 c1, bit 13). `VBAR` só existe a partir das Security Extensions (ARMv6K/ARMv7).

A consequência é a classe inteira de bugs que este documento ataca. O `apply_tbit`
(linha 3433) decide o modo de decodificação por **faixa de endereço**:

```cpp
// HEAD ecc629a, linhas 3422-3435
bool caller_is_kernel_stub  = (pc >= 0xb0000000u && pc < 0xb0020000u);
bool caller_is_ig_naming_arm = (pc >= 0xb0100000u && pc < 0xb0120000u);
auto apply_tbit = [&](u32 v){ return (caller_is_kernel_stub || caller_is_ig_naming_arm)
                                     ? (v & ~1u) : (v | 1u); };
```

Isto é uma **tabela de exceções que cresce a cada boot novo**: QW41 achou os stubs do kernel,
QW43 achou `ig_naming`, e o trabalho `[WIP não commitado]` em curso já está acrescentando
`quartz_servers` (`0xb0300000..0xb0330000`) e o stub do AMSS (`0x103dc000..0x103de000`).
Cada faixa nova é um sintoma da mesma causa raiz: **o modo do chamador não é medido, é chutado.**

Os seis problemas abertos, referenciados pelo resto do documento:

- **P1 — drift ARM/Thumb** no retorno de syscall (`apply_tbit`, linhas 3422–3435).
- **P2 — PC/largura de retorno.** O erro de *double-advance* (`pc + 4` sobre um `pc` que já é
  `svc+4`) **já foi corrigido** em QW17: no HEAD **não existe mais** nenhum `pc + 4` no hook, e
  a semântica "o `UC_HOOK_INTR` entrega `PC == svc+4`" está provada por testemunha executável
  (`notes/boot-investigation/intr_pc_witness.py`, Unicorn 2.1.2: evento em `0x1004` para uma SVC
  em `0x1000`; o caso com `+4` extra pula o `mov r0,#0x5a` e termina com `r0 == 0`).
  **O que ainda falta:** (a) nenhuma checagem de **largura** — uma SVC Thumb ocupa 2 bytes, e o
  retorno é tratado como se fosse sempre 4; (b) no ramo `else` (linhas 3553–3561), se `lr == 0`
  então `target_pc` fica 0 → **nenhum** `uc_reg_write(PC)`, **nenhum** `uc_emu_stop`, e
  `core0_.entry` não é atualizado: a execução continua de um ponto indefinido, em silêncio.
- **P3 — handoff cooperativo** (`did_handoff`, linha 3056): `uc_reg_write(uc, UC_ARM_REG_R0,
  &res_r0)` na linha 3408 roda **antes** dos ramos que tratam `did_handoff`.
  **ATENÇÃO — hipótese já REFUTADA, não reabrir:** QW99 (`ecc629a`) mediu o boot real: apenas
  **dois** handoffs no boot inteiro, ambos `L4_Ipc`, ambos com `res_r0 == 0` **e** `R0` vivo
  igual a 0. A escrita é um **no-op na prática**; aplicar `if (!did_handoff)` **não mudou o
  boot** (`PC=0x14` persistiu: 1264 ocorrências, ~7.043.000 insns em Core0, antes e depois) e o
  patch foi **revertido**. `tools/cpp/test_pc14_r0_preserve.cpp` existe no repo justamente para
  impedir que esta hipótese seja "consertada" de novo sem medição. A lição de kernel abaixo
  (continuations do Mach) vale como **disciplina estrutural**, não como correção de um bug vivo.
- **P4 — primeira ativação** de thread: sem contexto salvo, só `PC` e `SP` são escritos
  (linhas 3140–3143 e 3203–3206). Nenhum CPSR, nenhum UTCB/TLS, nenhum registrador banked.
  No HEAD o modo ainda é inferido por `want_thumb = (target_ip & 1) || is_amss_thread(next_tid)`.
- **P5 — cache de tradução**: 28 chamadas de `uc_ctl_remove_cache` no arquivo, várias com
  endereços hardcoded (`0xb000c720`, `0xb00033d0`, `0xb000c758`, …).
- **P6 — espaços de endereçamento**: `SpaceManager::activate()` (`tools/cpp/zeebo_l4_mmu.h`)
  desmapeia/remapeia regiões no Unicorn para trocar de SID — o equivalente a trocar TTBR/ASID.
  O próprio arquivo documenta a regra dura: `activate()` **nunca** pode rodar dentro de um hook,
  só entre fatias de `uc_emu_start`.

O que o `zeebo-lle` **já acertou** e serve de base: `cpu_context_read`/`cpu_context_write`
(`tools/cpp/zeebo_l4_thread.h`) salvam e restauram `r0..r12, SP, LR, PC, CPSR` juntos,
escrevem o CPSR **antes** de SP/LR (porque SP/LR são banked por modo) e derivam o bit T do PC
a partir do `CPSR.T` salvo. **Essa é exatamente a disciplina que os três kernels estudados aqui
aplicam** — o problema é que ela só vale no caminho de `ThreadSwitch`/IPC **com contexto salvo**,
e não no retorno de syscall (P1/P2) nem na primeira ativação (P4).

---

## 1. O endereço 0x14 é o slot reservado da tabela de vetores ARM

Em ARMv6 a tabela de vetores é:

| Offset | Vetor | Modo de entrada |
|---|---|---|
| `0x00` | Reset | Supervisor |
| `0x04` | Undefined Instruction | Undef |
| `0x08` | Supervisor Call (SVC/SWI) | Supervisor |
| `0x0C` | Prefetch Abort | Abort |
| `0x10` | Data Abort | Abort |
| `0x14` | **Reservado** (era *Address Exception* no ARM de 26 bits) | — |
| `0x18` | IRQ | IRQ |
| `0x1C` | FIQ | FIQ |

`0x14` é o **único slot reservado** da tabela: não existe evento arquitetural que salte para lá.

### 1.1. O que o projeto JÁ mediu (não repetir)

A cadeia da falha está medida e documentada (`test_pc14_r0_preserve.cpp`, ROADMAP QW99):

```
0xb0400150  movs r4, r0      ; R4 <- R0
0xb04001d4  ldr  r1, [r4]    ; R1 <- [R4]   -> na 2a invocacao o valor observado e 5
0xb04001e0  bx   r1          ; salta para 5 (LSB=1 -> Thumb, PC=4)
```

O valor é **5**, um inteiro pequeno, não um ponteiro. A origem do `5` já foi localizada no
`stmdb sp!, {r0-r12}` de `0xb000c3d4` (salva `r6=5` na transição de thread `0x8000c001`);
o elo que falta é ligar essa pilha salva ao `R0` da restauração.

A instrumentação **já existe** e não precisa ser reescrita:
- `[PC14]` (linhas 3598–3655): dispara quando `ad < 0x20`, imprime `CPSR/modo/T/LR/SP/SPSR`,
  `TID/TSID/active_sid`, `r0..r15`, um ring buffer de 24 PCs+opcodes e o **conteúdo bruto da
  página de vetores** `0x00..0x3c`.
- `[QW-ALIAS]` (`ZEEBO_PC14_ALIAS`): mede `r4`/`r1` no exato `ldr`/`bx`, compara
  `uc_mem_read` × `SpaceManager::resolve_host` × VTLB, com controles positivo e negativo.

**Lacuna real desse instrumento:** o `[PC14]` é *one-shot* (`static bool dumped`). Ele mostra a
**primeira** aterrissagem, não a distribuição das **1264**. Trocar o one-shot por um
**histograma `(pc_predecessor → contagem)`** responde a pergunta que hoje está em aberto:
é **uma** origem repetida ou **várias**? Se for uma só, o caso fecha por rastreio direto;
se forem várias, a hipótese "ponteiro corrompido pontual" cai e sobra decodificação (P1).

### 1.2. A técnica de kernel aplicável: página zero desmapeada / vetores altos

Medida no código (linha 2475):

```cpp
uc_mem_map(core0_.uc, 0x00000000, 0x00100000, UC_PROT_ALL); // Zero page / Vectors
```

O Core0 mapeia **1 MB a partir de 0, RWX**. É por isso que um `bx 5` **não falha**: a CPU
aterrissa em `0x4` em modo Thumb e, se a página estiver zerada, `0x0000` decodifica como
`movs r0,r0` (`lsls r0,r0,#0`) — um **slide de NOPs** que atravessa `0x14` e continua. Um bug
que deveria ser fatal na primeira ocorrência vira **1264 ocorrências silenciosas**.

É exatamente o problema que o Linux resolve com **vetores altos**: com `SCTLR.V=1` a tabela
vai para `0xFFFF0000` e a página `0x00000000` fica **desmapeada**, de modo que qualquer
desreferência nula vira Prefetch/Data Abort **imediato e localizado**. (O `zeebo-lle` já modela
isso **no Core1**, que mapeia `0xffff0000` na linha 2498 — a assimetria entre os dois cores é,
em si, um indício.)

**APLICAÇÃO NO ZEEBO-LLE (P1/P3):** sob flag (`ZEEBO_TRAP_ZEROPAGE=1`), mapear
`[0x00000000, 0x00001000)` como `UC_PROT_NONE` **apenas no Core0** e tratar
`UC_HOOK_MEM_FETCH_UNMAPPED` nessa faixa como **erro fatal com dump** (o mesmo dump do
`[PC14]`). Converte um deslize silencioso num *fail-fast* no primeiro evento.

**TESTE QUE PODE FALHAR:** rodar o boot com a flag ligada.
- Se o boot parar **exatamente** na primeira aterrissagem e o dump mostrar a mesma cadeia
  `0xb04001d4/0xb04001e0`, a hipótese "slide de NOPs na página zero" está **confirmada**.
- Se o boot **continuar igual** (mesmas 1264 ocorrências), a hipótese está **refutada**: alguém
  escreve conteúdo executável em `0x00..0x20`, e o dump da página de vetores (já coletado pelo
  `[PC14]`) mostra o quê.
- Controle negativo obrigatório: o Core1 **não** pode ser afetado — o AMSS/ARM9 usa memória
  baixa legitimamente (linha 2478 mapeia `0x0..0x800000`).

---

## 2. Achado de auditoria: `uc_ctl_remove_cache` nunca invalidou nada (P5)

**[FATO VERIFICADO — medido nesta sessão, com controle positivo e negativo]**

O header instalado (`/usr/include/unicorn/unicorn.h`, linhas 656–657) declara:

```c
#define uc_ctl_remove_cache(uc, address, end)                                  \
    uc_ctl(uc, UC_CTL_WRITE(UC_CTL_TB_REMOVE_CACHE, 2), (address), (end))
```

O segundo argumento é o **endereço FINAL**, não um tamanho. O `zeebo_lle_main.cpp`
passa **tamanho** em 27 dos 28 call-sites:

```cpp
uc_ctl_remove_cache(uc, 0xb000c720, 0x100);   // end=0x100 < address  -> UC_ERR_ARG
uc_ctl_remove_cache(uc, target_pc,  16);      // idem
```

Testemunha executável: `notes/boot-investigation/uc_remove_cache_witness.py`
(Unicorn 2.1.2, mesma versão do `intr_pc_witness.py`):

| caso | `address` | 2º argumento | resultado |
|---|---|---|---|
| forma usada hoje `(addr, tamanho)` | `0xb0000720` | `0x100` | **`UC_ERR_ARG`** |
| forma correta `(addr, end)` | `0xb0000720` | `0xb0000820` | OK |

O valor de retorno **é descartado** em todos os call-sites, então o erro nunca apareceu.

**Consequência:** toda a invalidação de TB feita no handler de syscall
(linhas 3443, 3444, 3451, 3456, 3487, 3501, 3512, 3523, 3534, 3539, 3552, 3564, 3803,
3861, 3862, 3878, 3893, 3904, 3911, 3918, 4383, 4394, 4401, 4408, mais 2162 e 2174)
**nunca aconteceu**. Os comentários do código que atribuem avanço do boot à
"invalidação do TB para recompilar o bloco seguinte" descrevem um efeito que não
existiu; o que de fato mudou o comportamento foi a escrita do PC com o LSB
(convenção BX), que força o modo de decodificação por outro caminho.

**Exceção — o único call-site correto:** `queue_tb_invalidate(pc, pc + size)`
(linha 4356, caminho de código automodificado do REX no Core1), drenado em
`drain_tb_invalidate` (linha 4684): esse passa `(begin, end)` de verdade e funciona.

**APLICAÇÃO NO ZEEBO-LLE (P5):** antes de qualquer trabalho de invalidação dirigida
por evento (o modelo `v6_coherent_user_range` do Linux ou o `BPIALL/ICIALLU` do
FreeBSD), corrigir a aridade e **checar o retorno**. Um wrapper resolve os dois:

```cpp
static inline void tb_invalidate(uc_engine* uc, u32 addr, u32 size, const char* who) {
    uc_err e = uc_ctl_remove_cache(uc, addr, addr + (size ? size : 4)); // end, nao tamanho
    if (e != UC_ERR_OK) {                     // PODE FALHAR: nunca silenciar
        fprintf(stderr, "[TB] remove_cache(%s) addr=0x%08x size=0x%x -> %s\n",
                who, addr, size, uc_strerror(e));
        abort();                              // disciplina KASSERT/INVARIANTS do FreeBSD
    }
}
```

**TESTE QUE PODE FALHAR:** trocar os 27 call-sites pelo wrapper e rodar o boot.
- Se o boot **mudar** (número de instruções, fronteira alcançada, contagem de `PC=0x14`),
  então a invalidação importa e várias conclusões de QW41/QW43 foram atribuídas à causa
  errada — cada uma precisa ser re-medida.
- Se o boot ficar **idêntico**, está provado que a invalidação era irrelevante nesses
  pontos, e os ~26 call-sites podem ser removidos em vez de "consertados".
- Controle: a métrica tem de ser a mesma já usada em QW99 (ocorrências de `PC=0x14` e
  total de insns do Core0), para ser comparável com o número publicado (1264 / ~7.043.000).

---

## 10. Linux (arch/arm e arch/arm64) — tecnicas de entrada/saida de excecao, modo ARM/Thumb, context switch e coerencia

### Procedencia, versao e licenca

- Kernel estudado: **Linux, tag `v6.12`** (arvore `torvalds/linux`), arquivos lidos diretamente do repositorio
  (`raw.githubusercontent.com/torvalds/linux/v6.12/...`). Todas as citacoes de linha abaixo referem-se a essa tag.
- Licenca: **GPL-2.0**. Os arquivos de `arch/arm/kernel/` carregam `SPDX-License-Identifier: GPL-2.0-only`
  (ex.: `arch/arm/kernel/entry-common.S:1`) ou `GPL-2.0` (`arch/arm/kernel/entry-header.S:1`).
- **AVISO DE LICENCA:** o zeebo-lle pode reimplementar o *padrao arquitetural* descrito aqui (salvar CPSR,
  restaurar modo+T atomicamente, calcular largura da instrucao, invalidar TLB por ASID). O que **nao** pode
  acontecer e' copiar trechos literais de codigo GPL-2.0 para dentro do emulador se o projeto nao for GPL-2.0
  compativel. Este documento cita fragmentos curtos como *evidencia tecnica* (uso legitimo de referencia), nao
  como material a ser colado.
- Marcacao usada: **[FATO VERIFICADO]** = eu li a linha citada. **[INFERENCIA]** = deducao minha a partir do que li.
  **NAO VERIFICADO** = nao consegui confirmar na fonte.

---

## 10.1 Entrada e saida de syscall em ARM: `vector_swi` e `ret_fast_syscall`

### 1.1 O caminho fisico: vetor 0x08 -> stub -> `vector_swi`

[FATO VERIFICADO] A tabela de vetores real do Linux ARM esta em `arch/arm/kernel/entry-armv.S:1073-1084`,
secao `.vectors`:

```
	.section .vectors, "ax", %progbits
	W(b)	vector_rst          @ 0x00
	W(b)	vector_und          @ 0x04
	W(ldr)	pc, .               @ 0x08  (SVC) -> relocado para .L__vector_swi
	W(b)	vector_pabt         @ 0x0c
	W(b)	vector_dabt         @ 0x10
	W(b)	vector_addrexcptn   @ 0x14
	W(b)	vector_irq          @ 0x18
	W(b)	vector_fiq          @ 0x1c
```

O slot 0x08 e' o unico que usa `ldr pc, [literal]` em vez de `b`: o comentario em
`arch/arm/kernel/entry-armv.S:922-925` explica que `.L__vector_swi` (`:926-927`, `.word vector_swi`) fica no
inicio da secao `.stubs`, "4k down", fora do alcance de um branch relativo. Ou seja: **o vetor 0x08 nao contem
codigo, contem um salto indireto para o handler real.** [FATO VERIFICADO]

O hardware, ao tomar a excecao SVC, ja fez tres coisas antes da primeira instrucao do handler executar:
1. copiou CPSR para `SPSR_svc`;
2. colocou o endereco da instrucao seguinte ao `svc` em `LR_svc`;
3. entrou em modo SVC com **T=0** (ARM) — ou T=1 se `SCTLR.TE` estiver setado (v7+).
[INFERENCIA a partir do codigo: `vector_swi` le `spsr` no passo 1 (`entry-common.S:182`) e usa `lr` como PC do
chamador (`:183-184`), o que so faz sentido se o hardware populou ambos. O ponto 3 e' [INFERENCIA]: o codigo
Linux nunca precisa checar o proprio modo porque ele *ja esta* em modo/ISA do handler.]

### 1.2 Salvar: `SPSR` carrega o bit T do chamador

[FATO VERIFICADO] `arch/arm/kernel/entry-common.S:171-187` (`ENTRY(vector_swi)`):

```
	sub	sp, sp, #PT_REGS_SIZE
	stmia	sp, {r0 - r12}			@ Calling r0 - r12
3:
 ARM(	add	r8, sp, #S_PC		)
 ARM(	stmdb	r8, {sp, lr}^		)	@ Calling sp, lr   <-- banked USER sp/lr
 THUMB(	mov	r8, sp			)
 THUMB(	store_user_sp_lr r8, r10, S_SP	)
	mrs	saved_psr, spsr			@ called from non-FIQ mode, so ok.
 TRACE(	mov	saved_pc, lr		)
	str	saved_pc, [sp, #S_PC]		@ Save calling PC
	str	saved_psr, [sp, #S_PSR]		@ Save CPSR
	str	r0, [sp, #S_OLD_R0]		@ Save OLD_R0
```

Tres pontos que importam para o zeebo-lle:

1. **O CPSR do chamador nao e' adivinhado — ele e' lido do SPSR** (`mrs saved_psr, spsr`). O bit T do chamador
   esta dentro dele (`PSR_T_BIT = 0x20`, `arch/arm/include/uapi/asm/ptrace.h`, `V4_PSR_T_BIT 0x00000020`).
   [FATO VERIFICADO]
2. **`stmdb r8, {sp, lr}^`** com o sufixo `^` salva `sp_usr`/`lr_usr` (registradores *banked* do modo usuario),
   nao os do modo SVC. Ou seja, a "troca de banco" e' explicita e visivel no salvamento. [FATO VERIFICADO]
3. `S_OLD_R0` guarda o `r0` original *antes* de qualquer clobber, porque o reinicio de syscall precisa dele
   (secao 2).

### 1.3 Restaurar: `movs pc, lr` / `rfeia` restauram modo+T **atomicamente**

[FATO VERIFICADO] `arch/arm/kernel/entry-header.S:309-330`, macro `restore_user_regs` (caminho kernel-ARM):

```
	mov	r2, sp
	ldr	r1, [r2, #\offset + S_PSR]	@ get calling cpsr
	ldr	lr, [r2, #\offset + S_PC]!	@ get pc
	tst	r1, #PSR_I_BIT | 0x0f
	bne	1f
	msr	spsr_cxsf, r1			@ save in spsr_svc
	...
	ldmdb	r2, {r0 - lr}^			@ get calling r0 - lr
	mov	r0, r0				@ ARMv5T and earlier require a nop
	add	sp, sp, #\offset + PT_REGS_SIZE
	movs	pc, lr				@ return & move spsr_svc into cpsr
1:	bug	"Returning to usermode but unexpected PSR bits set?", \@
```

E o caminho kernel-Thumb (`entry-header.S:340-360`) termina igual: `movs pc, lr`.
Para retorno a *kernel* (excecao em modo SVC) existe `svc_exit` (`entry-header.S:202-245`), que usa
`ldmia sp, {r0 - pc}^` (ARM) ou `stmdb lr!, {r0, r1, \rpsr}` + `rfeia sp!` (Thumb). [FATO VERIFICADO]

**Por que isso e' atomico e por que isso importa:**

- `movs pc, lr` executado em modo com SPSR = "escreve PC **e** copia SPSR->CPSR na mesma instrucao". O bit T novo
  vem do SPSR, nao do LSB do LR. Nao existe janela em que o PC ja e' o novo e o modo/T ainda e' o velho.
- `ldmia sp, {r0 - pc}^` (o `^` com PC na lista) faz o mesmo: restaura r0..r15 **e** CPSR<-SPSR de uma vez.
- `rfeia sp!` (ARMv6+) carrega PC e CPSR de dois words consecutivos na pilha, tambem atomicamente. Repare no
  layout que o Linux monta em `entry-header.S:239`: `stmdb lr!, {r0, r1, \rpsr}` empilha
  `{lr_usuario, pc, psr}` — o par `pc,psr` na ordem exigida pelo `rfe`.
- O `bug` em `entry-header.S:330/360` e' um **controle negativo embutido no proprio kernel**: se o CPSR salvo
  tem I setado ou modo != usuario (`tst r1, #PSR_I_BIT | 0x0f`), o kernel recusa retornar e explode. Ele nao
  tenta "consertar" o estado. [FATO VERIFICADO]

[INFERENCIA] O padrao geral: **o modo/ISA de retomada e' um dado salvo, transportado e restaurado junto com o
PC, nunca reconstruido por heuristica.** A unica heuristica que o Linux usa em torno de ISA e' no *callee*
(interworking BX, secao 1.5), nunca no retorno de excecao.

### 1.4 `struct pt_regs` e `ARM_cpsr`

[FATO VERIFICADO] `arch/arm/include/asm/ptrace.h:16-18` e `arch/arm/include/uapi/asm/ptrace.h`:

```
struct pt_regs { unsigned long uregs[18]; };
#define ARM_cpsr	uregs[16]
#define ARM_pc		uregs[15]
#define ARM_lr		uregs[14]
#define ARM_sp		uregs[13]
...
#define ARM_r0		uregs[0]
#define ARM_ORIG_r0	uregs[17]
```

Os offsets em assembly saem de `arch/arm/kernel/asm-offsets.c:68-86`: `S_R0`, `S_SP`, `S_LR`, `S_PC`, `S_PSR`,
`S_OLD_R0`, `PT_REGS_SIZE`. [FATO VERIFICADO]

Os predicados derivados sao *funcoes puras do CPSR salvo*, nunca do endereco:

```c
#define user_mode(regs)   (((regs)->ARM_cpsr & 0xf) == 0)          /* ptrace.h:28-29 */
#define thumb_mode(regs)  (((regs)->ARM_cpsr & PSR_T_BIT))         /* ptrace.h:32-33 */
#define isa_mode(regs)    (FIELD_GET(PSR_J_BIT,(regs)->ARM_cpsr)<<1 | \
                           FIELD_GET(PSR_T_BIT,(regs)->ARM_cpsr))  /* ptrace.h:39-41 */
#define processor_mode(regs) ((regs)->ARM_cpsr & MODE_MASK)        /* ptrace.h:46-47 */
```

[FATO VERIFICADO] Note `isa_mode()`: o estado de ISA e' um par **(J,T)**, nao um bit so — 00=ARM, 01=Thumb,
10=Jazelle, 11=ThumbEE.

### 1.5 A excecao: `badr` e o LSB do endereco (interworking BX)

[FATO VERIFICADO] `arch/arm/include/asm/assembler.h:207-215`:

```
	.macro	badr\c, rd, sym
#ifdef CONFIG_THUMB2_KERNEL
	adr\c	\rd, \sym + 1
#else
	adr\c	\rd, \sym
#endif
	.endm
```

Comentario em `:202-205`: "Assembly version of `adr rd, BSYM(sym)`. This should only be used to reference local
symbols in the same assembly file". `badr lr, \ret` e' usado em `invoke_syscall`
(`entry-header.S:398` e `:407`) para montar o endereco de retorno do `sys_*`.
E `ret\c, reg` (`assembler.h:546-558`) so usa `bx` quando o registrador e' `lr`; caso contrario `mov pc, reg`.

[INFERENCIA] Portanto o Linux tem **duas convencoes distintas e nao intercambiaveis**:
- **retorno de excecao** -> ISA vem do SPSR (`movs pc, lr` / `rfe`), LSB do PC e' irrelevante;
- **chamada/branch normal dentro do kernel** -> ISA vem do LSB do endereco (`bx`/`blx`), via `badr`.
Misturar as duas e' exatamente o bug P1 do zeebo-lle.

### APLICACAO NO ZEEBO-LLE (P1, P2)

O emulador hoje faz, em `tools/cpp/zeebo_lle_main.cpp:3454-3467`, a adivinhacao por faixa:

```
bool caller_is_kernel_stub = (pc >= 0xb0000000u && pc < 0xb0020000u);
bool caller_is_ig_naming_arm = (pc >= 0xb0100000u && pc < 0xb0120000u) || ...
auto apply_tbit = [&](u32 v) { return (caller_is_kernel_stub||caller_is_ig_naming_arm) ? (v&~1u) : (v|1u); };
```

Isso ja falhou uma vez de forma documentada: o comentario QW42 em `:3505-3519` registra que uma **copia local**
do trap-stack dentro de `ig_naming` era ARM, mas caia fora da faixa "kernel stub", e teve que ganhar um remendo
de faixa novo. Esse e' o modo de falha estrutural da heuristica: cada nova copia de stub exige uma faixa nova.

Reimplementacao recomendada (padrao Linux, sem copiar codigo):

1. Criar um `struct L4PtRegs` no emulador com **18 campos**, incluindo `cpsr` explicito, espelhando o papel de
   `pt_regs`. Nao e' preciso o mesmo layout; e' preciso o mesmo *conteudo*: `r0..r12, sp_usr, lr_usr, pc, cpsr,
   orig_r0`.
2. No `UC_HOOK_INTR`, **ler o CPSR real do Unicorn** (`uc_reg_read(uc, UC_ARM_REG_CPSR, &cpsr)`) e gravar em
   `regs.cpsr`. Como o prompt informa que nao ha excecao real (nao ha SPSR salvo por hardware, o CPSR continua
   o do chamador), esse CPSR lido **ja e' o SPSR que o hardware teria salvo**. Isso torna o T-bit um dado
   medido, nao adivinhado. Substitui `caller_is_kernel_stub`/`caller_is_ig_naming_arm`/`apply_tbit` inteiros.
3. No epilogo, escrever `PC` e depois `CPSR` de volta a partir de `regs`, nessa ordem, e nunca derivar T do LSB
   do PC. Se o Unicorn insistir em derivar T do LSB do PC escrito (como o comentario QW41 em `:3448-3452`
   afirma), entao o epilogo deve ser: `pc_to_write = (regs.pc & ~1u) | ((regs.cpsr & 0x20) ? 1u : 0u)` — o LSB
   vira **funcao do CPSR salvo**, nao de uma faixa de endereco. Isso preserva a semantica de `movs pc, lr`.
4. Guardar `orig_r0` antes de escrever o retorno em `r0` (secao 2 depende disso).

### TESTE QUE PODE FALHAR (P1)

Controle negativo executavel, sem heuristica de faixa:

1. Instrumentar o hook de INTR para logar, por syscall: `(pc, cpsr_lido, T_bit_lido, T_bit_que_apply_tbit_daria)`.
2. Rodar o boot atual ate o ponto conhecido (`0xb0358bb4` / `0xb03ba634`, citados em `:3516-3518`).
3. **Criterio de falha:** se existir **qualquer** syscall em que `T_bit_lido != T_bit_que_apply_tbit_daria`,
   a heuristica de faixa esta comprovadamente errada naquele ponto — e o CPSR medido e' a fonte correta.
4. **Criterio de falha inverso (mais forte):** trocar `apply_tbit` pelo T-bit medido e rodar. Se o boot
   regredir (chegar *menos* longe que `0xb0400064`), entao a premissa "o CPSR do Unicorn no hook de INTR e' o
   CPSR do chamador" esta errada, e o emulador precisa salvar o CPSR *antes* do `svc` (hook de code em
   `svc`), nao depois. Esse teste distingue as duas hipoteses de forma binaria.

---

## 10.2 Largura da instrucao SVC, numero de syscall, e o reinicio que recua 2 ou 4 bytes

### 2.1 Como o Linux obtem o numero da syscall (tres ABIs, tres mecanismos)

[FATO VERIFICADO] `arch/arm/kernel/entry-common.S:199-228`:

```
#if defined(CONFIG_OABI_COMPAT)
#ifdef CONFIG_ARM_THUMB
	tst	saved_psr, #PSR_T_BIT
	movne	r10, #0				@ no thumb OABI emulation
 USER(	ldreq	r10, [saved_pc, #-4]	)	@ get SWI instruction
#else
 USER(	ldr	r10, [saved_pc, #-4]	)	@ get SWI instruction
#endif
 ARM_BE8(rev	r10, r10)			@ little endian instruction
#elif defined(CONFIG_AEABI)
	/* Pure EABI user space always put syscall number into scno (r7). */
#elif defined(CONFIG_ARM_THUMB)
	/* Legacy ABI only, possibly thumb mode. */
	tst	saved_psr, #PSR_T_BIT		@ this is SPSR from save_user_regs
	addne	scno, r7, #__NR_SYSCALL_BASE	@ put OS number in
 USER(	ldreq	scno, [saved_pc, #-4]	)
#endif
```

Leitura tecnica, item por item:

- **EABI puro:** o numero vem de **r7**. Nao ha releitura de instrucao, nao ha dependencia de largura.
  Esse e' o caminho moderno e o mais robusto.
- **OABI / legacy:** o numero esta *dentro* do imediato do `swi`, entao o kernel precisa **reler a instrucao
  da memoria do usuario**: `ldr r10, [saved_pc, #-4]`. O `-4` e' o recuo para ARM.
- **Thumb + OABI:** `tst saved_psr, #PSR_T_BIT` / `movne r10, #0` — o kernel **desiste** ("no thumb OABI
  emulation"). Ele nao tenta `[pc, #-2]`. [FATO VERIFICADO]
- O mascaramento do opcode e' `bic scno, scno, #0xff000000` (`:250`) / `bics r10, r10, #0xff000000` (`:244`),
  isto e', 24 bits de imediato do `swi` ARM.

[INFERENCIA] Licao: quando o Linux *precisa* reler a instrucao que causou a trap, ele decide o offset
(`-4`) **a partir do T-bit do SPSR** e explicitamente recusa o caso Thumb+OABI em vez de chutar.

Ha tambem o tratamento de falha ao reler a instrucao, `entry-common.S:287-291`:

```
9001:
	sub	lr, saved_pc, #4
	str	lr, [sp, #S_PC]
	get_thread_info tsk
	b	ret_fast_syscall
```

Ou seja: se o `ldr` do opcode falhou, o kernel **reposiciona o PC salvo de volta para a propria instrucao `swi`**
e retorna; o usuario re-executa o `svc` e toma o page fault de verdade. Isso e' um padrao de recuperacao
sem inventar valores. [FATO VERIFICADO]

### 2.2 O reinicio de syscall: recuo de 2 (Thumb) ou 4 (ARM)

[FATO VERIFICADO] `arch/arm/kernel/signal.c:537-567`, `do_signal()`:

```c
	if (syscall) {
		continue_addr = regs->ARM_pc;
		restart_addr = continue_addr - (thumb_mode(regs) ? 2 : 4);
		retval = regs->ARM_r0;
		switch (retval) {
		case -ERESTART_RESTARTBLOCK:
			restart -= 2;
			fallthrough;
		case -ERESTARTNOHAND:
		case -ERESTARTSYS:
		case -ERESTARTNOINTR:
			restart++;
			regs->ARM_r0 = regs->ARM_ORIG_r0;
			regs->ARM_pc = restart_addr;
			break;
		}
	}
```

**Esta e' a linha-chave do tema P2**: `restart_addr = continue_addr - (thumb_mode(regs) ? 2 : 4)`.
O kernel calcula a largura da instrucao `svc` **exclusivamente a partir do T-bit do CPSR salvo**, e nao de faixa
de endereco, nem de tabela de stubs, nem de flag de thread.

Observe tambem `regs->ARM_r0 = regs->ARM_ORIG_r0` — o reinicio restaura o primeiro argumento a partir da copia
guardada em `S_OLD_R0` na entrada. Sem `orig_r0` o reinicio corromperia o argumento. [FATO VERIFICADO]

E ha um *guard* contra reinicio duplo, `signal.c:580` e `:593`:
`if (unlikely(restart) && regs->ARM_pc == restart_addr)` — so desfaz o recuo se o PC ainda for o que ele mesmo
escreveu (um debugger pode ter mexido). Padrao de "verifique que o mundo ainda e' o que voce deixou".

O mesmo calculo 2-ou-4 aparece em outros tres lugares independentes:
- `arch/arm/kernel/traps.c:624` — `regs->ARM_pc -= thumb_mode(regs) ? 2 : 4;` no `NR(breakpoint)` de `arm_syscall`;
- `arch/arm/kernel/traps.c:562-565` — endereco reportado no `arm_notify_die` de "bad syscall";
- `arch/arm/kernel/traps.c:688-691` — idem em "bad syscall(2)".
[FATO VERIFICADO]

Em arm64 o mesmo padrao sobrevive para tarefas compat: `arch/arm64/kernel/signal.c:1407`,
`restart_addr = continue_addr - (compat_thumb_mode(regs) ? 2 : 4);`. [FATO VERIFICADO]

### 2.3 Largura de instrucao Thumb: `is_wide_instruction` e a composicao 16+16

[FATO VERIFICADO] `arch/arm/include/asm/ptrace.h:116-122`:

```c
/*
 * True if instr is a 32-bit thumb instruction. This works if instr
 * is the first or only half-word of a thumb instruction. ...
 */
#define is_wide_instruction(instr)	((unsigned)(instr) >= 0xe800)
```

Uso em `arch/arm/kernel/traps.c:471-481` (`do_undefinstr`, caminho usuario/Thumb):

```c
	} else if (thumb_mode(regs)) {
		if (get_user(instr, (u16 __user *)pc))  goto die_sig;
		instr = __mem_to_opcode_thumb16(instr);
		if (is_wide_instruction(instr)) {
			unsigned int instr2;
			if (get_user(instr2, (u16 __user *)pc+1)) goto die_sig;
			instr2 = __mem_to_opcode_thumb16(instr2);
			instr = __opcode_thumb32_compose(instr, instr2);
		}
	} else {
		if (get_user(instr, (u32 __user *)pc))  goto die_sig;
		instr = __mem_to_opcode_arm(instr);
	}
```

Isto e', a decodificacao e' em **duas etapas**: le 16 bits, testa `>= 0xe800`, e so entao le o segundo
halfword e compoe com `__opcode_thumb32_compose` (`arch/arm/include/asm/opcodes.h:151-154`). As macros
`__mem_to_opcode_*` / `__opcode_to_mem_*` existem porque a representacao em memoria difere da canonica em BE8
(`opcodes.h:52-53, 100-133`). [FATO VERIFICADO]

### 2.4 IT-block: o estado escondido que quebra "avancar o PC"

[FATO VERIFICADO] `arch/arm/include/asm/ptrace.h:173-196`:

```c
/*
 * Update ITSTATE after normal execution of an IT block instruction.
 *	ITSTATE<1:0> are in CPSR<26:25>
 *	ITSTATE<7:2> are in CPSR<15:10>
 */
static inline unsigned long it_advance(unsigned long cpsr)
{
	if ((cpsr & 0x06000400) == 0) {
		cpsr &= ~PSR_IT_MASK;
	} else {
		const unsigned long mask = 0x06001c00;
		unsigned long it = cpsr & mask;
		it <<= 1;
		it |= it >> (27 - 10);
		it &= mask;
		cpsr &= ~mask;
		cpsr |= it;
	}
	return cpsr;
}
```

E o consumidor, `arch/arm/probes/kprobes/core.c:209-221`:

```c
static void __kprobes singlestep_skip(struct kprobe *p, struct pt_regs *regs)
{
#ifdef CONFIG_THUMB2_KERNEL
	regs->ARM_cpsr = it_advance(regs->ARM_cpsr);
	if (is_wide_instruction(p->opcode))
		regs->ARM_pc += 4;
	else
		regs->ARM_pc += 2;
#else
	regs->ARM_pc += 4;
#endif
}
```

**Esta funcao e' o modelo exato do que P2 precisa.** "Pular uma instrucao" em Thumb-2 nao e' `pc += 2`: e'
(a) avancar o ITSTATE no CPSR **e** (b) avancar o PC por 2 ou 4 conforme `is_wide_instruction`. Se o ITSTATE
nao for avancado, a proxima instrucao pode ser executada com a condicao errada (ou nem executada).

Ainda em `signal.c:356-372`, na entrega de sinal, o kernel limpa o IT state ao trocar de ISA:
```c
		/*
		 * Clear the If-Then Thumb-2 execution state. ARM spec
		 * requires this to be all 000s in ARM mode. Snapdragon
		 * S4/Krait misbehaves on a Thumb=>ARM signal transition without this.
		 */
		cpsr &= ~PSR_IT_MASK;
		if (thumb) cpsr |= PSR_T_BIT; else cpsr &= ~PSR_T_BIT;
```
[FATO VERIFICADO] — inclusive com mencao a hardware Qualcomm real que quebra sem isso.

### APLICACAO NO ZEEBO-LLE (P2)

Estado atual: o prompt descreve que o hook de INTR entrega PC ja em `svc+4`, o codigo soma `+4` em alguns ramos
e usa LR em outros, e nao ha checagem de largura. No fonte, `zeebo_lle_main.cpp:3470-3472` e `:3532-3534`
documentam justamente "UC_HOOK_INTR entrega pc == svc+4" e o bug QW17 de avancar duas vezes.

Reimplementacao proposta (padrao Linux):

1. Definir **uma unica** funcao no emulador, algo como:
   ```cpp
   // svc_width(cpsr) -> 2 se T-bit setado, 4 caso contrario  (modelo: signal.c:548)
   static inline u32 svc_width(u32 cpsr) { return (cpsr & 0x20u) ? 2u : 4u; }
   ```
   e usa-la em **todos** os ramos. O endereco da instrucao `svc` passa a ser
   `svc_addr = pc_entregue_pelo_hook - svc_width(cpsr)` quando o hook entrega `svc+largura`,
   ou `svc_addr = pc_entregue` quando entrega o proprio `svc` — o ponto e' medir isso **uma vez**, documentar,
   e nunca mais somar `+4` ad hoc.
2. O PC de retomada e' **sempre** `svc_addr + svc_width(cpsr)`. Nunca `pc+4` fixo.
3. Guardar `orig_r0` no `L4PtRegs` na entrada, para permitir a semantica de reinicio (util quando uma syscall
   L4 precisa ser re-executada, ex.: IPC que ainda nao tem par pronto).
4. Se algum stub L4 usar SVC dentro de um bloco IT (possivel no AMSS Thumb-2), portar a *logica* de
   `it_advance` (reescrita em C++ proprio, nao copiada) e aplica-la ao CPSR salvo ao retomar.

### TESTE QUE PODE FALHAR (P2)

1. Adicionar um assert no hook de INTR: ler 4 bytes em `svc_addr = pc - svc_width(cpsr)` e verificar que o
   opcode e' de fato um SVC:
   - ARM: `(op & 0x0f000000) == 0x0f000000`;
   - Thumb16: `(op16 & 0xff00) == 0xdf00`.
2. **Criterio de falha:** se o assert disparar em qualquer syscall do boot, a combinacao
   (offset entregue pelo hook, largura calculada) esta errada — e o log dira exatamente em qual endereco.
3. Segundo controle negativo: forcar `svc_width()` a retornar sempre 4 e rodar o boot. Se o boot **nao**
   regredir, entao ou (a) nao existe nenhum SVC Thumb no caminho testado (e a hipotese "AMSS e' 100% Thumb" do
   comentario em `:3441-3444` esta errada), ou (b) o teste nao cobre o caminho critico. Ambas as conclusoes sao
   acionaveis; hoje nao da para distinguir porque a largura nunca e' checada.

---

## 10.3 Troca de contexto: `__switch_to` e a regra "quem troca de thread nao volta pelo epilogo normal"

### 3.1 `__switch_to`: o que e' salvo e por que e' pouco

[FATO VERIFICADO] `arch/arm/kernel/entry-armv.S:511-579`:

```
ENTRY(__switch_to)
	add	ip, r1, #TI_CPU_SAVE
 ARM(	stmia	ip!, {r4 - sl, fp, sp, lr} )	@ Store most regs on stack
 THUMB(	stmia	ip!, {r4 - sl, fp}	   )
 THUMB(	str	sp, [ip], #4		   )
 THUMB(	str	lr, [ip], #4		   )
	ldr	r4, [r2, #TI_TP_VALUE]
	ldr	r5, [r2, #TI_TP_VALUE + 4]
	...
	switch_tls r1, r4, r5, r3, r7
	...
	mov	r0, r5
	set_current r7, r8
	ldmia	r4, {r4 - sl, fp, sp, pc}	@ Load all regs saved previously
```

O bloco salvo e' `struct cpu_context_save` (`arch/arm/include/asm/thread_info.h:45-57`):
`r4, r5, r6, r7, r8, r9, sl, fp, sp, pc, extra[2]`. **Onze words.** [FATO VERIFICADO]

Repare no que **nao** esta ali: r0-r3, r12, lr, **e CPSR**. Motivo [INFERENCIA, forte]: `__switch_to` e' chamado
como funcao C normal (AAPCS), sempre do mesmo modo (SVC), sempre com o mesmo estado de ISA do kernel. r0-r3/r12
sao caller-saved; o CPSR de *usuario* da thread nao esta aqui porque ele mora no `pt_regs` no topo da pilha de
kernel daquela thread, e sera restaurado por `restore_user_regs` quando aquela thread voltar para o usuario.

Ou seja: **o Linux tem dois niveis de contexto**, e nao os mistura:
- `cpu_context_save` = contexto de *kernel* de uma thread (callee-saved + sp + pc). Sem CPSR.
- `pt_regs` no topo da pilha = contexto de *usuario* (18 words, **com** CPSR). 

A retomada e' `ldmia r4, {r4 - sl, fp, sp, pc}`: carrega SP e PC **na mesma instrucao**, a partir do bloco
salvo da thread destino. Nao ha "escreve PC, depois escreve SP". [FATO VERIFICADO]

Na variante Thumb2/VMAP_STACK (`entry-armv.S:554-576`) o codigo carrega para `ip`/`lr` e faz
`set_current r1, r2` / `mov sp, ip` / `ret lr`, com o comentario explicito (`:566-573`) de que
`set_current` precisa ficar **o mais perto possivel** da atualizacao de SP, para nao existir janela onde
`current` e SP discordam. [FATO VERIFICADO] Esse e' exatamente o risco de P3: janela de inconsistencia entre
"qual thread eu acho que sou" e "qual pilha estou usando".

### 3.2 O epilogo que **nao** roda: `invoke_syscall` e `ret_from_fork`

[FATO VERIFICADO] `arch/arm/kernel/entry-header.S:392-415` (`invoke_syscall`) termina com:

```
	badr	lr, \ret			@ return address
	ldrcc	pc, [\table, \nr, lsl #2]	@ call sys_* routine
```

O `sys_*` e' chamado com `lr` = `__ret_fast_syscall`. Uma syscall que **troca de thread** (ex.: `sys_pause`,
qualquer bloqueio) entra em `schedule()` -> `__switch_to`, e a partir dali **o `lr` daquela invocacao fica
congelado na pilha de kernel da thread antiga**. Quem continua executando e' a thread nova, com o *seu* proprio
`pc` salvo. O epilogo de retorno da thread antiga so roda quando ela for reescalonada. [INFERENCIA, mas
suportada diretamente pelo mecanismo de `__switch_to` acima.]

O caso de thread nova e' explicito em `arch/arm/kernel/entry-common.S:132-140`:

```
ENTRY(ret_from_fork)
	bl	schedule_tail
	cmp	r5, #0
	movne	r0, r4
	badrne	lr, 1f
	retne	r5
1:	get_thread_info tsk
	b	ret_slow_syscall
ENDPROC(ret_from_fork)
```

E `arch/arm/kernel/process.c:266-267`:
```c
	thread->cpu_context.pc = (unsigned long)ret_from_fork;
	thread->cpu_context.sp = (unsigned long)childregs;
```

[FATO VERIFICADO] Ou seja: a thread nova **nunca** executa o epilogo da syscall que a criou. Ela comeca em
`ret_from_fork`, com SP apontando para o *seu* `pt_regs`, e so entao cai em `ret_slow_syscall` ->
`restore_user_regs`, que restaura o `pt_regs` **dela**.

### APLICACAO NO ZEEBO-LLE (P3)

Sintoma medido pelo projeto: `L4_Ipc` (syscall 0x00) com handoff faz salto espurio para `PC=0x14`.
O codigo ja tem um remendo em `zeebo_lle_main.cpp:3483-3491`:

```cpp
} else if (syscall == 0x00) {
    if (did_handoff) {
        // Handoff cooperativo já reescreveu PC/SP para a thread alvo ...
        // NÃO sobrescrever de volta para o wrapper IPC (0xb000c834) ...
        uc_reg_read(uc, UC_ARM_REG_PC, &target_pc);
```

O remendo `did_handoff` esta **conceitualmente certo** e coincide com o padrao do Linux, mas esta implementado
como excecao dentro do epilogo unico. O padrao do kernel e' mais forte: **separar os dois caminhos na origem**.

Proposta (modelo `invoke_syscall` + `__switch_to` + `ret_from_fork`):

1. Cada handler de syscall L4 no emulador retorna um `enum class SyscallOutcome { Return, Switched }`
   (analogo a "retorna via `lr`=`__ret_fast_syscall`" vs "nunca retorna, foi para `__switch_to`").
2. O epilogo comum roda **se e somente se** `outcome == Return`. Com `Switched`, o epilogo e' `return;` puro:
   nao escreve R0, nao escreve PC, nao escreve SP, nao chama `uc_ctl_remove_cache` em endereco do chamador.
   Isso elimina por construcao a classe de bug "o epilogo corrompe a thread destino".
3. O valor de retorno (R0) da thread **suspensa** deve ser escrito no `L4PtRegs` **dela** (o analogo de
   `str r0, [sp, #S_R0+S_OFF]` em `entry-common.S:55` / `:68`), nunca nos registradores vivos da CPU. A thread
   suspensa so ve esse R0 quando for retomada e seu contexto for restaurado.
4. A retomada da thread destino deve escrever `SP` e `PC` (e `CPSR`) **do bloco de contexto dela**, na mesma
   rotina, sem passar pelo epilogo do chamador — o analogo de `ldmia r4, {r4 - sl, fp, sp, pc}`.

### TESTE QUE PODE FALHAR (P3)

1. Instrumentar: gravar, para cada syscall, `(tid_antes, tid_depois, pc_escrito_pelo_epilogo,
   sp_escrito_pelo_epilogo)`.
2. **Assert duro:** `tid_antes != tid_depois` implica que o epilogo **nao** escreveu nem PC nem SP.
   Se esse assert disparar, o bug de P3 esta reproduzido no ato e com o TID exato.
3. Segundo controle negativo: desativar a condicao `did_handoff` (voltar a sempre escrever `target_pc =
   apply_tbit(pc)` no `case 0x00`). **Predicao falsificavel:** o `PC=0x14` deve reaparecer. Se **nao**
   reaparecer, entao `did_handoff` nao e' a causa do `PC=0x14` e a investigacao deve migrar para o stub (ver
   secao 7 sobre `PC=0x14` e secao 3.3 abaixo sobre SP corrompido).

### 3.3 Nota lateral: SP de trap corrompido leva a `pop {..., pc}` com lixo

O proprio fonte do zeebo-lle ja documenta esse mecanismo em `zeebo_lle_main.cpp:3502-3504`:
"É MANDATÓRIO restaurar SP=ip: sem isso o `add lr,sp,#0x30` soma sobre o sp de trap corrompido
(mvn = 0xffffff0c) e o `pop {...,pc}` desempilha lixo, saltando para PC=0x00000000."

[INFERENCIA] `PC=0x14` e `PC=0x00000000` sao o **mesmo modo de falha** com offsets diferentes: um `pop`/`ldm`
com PC na lista lendo de uma pilha errada. O Linux evita isso porque `restore_user_regs` reconstroi SP de forma
deterministica (`add sp, sp, #\offset + PT_REGS_SIZE`, `entry-header.S:328`) a partir de um layout fixo, e
porque o `bug` em `:330` recusa retornar quando o PSR salvo nao faz sentido. Ver secao 7 para o significado
especifico de 0x14.

---

## 10.4 Primeira ativacao de thread: qual e' o estado minimo

### 4.1 `copy_thread` — o contexto de kernel de uma thread que nunca rodou

[FATO VERIFICADO] `arch/arm/kernel/process.c:235-278`:

```c
int copy_thread(struct task_struct *p, const struct kernel_clone_args *args)
{
	struct thread_info *thread = task_thread_info(p);
	struct pt_regs *childregs = task_pt_regs(p);

	memset(&thread->cpu_context, 0, sizeof(struct cpu_context_save));
	...
	if (likely(!args->fn)) {
		*childregs = *current_pt_regs();      /* thread de usuario: herda tudo */
		childregs->ARM_r0 = 0;                /* fork() retorna 0 no filho    */
		if (stack_start) childregs->ARM_sp = stack_start;
	} else {
		memset(childregs, 0, sizeof(struct pt_regs));   /* kernel thread */
		thread->cpu_context.r4 = (unsigned long)args->fn_arg;
		thread->cpu_context.r5 = (unsigned long)args->fn;
		childregs->ARM_cpsr = SVC_MODE;
	}
	thread->cpu_context.pc = (unsigned long)ret_from_fork;
	thread->cpu_context.sp = (unsigned long)childregs;
	...
	if (clone_flags & CLONE_SETTLS) thread->tp_value[0] = tls;
	thread->tp_value[1] = get_tpuser();
	thread_notify(THREAD_NOTIFY_COPY, thread);
	return 0;
}
```

Inventario do estado minimo criado, item por item: [FATO VERIFICADO]

| Campo | Valor | Papel |
|---|---|---|
| `cpu_context` (11 words) | zerado, depois `pc`/`sp` | contexto de kernel |
| `cpu_context.pc` | `ret_from_fork` | ponto de entrada sintetico |
| `cpu_context.sp` | `childregs` (= topo da pilha de kernel) | pilha de kernel |
| `cpu_context.r4/r5` | `fn_arg`/`fn` (so kernel thread) | argumento e alvo |
| `pt_regs` inteiro | copia do pai, ou zerado | contexto de usuario |
| `pt_regs.ARM_r0` | 0 | valor de retorno do filho |
| `pt_regs.ARM_cpsr` | herdado, ou `SVC_MODE` | **modo + T-bit** |
| `tp_value[0..1]` | TLS (TPIDRURO/TPIDRURW) | TLS por thread |
| `cpu_domain` | `get_domain()` (se `CONFIG_CPU_USE_DOMAINS`) | dominio MMU |

Pontos nao obvios:
- **`cpu_context` e' zerado antes de ser preenchido.** Nada de lixo herdado.
- **O PC inicial e' um trampolim sintetico** (`ret_from_fork`), nao a funcao alvo. A funcao alvo vai em `r5`.
  Isso garante que o *primeiro* codigo a rodar na thread nova seja codigo do kernel que sabe montar o resto
  (`schedule_tail`, `get_thread_info tsk`).
- **`ARM_cpsr` e' sempre definido explicitamente**, nunca deixado indefinido.

### 4.2 `start_thread` — o contexto de usuario de um `execve`

[FATO VERIFICADO] `arch/arm/include/asm/processor.h:52-79`:

```c
#define start_thread(regs,pc,sp)					\
({									\
	...								\
	memset(regs->uregs, 0, sizeof(regs->uregs));			\
	...								\
	if (current->personality & ADDR_LIMIT_32BIT)			\
		regs->ARM_cpsr = USR_MODE;				\
	else								\
		regs->ARM_cpsr = USR26_MODE;				\
	if (elf_hwcap & HWCAP_THUMB && pc & 1)				\
		regs->ARM_cpsr |= PSR_T_BIT;				\
	regs->ARM_cpsr |= PSR_ENDSTATE;					\
	regs->ARM_pc = pc & ~1;		/* pc */			\
	regs->ARM_sp = sp;		/* sp */			\
})
```

**Esta macro e' o modelo canonico para P4.** Sequencia exata:
1. `memset` de **todos** os 18 uregs (zero determinístico, sem lixo);
2. modo: `USR_MODE` (0x10) ou `USR26_MODE`;
3. **T-bit derivado do LSB do endereco de entrada** (`pc & 1`) e gravado **no CPSR**;
4. endianness: `PSR_ENDSTATE` (= `PSR_E_BIT` em BE, 0 em LE);
5. **`ARM_pc = pc & ~1`** — o LSB e' *consumido* para decidir o T-bit e depois **removido** do PC.

[FATO VERIFICADO] Ou seja, a convencao "LSB do ponteiro = Thumb" e' traduzida **uma unica vez**, na criacao do
contexto, para o formato canonico (PC limpo + T no CPSR). Depois disso, so o CPSR manda.

### 4.3 TLS na primeira ativacao

[FATO VERIFICADO] `arch/arm/include/asm/tls.h`, macro `switch_tls_v6k`:

```
	.macro switch_tls_v6k, base, tp, tpuser, tmp1, tmp2
	mrc	p15, 0, \tmp2, c13, c0, 2	@ get the user r/w register
	@ TLS register update is deferred until return to user space
	mcr	p15, 0, \tpuser, c13, c0, 2	@ set the user r/w register
	str	\tmp2, [\base, #TI_TP_VALUE + 4] @ save it
	.endm
```

E em `restore_user_regs` (`entry-header.S:300-305`):

```
	@ The TLS register update is deferred until return to user space so we
	@ can use it for other things while running in the kernel
	mrc	p15, 0, r1, c13, c0, 3		@ get current_thread_info pointer
	ldr	r1, [r1, #TI_TP_VALUE]
	mcr	p15, 0, r1, c13, c0, 3		@ set TLS register
```

[FATO VERIFICADO] Detalhe importante: em ARMv6K+ o kernel **usa TPIDRURO (c13,c0,3) como ponteiro para o
`thread_info` enquanto esta no kernel**, e so restaura o TLS do usuario no ultimo instante, na saida.
Ha ainda o fallback para ARMv6 sem `HWCAP_TLS`: o valor e' escrito em `0xffff0ff0`
(`tls.h`, `switch_tls_v6`: `streq \tp, [\tmp2, #-15]` com `tmp2 = 0xffff0fff`), lido pelo kuser helper
`__kuser_get_tls` em `0xffff0fe0` (`entry-armv.S:821-825`).

### APLICACAO NO ZEEBO-LLE (P4)

O estado minimo para ativar uma thread L4/OKL4 pela primeira vez, por analogia direta:

1. **Zerar** o bloco de contexto inteiro antes de preencher (modelo: `memset(&thread->cpu_context, 0, ...)` e
   `memset(regs->uregs, 0, ...)`). Nada de reaproveitar registradores da thread anterior.
2. **CPSR explicito e obrigatorio**: modo (USR=0x10 / SVC=0x13 conforme o privilegio L4 da thread) + T-bit.
   O T-bit deve vir do bit 0 do *entry point* que o `L4_ExchangeRegisters`/`ThreadControl` forneceu, e o PC
   gravado deve ser `entry & ~1` (modelo: `processor.h:74-77`). Hoje o emulador deriva ISA por faixa de
   endereco; aqui a fonte correta ja existe no proprio argumento da syscall.
3. **SP obrigatorio**. Sem SP valido, o primeiro `push`/`stm` da thread grava em endereco arbitrario — e' a
   origem plausivel dos `pop {..., pc}` com lixo (secao 3.3).
4. **UTCB/TLS**: no OKL4/Iguana o analogo de `tp_value` e' o ponteiro de UTCB. Ele precisa existir **antes** do
   primeiro acesso da thread, e precisa ser trocado no handoff (modelo: `switch_tls` dentro de `__switch_to`,
   `entry-armv.S:526`). Um UTCB stale e' indistinguivel de corrupcao de memoria no sintoma.
5. **Registradores banked**: como o zeebo-lle emula syscalls em C++ sem modos reais, `sp_usr`/`lr_usr` nao sao
   bancados pelo hardware emulado. Entao o bloco de contexto por thread **precisa** conter SP e LR como campos
   proprios, salvos/restaurados explicitamente (modelo: `stmdb r8, {sp, lr}^` na entrada,
   `entry-common.S:179`, e `ldmdb r2, {r0 - lr}^` na saida, `entry-header.S:324`).
6. **Trampolim sintetico**: considerar dar a toda thread nova um PC inicial que aponte para um stub do proprio
   emulador (analogo de `ret_from_fork`), que valida o contexto (CPSR sensato? SP mapeado? UTCB nao-nulo?)
   antes de saltar para o entry point real. Custo: um bloco traduzido. Beneficio: falha ruidosa em vez de
   deriva silenciosa.

### TESTE QUE PODE FALHAR (P4)

1. Implementar `validate_thread_ctx(tid)` chamado **antes** de toda primeira ativacao, com quatro asserts:
   - `(cpsr & 0x1f)` pertence a `{0x10, 0x13, 0x1f}`;
   - `(pc & 1) == 0` (o LSB ja foi consumido para o T-bit);
   - `(cpsr & 0x20) != 0` implica `(pc & 1) == 0` **e** `(pc & 1) == 0` (PC par sempre; alinhamento Thumb = 2);
     `(cpsr & 0x20) == 0` implica `(pc & 3) == 0` (alinhamento ARM = 4);
   - `sp != 0` e `sp` cai numa regiao mapeada conhecida.
2. **Criterio de falha:** qualquer assert que dispare identifica uma thread ativada com contexto incompleto.
   Espera-se especificamente que threads criadas por `L4_ExchangeRegisters` (syscall 0x0c, tratada em
   `zeebo_lle_main.cpp:3495` em diante) sejam as primeiras a falhar, porque esse caminho hoje nao define CPSR.
3. Controle negativo adicional: zerar deliberadamente o SP de uma thread antes da primeira ativacao. Se o
   sintoma resultante for um salto para um endereco baixo (`0x00000000`, `0x14`), fica **provado** que o
   `PC=0x14` de P3 pertence a familia "ldm/pop com pilha invalida", e nao a familia "vetor de excecao tomado".

---

## 10.5 Coerencia de I-cache: o analogo exato do TB invalidate

### 5.1 O contrato: `flush_icache_range`

[FATO VERIFICADO] `arch/arm/include/asm/cacheflush.h:270-274`:

```c
/*
 * Perform necessary cache operations to ensure that data previously
 * stored within this range of addresses can be executed by the CPU.
 */
#define flush_icache_range(s,e)		__cpuc_coherent_kern_range(s,e)
```
e `:263-268`:
```c
/*
 * flush_icache_user_range is used when we want to ensure that the
 * Harvard caches are synchronised for the user space address range.
 * This is used for the ARM private sys_cacheflush system call.
 */
#define flush_icache_user_range(s,e)	__cpuc_coherent_user_range(s,e)
```

A implementacao ARMv6 e' `v6_coherent_user_range` (`arch/arm/mm/cache-v6.S:138-159`):

```
SYM_TYPED_FUNC_START(v6_coherent_user_range)
	bic	r0, r0, #CACHE_LINE_SIZE - 1
1:
 USER(	mcr	p15, 0, r0, c7, c10, 1	)	@ clean D line
	add	r0, r0, #CACHE_LINE_SIZE
	cmp	r0, r1
	blo	1b
	mov	r0, #0
	mcr	p15, 0, r0, c7, c10, 4		@ drain write buffer
	mcr	p15, 0, r0, c7, c5, 0		@ I+BTB cache invalidate
	ret	lr
```

[FATO VERIFICADO] A sequencia e' **tres passos, nesta ordem**, e cada um e' necessario:
1. **clean D-line por linha** (`c7,c10,1`) — o dado escrito ainda pode estar so no D-cache;
2. **drain write buffer / DSB** (`c7,c10,4`) — garante que o clean chegou a memoria;
3. **invalidate I-cache + BTB** (`c7,c5,0`) — descarta a instrucao antiga **e a predicao de branch**.

O `bic r0, r0, #CACHE_LINE_SIZE - 1` no inicio: o range e' **alinhado para baixo** ate a linha de cache.
[FATO VERIFICADO] Nao se invalida "o endereco"; invalida-se **a granularidade real do hardware**.

Ha ainda `9001:` (`cache-v6.S:165-167`) — um fixup que retorna `-EFAULT` se o endereco nao estiver mapeado.

### 5.2 Onde o kernel realmente chama isso (quatro casos, todos relevantes)

[FATO VERIFICADO]

1. **Depois de copiar os vetores** — `arch/arm/kernel/traps.c:867-895` (`early_trap_init`):
   ```c
	copy_from_lma(vectors_base, __vectors_start, __vectors_end);
	copy_from_lma(vectors_base + 0x1000, __stubs_start, __stubs_end);
	kuser_init(vectors_base);
	flush_vectors(vectors_base, 0, PAGE_SIZE * 2);
   ```
   com `flush_vectors` -> `flush_icache_range` (`traps.c:822-828`).
2. **Depois de escrever o trampolim de sigreturn na pilha** — `arch/arm/kernel/signal.c:424-431`:
   ```c
			/* Ensure that the instruction cache sees the return code
			   written onto the stack. */
			flush_icache_range((unsigned long)rc, (unsigned long)(rc + 3));
			retcode = ((unsigned long)rc) + thumb;
   ```
   Note `+ thumb`: o endereco de retorno carrega o LSB de interworking.
3. **Depois de copiar a instrucao para o slot do kprobe** — `arch/arm/probes/kprobes/core.c:97-98`:
   `flush_insns(p->ainsn.insn, sizeof(p->ainsn.insn[0]) * MAX_INSN_SIZE);`
4. **Re-patch de vetores em runtime (Spectre-BHB)** — `traps.c:831-864`, `spectre_bhb_update_vectors()`:
   `copy_from_lma(vectors_page, vec_start, vec_end); flush_vectors(...)`.
   Este e' o caso mais parecido com o zeebo-lle: **codigo ja executado sendo reescrito em runtime**, com flush
   imediatamente a seguir e recusa explicita se for tarde demais (`if (system_state >= SYSTEM_FREEING_INITMEM)
   return SPECTRE_VULNERABLE;`).

Ha tambem o `sys_cacheflush` do proprio ARM, `traps.c:570-605` (`__do_cache_op`), que processa em pedacos de
`PAGE_SIZE` com `cond_resched()` entre eles — reconhecimento de que flush de range grande e' caro.

### 5.3 Por que isso e' o analogo do TB invalidate

[INFERENCIA, mas direta] Num emulador TCG/Unicorn, um *translation block* e' uma copia traduzida de codigo
hospede. Ele desempenha o papel do I-cache: uma copia potencialmente stale da instrucao. Portanto:

| Hardware ARM | Unicorn/TCG |
|---|---|
| I-cache line stale apos escrita | TB stale apos escrita em memoria de codigo |
| `mcr p15,0,r0,c7,c5,0` (ICIALLU) | `uc_ctl_remove_cache(uc, lo, hi)` |
| BTB/BPIALL (`c7,c5,6`) | invalidacao do chaining de TB |
| alinhar range a linha de cache | invalidar **o bloco inteiro**, nao o endereco exato |
| DSB antes do invalidate | garantir que a escrita ja esta visivel antes de remover o TB |

A regra do Linux e' **invalidar no ponto da escrita, nao no ponto do uso**, e sempre por *range derivado do que
foi escrito*.

### APLICACAO NO ZEEBO-LLE (P5)

Hoje o emulador chama `uc_ctl_remove_cache` em **enderecos hardcoded**. Amostra real, todos em
`tools/cpp/zeebo_lle_main.cpp`:

```
:3479  uc_ctl_remove_cache(uc, 0xb000c720, 0x100);
:3480  uc_ctl_remove_cache(uc, 0xb00033d0, 0x100);
:3481  uc_ctl_remove_cache(uc, 0x103dcd14, 0x100);
:3493  uc_ctl_remove_cache(uc, 0xb000c800, 0x100);
:3526  uc_ctl_remove_cache(uc, 0xb000c758, 0x40);
:3540  uc_ctl_remove_cache(uc, 0xb000c930, 0x40);
:3551  uc_ctl_remove_cache(uc, 0xb000c798, 0x24);
:3562  uc_ctl_remove_cache(uc, 0xb000c944, 0x20);
:3578  uc_ctl_remove_cache(uc, 0xb000c7b8, 0x14);
:3591  uc_ctl_remove_cache(uc, 0xb000c7cc, 0x34);
```

Problema estrutural: esses enderecos sao dos **stubs**, nao da **escrita**. Quando o estado reescrito for de
outro stub (uma copia local, como a de `ig_naming` documentada em `:3509-3513`), a invalidacao nao cobre.

Reimplementacao (modelo: "invalide o que voce escreveu, arredondado para a granularidade do hardware"):

1. Centralizar em uma funcao unica:
   ```cpp
   // modelo conceitual: v6_coherent_user_range (cache-v6.S:138-159)
   static void lle_icache_invalidate(uc_engine* uc, u32 start, u32 len) {
       constexpr u32 LINE = 0x40;                 // granularidade escolhida
       u32 lo = start & ~(LINE - 1);
       u32 hi = (start + len + LINE - 1) & ~(LINE - 1);
       uc_ctl_remove_cache(uc, lo, hi);           // range, nao (base,size)
   }
   ```
   **Atencao a assinatura real:** `uc_ctl_remove_cache(uc, address, end)` recebe **inicio e fim**, nao
   inicio e tamanho. [NAO VERIFICADO neste trabalho — nao li o header do Unicorn 2.x aqui; conferir em
   `unicorn/unicorn.h` antes de aplicar. Se for `(address, end)`, as chamadas atuais com `0x100`/`0x40` estao
   invalidando `[0xb000c720, 0x100)` = um range invertido/vazio, o que seria um bug latente grave.]
2. Chamar `lle_icache_invalidate` **sempre que o emulador escrever PC/SP/CPSR de um contexto**, com range
   derivado do PC escrito (`pc-8, 64` cobre o bloco atual e o anterior), em vez de listas fixas.
3. Chamar tambem quando uma syscall de MMU (`L4_MapControl`, 0x14) mudar o mapeamento — porque um TB traduzido
   a partir do mapeamento antigo e' stale exatamente como uma I-cache line apos troca de pagina.
   (O Linux faz o analogo: `flush_context()` em `arch/arm/mm/context.c:161-162` chama `__flush_icache_all()`
   quando `icache_is_vivt_asid_tagged()`.)

### TESTE QUE PODE FALHAR (P5)

1. Confirmar a assinatura de `uc_ctl_remove_cache` no header instalado. **Criterio de falha imediata:** se for
   `(address, end)`, toda chamada atual do tipo `uc_ctl_remove_cache(uc, 0xb000c720, 0x100)` esta errada
   (`end < address`) e nunca invalidou nada — o que significaria que o boot atual funciona *apesar* das
   chamadas, e que P5 tem uma causa diferente da suposta.
2. Segundo teste: comentar **todas** as chamadas `uc_ctl_remove_cache` e rodar o boot.
   - Se o boot **nao mudar**, as invalidacoes hardcoded sao inuteis (hipotese 1 confirmada ou TBs ja
     invalidados por outro mecanismo).
   - Se o boot regredir, elas sao necessarias — e entao substituir por `lle_icache_invalidate(pc-8, 64)`
     generico deve reproduzir o comportamento **sem** nenhum endereco hardcoded. Se o generico nao reproduzir,
     ha um caminho de escrita de codigo que nao passa pelo epilogo de syscall, e ele precisa ser encontrado.

---

## 10.6 MMU: ASID, troca de espaco de enderecos e invalidacao seletiva de TLB

### 6.1 O ContextID ARMv6: ASID nos 8 bits baixos

[FATO VERIFICADO] `arch/arm/mm/context.c:22-38`:

```
 * On ARMv6, we have the following structure in the Context ID:
 *
 * 31                         7          0
 * +-------------------------+-----------+
 * |      process ID         |   ASID    |
 * +-------------------------+-----------+
 * |              context ID             |
 * +-------------------------------------+
 *
 * The ASID is used to tag entries in the CPU caches and TLBs.
 * The context ID is used by debuggers and trace logic, and
 * should be unique within all running processes.
```

### 6.2 A troca de espaco: `cpu_v6_switch_mm` (o CPU do Zeebo e' ARM1136 = ARMv6)

[FATO VERIFICADO] `arch/arm/mm/proc-v6.S:103-121`:

```
SYM_TYPED_FUNC_START(cpu_v6_switch_mm)
#ifdef CONFIG_MMU
	mov	r2, #0
	mmid	r1, r1				@ get mm->context.id
	ALT_SMP(orr	r0, r0, #TTB_FLAGS_SMP)
	ALT_UP(orr	r0, r0, #TTB_FLAGS_UP)
	mcr	p15, 0, r2, c7, c5, 6		@ flush BTAC/BTB
	mcr	p15, 0, r2, c7, c10, 4		@ drain write buffer
	mcr	p15, 0, r0, c2, c0, 0		@ set TTB 0
	...
	mcr	p15, 0, r1, c13, c0, 1		@ set context ID
#endif
	ret	lr
```

Ordem em ARMv6: **flush BTB -> drain WB -> TTBR0 -> ContextID**.
Compare com ARMv7 (`arch/arm/mm/proc-v7-2level.S:43-62`), que inverte e adiciona barreiras:

```
	mcr	p15, 0, r1, c13, c0, 1		@ set context ID
	isb
	mcr	p15, 0, r0, c2, c0, 0		@ set TTB 0
	isb
```

[FATO VERIFICADO] Duas arquiteturas, duas ordens, ambas com barreiras explicitas. A ordem **nao** e'
arbitraria. [INFERENCIA] Um emulador que troca espaco de enderecos precisa escolher uma ordem e invalidar
traducoes entre os dois passos, porque no intervalo o par (TTBR, ASID) e' inconsistente.

### 6.3 O problema do intervalo inconsistente e a solucao do Linux

[FATO VERIFICADO] `arch/arm/mm/context.c:237-276` (`check_and_switch_context`) e `:85-98`:

```c
	/*
	 * We cannot update the pgd and the ASID atomicly with classic
	 * MMU, so switch exclusively to global mappings to avoid
	 * speculative page table walking with the wrong TTBR.
	 */
	cpu_set_reserved_ttbr0();
```
com
```c
static void cpu_set_reserved_ttbr0(void)
{
	u32 ttb;
	/* Copy TTBR1 into TTBR0. This points at swapper_pg_dir, which contains
	   only global entries so any speculative walks are perfectly safe. */
	asm volatile(
	"	mrc	p15, 0, %0, c2, c0, 1		@ read TTBR1\n"
	"	mcr	p15, 0, %0, c2, c0, 0		@ set TTBR0\n"
	: "=r" (ttb));
	isb();
}
```

Padrao: **o intervalo inconsistente e' preenchido com um estado seguro conhecido** (tabela so com mapeamentos
globais), nao deixado indefinido. E o comentario em `:218-221` explica por que ASID 0 e' reservado:
"We always count from ASID #1, as we reserve ASID #0 to switch via TTBR0 and to avoid speculative page table
walks from hitting in any partial walk caches". [FATO VERIFICADO]

### 6.4 Rollover de ASID

[FATO VERIFICADO] `arch/arm/mm/context.c:223-234` (`new_context`):

```c
	asid = find_next_zero_bit(asid_map, NUM_USER_ASIDS, cur_idx);
	if (asid == NUM_USER_ASIDS) {
		generation = atomic64_add_return(ASID_FIRST_VERSION, &asid_generation);
		flush_context(cpu);
		asid = find_next_zero_bit(asid_map, NUM_USER_ASIDS, 1);
	}
```

e `flush_context` (`:136-163`):
```c
	bitmap_clear(asid_map, 0, NUM_USER_ASIDS);
	... preserva reserved_asids por CPU ...
	cpumask_setall(&tlb_flush_pending);
	if (icache_is_vivt_asid_tagged())
		__flush_icache_all();
```
com o consumidor em `check_and_switch_context` (`:265-268`):
```c
	if (cpumask_test_and_clear_cpu(cpu, &tlb_flush_pending)) {
		local_flush_bp_all();
		local_flush_tlb_all();
	}
```

[FATO VERIFICADO] Estrutura: ASID e' um numero **de 8 bits** que acaba; quando acaba, sobe a *geracao*, marca
TLB flush pendente para todas as CPUs, e o flush acontece preguicosamente na proxima troca de contexto de cada
CPU. E — ponto crucial para emulacao — **o I-cache tambem e' invalidado** quando ele e' VIVT tagueado por ASID.

### 6.5 Invalidacao seletiva por ASID e por MVA

[FATO VERIFICADO] `arch/arm/include/asm/tlbflush.h:360-377`:

```c
static inline void __local_flush_tlb_mm(struct mm_struct *mm)
{
	const int asid = ASID(mm);
	...
	tlb_op(TLB_V6_U_ASID, "c8, c7, 2", asid);
	tlb_op(TLB_V6_D_ASID, "c8, c6, 2", asid);
	tlb_op(TLB_V6_I_ASID, "c8, c5, 2", asid);
}
```

E por endereco, `arch/arm/mm/tlb-v6.S` (`v6wbi_flush_user_tlb_range`):

```
	mcr	p15, 0, ip, c7, c10, 4		@ drain write buffer
	mov	r0, r0, lsr #PAGE_SHIFT		@ align address
	asid	r3, r3				@ mask ASID
	orr	r0, r3, r0, lsl #PAGE_SHIFT	@ Create initial MVA
	...
1:
	mcr	p15, 0, r0, c8, c6, 1		@ TLB invalidate D MVA
	tst	r2, #VM_EXEC			@ Executable area ?
	mcrne	p15, 0, r0, c8, c5, 1		@ TLB invalidate I MVA
	add	r0, r0, #PAGE_SZ
	cmp	r0, r1
	blo	1b
	mcr	p15, 0, ip, c7, c10, 4		@ data synchronization barrier
	ret	lr
```

[FATO VERIFICADO] O "MVA" passado ao `mcr` e' **endereco alinhado a pagina OR ASID nos bits baixos** —
`orr r0, r3, r0, lsl #PAGE_SHIFT`. E o I-TLB so e' invalidado se a VMA for executavel (`tst r2, #VM_EXEC`).
E ha `dsb` antes e depois. `v6wbi_flush_kern_tlb_range` ainda acrescenta `mcr p15,0,r2,c7,c5,4` (prefetch
flush = ISB) no fim, porque mudou mapeamento de kernel.

### APLICACAO NO ZEEBO-LLE (P6)

`L4_MapControl` e' a syscall 0x14, tratada em `zeebo_lle_main.cpp:3527` em diante. O padrao Linux sugere:

1. **Modelar o espaco de enderecos L4 como um par (tabela, asid)**, e nao so como tabela. Mesmo que o Unicorn
   use um unico espaco plano, manter o ASID logico permite (a) invalidar seletivamente e (b) detectar quando
   duas threads de espacos diferentes compartilham um TB — que e' um bug real de emulacao.
2. **Ordem fixa e documentada na troca de espaco**, com "estado seguro" no meio, modelando
   `cpu_set_reserved_ttbr0`: ao trocar de espaco, primeiro apontar para um mapeamento so-global/seguro,
   depois trocar o ASID, depois o mapeamento novo. Isso evita o analogo do "speculative walk com TTBR errado":
   no emulador, um TB traduzido no meio da troca.
3. **Invalidacao seletiva no `L4_MapControl`**: invalidar TBs apenas no range `[vaddr, vaddr+size)` da pagina
   mapeada/desmapeada, arredondado ao page-size (o KIP do OKL4 expoe a page-size mask — modelo: o kernel usa
   `PAGE_SHIFT`/`PAGE_MASK`, `tlb-v6.S`). Nao invalidar tudo; nao invalidar enderecos fixos.
4. **Rollover**: se o emulador mantiver um cache de traducao por espaco, precisa de um analogo de
   `asid_generation` — quando o espaco e' destruido e o id reciclado, TODOS os TBs daquele id precisam morrer
   (modelo: `flush_context()` + `tlb_flush_pending`). Reutilizar um id sem invalidar e' o bug classico.

### TESTE QUE PODE FALHAR (P6)

1. Instrumentar `L4_MapControl` para logar `(space_id, vaddr, size, perms, old_mapping_existia?)`.
2. **Controle negativo forte:** criar um teste que mapeia um endereco V com codigo A, executa-o (forcando
   traducao de TB), **remapeia** V para codigo B via `L4_MapControl`, e executa V de novo.
   - **Criterio de falha:** se o emulador executar A na segunda vez, a invalidacao pos-`MapControl` nao existe
     ou nao cobre. Esse teste e' inteiramente sintetico e nao depende do boot do Zeebo.
3. Terceiro teste: dois "espacos" que mapeiam o **mesmo** vaddr para conteudos diferentes, alternando via
   handoff de thread. Se o segundo espaco executar o codigo do primeiro, o modelo de ASID esta faltando.

---

## 10.7 A tabela de vetores ARM, `0xffff0000` vs `0x00000000`, e o que significa `PC=0x14`

### 7.1 Layout canonico dos vetores ARM (PL1)

[FATO VERIFICADO — ordem e nomes lidos diretamente de `arch/arm/kernel/entry-armv.S:1073-1084`]

| Offset | Vetor | Simbolo Linux | Modo de entrada |
|---|---|---|---|
| `0x00` | Reset | `vector_rst` | SVC |
| `0x04` | Undefined Instruction | `vector_und` | UND |
| `0x08` | Supervisor Call (SVC/SWI) | `ldr pc, .L__vector_swi` -> `vector_swi` | SVC |
| `0x0c` | Prefetch Abort | `vector_pabt` | ABT |
| `0x10` | Data Abort | `vector_dabt` | ABT |
| `0x14` | **Reservado / Address Exception (legado 26-bit) / Hyp Trap** | `vector_addrexcptn` | — |
| `0x18` | IRQ | `vector_irq` | IRQ |
| `0x1c` | FIQ | `vector_fiq` | FIQ |

O modo de entrada de cada stub vem da macro `vector_stub` (`entry-armv.S:853, 944, 967, 990, 1013, 1052`):
`vector_stub irq, IRQ_MODE, 4` / `dabt, ABT_MODE, 8` / `pabt, ABT_MODE, 4` / `und, UND_MODE` (correcao 0) /
`fiq, FIQ_MODE, 4`. [FATO VERIFICADO]

Note as **correcoes de LR** no terceiro argumento (`sub lr, lr, #\correction`, `entry-armv.S:863-865`):
IRQ e' `-4`, Data Abort e' `-8`, Prefetch Abort e' `-4`, Undef e' `0`. Isso e' o hardware ARM colocando em LR
o PC "adiantado" pelo pipeline, e o software corrigindo por um valor **especifico de cada excecao**.
[FATO VERIFICADO]

### 7.2 O que o Linux poe em 0x14

[FATO VERIFICADO] `arch/arm/kernel/entry-armv.S:1034-1042`:

```
/*=============================================================================
 * Address exception handler
 *-----------------------------------------------------------------------------
 * These aren't too critical.
 * (they're not supposed to happen, and won't happen in 32-bit data mode).
 */

vector_addrexcptn:
	b	vector_addrexcptn
```

**Um loop infinito.** O Linux nao tenta tratar: "they're not supposed to happen".

[FATO VERIFICADO] Em contrapartida, na tabela de vetores **Hyp** do proprio Linux ARM
(`arch/arm/kernel/hyp-stub.S:231-240`):

```
ENTRY(__hyp_stub_vectors)
__hyp_stub_reset:	W(b)	.
__hyp_stub_und:		W(b)	.
__hyp_stub_svc:		W(b)	.
__hyp_stub_pabort:	W(b)	.
__hyp_stub_dabort:	W(b)	.
__hyp_stub_trap:	W(b)	__hyp_stub_do_trap     <-- offset 0x14
__hyp_stub_irq:		W(b)	.
__hyp_stub_fiq:		W(b)	.
```

Contando: reset=0x00, und=0x04, svc=0x08, pabort=0x0c, dabort=0x10, **trap=0x14**, irq=0x18, fiq=0x1c.
Esta e' a evidencia direta de que, na tabela apontada por HVBAR (ARMv7-A com Virtualization Extensions),
o slot `0x14` e' o **Hyp Trap Entry**. [FATO VERIFICADO — lido do fonte, nao da ARM ARM]

### 7.3 Alto ou baixo: `0xffff0000` vs `0x00000000`

[FATO VERIFICADO] `arch/arm/include/asm/cp15.h:23` e `:46`:
```c
#define CR_V	(1 << 13)	/* Vectors relocated to 0xffff0000	*/
...
#define vectors_high()	(get_cr() & CR_V)
```
e `arch/arm/mm/mmu.c:725`:
```c
#define vectors_base()	(vectors_high() ? 0xffff0000 : 0)
```

O mapeamento, `arch/arm/mm/mmu.c:1423-1450`:
```c
	/*
	 * Create a mapping for the machine vectors at the high-vectors
	 * location (0xffff0000).  If we aren't using high-vectors, also
	 * create a mapping at the low-vectors virtual address.
	 */
	map.pfn = __phys_to_pfn(virt_to_phys(vectors));
	map.virtual = 0xffff0000;
	map.length = PAGE_SIZE;
	...
	create_mapping(&map);

	if (!vectors_high()) {
		map.virtual = 0;
		map.length = PAGE_SIZE * 2;
		map.type = MT_LOW_VECTORS;
		create_mapping(&map);
	}
```

[FATO VERIFICADO] Consequencia pratica enorme: **com vetores altos (`SCTLR.V=1`), o endereco virtual 0 nao e'
mapeado.** Qualquer salto para `0x00`..`0x1c` vira Prefetch Abort e o kernel mata o processo. Com vetores
baixos, `0x00000000` e' codigo executavel de verdade.

### 7.4 O envenenamento da pagina de vetores — um controle negativo dentro do kernel

[FATO VERIFICADO] `arch/arm/kernel/traps.c:875-882`:

```c
	/*
	 * Poison the vectors page with an undefined instruction.  This
	 * instruction is chosen to be undefined for both ARM and Thumb
	 * ISAs.  The Thumb version is an undefined instruction with a
	 * branch back to the undefined instruction.
	 */
	for (i = 0; i < PAGE_SIZE / sizeof(u32); i++)
		((u32 *)vectors_base)[i] = 0xe7fddef1;
```

`0xe7fddef1` e' escolhido para ser **undefined tanto em ARM quanto em Thumb**. Em Thumb, `0xdef1` e' undefined
e `0xe7fd` e' um branch de volta para ele. [FATO VERIFICADO — o comentario afirma isso explicitamente]

Isso e' exatamente o tipo de armadilha que o zeebo-lle precisa (secao 8.4).

### 7.5 O que um salto para `PC=0x14` significa num sistema real

Analise, combinando os fatos acima:

- **Num sistema com vetores altos**: `PC=0x14` e' um salto para memoria nao mapeada -> Prefetch Abort.
  Ele **nunca** e' "o vetor reservado sendo tomado", porque o vetor esta em `0xffff0014`. [INFERENCIA forte]
- **Num sistema com vetores baixos** e tabela ARM padrao: `0x14` e' o slot reservado. Tomar esse vetor exigiria
  uma "Address Exception", que so existe em modo 26-bit legado ("won't happen in 32-bit data mode",
  `entry-armv.S:1038`). Em ARMv6/ARMv7 32-bit, **o hardware nao gera essa excecao**. [INFERENCIA forte,
  ancorada no comentario do kernel]
- Logo: **`PC=0x14` quase certamente nao e' uma excecao. E' um salto indireto com valor errado.**

Candidatos concretos, em ordem de probabilidade [INFERENCIA]:
1. `ldm`/`pop` com PC na lista lendo de uma pilha invalida — e' literalmente o mecanismo que o proprio projeto
   ja documentou em `zeebo_lle_main.cpp:3502-3504` para o caso `PC=0x00000000`. Um offset de 5 words
   (`0x14 = 5*4`) dentro de uma pilha de zeros produz `0x14` se o valor lido for `0x14`; mais provavel ainda:
   `sp` aponta para uma estrutura onde o campo em offset 0x14 e' lido como PC.
2. Salto via ponteiro de funcao nulo somado a um offset de vtable/jump-table de `0x14` (`ldr pc, [r0, #0x14]`
   com `r0 == 0`, ou `add pc, r0, #0x14`).
3. UTCB/KIP nulo: o OKL4 acessa campos do UTCB por offset. Um UTCB nulo + offset 0x14 gera tanto leitura de
   `0x14` quanto salto para `0x14`.
4. Tabela de vetores/handlers do proprio OKL4 indexada errada.

### APLICACAO NO ZEEBO-LLE (P3, P1)

1. **Mapear a pagina 0 e envenena-la.** Escrever `0xe7fddef1` (ou outro padrao undefined-em-ambas-ISAs) em
   `0x00000000..0x00001000` e instalar um hook de execucao nessa faixa. Modelo: `traps.c:881-882`.
   Assim, um salto para `0x14` deixa de ser "deriva silenciosa" e vira um evento com timestamp e contexto.
2. **Instalar um `UC_HOOK_MEM_FETCH_UNMAPPED` / hook de codigo em `[0x0, 0x100)`** que dumpe: TID atual, SP,
   os 16 words no topo da pilha, LR, e o PC anterior. Isso distingue as quatro hipoteses acima em uma unica
   execucao.
3. **Nao "tratar" `PC=0x14`**. Modelo `vector_addrexcptn`: o Linux faz `b .` — um estado terminal visivel.
   O emulador deve abortar com diagnostico, nao continuar.

### TESTE QUE PODE FALHAR

1. Envenenar a pagina 0 com `0xe7fddef1` e rodar o boot.
   - **Criterio de falha (esperado):** o emulador para em `0x14` com o dump completo. Se o boot **continuar**
     normalmente, entao alguma coisa legitima le/executa na pagina 0 e a premissa "pagina 0 nao e' usada" esta
     errada — descoberta valiosa por si so.
2. Segundo teste, discriminante: logar `sp` no instante do salto para `0x14`. Se `(sp & 0xfff) == 0` ou `sp`
   estiver fora de toda regiao mapeada, hipotese 1 (pilha invalida) confirmada. Se `sp` for valido e a memoria
   em `[sp, sp+0x40)` nao contiver `0x14`, hipotese 1 refutada e a investigacao migra para ponteiro de funcao
   (hipoteses 2-4).
3. Terceiro teste: comparar com o caso conhecido `PC=0x00000000` de `:3504`. Se os dois sintomas ocorrerem no
   mesmo stub com SPs diferentes, esta provado que sao a mesma familia de bug (pilha), nao dois bugs.

---

## 10.8 Tecnicas de robustez reaproveitaveis

### 8.1 `undef_hook`: um despachante por (opcode, CPSR)

[FATO VERIFICADO] `arch/arm/kernel/traps.c:440-450`:

```c
	list_for_each_entry(hook, &undef_hook, node)
		if ((instr & hook->instr_mask) == hook->instr_val &&
		    (regs->ARM_cpsr & hook->cpsr_mask) == hook->cpsr_val)
			fn = hook->fn;
	...
	return fn ? fn(regs, instr) : 1;
```

Uso real, `arch/arm/probes/kprobes/core.c:437-461`:
```c
static struct undef_hook kprobes_thumb16_break_hook = {
	.instr_mask	= 0xffff,
	.instr_val	= KPROBE_THUMB16_BREAKPOINT_INSTRUCTION,
	.cpsr_mask	= MODE_MASK,
	.cpsr_val	= SVC_MODE,
	.fn		= kprobe_trap_handler,
};
```

[FATO VERIFICADO] O ponto elegante: **o filtro inclui o CPSR**, nao so o opcode. Um mesmo opcode pode ser
tratado de formas diferentes conforme modo/ISA. Em `arch/arm/kernel/ptrace.c:215-232` ha tres hooks de
breakpoint filtrados por `.cpsr_mask = PSR_T_BIT` com `.cpsr_val` 0 ou `PSR_T_BIT` — literalmente
"este opcode, mas so em Thumb". [FATO VERIFICADO]

### 8.2 kprobes: reentrada segura por maquina de estados

[FATO VERIFICADO] `arch/arm/probes/kprobes/core.c:267-289`:

```c
		} else if (cur) {
			/* Kprobe is pending, so we're recursing. */
			switch (kcb->kprobe_status) {
			case KPROBE_HIT_ACTIVE:
			case KPROBE_HIT_SSDONE:
			case KPROBE_HIT_SS:
				kprobes_inc_nmissed_count(p);
				save_previous_kprobe(kcb);
				set_current_kprobe(p);
				kcb->kprobe_status = KPROBE_REENTER;
				singlestep(p, regs, kcb);
				restore_previous_kprobe(kcb);
				break;
			case KPROBE_REENTER:
				/* A nested probe was hit in FIQ, it is a BUG */
				pr_warn("Failed to recover from reentered kprobes.\n");
				dump_kprobe(p);
				fallthrough;
			default:
				BUG();
			}
		}
```

E o comentario de entrada (`:229-235`): "Called with IRQs disabled. IRQs must remain disabled ... The current
kprobes implementation cannot process more than one nested level of kprobe". [FATO VERIFICADO]

Padrao: **um nivel de reentrada, explicito, com save/restore de estado anterior, e `BUG()` no nivel seguinte.**
Nao ha tentativa de suportar profundidade arbitraria.

Complemento em `:331-346` (`kprobe_fault_handler`): se o single-step causou fault, `regs->ARM_pc = (long)cur->addr;`
— **restaura o PC para a instrucao original** e deixa o fault handler normal agir.

### 8.3 `valid_user_regs`: sanitizar CPSR vindo de fora

[FATO VERIFICADO] `arch/arm/include/asm/ptrace.h:58-86`:

```c
static inline int valid_user_regs(struct pt_regs *regs)
{
	unsigned long mode = regs->ARM_cpsr & MODE_MASK;

	/* Always clear the F (FIQ) and A (delayed abort) bits */
	regs->ARM_cpsr &= ~(PSR_F_BIT | PSR_A_BIT);

	if ((regs->ARM_cpsr & PSR_I_BIT) == 0) {
		if (mode == USR_MODE) return 1;
		if (elf_hwcap & HWCAP_26BIT && mode == USR26_MODE) return 1;
	}

	/* Force CPSR to something logical... */
	regs->ARM_cpsr &= PSR_f | PSR_s | PSR_x | PSR_T_BIT | MODE32_BIT;
	if (!(elf_hwcap & HWCAP_26BIT))
		regs->ARM_cpsr |= USR_MODE;

	return 0;
}
```

Chamado em `arch/arm/kernel/ptrace.c:181` e `:565` antes de aceitar registradores vindos de um debugger.
[FATO VERIFICADO] Note que a funcao **corrige e retorna 0** em vez de so rejeitar — e o T-bit e' um dos poucos
bits preservados na mascara de saneamento.

### 8.4 Endereco de retorno de sinal: LSB de ISA e flush combinados

[FATO VERIFICADO] `arch/arm/kernel/signal.c:410-431` ja citado na secao 5.2. O detalhe:
`retcode = mm->context.sigpage + signal_return_offset + (idx << 2) + thumb;` — o `+ thumb` e' o LSB de
interworking, e `idx = thumb << 1` seleciona **um trampolim diferente para ARM e para Thumb**
(`sigreturn_codes[]`). Nao ha um trampolim unico "esperto"; ha dois, escolhidos pelo T-bit. [FATO VERIFICADO]

### APLICACAO NO ZEEBO-LLE

1. **Despachante de syscall por (opcode, CPSR), nao por faixa de PC.** Modelo `undef_hook`. Hoje
   `zeebo_lle_main.cpp:3059` e `:3072-3074` documentam que "a identidade da syscall vem do SP, não do imediato
   — que é sempre 0x14" e que a decodificacao e' por `sp & 0xFF`. Isso e' fragil da mesma forma que a
   heuristica de faixa. Adicionar o CPSR (modo + T) como parte da chave de despacho torna cada decisao
   verificavel.
2. **Maquina de estados de reentrada** para syscalls que podem recursar (IPC que acorda thread que faz IPC).
   Modelo kprobes: `NONE -> ACTIVE -> REENTER -> abort`. Um nivel, explicito, e aborta no segundo.
3. **`lle_valid_ctx()`** aplicado a todo CPSR que o emulador escreve num contexto: forcar modo valido, limpar
   bits impossiveis, **preservar T**, e logar quando corrigir. Modelo `valid_user_regs`.
4. **Envenenar memoria nao usada** com padrao undefined-em-ambas-ISAs (`0xe7fddef1`), nao com zeros.
   Zeros sao `andeq r0,r0,r0` em ARM (um NOP efetivo) e `movs r0,r0` em Thumb — executaveis, portanto
   silenciosos. Modelo `traps.c:881`.

### TESTE QUE PODE FALHAR

1. Preencher com `0xe7fddef1` toda a RAM emulada **antes** do carregamento das imagens. Rodar o boot.
   - **Criterio de falha:** se o boot regredir, existe codigo que depende de memoria zerada (BSS nao
     inicializado, por exemplo) — e isso e' um bug de carregamento de imagem, nao do envenenamento.
   - Se o boot nao mudar, o emulador ganhou deteccao de "execucao em memoria nao inicializada" de graca.
2. Implementar `lle_valid_ctx()` em modo **somente-log** (nao corrige, so reporta) por uma execucao inteira.
   Se ele reportar zero correcoes, o modelo de CPSR do emulador esta consistente. Se reportar, cada report e'
   um bug localizado com PC e TID.

---

## 10.9 arm64: o contraste que confirma a regra (ERET e a morte da heuristica)

### 9.1 `kernel_exit`: PC e PSTATE em registradores de sistema, retorno com `eret`

[FATO VERIFICADO] `arch/arm64/kernel/entry.S:412-413` e `:461-462`, macro `kernel_exit`:

```
	msr	elr_el1, x21			// set up the return data
	msr	spsr_el1, x22
	ldp	x0, x1, [sp, #16 * 0]
	...
	ldr	lr, [sp, #S_LR]
	add	sp, sp, #PT_REGS_SIZE		// restore sp
	...
	eret
	sb
```

[FATO VERIFICADO] Em arm64 o estado de retorno mora em **dois registradores de sistema dedicados**:
`ELR_EL1` (endereco) e `SPSR_EL1` (PSTATE completo, incluindo nivel de excecao, estado de execucao AArch32/64,
e — para compat 32-bit — o bit T). `eret` aplica os dois de uma vez. O `sb` (speculation barrier) depois do
`eret` e' mitigacao de especulacao.

[INFERENCIA] A arquitetura arm64 **removeu a possibilidade** de fazer o que o zeebo-lle faz hoje: nao existe
"escrever PC e depois torcer para o modo estar certo". O par (ELR, SPSR) e' a unica interface.

### 9.2 arm64 tambem usa o T-bit para largura, mas so em compat

[FATO VERIFICADO] `arch/arm64/kernel/signal.c:1407`:
```c
		restart_addr = continue_addr - (compat_thumb_mode(regs) ? 2 : 4);
```
Identico a `arch/arm/kernel/signal.c:548`. A regra "largura = f(T-bit)" sobreviveu intacta a mudanca de
arquitetura. [FATO VERIFICADO]

### 9.3 Numero de syscall: registrador, nunca opcode

[FATO VERIFICADO] `arch/arm64/kernel/syscall.c:149-159`:
```c
void do_el0_svc(struct pt_regs *regs)
{
	el0_svc_common(regs, regs->regs[8], __NR_syscalls, sys_call_table);
}
#ifdef CONFIG_COMPAT
void do_el0_svc_compat(struct pt_regs *regs)
{
	el0_svc_common(regs, regs->regs[7], __NR_compat32_syscalls, compat_sys_call_table);
}
#endif
```
`x8` (nativo) ou `r7` (compat). **Nenhuma releitura de instrucao.** E `regs->orig_x0 = regs->regs[0];` em
`:78` — a mesma preservacao de `orig_r0` do ARM 32. [FATO VERIFICADO]

### APLICACAO NO ZEEBO-LLE

O OKL4 no Zeebo ja passa a identidade da syscall de um jeito nao-canonico: pelo SP
(`zeebo_lle_main.cpp:3072-3074`, "decodificar por `sp&0xFF`"). Isso e' fixo pelo ABI do OKL4, nao da para mudar.
Mas a **licao arm64 aplicavel** e': manter `(pc_de_retorno, cpsr_de_retorno)` como um **par explicito** no
contexto da thread, escrito e lido sempre junto, nunca separadamente. Em C++:

```cpp
struct RetState { u32 elr; u32 spsr; };   // modelo: ELR_EL1 + SPSR_EL1
// epilogo unico:
static void lle_eret(uc_engine* uc, const RetState& r) {
    u32 pc = (r.elr & ~1u) | ((r.spsr & 0x20u) ? 1u : 0u);  // T do SPSR, nao da faixa
    uc_reg_write(uc, UC_ARM_REG_CPSR, &r.spsr);
    uc_reg_write(uc, UC_ARM_REG_PC,   &pc);
}
```

Um unico ponto no codigo escreve PC de retorno. Qualquer ramo que precise de outro comportamento (handoff) usa
`SyscallOutcome::Switched` (secao 3) e **nao chama** `lle_eret`.

### TESTE QUE PODE FALHAR

1. `grep -c 'UC_ARM_REG_PC' tools/cpp/zeebo_lle_main.cpp` antes e depois da refatoracao.
   **Criterio de sucesso mensuravel:** o numero de escritas de PC no caminho de syscall cai para 1
   (`lle_eret`). Se nao cair, sobrou um caminho paralelo — e ele e' candidato a causa de P1/P2/P3.
2. Adicionar um assert em `lle_eret`: `assert((r.spsr & 0x1f) != 0)` — CPSR com modo zero significa que o
   contexto nunca foi inicializado. **Criterio de falha:** dispara exatamente nas threads de P4.

---

## 10.10 Resumo: tecnica -> problema

| # | Tecnica Linux | Simbolo / arquivo | Resolve |
|---|---|---|---|
| 1 | CPSR do chamador lido do SPSR, nunca adivinhado | `mrs saved_psr, spsr`, `entry-common.S:182` | P1 |
| 2 | Retorno atomico modo+T+PC | `movs pc, lr`, `entry-header.S:329/359`; `rfeia sp!`, `:243` | P1, P3 |
| 3 | Recusa explicita de retorno com PSR invalido | `bug "Returning to usermode..."`, `entry-header.S:330` | P1, P4 |
| 4 | Predicados derivados do CPSR salvo | `thumb_mode()`/`isa_mode()`, `ptrace.h:32-44` | P1 |
| 5 | Largura de SVC = f(T-bit) | `continue_addr - (thumb_mode(regs) ? 2 : 4)`, `signal.c:548` | P2 |
| 6 | `orig_r0` preservado para reinicio | `S_OLD_R0`, `entry-common.S:186`; `signal.c:563` | P2 |
| 7 | Largura Thumb por `is_wide_instruction` + compose 16+16 | `ptrace.h:122`; `traps.c:471-481` | P2 |
| 8 | Avanco de IT-state ao pular instrucao | `it_advance()`, `ptrace.h:180`; `core.c:213` | P2 |
| 9 | Duas convencoes separadas: SPSR (excecao) vs LSB/`bx` (chamada) | `badr`, `assembler.h:208`; `ret`, `:547` | P1 |
| 10 | Contexto de kernel (11 words, sem CPSR) != contexto de usuario (`pt_regs`, com CPSR) | `cpu_context_save`, `thread_info.h:45`; `pt_regs`, `ptrace.h:16` | P3, P4 |
| 11 | `ldmia r4, {r4-sl, fp, sp, pc}`: SP+PC numa instrucao | `__switch_to`, `entry-armv.S:553` | P3 |
| 12 | Thread nova comeca em trampolim sintetico | `ret_from_fork`, `entry-common.S:132`; `process.c:266` | P4 |
| 13 | `memset` do contexto + CPSR sempre explicito | `process.c:243/261/264`; `processor.h:61-77` | P4 |
| 14 | LSB de ISA consumido uma vez, na criacao | `regs->ARM_pc = pc & ~1`, `processor.h:77` | P1, P4 |
| 15 | TLS/UTCB trocado dentro do switch, restaurado na saida | `switch_tls`, `entry-armv.S:526`; `entry-header.S:300-305` | P4 |
| 16 | clean-D -> DSB -> invalidate-I+BTB, range alinhado a linha | `v6_coherent_user_range`, `cache-v6.S:138-159` | P5 |
| 17 | Invalidar no ponto da **escrita**, nao do uso | `traps.c:889-894`, `signal.c:428`, `core.c:97` | P5 |
| 18 | Re-patch de codigo vivo com flush imediato e recusa se tarde | `spectre_bhb_update_vectors`, `traps.c:831-864` | P5 |
| 19 | ASID de 8 bits + geracao + rollover preguicoso | `context.c:189-235`, `flush_context()` `:136` | P6 |
| 20 | Estado seguro durante o intervalo inconsistente | `cpu_set_reserved_ttbr0()`, `context.c:85-98` | P6 |
| 21 | TLB invalidado por MVA-com-ASID, I-TLB so se executavel | `tlb-v6.S`, `v6wbi_flush_user_tlb_range` | P6 |
| 22 | Ordem fixa TTBR/ContextID com barreiras | `proc-v6.S:103-121`; `proc-v7-2level.S:43-62` | P6 |
| 23 | Slot 0x14 = reservado (PL1) / Hyp Trap (HVBAR) | `entry-armv.S:1041`; `hyp-stub.S:237` | P3 |
| 24 | Vetor "impossivel" = loop infinito, nao tratamento | `vector_addrexcptn: b vector_addrexcptn` | P3 |
| 25 | Pagina de vetores envenenada com undef-em-ambas-ISAs | `0xe7fddef1`, `traps.c:882` | P3, P1 |
| 26 | Vetores altos deixam a pagina 0 **nao mapeada** | `vectors_base()`, `mmu.c:725/1438` | P3 |
| 27 | Despacho por (opcode & mask, CPSR & mask) | `call_undef_hook`, `traps.c:444-445` | P1 |
| 28 | Reentrada de um nivel so, com `BUG()` no segundo | `kprobe_handler`, `core.c:267-289` | P3 |
| 29 | Saneamento de CPSR vindo de fora, preservando T | `valid_user_regs()`, `ptrace.h:58-86` | P1, P4 |
| 30 | arm64: (ELR, SPSR) + `eret` — heuristica impossivel | `kernel_exit`, `entry.S:412-461` | P1, P2 |

---

## 10.11 Limites desta verificacao

- **NAO VERIFICADO:** a assinatura exata de `uc_ctl_remove_cache` no Unicorn 2.x instalado neste projeto
  (`(uc, address, end)` vs `(uc, address, size)`). Isso muda a leitura das chamadas em
  `zeebo_lle_main.cpp:3479-3603` e e' o primeiro item a conferir (secao 5, teste 1).
- **NAO VERIFICADO:** o texto da ARM Architecture Reference Manual sobre o slot 0x14. A afirmacao de que ele e'
  Hyp Trap vem da tabela de vetores do proprio Linux (`hyp-stub.S:231-240`), nao da ARM ARM.
- **NAO VERIFICADO:** o comportamento exato do hardware ARM1136EJ-S do Zeebo quanto a `SCTLR.V` (vetores altos
  ou baixos) no boot real do console. Isso decide se a pagina 0 e' mapeada e muda a interpretacao de `PC=0x14`.
  O arquivo `refs/msm7201a-mmu-map.txt` do proprio repositorio pode ter essa informacao; nao foi consultado
  neste trabalho.
- **NAO VERIFICADO:** se o OKL4 2.1.1 usa vetores altos ou baixos, e onde ele instala sua propria tabela.
  `refs/okl4-2.1.1-fix7` esta disponivel no repositorio e deve ser lido antes de aplicar a secao 7.
- **NAO VERIFICADO:** se algum SVC no caminho de boot do Zeebo esta dentro de um bloco IT. A secao 2.4 assume
  que e' possivel (AMSS/BREW e' Thumb-2), mas isso nao foi medido.
- **[INFERENCIA] e nao fato:** todas as afirmacoes sobre o que o hardware faz antes da primeira instrucao do
  handler (copiar CPSR->SPSR, LR = PC+offset, entrar com T=0). Elas sao consistentes com o codigo lido, mas o
  codigo nao as declara — ele as pressupoe.
- Versao: tudo acima e' `v6.12`. Trechos como `vector_swi` mudaram de forma entre versoes (o registro
  `saved_psr`/`saved_pc` com alias condicional em `entry-common.S:21-28` e' relativamente recente). Citacoes de
  linha nao valem para outras tags.

---

## 20. FreeBSD (sys/arm, sys/arm64), com contraste NetBSD/OpenBSD

Foco: como um kernel BSD real entra e sai de uma syscall em ARM, como ele decide a
largura da instrucao (ARM x Thumb), como ele ativa uma thread nova, e como ele invalida
MMU/TLB/cache ao trocar de espaco de enderecos. Tudo mapeado para P1..P6 do zeebo-lle.

### 20.0 Fontes lidas, versoes e licenca

[FATO VERIFICADO] Arquivos baixados e lidos localmente (copias em `/tmp/fbsd`, nao gravadas
no repositorio):

- FreeBSD, tag `release/13.2.0` de `freebsd/freebsd-src`:
  `sys/arm/arm/exception.S`, `sys/arm/arm/swtch.S`, `sys/arm/arm/swtch-v6.S`,
  `sys/arm/arm/syscall.c`, `sys/arm/arm/trap-v6.c`, `sys/arm/arm/vm_machdep.c`,
  `sys/arm/arm/pmap-v6.c`, `sys/arm/include/frame.h`, `sys/arm/include/pcb.h`,
  `sys/arm/include/armreg.h`, `sys/arm/include/cpu.h`, `sys/arm/include/cpu-v6.h`,
  `sys/kern/subr_syscall.c`, `sys/kern/subr_trap.c`,
  `sys/arm64/arm64/{exception.S,trap.c,swtch.S,vm_machdep.c,pmap.c}`.
- NetBSD, branch `trunk`: `sys/arch/arm/arm/syscall.c` (`$NetBSD: syscall.c,v 1.69
  2023/10/05 19:41:03 ad Exp $`), `sys/arch/arm/include/locore.h`,
  `sys/arch/arm/include/armreg.h`, `sys/arch/arm/arm32/vm_machdep.c`,
  `sys/arch/arm/arm32/cpuswitch.S`.
- OpenBSD, branch `master`: `sys/arch/arm64/arm64/syscall.c`
  (`$OpenBSD: syscall.c,v 1.20 2026/03/08 ...`).

[FATO VERIFICADO] Licencas, lidas nos cabecalhos dos proprios arquivos:

- FreeBSD moderno e majoritariamente BSD-2-Clause. Porem os arquivos ARM herdados do
  RiscBSD/NetBSD carregam BSD **4 clausulas** (com clausula de propaganda):
  `sys/arm/arm/exception.S`, `sys/arm/arm/syscall.c` e `sys/arm/include/frame.h` contem
  "All advertising materials mentioning features or use of this software must display the
  following acknowledgement" (Copyright 1994-1997 Mark Brinicombe / Brini).
- NetBSD `sys/arch/arm/arm/syscall.c`: BSD-2-Clause (The NetBSD Foundation).
- OpenBSD `sys/arch/arm64/arm64/syscall.c`: licenca ISC (Dale Rahn).

**Aviso de licenca para o zeebo-lle**: o *padrao arquitetural* descrito aqui (ordem de
salvamento de estado, derivacao de largura de instrucao a partir de PSR_T, trampolim de
primeira ativacao, invalidacao de TLB/BTB na troca de TTBR) pode ser reimplementado
livremente. **Codigo nao deve ser copiado literalmente**, nem mesmo trechos curtos de
`exception.S`/`syscall.c`, por causa da clausula de propaganda BSD-4 nesses arquivos
especificos. Reescreva em C++ proprio, sem transcrever comentarios nem sequencias de
instrucoes.

### 20.1 O trap de software real: a SPSR e a unica fonte de verdade

[FATO VERIFICADO] `sys/arm/arm/exception.S`, `ASENTRY_NP(swi_entry)` (linha 197). O
comentario imediatamente acima (linhas 191-196) diz: "The hardware switches to svc32 mode
on a swi, so we're already on the right stack; just build a trapframe and call the
handler." O corpo tem tres instrucoes: `PUSHFRAME`, `mov r0, sp`, `bl swi_handler`.

[FATO VERIFICADO] `PUSHFRAME` (linhas 81-90) salva, nesta ordem: `lr` (endereco de retorno,
que no modo SVC apos um SWI e' o endereco da instrucao seguinte), `r0-r12`, depois
`r13-r14` **do modo usuario** com `stmia r0, {r13-r14}^`, e por fim `mrs r0, spsr` +
`str r0, [sp, #-4]!`. A SPSR fica no **offset 0** do frame, ver `struct trapframe`
(`sys/arm/include/frame.h`, linha 63): `tf_spsr` e' o primeiro campo, e `tf_pc` e' o ultimo
antes de `tf_pad`.

[FATO VERIFICADO] `PULLFRAME` (linhas 97-105) faz o inverso: `ldr r0, [sp], #4` +
`msr spsr_fsxc, r0`, `clrex`, `ldmia sp, {r0-r14}^`, e o retorno e' `movs pc, lr`
(linha 210, dentro de `ASEENTRY_NP(swi_exit)`). O sufixo `s` em `movs pc, lr` e' o que
copia SPSR -> CPSR atomicamente com o salto. Modo, bit T, bits I/F e flags de condicao
voltam **todos juntos**, de um unico valor salvo.

[FATO VERIFICADO] Quem quer saber "o trap veio de usuario?" nao olha endereco: olha a SPSR.
`sys/arm/include/cpu.h` linha 52:
`#define TRAPF_USERMODE(frame) ((frame->tf_spsr & PSR_MODE) == PSR_USR32_MODE)`.
Usado em `sys/arm/arm/trap-v6.c` linha 304 (`usermode = TRAPF_USERMODE(tf)`) e 561.

[FATO VERIFICADO] NetBSD vai alem e **valida** a SPSR na entrada da syscall:
`sys/arch/arm/arm/syscall.c` linha 104, `KASSERT(VALID_PSR(tf->tf_spsr));`, com
`VALID_PSR` definido em `sys/arch/arm/include/locore.h` linhas 132-137:
modo tem que ser `PSR_USR32_MODE` e os bits I/F tem que estar zerados.

[INFERENCIA] A licao estrutural: o kernel nunca "adivinha" o estado do chamador porque ele
tem um registrador de hardware (SPSR) que guardou o CPSR inteiro no instante da excecao. O
zeebo-lle nao tem isso porque intercepta o SVC via `UC_HOOK_INTR` sem modo de excecao real
(nao ha SPSR salva, nao ha vetor 0x08). Logo o emulador precisa **fabricar** a SPSR: ler o
CPSR do chamador no momento do hook e guarda-lo num frame explicito.

#### APLICACAO NO ZEEBO-LLE

- P1 direto: criar `struct ZeeboTrapFrame { u32 spsr; u32 r[13]; u32 usr_sp; u32 usr_lr;
  u32 pc; }` e, no inicio do hook de INTR (`c0_intr_hook`, `tools/cpp/zeebo_lle_main.cpp`),
  fazer `uc_reg_read(uc, UC_ARM_REG_CPSR, &tf.spsr)` **antes** de qualquer escrita. O bit T
  do retorno passa a sair de `tf.spsr & (1<<5)`, nunca de faixa de endereco.
- Elimina as heuristicas de `zeebo_lle_main.cpp` linhas 3453-3468
  (`caller_is_kernel_stub`, `caller_is_ig_naming_arm`, `apply_tbit`), que hoje codificam
  `0xb0000000..0xb0020000`, `0xb0100000..0xb0120000`, `0xb0300000..0xb0330000` e
  `0x103dc000..0x103de000`.
- P3: um frame explicito separa "estado do chamador" de "estado a escrever no retorno". O
  epilogo passa a ser: `if (!did_handoff) restore_from(tf);` e nada mais.

#### TESTE QUE PODE FALHAR

`tools/cpp/test_svc_thumb_resume.cpp` ja existe; adicionar um caso negativo: montar um
stub em RAM com `svc` **Thumb** (`0xDF00 | imm8`) num endereco dentro da faixa ARM
hardcoded `0xb0100000..0xb0120000`, executar e exigir que apos o retorno
`CPSR.T == 1` e que a proxima instrucao Thumb execute. Com a heuristica atual o teste
**deve falhar** (o codigo forca `v & ~1u` para essa faixa); com a SPSR fabricada ele passa.
Controle negativo simetrico: `svc` ARM em `0x10400000` (fora das faixas) deve retomar com
`CPSR.T == 0`; hoje o `apply_tbit` forca `v | 1u` e quebra.

### 20.2 Largura da instrucao e reconstrucao do PC (ERESTART)

[FATO VERIFICADO] `sys/arm/arm/vm_machdep.c`, `cpu_set_syscall_retval` (linha 165). No caso
`ERESTART` (linhas 176-186), com o comentario "Reconstruct the pc to point at the swi":

```
#if __ARM_ARCH >= 7
        if ((frame->tf_spsr & PSR_T) != 0)
                frame->tf_pc -= THUMB_INSN_SIZE;
        else
#endif
                frame->tf_pc -= INSN_SIZE;
```

`INSN_SIZE == 4` (`sys/arm/include/armreg.h` linha 50), `THUMB_INSN_SIZE == 2`
(linha 453, com o comentario honesto "Some are 4 bytes."), `PSR_T == 0x00000020`
(linha 63).

[FATO VERIFICADO] NetBSD tem exatamente o mesmo bloco em
`sys/arch/arm/arm/syscall.c` linhas 255-266 (`case ERESTART:`), tambem condicionado a
`tf->tf_spsr & PSR_T_bit` (`PSR_T_bit` = `(1 << 5)`, `sys/arch/arm/include/armreg.h`
linha 82).

[FATO VERIFICADO] Ou seja: a largura da instrucao que causou o trap **nunca** e' inferida
por faixa de endereco nem por flag de "que tipo de thread e' essa". Ela e' derivada de um
unico bit do estado salvo do chamador.

[FATO VERIFICADO] Detalhe de sinal: no ARM32 o `tf_pc` salvo ja aponta para **depois** do
SWI (e o `lr` do modo SVC). Por isso o kernel **subtrai** para reexecutar. No AArch64,
`sys/arm64/arm64/vm_machdep.c` linha 162 faz `frame->tf_elr -= 4;` pelo mesmo motivo (ELR
aponta para a instrucao seguinte ao `svc`).

[FATO VERIFICADO] Contraste OpenBSD/arm64, `sys/arch/arm64/arm64/syscall.c`: linha 44,
`frame->tf_elr += 8; /* Skip over speculation-blocking barrier. */`, e no `ERESTART`
(linha 67) `frame->tf_elr -= 12;`. Os numeros 8 e 12 sao constantes derivadas de uma ABI
de userland documentada (svc seguido de barreira de especulacao), nao de chute. O ponto
importante e': o ajuste de PC e' **uma constante justificada pela ABI conhecida**, e o
caminho de "desfazer" e' o exato inverso do caminho de "avancar".

#### APLICACAO NO ZEEBO-LLE

- P2 direto: hoje `zeebo_lle_main.cpp` linha 3471-3475 comenta "UC_HOOK_INTR delivers pc
  already at svc+4" e usa `pc` cru em alguns ramos e `pc+4` em outros. Substituir por
  uma unica funcao:

```
static u32 svc_insn_size(u32 spsr) { return (spsr & (1u<<5)) ? 2u : 4u; }
// endereco do svc que disparou:
u32 svc_va   = tf.pc_at_hook - svc_insn_size(tf.spsr);
// retomada normal:
u32 resume   = svc_va + svc_insn_size(tf.spsr);
```

  Assim o "+4" some do codigo: existe so o par `svc_va` / `resume`, e um dos dois e'
  sempre derivado do outro.
- Isso tambem da o caminho para um "ERESTART" do lado do emulador: se uma syscall L4 nao
  pode ser concluida (ex.: recurso ainda nao inicializado), reexecutar o `svc` e' escrever
  `PC = svc_va | (spsr>>5 & 1)` — mesma formula, sinal invertido.
- P1 acoplado: o LSB do PC escrito e' que define o modo no Unicorn (o proprio codigo nota
  isso em `zeebo_lle_main.cpp` linhas 3448-3452). Entao o unico lugar que injeta LSB deve
  ser `resume_pc(tf)`, derivado de `tf.spsr`.

#### TESTE QUE PODE FALHAR

Novo `test_svc_pc_width.cpp`: para cada combinacao {ARM, Thumb} x {syscall 0xb4, 0x00,
0x0c}, montar em RAM `nop; svc #imm; marker_insn` e verificar apos o retorno que
`PC == endereco(marker_insn)` **exatamente**, nao `+2`, nao `+4`. Falha esperada hoje no
caso Thumb: o hook entrega `pc` = svc+4 tambem em Thumb, pulando 2 bytes a mais.
Segundo controle: apos forcar um caminho de "restart", exigir
`PC == endereco(svc)` e que o `svc` execute uma segunda vez (contador no hook == 2).

### 20.3 O numero da syscall: registrador vs imediato da instrucao

[FATO VERIFICADO] FreeBSD/EABI le o numero da syscall de um registrador, nunca do imediato
do SWI: `sys/arm/arm/syscall.c`, `cpu_fetch_syscall_args` (linha 100), linha 110:
`sa->code = td->td_frame->tf_r7;`. Os quatro primeiros argumentos vem de `tf_r0..tf_r3`
(`nap = 4`, `ap = &td->td_frame->tf_r0`), e o restante por `copyin` da pilha de usuario
(`td->td_frame->tf_usr_sp`).

[FATO VERIFICADO] NetBSD ainda suporta o modelo antigo (numero no imediato) e mostra
exatamente o problema do Thumb. `sys/arch/arm/arm/syscall.c`, `swi_handler` (linha 94):

- Se `tf->tf_spsr & PSR_T_bit` (linha 131): `insn = 0xef000000 | SWI_OS_NETBSD | tf->tf_r0;`
  e `tf->tf_r0 = tf->tf_ip;` (linhas 132-133). Ou seja: em Thumb o imediato do SWI so tem
  8 bits, entao a ABI passa o numero em `r0` e o kernel **sintetiza** uma instrucao ARM
  equivalente.
- Caso contrario (linha 138): `insn = read_insn(tf->tf_pc - INSN_SIZE, true);` — o kernel
  **relê a instrucao da memoria do usuario** no endereco reconstruido.
- Antes disso, linhas 60-79 (`#ifndef THUMB_CODE`): se `((tf->tf_pc - INSN_SIZE) & 3) != 0`
  o kernel entrega `SIGILL/ILL_ILLOPC` em vez de tentar ler desalinhado.
- `read_thumb_insn` (`sys/arch/arm/include/locore.h` linha 224) comeca com `va &= ~1;`
  (linha 226): o bit 0 do endereco e' o seletor de modo, nao parte do endereco.

#### APLICACAO NO ZEEBO-LLE

- O OKL4 2.1.1 codifica a syscall no imediato do `svc` (o proprio codigo ja mostra
  `svc #0x140c` no comentario de `zeebo_lle_main.cpp` linha 3498). Entao o zeebo-lle esta
  no caso NetBSD, nao no caso FreeBSD/EABI: o numero vem da instrucao.
- Adotar o padrao NetBSD: no hook, calcular `svc_va = pc - svc_insn_size(spsr)`, **ler a
  instrucao da memoria do guest** e decodificar:
  - ARM: `insn & 0x00FFFFFF`, exigindo `(insn & 0x0F000000) == 0x0F000000`;
  - Thumb: halfword, exigindo `(hw & 0xFF00) == 0xDF00`, imediato = `hw & 0x00FF`.
- Se o que esta em `svc_va` **nao** for um SVC, isso e' prova de que a largura/modo foram
  calculados errado. Esse e' um detector barato e exato para P1/P2 (NetBSD usa o mesmo
  teste como workaround do bug do ARM700, linhas 116-124).
- `va &= ~1` antes de qualquer leitura de codigo Thumb, como em `read_thumb_insn`.

#### TESTE QUE PODE FALHAR

`test_svc_decode_roundtrip.cpp`: em cada entrada do hook de INTR, recalcular `svc_va` e
verificar `decoded_imm == syscall_id_que_o_dispatcher_usou`. Rodar o boot completo com o
assert ligado. Se qualquer caminho (stub L4 ARM, chamada Thumb do AMSS em 0x103dcd18)
estiver com a largura errada, o teste falha com o endereco exato — ao contrario de hoje,
em que a falha aparece longe, como `UC_ERR_INSN_INVALID` ou salto para `PC=0x14`.

### 20.4 Troca de contexto: o PC de retomada vem de um slot salvo, nao de heuristica

[FATO VERIFICADO] `sys/arm/arm/swtch-v6.S`, `ENTRY(cpu_switch)` (linha 293). Salvamento
(linhas 300-304):

```
        ldr     r3, [r0, #(TD_PCB)]
        add     r3, #(PCB_R4)
        stmia   r3, {r4-r12, sp, lr, pc}
        mrc     CP15_TPIDRURW(r4)
        str     r4, [r3, #(PCB_TPIDRURW - PCB_R4)]
```

Restauracao (linhas 468-472):

```
        ldr     r3, [r7, #PCB_TPIDRURW]
        mcr     CP15_TPIDRURW(r3)       /* write tls thread reg 2 */
        mcr     CP15_TPIDRURO(r3)       /* write tls thread reg 3 */
        add     r3, r7, #PCB_R4
        ldmia   r3, {r4-r12, sp, pc}
```

[FATO VERIFICADO] `struct switchframe` (`sys/arm/include/frame.h`) tem a ordem
`sf_r4..sf_r12, sf_sp, sf_lr, sf_pc, sf_tpidrurw, sf_spare0`. A lista
`{r4-r12, sp, pc}` do `ldmia` consome 11 slots: `sf_r4..sf_r12` (9), `sf_sp` (10) e o
**11o slot, `sf_lr`, e' carregado em PC**. O `sf_pc` salvo pelo `stmia` fica apenas como
registro para depuracao/unwind; ele nao e' o endereco de retomada.

[INFERENCIA] Consequencia de projeto: existe **um unico slot** que define onde a thread
volta a executar (`sf_lr`), e ele e' escrito de forma explicita por quem cria a thread
(ver secao 5). Nenhum caminho do kernel "decide" o PC de retomada no momento da troca.

[FATO VERIFICADO] O estado de TLS tambem e' parte do contexto obrigatorio: `TPIDRURW`
salvo/restaurado (linhas 303-304 e 468-470), e `TPIDRPRW` recebe o ponteiro da thread
corrente na linha 454 (`mcr CP15_TPIDRPRW(r11)`), alem de `PC_CURTHREAD`/`PC_CURPCB` no
PCPU (linhas 453 e 457).

[FATO VERIFICADO] NetBSD: `cpu_switchto` em `sys/arch/arm/arm32/cpuswitch.S` linha 111,
com o mesmo padrao de switchframe; o trampolim de thread nova e' `lwp_trampoline`
(linha 298).

#### APLICACAO NO ZEEBO-LLE (P3)

- O sintoma "salto espurio para PC=0x14" no `L4_Ipc` (syscall 0x00) vem de o epilogo de
  retorno escrever PC/SP **na thread que ja foi trocada**. O codigo atual trata isso com
  um booleano `did_handoff` (`zeebo_lle_main.cpp` linhas 3483-3494). Isso funciona mas nao
  escala: cada syscall nova precisa lembrar de checar a flag.
- Padrao BSD equivalente: o dispatcher devolve um **enum**, nao um bool:

```
enum class SyscallOutcome { ReturnToCaller, SwitchedThread, Restart, Fault };
```

  `ReturnToCaller` -> escreve r0 e `resume_pc(tf)`; `SwitchedThread` -> o handler ja
  carregou o contexto completo do alvo (equivalente a `ldmia {r4-r12, sp, pc}`) e o
  epilogo **nao toca em PC/SP/R0**; `Restart` -> `PC = svc_va`; `Fault` -> aborta com log.
- Regra dura, copiada do `cpu_switch`: em `SwitchedThread`, o PC vem sempre de um campo
  salvo do contexto do alvo (`ctx.pc` + bit T de `ctx.cpsr`), nunca de `pc`, `lr` ou do
  frame do chamador.

#### TESTE QUE PODE FALHAR

Estender `tools/cpp/test_l4_ipc_dispatch.cpp`: apos um `L4_Ipc` que faz handoff, assertar
(a) `PC_final == ctx_alvo.pc` e (b) `SP_final == ctx_alvo.sp` e (c) que o R0 do **chamador**
salvo em memoria nao mudou. Controle negativo executavel: injetar deliberadamente
`outcome = ReturnToCaller` num handoff e exigir que o assert dispare. Se o assert nao
disparar, o dispatcher nao esta validando nada.

### 20.5 Primeira ativacao de thread: o trampolim que finge um retorno de syscall (P4)

Esta e' a tecnica mais diretamente aproveitavel do FreeBSD/ARM.

[FATO VERIFICADO] `sys/arm/arm/vm_machdep.c`, `cpu_fork` (linha 96). Estado minimo montado
para uma thread que **nunca rodou**:

```
        td2->td_frame = (struct trapframe *)pcb2 - 1;
        *td2->td_frame = *td1->td_frame;          /* trapframe copiado do pai */
        pmap_set_pcb_pagedir(vmspace_pmap(p2->p_vmspace), pcb2);
        pcb2->pcb_regs.sf_r4 = (register_t)fork_return;
        pcb2->pcb_regs.sf_r5 = (register_t)td2;
        pcb2->pcb_regs.sf_lr = (register_t)fork_trampoline;
        pcb2->pcb_regs.sf_sp = STACKALIGN(td2->td_frame);
        pcb2->pcb_regs.sf_tpidrurw = (register_t)get_tls();
        tf = td2->td_frame;
        tf->tf_spsr &= ~PSR_C;
        tf->tf_r0 = 0;
        tf->tf_r1 = 0;
        td2->td_md.md_spinlock_count = 1;
        td2->td_md.md_saved_cspr = PSR_SVC32_MODE;
```

(linhas 129-152 aproximadamente; `sf_lr = fork_trampoline` esta na linha 137.)

[FATO VERIFICADO] `sys/arm/arm/swtch.S`, `ENTRY(fork_trampoline)` (linha 112):

```
        mov     fp, #0
        mov     r2, sp
        mov     r1, r5
        mov     r0, r4
        ldr     lr, =swi_exit           /* linha 118 */
        b       fork_exit
```

[FATO VERIFICADO] E o destino desse `lr` e' `ASEENTRY_NP(swi_exit)` em
`sys/arm/arm/exception.S` linha 207, com o comentario (linhas 201-206): "The
fork_trampoline() code in swtch.S aranges for the MI fork_exit() to return to swi_exit
here, to return to userland. The net effect is that a newly created thread appears to
return from a SWI just like the parent thread that created it."

[FATO VERIFICADO] Portanto o **estado minimo** de uma thread nova, em FreeBSD/ARM, e':

1. um `trapframe` completo em memoria (SPSR + r0-r12 + usr_sp + usr_lr + pc), copiado de um
   frame valido e depois ajustado (`tf_r0 = 0`, limpa `PSR_C` = "sem erro");
2. um `switchframe` cujo `sf_sp` aponta para esse trapframe e cujo `sf_lr` aponta para o
   trampolim;
3. o diretorio de paginas (`pcb_pagedir`, via `pmap_set_pcb_pagedir`);
4. o TLS (`sf_tpidrurw`);
5. contadores/flags de consistencia (`md_spinlock_count = 1`, `md_saved_cspr`).

Nao existe caminho "ativar thread sem contexto": o contexto e' construido por inteiro antes,
e a primeira execucao passa pelo **mesmo** epilogo de saida de syscall que todas as outras.

[FATO VERIFICADO] NetBSD e' ainda mais explicito sobre o CPSR inicial:
`sys/arch/arm/arm32/vm_machdep.c`, `cpu_lwp_fork` (linha 105), linhas 160-166:

```
        sf = (struct switchframe *)tf - 1;
        sf->sf_r4 = (u_int)func;
        sf->sf_r5 = (u_int)arg;
        sf->sf_r7 = PSR_USR32_MODE;     /* for returning to userspace */
        sf->sf_sp = (u_int)tf;
        sf->sf_pc = (u_int)lwp_trampoline;
```

`sf_r7` carrega literalmente o **modo do PSR** com que a LWP vai voltar a userland, e
`lwp_trampoline` (`cpuswitch.S` linha 298) termina em `PULLFRAME` + `movs pc, lr`
(linhas 327-329) — mesmo epilogo do retorno de trap.

[FATO VERIFICADO] arm64 repete o padrao: `sys/arm64/arm64/vm_machdep.c` linhas 108-111
(`pcb_x[8] = fork_return`, `pcb_x[9] = td2`, `pcb_lr = fork_trampoline`,
`pcb_sp = td2->td_frame`) e `sys/arm64/arm64/swtch.S` `ENTRY(fork_trampoline)` linha 208,
que apos `fork_exit` restaura `sp_el0`, `spsr_el1`, `elr_el1` e faz `ERET` (linhas 219-249).

#### APLICACAO NO ZEEBO-LLE

- P4 resolvido por construcao: **proibir** ativacao de thread sem contexto. Criar
  `ZeeboThreadCtx { u32 r[13]; u32 sp; u32 lr; u32 pc; u32 cpsr; u32 utcb; u32 space_id; }`
  e exigir que `L4_ThreadControl`/`L4_ExchangeRegisters` preencham **todos** os campos
  antes de a thread entrar na fila de execucao.
- Campo obrigatorio equivalente ao `sf_r7` do NetBSD: `cpsr` completo, com modo (0x10 usr /
  0x13 svc), bits I/F e **bit T**. O bit T do `cpsr` e o LSB de `pc` devem ser coerentes; se
  divergirem, e' bug (ver teste abaixo).
- Equivalente ao `pcb_pagedir`: guardar o `space_id`/TTBR da thread no contexto e aplica-lo
  na ativacao (secao 6), nunca deixar o TTBR "herdado" da thread anterior.
- Equivalente ao `utcb`/TLS: o OKL4 usa UTCB por thread; guardar o ponteiro no contexto e
  escreve-lo no registrador/endereco fixo do UTCB na ativacao, como o `cpu_switch` faz com
  `TPIDRURW`/`TPIDRURO`.
- Equivalente ao `fork_trampoline`: uma unica funcao `activate_thread(ctx)` que escreve o
  contexto e cai no **mesmo** caminho de retomada usado pelo retorno de syscall. Nada de
  ativacao com caminho proprio. Os testes ja existentes
  `tools/cpp/test_exregs_activation.cpp` e `test_exregs_frame_resume.cpp` sao o lugar
  natural para amarrar isso.

#### TESTE QUE PODE FALHAR

`test_thread_first_activation.cpp`:
1. Criar thread com `pc` Thumb (LSB=1) e `cpsr.T = 0` — assert deve disparar
   ("T-bit incoerente com LSB do PC"). Hoje ninguem verifica, entao esse teste **falha**
   ate o assert existir.
2. Criar thread sem `sp` (0) e ativar — assert deve disparar antes de executar uma
   instrucao. Controle negativo: sem o assert, o guest executa e morre longe, com
   `PC=0x14` ou `UC_ERR_EXCEPTION`, que e' exatamente o sintoma reportado em P3.
3. Ativar duas vezes a mesma thread e exigir que o segundo `pc` seja o salvo pela
   suspensao, nao o `pc` de criacao.

### 20.6 MMU: troca de TTBR com TTBR intermediario, ASID e invalidacao (P6)

[FATO VERIFICADO] `sys/arm/arm/swtch-v6.S`, `ENTRY(cpu_context_switch)` (linhas 116-160).
Sequencia integral do ponto de vista arquitetural:

1. `DSB`;
2. carrega `pmap_kern_ttb` e faz `mcr CP15_TTBR0(r1)` — troca para o TTB do **kernel** como
   passo intermediario. O comentario (linhas 118-126) explica: so e' seguro trocar
   diretamente entre tabelas quando o tamanho do mapeamento de cada VA e' igual nas duas;
   por isso passa pelo kernel pmap, onde os tamanhos sao iguais (ou nao mapeado);
3. `ISB`; `mcr CP15_TLBIASID(r2)` com `r2 = CPU_ASID_KERNEL` (=0, `sys/arm/include/cpu-v6.h`
   linha 59) — invalida as entradas **nao globais**;
4. `DSB`; `mcr CP15_TTBR0(r0)` (TTB novo); `ISB`;
5. **de novo** `mcr CP15_TLBIASID(r2)`, porque o mapeamento de `PT2MAP` difere (linhas
   136-140);
6. invalidacao do **preditor de saltos**: `CP15_BPIALL`, ou `CP15_ICIALLU` em CPUs onde
   `BPIALL` e' NOP (linhas 141-157), com a justificativa explicita: "the branch predictor is
   not architecturally invisible";
7. `DSB`; `mov pc, lr`.

[FATO VERIFICADO] `cpu_switch` so chama isso quando o TTB muda de verdade
(`swtch-v6.S` linhas 337-347): le `CP15_TTBR0`, compara com `pcb_pagedir` do novo PCB, e
pula (`beq sw0`) se for igual.

[FATO VERIFICADO] `pmap_activate` (`sys/arm/arm/pmap-v6.c` linha 6186) e' a versao em C:
`critical_enter()`, atualiza `pm_active` por CPU, `ttb = pmap_ttb_get(pmap)`,
`td->td_pcb->pcb_pagedir = ttb`, `cp15_ttbr_set(ttb)` (linha 6212), `PCPU_SET(curpmap,
pmap)`, `critical_exit()`.

[FATO VERIFICADO] Invalidacao seletiva de TLB, `sys/arm/include/cpu-v6.h`:
`tlb_flush_all_local` (linha 334) = `dsb; TLBIALL; dsb`;
`tlb_flush_all_ng_local` (linha 344) = `dsb; TLBIASID(CPU_ASID_KERNEL); dsb`;
`tlb_flush_local(va)` (linha 355) = `KASSERT((va & PAGE_MASK) == 0, ...)` seguido de
`dsb; TLBIMVA(va | CPU_ASID_KERNEL); dsb`. Note o **assert de alinhamento de pagina antes
da operacao de TLB** (linha 358).

[FATO VERIFICADO] arm64 mostra a versao "cara" do problema, com ASID de verdade:
`sys/arm64/arm64/pmap.c` linhas 345-360 definem cookie = (ASID, epoch):
`COOKIE_FROM(asid, epoch)`, `COOKIE_TO_ASID`, `COOKIE_TO_EPOCH`; `struct asid_set`
(linhas 313-320) com `asid_bits`, bitmap, `asid_next`, `asid_epoch` e mutex.
`pmap_to_ttbr0` (linha 6936) monta `pm_ttbr | ASID_TO_OPERAND(COOKIE_TO_ASID(pm_cookie)) |
ttbr_flags`. `pmap_activate_int` (linha 7004) faz: se ja e' o `curpmap`, so `dsb(ish)` e
retorna `false`; senao `PCPU_SET(curpmap, pmap)`, `dsb(ish)`, e **se a epoch do cookie for
diferente da epoch do set, realoca o ASID** (`pmap_alloc_asid`) antes de
`set_ttbr0(pmap_to_ttbr0(pmap))`.

[INFERENCIA] O mecanismo de epoch existe porque ASIDs sao um recurso finito reciclado:
quando o alocador da a volta, ele incrementa a epoch e invalida tudo, e cada pmap
"descobre" na proxima ativacao que o seu ASID caducou. Isso e' o modelo certo para um
emulador com vTLB: um contador de geracao por espaco de enderecos.

#### APLICACAO NO ZEEBO-LLE

- P6: em `zeebo_l4_mmu.h` / `L4_MapControl`, adotar a regra do `cpu_context_switch`:
  toda troca de espaco passa por (a) barreira, (b) invalidacao das entradas nao globais do
  vTLB, (c) troca do ponteiro de tabela, (d) invalidacao de novo, (e) invalidacao do cache
  de traducao (secao 7). Nunca trocar o espaco "no meio" de uma retomada de syscall.
- Pular trabalho quando o espaco nao muda, como o `cmp r0, r1 / beq sw0`: isso corta o custo
  e, mais importante, torna a troca um evento raro e logavel.
- Implementar `space_generation` por espaco de enderecos (equivalente a `asid_epoch`): cada
  `L4_MapControl` que altera mapeamentos incrementa a geracao; o contexto da thread guarda a
  geracao vista na ultima ativacao; se diferir, invalida vTLB + cache de traducao **daquele
  espaco** em vez de invalidar tudo.
- Copiar o `KASSERT` de alinhamento: toda entrada de mapa deve passar por
  `assert((va & (page_size-1)) == 0)` usando o page-size mask real do KIP, nao um valor
  fixo de 4096.

#### TESTE QUE PODE FALHAR

Estender `tools/cpp/test_l4_space_switch.cpp` e `test_vtlb_remap_asymmetry.cpp`: mapear VA
`V` -> frame A no espaco 1 e VA `V` -> frame B no espaco 2, com conteudos diferentes;
executar codigo em `V` no espaco 1, trocar de espaco, executar em `V` de novo e exigir o
comportamento do frame B. Sem invalidacao (de vTLB **e** de cache de traducao) o teste
falha lendo A — e' o controle negativo direto de P5+P6. Segundo caso: `L4_MapControl` com
VA desalinhada em relacao ao page-size mask do KIP deve abortar; hoje, provavelmente,
mapeia silenciosamente.

### 20.7 Cache de instrucao e preditor: estado "visivel" que precisa de invalidacao explicita (P5)

[FATO VERIFICADO] `sys/arm/include/cpu-v6.h`, `icache_sync(va, size)` (linha 459):
`dsb()`, alinhamento `va &= ~cpuinfo.dcache_line_mask`, loop de `_CP15_DCCMVAU(va)` por
linha de cache, depois `ICIMVAU`/`ICIALLUIS` conforme o caso, `dsb()`, `isb()`.
`icache_inv_all()` (linha 484) = `ICIALLUIS`/`ICIALLU` + `dsb` + `isb`.

[FATO VERIFICADO] O `cpu_context_switch` invalida o preditor de saltos porque ele "nao e'
arquiteturalmente invisivel" (`swtch-v6.S` linhas 141-147, citando o ARM ARM ARMv7-A/R,
paginas B2-1264/65).

[INFERENCIA] Traducao direta para emulador: o cache de blocos traduzidos (TB) do QEMU/Unicorn
e' exatamente um cache de instrucoes **arquiteturalmente visivel** do ponto de vista do
guest. Qualquer evento que, no hardware, exigiria `ICIALLU`/`BPIALL`/`TLBIASID` — escrita em
codigo, troca de tabela de paginas, mudanca de permissao, mudanca de modo ARM/Thumb sobre o
mesmo endereco — exige `uc_ctl_remove_cache` no emulador.

#### APLICACAO NO ZEEBO-LLE

- P5: hoje `zeebo_lle_main.cpp` chama `uc_ctl_remove_cache` em enderecos **hardcoded**
  (linhas 3479-3481: `0xb000c720`, `0xb00033d0`, `0x103dcd14`; linha 3493: `0xb000c800`),
  e no ramo de handoff usa o PC alvo (linha 3488). Isso e' equivalente a invalidar so
  algumas linhas de cache conhecidas e torcer.
- Substituir por uma politica derivada de evento, nao de endereco:
  - escrita em memoria marcada como executavel -> invalida a faixa escrita
    (equivalente a `icache_sync(va, size)`);
  - troca de espaco de enderecos -> invalida tudo do espaco antigo
    (equivalente a `TLBIASID` + `BPIALL`);
  - `L4_MapControl` que muda permissao/frame -> invalida a faixa de VA afetada;
  - retomada com bit T diferente do bit T com que aquele endereco foi traduzido ->
    invalida a faixa (um mesmo VA traduzido como ARM e como Thumb sao dois blocos
    incompativeis).
- O ultimo item explica diretamente a corrupcao citada em P1: `0xe51ff004` lida como Thumb.
  Mesmo apos corrigir o bit T, se o TB antigo (traduzido em Thumb) continuar no cache, o
  erro persiste.

#### TESTE QUE PODE FALHAR

`test_tb_invalidate_mode_flip.cpp`: escrever em `V` uma instrucao ARM, executar (forca
traducao ARM), reescrever o mesmo `V` com codigo Thumb, saltar para `V|1` e exigir execucao
Thumb correta. Sem invalidacao por faixa o Unicorn reusa o bloco ARM e o teste falha.
Variante: mesmo `V`, dois espacos de enderecos com codigos diferentes (junta com o teste da
secao 6). Ja existe `tools/cpp/test_jit_code_hook.cpp` como ponto de partida.

### 20.8 arm64 como modelo limpo: a causa da excecao e' dado, nao heuristica (P1)

[FATO VERIFICADO] `sys/arm64/arm64/trap.c`, `do_el0_sync` (linha 530). O dispatch e' um
`switch (exception)` onde `exception = ESR_ELx_EXCEPTION(esr)` e `esr = frame->tf_esr`. Os
casos incluem `EXCP_SVC32` e `EXCP_SVC64` — **o hardware informa se o chamador era AArch32
ou AArch64**, e ambos caem em `svc_handler(td, frame)` (linha 194).

[FATO VERIFICADO] `svc_handler` valida o campo ISS antes de aceitar:
`if ((frame->tf_esr & ESR_ELx_ISS_MASK) == 0) { syscallenter(td); syscallret(td); } else {
call_trapsignal(td, SIGILL, ILL_ILLOPN, (void *)frame->tf_elr, ...); userret(td, frame); }`
(linhas 197-204). Ou seja: `svc #imm` com imediato diferente de 0 **nao** e' syscall; e'
SIGILL. Nada de "provavelmente e' syscall".

[FATO VERIFICADO] `do_el0_sync` comeca com um assert de sanidade do ambiente
(linhas 536-539): `KASSERT((uintptr_t)get_pcpu() >= VM_MIN_KERNEL_ADDRESS, ("Invalid pcpu
address from userland: %p (tpidr %lx)", ...))`.

[FATO VERIFICADO] O retorno usa `restore_registers` (`sys/arm64/arm64/exception.S`
linha 107), que restaura `sp_el0`, `spsr_el1`, `elr_el1` e os registradores, e termina em
`ERET` (ex.: `handle_el1h_sync`, linhas 195-202). De novo: PSR e PC restaurados juntos, de
slots salvos.

[FATO VERIFICADO] Os casos de abort sao dispatchados por tabela indexada pelo codigo de
fault (`abort_handlers[]`, linhas 96-117), e o `default` de um DFSC desconhecido imprime
registradores e da `panic("Unhandled EL0 %s abort: %x", ...)`.

#### APLICACAO NO ZEEBO-LLE

- P1: o zeebo-lle **tem** o equivalente ao ESR — o proprio `intno` do `UC_HOOK_INTR` mais o
  CPSR lido no instante do hook e o imediato decodificado do `svc`. Use esses tres dados
  como "ESR sintetico" e trate qualquer combinacao desconhecida como falha, nao como
  syscall.
- Trocar cadeias de `if (pc >= X && pc < Y)` por uma tabela `svc_handlers[imm]` com entradas
  nulas explicitas; `imm` sem handler -> log + abort controlado, como o `panic` do
  `default` do arm64.
- Guardar no log de trace, por syscall, a tripla `(svc_va, modo, imm)` — e' o "ESR" que
  permite diagnosticar P1/P2 depois do fato.

#### TESTE QUE PODE FALHAR

`test_svc_unknown_imm.cpp`: emitir `svc #0x9999` (imediato sem handler) e exigir que o
emulador pare com erro identificavel (codigo de saida/flag), nao que continue. Hoje,
qualquer caminho que caia no `else` final do dispatcher provavelmente retoma execucao e
corrompe silenciosamente. Controle negativo adicional: emitir `svc` a partir de um endereco
**nao mapeado como executavel** e exigir falha imediata.

### 20.9 INVARIANTS/KASSERT/witness: falhar cedo em vez de continuar corrompido

[FATO VERIFICADO] Assembly com assert. `sys/arm/arm/swtch-v6.S`, dentro de `cpu_switch`:
linhas 295-298 `#ifdef INVARIANTS / cmp r0, #0 / beq badsw2`; linhas 306-309 idem para o
novo thread (`badsw3`); linhas 342-345 `cmp r0, #0 / beq badsw4` para "new pagedir is NULL".
Os alvos (linhas 474-507) chamam `panic` com mensagens literais:
`"cpu_throw: no newthread supplied.\n"`, `"cpu_switch: no curthread supplied.\n"`,
`"cpu_switch: no newthread supplied.\n"`, `"cpu_switch: new pagedir is NULL.\n"`.

[FATO VERIFICADO] `sys/kern/subr_trap.c`, `userret()` (linha 98) — o ponto unico por onde
toda volta a userland passa — concentra a bateria de invariantes:

- linha 104: `KASSERT((p->p_flag & P_WEXIT) == 0, ("Exiting process returns to usermode"))`;
- linha 168: `WITNESS_WARN(WARN_PANIC, NULL, "userret: returning")` — ordem de locks;
- linha 169: `td_critnest == 0` ("Returning in a critical section");
- linha 171: `td_locks == 0`; 173: `td_rw_rlocks == 0`; 176: `td_sx_slocks == 0`;
  179: `td_lk_slocks == 0`;
- linha 182: `(td->td_pflags & TDP_NOFAULTING) == 0`;
- linha 188: `KASSERT(0, ("userret: Returning with sleep disabled"))`;
- linhas 190-197: thread pinned, vnode pre-alocado, sinais adiados, `td_vslock_sz == 0`.

[FATO VERIFICADO] `sys/kern/subr_syscall.c`: `syscallenter` (linha 58) tem
`KASSERT((td->td_pflags & TDP_NERRNO) == 0, ("%s: TDP_NERRNO set", __func__))` antes de
chamar a syscall; `syscallret` (linha 212) comeca com
`KASSERT((td->td_pflags & TDP_FORKING) == 0, ("fork() did not clear TDP_FORKING upon
completion"))` e `KASSERT(td->td_errno != ERELOOKUP, ("ERELOOKUP not consumed syscall %d",
td->td_sa.code))`. `cpu_fetch_syscall_args` do arm64 (`trap.c` linha 134) tem
`KASSERT(sa->callp->sy_narg <= nitems(sa->args), ("Syscall %d takes too many arguments", ...))`.

[FATO VERIFICADO] Quando nao da para assertar, o kernel **panica cedo**:
`sys/arm/arm/trap-v6.c`, `abort_fatal` termina em `panic("Fatal abort")` (linha 613); o
arm64 imprime todos os registradores + FAR + ESR antes do `panic` (`trap.c`, `data_abort`/
`do_el0_sync`).

[INFERENCIA] O valor de engenharia nao esta no assert em si, mas em **onde** ele fica: num
ponto de estrangulamento unico (`userret`, `cpu_switch`, `syscallret`) por onde todo fluxo
passa. Um assert num caminho lateral pega pouco; um assert no funil pega tudo.

#### APLICACAO NO ZEEBO-LLE

Criar `ZEEBO_ASSERT(cond, fmt, ...)` compilado por `-DZEEBO_INVARIANTS` (ligado no
`make test`, desligado no build de release, exatamente como `INVARIANTS`), e colocar a
bateria no **funil**: uma funcao `syscall_return(tf, outcome)` por onde todo retorno de
syscall passa. Asserts minimos, todos verificaveis com o que ja existe hoje:

1. `ZEEBO_ASSERT(svc_insn_at(svc_va, tf.spsr), "PC/largura errados: svc_va=%08x T=%u")`
   — a instrucao em `svc_va` tem mesmo que ser um SVC (secao 3).
2. `ZEEBO_ASSERT(((resume_pc & 1u) != 0) == ((tf.spsr >> 5) & 1u), "T-bit incoerente")`.
3. `ZEEBO_ASSERT(!(outcome == SwitchedThread && wrote_caller_regs), "epilogo tocou a thread
   errada")` — P3.
4. `ZEEBO_ASSERT(cur_tid == expected_tid, ...)` na entrada e na saida do dispatcher; se
   mudou, `outcome` tem que ser `SwitchedThread`.
5. `ZEEBO_ASSERT(resume_pc >= 0x1000, "PC absurdo")` — o sintoma `PC=0x14` de P3 e' pego
   aqui, no instante em que nasce, e nao 300 instrucoes depois.
6. `ZEEBO_ASSERT(sp_alinhado_8(tf.usr_sp))` — equivalente ao `STACKALIGN` do `cpu_fork`.
7. `ZEEBO_ASSERT(ctx.space_id == space_ativo, "TTBR nao corresponde a thread")` — equivalente
   ao `badsw4` ("new pagedir is NULL").

Em caso de violacao: dump completo (todos os registradores, CPSR, tid, svc_va, imm, ultimos
N blocos do trace) e **abort**, no modelo do `print_registers()` + `panic` do arm64. Nunca
"continuar e ver o que acontece".

#### TESTE QUE PODE FALHAR

`test_invariants_fire.cpp`: um teste por assert, que **provoca** a violacao de proposito
(escreve PC=0x14, forca T-bit incoerente, faz handoff e depois escreve r0 do chamador) e
exige que o processo aborte com a mensagem esperada. Um assert que nunca dispara em teste
nenhum e' um assert nao verificado: se `test_invariants_fire` passa sem o assert existir,
o teste esta errado.

### 20.10 Mapeamento consolidado P1..P6

| Problema | Tecnica BSD | Referencia verificada |
|---|---|---|
| P1 drift ARM/Thumb | bit T vem da SPSR salva, restaurada junto com o PC (`movs pc, lr`); em arm64 o ESR distingue `EXCP_SVC32`/`EXCP_SVC64` | `sys/arm/arm/exception.S:97-105,210`; `sys/arm64/arm64/trap.c:530+` |
| P2 PC de retorno/largura | `tf_pc -= (spsr & PSR_T) ? THUMB_INSN_SIZE : INSN_SIZE`; NetBSD relê a instrucao em `tf_pc - INSN_SIZE` | `sys/arm/arm/vm_machdep.c:178-186`; NetBSD `syscall.c:131-138,255-266` |
| P3 handoff/IPC | PC de retomada vem sempre de slot salvo (`sf_lr` via `ldmia {r4-r12,sp,pc}`), nunca do frame do chamador | `sys/arm/arm/swtch-v6.S:300-304,468-472` |
| P4 primeira ativacao | trapframe completo + switchframe com `sf_lr = fork_trampoline`, e a thread nova sai pelo mesmo `swi_exit`; NetBSD guarda `sf_r7 = PSR_USR32_MODE` | `vm_machdep.c:96-152`; `swtch.S:112-118`; `exception.S:201-213`; NetBSD `vm_machdep.c:160-166` |
| P5 cache de traducao | invalidacao explicita de icache/BTB porque "nao e' arquiteturalmente invisivel"; `icache_sync` por faixa | `swtch-v6.S:141-157`; `cpu-v6.h:459-493` |
| P6 MMU/espacos | TTBR intermediario do kernel, `TLBIASID` duas vezes, skip quando TTB nao muda; arm64 com ASID+epoch | `swtch-v6.S:116-160,337-347`; `pmap-v6.c:6186-6215`; `arm64/pmap.c:345-360,6936,7004` |

Ordem sugerida de adocao, do maior retorno ao menor: (1) frame explicito com SPSR
fabricada + `svc_insn_size` (mata P1 e P2 juntos); (2) `SyscallOutcome` com epilogo unico
(mata P3 e cria o funil de asserts); (3) `ZeeboThreadCtx` obrigatorio (P4); (4) invalidacao
de cache de traducao por evento (P5); (5) `space_generation` e invalidacao seletiva (P6).

---

## 30. Darwin/XNU (Mach) e a familia L4: continuations, handoff de IPC e retorno de trap em ARM

### 30.0 Fontes, versoes e licenca

Tudo abaixo foi lido diretamente nas arvores publicas da Apple
(`github.com/apple-oss-distributions/xnu`), em duas tags, porque o suporte a **ARM de 32 bits**
(`osfmk/arm/*`, `bsd/dev/arm/*` de 32 bits) foi removido nas versoes recentes:

| Tag usada | Para que serve aqui |
|---|---|
| `xnu-4903.221.2` (macOS 10.14 / iOS 12) | **ARM32 real**: `osfmk/arm/locore.s`, `osfmk/arm/cswitch.s`, `osfmk/arm/pcb.c`, `osfmk/arm/status.c`, `osfmk/arm/trap.c`, `bsd/dev/arm/{systemcalls,unix_signal}.c` |
| `xnu-8792.81.2` (macOS 13) | Escalonador e IPC atuais: `osfmk/kern/sched_prim.c`, `osfmk/kern/syscall_subr.c`, `osfmk/ipc/*`, `osfmk/arm64/locore.s`, `osfmk/arm64/machine_routines_asm.s` |

**Licenca: APSL 2.0** (Apple Public Source License Version 2.0). [FATO VERIFICADO] O cabecalho de
`osfmk/arm/cswitch.s` diz literalmente: *"This file contains Original Code and/or Modifications of
Original Code as defined in and that are subject to the Apple Public Source License Version 2.0"*,
com o marcador `@APPLE_OSREFERENCE_LICENSE_HEADER_START@`.

> **Aviso de higiene de licenca.** APSL 2.0 e' uma licenca de *copyleft por arquivo*: arquivo
> derivado de arquivo APSL continua APSL e exige publicacao das modificacoes. O `zeebo-lle` pode
> **reimplementar o padrao/arquitetura** (continuation, par PC+CPSR, handoff) livremente — ideias e
> tecnicas nao sao cobertas — mas **nao deve copiar codigo literal** (nem assembly, nem structs,
> nem blocos de C) para dentro da arvore. Nas citacoes abaixo eu cito <= 3 linhas por ponto,
> exatamente para permitir verificacao sem importacao de codigo.
> A parte seL4 (secao 30.5) tem licenca diferente e esta anotada la'.

Convencoes: `[FATO VERIFICADO]` = eu li o arquivo citado nesta sessao. `[INFERENCIA]` = deducao
minha a partir do que li. `NAO VERIFICADO` = nao consegui confirmar, e digo isso em vez de inventar.

---

### 30.1 Continuations: a thread que bloqueia **nao volta** pelo epilogo da syscall

#### 30.1.1 O mecanismo

No Mach, uma thread que vai dormir pode declarar uma **funcao de continuacao**: em vez de
"parar no meio do kernel e depois retomar do meio", ela diz *"quando eu acordar, comece aqui, do
zero, com pilha nova"*. O tipo esta em `osfmk/kern/thread.h`:
`typedef void (*thread_continue_t)(void *param, wait_result_t)` [FATO VERIFICADO — o prototipo
aparece tambem no comentario acima de `Call_continuation` em `osfmk/arm/cswitch.s`].

O caminho de bloqueio e' `thread_block_parameter()` -> `thread_block_reason()` em
`osfmk/kern/sched_prim.c`. [FATO VERIFICADO] Linhas 3773-3786 (xnu-8792):

```c
	self->continuation = continuation;
	self->parameter = parameter;
	...
	do {
		thread_lock(self);
		new_thread = thread_select(self, processor, &reason);
		thread_unlock(self);
	} while (!thread_invoke(self, new_thread, reason));
```

`thread_invoke()` (`sched_prim.c:2894`) le a continuacao logo no inicio
(`thread_continue_t continuation = self->continuation;`, linha 2907) e, se ela existir e a thread
destino nao tiver pilha de kernel propria, faz **stack handoff**. O comentario do proprio codigo e'
a frase mais importante desta secao (linhas 3027-3032):

> *"This is where we actually switch thread identity, and address space if required. However,
> register state is not switched - this routine leaves the stack and register state active on the
> current CPU."*

Ou seja: **com continuation nao se salva contexto de registrador nenhum**. Em seguida, linha 3063:

```c
			assert(continuation);
			call_continuation(continuation, parameter,
			    thread->wait_result, enable_interrupts);
			/*NOTREACHED*/
```

`/*NOTREACHED*/` e' literal: `thread_block()` **nunca retorna** quando ha continuation.

#### 30.1.2 A prova no assembly ARM32

[FATO VERIFICADO] `osfmk/arm/cswitch.s`, simbolo `Switch_context` (xnu-4903):

```asm
LEXT(Switch_context)
	teq		r1, #0			// Test if blocking on continuaton
	bne		switch_threads		// No need to save GPR/NEON state if we are
```

`r1` e' o argumento `cont`. Se ha continuation, o kernel **pula** o `stmia r3!, {r4-r14}` que
salvaria os callee-saved e o VFP. O contexto salvo de uma thread bloqueada em continuation e'
exatamente: *nada*. Toda a informacao necessaria esta em `thread->continuation`,
`thread->parameter` e `thread->wait_result`.

O lado do retorno, `Call_continuation` (mesmo arquivo), reconstroi a pilha do zero:

```asm
LEXT(Call_continuation)
	mrc		p15, 0, r9, c13, c0, 4		// Read TPIDRPRW
	ldr		sp, [r9, TH_KSTACKPTR]		// Set stack pointer
	mov		r7, #0				// Clear frame pointer
```

`TPIDRPRW` e' o ponteiro da thread *atual* — nao ha risco de usar o descritor do chamador antigo.
O frame pointer e' zerado: a pilha comeca limpa, sem heranca do caminho que bloqueou.

#### 30.1.3 Por que isso mata a classe de bug do P3

Cadeia completa de um `mach_msg` que bloqueia no receive
[FATO VERIFICADO, xnu-4903, arquivo por arquivo]:

1. `osfmk/ipc/mach_msg.c:594` (`mach_msg_overwrite_trap`):
   `self->ith_continuation = thread_syscall_return;`
   — a thread **anota, antes de dormir, quem vai escrever o valor de retorno dela**.
2. `osfmk/ipc/ipc_mqueue.c:988-990` (`ipc_mqueue_receive`):
   `if (self->ith_continuation) thread_block(ipc_mqueue_receive_continue); /* NOTREACHED */`
3. Troca de thread (secao 30.1.1). O epilogo da trap **nao roda**.
4. Ao acordar: `Call_continuation` -> `ipc_mqueue_receive_continue`
   (`ipc_mqueue.c:932`: `ipc_mqueue_receive_results(wresult); mach_msg_receive_continue();`)
   -> `mach_msg_receive_continue` (`mach_msg.c:479`) -> `(*self->ith_continuation)(mr)`
   -> `thread_syscall_return(mr)`.
5. `osfmk/arm/locore.s:1821`, simbolo `thread_syscall_return`:

```asm
LEXT(thread_syscall_return)
	mrc		p15, 0, r9, c13, c0, 4		// Read TPIDRPRW
	add		r1, r9, ACT_PCBDATA		// Get User PCB
	str		r0, [r1, SS_R0]			// set return value
```

O valor de retorno e' escrito no PCB da thread lida de **TPIDRPRW** (a thread que esta rodando
agora), nunca num ponteiro capturado antes do bloqueio.

E o caminho nao-bloqueante do ARM32 e' igualmente blindado: em `fleh_swi_unix`
(`locore.s:686-695`) o codigo faz `bl EXT(unix_syscall)` seguido de `b .` — um *loop infinito*
deliberado. [INFERENCIA justificada pelo codigo] O autor esta declarando que `unix_syscall` **nunca
retorna**; se retornasse, o kernel trava em vez de executar um epilogo potencialmente errado.
Nao existe "epilogo comum" que possa ser executado depois de uma troca de thread.

#### APLICACAO NO ZEEBO-LLE (P3, P2, P1)

Hoje o `c0_intr_hook` (`tools/cpp/zeebo_lle_main.cpp`, ~linha 3049) faz o epilogo de syscall
*sempre no mesmo lugar*: escreve `R0`, escreve `SP` (de `ip`), escreve `PC`. No ramo
`syscall == 0x00` (L4_Ipc) ja existe uma gambiarra — a flag `did_handoff` (~linha 3487) que
**re-le** o PC do Unicorn para nao sobrescrever a thread destino. Isso e' exatamente o sintoma que
o Mach elimina por construcao. A traducao direta do padrao:

1. Modele cada thread do guest com um `struct GuestThread { u32 r[13], sp_user, lr_user, pc, cpsr;
   u32 utcb; ThreadId tid; Continuation cont; u32 wait_result; }`.
2. Faca o hook de SVC **retornar um veredito**, nunca escrever registradores diretamente:
   `enum class SvcOutcome { ResumeCaller, Blocked, SwitchedTo };`.
3. Regra dura equivalente ao `/*NOTREACHED*/`: se o handler devolveu `Blocked` ou `SwitchedTo`,
   o codigo comum **nao toca** em R0/SP/PC. O unico lugar que carrega estado de CPU e' um
   `load_guest_context(GuestThread&)` chamado com a thread que sera' executada.
4. O analogo de `thread_syscall_index/ith_continuation`: guarde em `GuestThread` o campo
   `pending_ret_r0` e escreva-o **dentro de `load_guest_context`**, a partir do descritor da thread
   que vai rodar. Nunca a partir de variavel local do handler.
5. `did_handoff` some: ele vira o caso normal `SwitchedTo`.

#### TESTE QUE PODE FALHAR (controle negativo executavel)

Instrumento: um contador global `epilogue_writes_pc` incrementado em **todo** `uc_reg_write(uc,
UC_ARM_REG_PC, ...)` do `c0_intr_hook`, e um `tid_at_entry` capturado na entrada do hook.

```
ASSERT no fim do hook:
  if (current_tid() != tid_at_entry)  // houve troca de thread
      assert(epilogue_writes_pc == writes_feitos_por_load_guest_context);
```

Rode o boot com `L4_Ipc` de handoff (o caso AMSS/BREW @0x10137000 citado no proprio comentario da
linha ~3489). **O teste falha hoje** se alguem reintroduzir a escrita de PC do ramo nao-handoff no
caminho de troca: o assert dispara *antes* de o guest pular para PC=0x14. Isto e' o ponto: o
controle negativo detecta o bug no instante da corrupcao, e nao 2 ms depois, no salto espurio.

---

### 30.2 Handoff de IPC: doar o processador (e a fatia de tempo) ao receptor

#### 30.2.1 A primitiva generica

[FATO VERIFICADO] `osfmk/kern/syscall_subr.c:348-384`, `thread_handoff_internal()`:

```c
		thread_t pulled_thread = thread_prepare_for_handoff(thread, option);
		...
		if (pulled_thread != THREAD_NULL) {
			int result = thread_run(self, continuation, parameter, pulled_thread);
```

e o fallback, linha 382: `int result = thread_block_parameter(continuation, parameter);`.
Isto e' a regra de ouro do handoff: **tente doar o CPU ao alvo; se nao der, bloqueie normalmente**.
Nunca "force" o alvo a rodar.

`thread_run()` (`osfmk/kern/sched_prim.c:3823`) marca o motivo como `AST_HANDOFF`:

```c
	if ((self->state & TH_IDLE) == 0) {
		reason = AST_HANDOFF;
	}
	...
	while (!thread_invoke(self, new_thread, reason)) { ... }
```

E a doacao de quantum acontece em `thread_dispatch()`, `sched_prim.c:3501-3508`:

```c
			if ((thread->reason & (AST_HANDOFF | AST_QUANTUM)) == AST_HANDOFF) {
				self->quantum_remaining = thread->quantum_remaining;
				thread->reason |= AST_QUANTUM;
				thread->quantum_remaining = 0;
			}
```

O receptor herda **o resto da fatia do emissor**, e o emissor fica com zero. Do ponto de vista do
escalonador global, a chamada IPC e' contabilizada como se fosse uma unica thread continuando a
trabalhar. [INFERENCIA] E' exatamente o que torna RPC sincrono barato e evita que um servidor
"ganhe prioridade de graca".

`thread_handoff_parameter()` (`syscall_subr.c:387`) termina com
`panic("NULL continuation passed to %s", __func__);` — [FATO VERIFICADO] o handoff com continuation
**e' proibido de retornar**. Mesmo contrato da secao 30.1.

#### 30.2.2 Entrega direta da mensagem: o emissor escreve no descritor do **receptor**

[FATO VERIFICADO] `osfmk/ipc/ipc_mqueue.c`, `ipc_mqueue_post()` (xnu-4903, linhas 741-836). O
emissor identifica a thread bloqueada e escreve **nos campos dela**:

```c
		receiver = waitq_wakeup64_identify_locked(waitq, IPC_MQUEUE_RECEIVE, THREAD_AWAKENED, ...);
		...
			receiver->ith_state = MACH_MSG_SUCCESS;
		...
			receiver->ith_kmsg = kmsg;
			receiver->ith_seqno = mqueue->imq_seqno++;
```

Nao ha nenhuma escrita em registrador de CPU aqui. A mensagem vai para `receiver->ith_kmsg`; quem
transforma isso em valor de retorno de usuario e' a **continuation do receptor**
(`ipc_mqueue_receive_results` -> `mach_msg_receive_continue` -> `thread_syscall_return`), ja' com o
receptor no CPU. Ha' inclusive um teste explicito de "esse dorminhoco sabe receber mensagem?"
(linhas 801-810):

```c
		if (receiver->ith_state != MACH_RCV_IN_PROGRESS) {
			thread_unlock(receiver); splx(th_spl); continue;   /* so' acorda, nao entrega */
		}
```

#### 30.2.3 Comparacao conceitual com `L4_Ipc` do OKL4

| Mach (XNU) | OKL4 / L4 v4 (`L4_Ipc`, syscall 0x00 no gate do Zeebo) |
|---|---|
| `mach_msg` com `MACH_SEND_MSG \| MACH_RCV_MSG` | `L4_Call` / `L4_ReplyWait` (send-phase + receive-phase numa syscall) |
| `thread_handoff` -> `thread_run` -> `AST_HANDOFF` | *switch direto* para o par de IPC, doando o tempo restante |
| mensagem em `receiver->ith_kmsg` (buffer do kernel) | mensagem nos **MRs** do UTCB do receptor (registradores virtuais) |
| `thread_syscall_return` roda **na** thread acordada | fastpath escreve o resultado nos registradores do receptor antes do `eret` |

[INFERENCIA, bem fundamentada] Sao a mesma familia semantica: *call/reply-wait com doacao de
processador*. A diferenca operacional que importa para o emulador e' onde o resultado e'
materializado. No Mach ele e' materializado **tarde** (na continuation, com a thread ja no CPU);
no L4 ele e' materializado **cedo** (no fastpath, escrevendo o banco de registradores do receptor).
Ambos concordam no ponto critico: **o resultado e' escrito no estado do destinatario, identificado
explicitamente, nunca "no que estiver no CPU agora"**.

#### APLICACAO NO ZEEBO-LLE (P3)

No ramo `case 0x00: // L4_Ipc` (~linha 3102 de `zeebo_lle_main.cpp`) o codigo hoje decide o proximo
`tid` e consulta `service_registry_.is_amss_thread(next_tid)` em quatro pontos diferentes
(linhas ~3162, 3176, 3226, 3239). Reescreva como uma funcao unica com o contrato do Mach:

```
SvcOutcome do_ipc(GuestThread& caller, ...) {
    GuestThread* dst = resolve_partner(...);
    if (!dst || !dst->waiting_for(caller.tid))     // nao ha' par pronto
        return block_caller(caller, Continuation::IpcWait);   // == thread_block_parameter
    deliver_message(/*from*/caller, /*to*/*dst);   // == ipc_mqueue_post: escreve NO dst
    dst->pending_ret_r0 = ...;                     // == receiver->ith_state / ith_kmsg
    donate_quantum(caller, *dst);                  // == AST_HANDOFF
    return SvcOutcome::SwitchedTo{dst->tid};
}
```

Invariante derivado de `ipc_mqueue_post`: **`deliver_message` so' pode escrever em campos de
`dst`**. Se precisar escrever num registrador de CPU, esta' errado — ainda nao e' a vez do `dst`.

#### TESTE QUE PODE FALHAR

Controle negativo de propriedade de estado, executavel em C++ de debug:

1. Antes de `deliver_message`, calcule `crc_caller = crc32(caller.regs)`.
2. Depois, `assert(crc32(caller.regs) == crc_caller)` — a entrega nao pode tocar no emissor.
3. Instale um `UC_HOOK_BLOCK` que registre `(tid, pc)` do primeiro bloco executado apos o handoff.
   `assert(pc == dst.pc)` com o mesmo `dst.tid` devolvido pelo `SwitchedTo`.

Falha esperada **hoje** no caso 2 se alguem restaurar `if (ip) uc_reg_write(uc, UC_ARM_REG_SP,
&ip);` no ramo de handoff (linha ~3495 faz isso no ramo irmao): o SP do emissor sobrescreve o SP da
thread destino e o guest salta para lixo — o PC=0x14 observado. O teste 3 pega a mesma falha pelo
outro lado.

---

### 30.3 Estado de maquina por thread em ARM32: `arm_saved_state`, bit T e primeira ativacao

#### 30.3.1 Onde o CPSR mora

[FATO VERIFICADO] O estado de usuario de uma thread vive em `thread->machine.PcbData`, do tipo
`struct arm_saved_state` (`osfmk/arm/thread.h`, `osfmk/mach/arm/thread_status.h`), com os campos
`r[13]`, `sp`, `lr`, `pc`, `cpsr` (alem de `fsr`/`far`). `osfmk/arm/status.c:304`,
`machine_thread_set_state()`, mostra a regra de seguranca:

```c
			old_psr = saved_state->cpsr;
			memcpy((char *) saved_state, (char *) state, sizeof(*state));
			/* do not allow privileged bits of the PSR to be changed */
			saved_state->cpsr = (saved_state->cpsr & ~PSR_USER_MASK) | (old_psr & PSR_USER_MASK);
```

Traduzindo: o CPSR de uma thread e' **um campo persistente do descritor**, e o kernel so' deixa o
usuario mexer nos bits de usuario. **Nunca** se re-deriva o modo por heuristica.

#### 30.3.2 ARM vs Thumb: o bit T e' dado, nao palpite

Tres pontos independentes, todos [FATO VERIFICADO]:

**(a) Definicao.** `osfmk/arm/proc_reg.h:257`:

```c
#define PSR_TF			0x00000020	/* thumb flag (BX ARMv4T) */
#define PSR_TFb			         5	/* thumb flag (BX ARMv4T) */
```

**(b) Entrada de um novo contexto: o bit 0 do endereco vira o bit T.** `bsd/dev/arm/unix_signal.c`,
`sendsig_set_thread_state32()` (linhas 193-205):

```c
	if (trampact & 1) {
		regs->pc = trampact & ~1;
		regs->cpsr = PSR_USERDFLT | PSR_TF;
	} else {
		regs->pc = trampact;
		regs->cpsr = PSR_USERDFLT;
	}
```

Este e' o *unico* lugar onde o XNU "decide" ARM vs Thumb, e ele decide pela **convencao `BX`**:
LSB=1 => Thumb. O mesmo idioma aparece no tratamento de fault recovery,
`osfmk/arm/trap.c:437-438`:

```c
				if (recover != 0) {
					regs->pc = (register_t) (recover & ~0x1);
					regs->cpsr = (regs->cpsr & ~PSR_TF) | ((recover & 0x1) << PSR_TFb);
```

**(c) Largura da instrucao: derivada do bit T, e quando preciso, da decodificacao.**
`osfmk/arm/trap.c:726-730` (fim de `sleh_alignment`, avancar para a proxima instrucao):

```c
	if (rc == KERN_SUCCESS) {
		if (regs->cpsr & PSR_TF)
			regs->pc += 2;
		else
			regs->pc += 4;
```

E, quando 2 nao basta (Thumb-2 de 32 bits), o kernel **le a instrucao** em vez de chutar,
`osfmk/arm/trap.c:397`:

```c
					regs->pc += ((regs->cpsr & PSR_TF) && !IS_THUMB32(*((uint16_t*) (regs->pc)))) ? 2 : 4;
```

O mesmo padrao "le 16 bits, testa `IS_THUMB32`, recarrega 32 bits" esta em `sleh_undef`
(`trap.c:172-185`). Resumo da regra XNU: **modo vem do CPSR salvo; largura vem do modo + do opcode
lido; nada vem de faixa de endereco.**

Um contra-exemplo honesto, tambem [FATO VERIFICADO]: `bsd/dev/arm/systemcalls.c:284-285`
faz `if (error == ERESTART) { ss32->pc -= 4; }` — constante fixa, sem olhar `PSR_TF`. [INFERENCIA]
Isso so' e' seguro porque o stub de syscall Unix do `libsystem_kernel` em armv7 executa a `svc` em
estado ARM; para uma `svc` Thumb de 2 bytes esse `-4` estaria errado. Serve de aviso: mesmo o XNU
tem um ponto onde a largura da SVC e' assumida, e e' precisamente o tipo de suposicao que o
`zeebo-lle` esta pagando caro no P2.

#### 30.3.3 Primeira ativacao (P4): qual e' o estado minimo

[FATO VERIFICADO] `osfmk/arm/pcb.c:222-244`, `machine_stack_attach()` — a thread que nunca rodou
recebe **exatamente cinco** campos, e nada mais:

```c
	savestate->lr = (uint32_t) thread_continue;
	savestate->sp = thread->machine.kstackptr;
	savestate->r[7] = 0x0UL;
	savestate->r[9] = (uint32_t) NULL;
	savestate->cpsr = PSR_SVC_MODE | PSR_INTMASK;
```

Lendo junto com `Load_context`/`Switch_context` (`osfmk/arm/cswitch.s`, `ldmia r3!, {r4-r14}`), o
"contrato de primeira ativacao" do XNU/ARM e':

| Item | Valor inicial | Por que |
|---|---|---|
| PC efetivo (via `lr`) | `thread_continue` | ponto de entrada canonico, nao "o que tinha antes" |
| SP | `kstackptr` | pilha propria, jamais herdada |
| CPSR | `PSR_SVC_MODE \| PSR_INTMASK` | **modo e mascara de IRQ explicitos**, T=0 implicito |
| frame pointer (`r7`) | 0 | corta o backtrace, evita seguir lixo |
| `r9` | NULL | registrador de convencao zerado |
| TLS | escrito no switch, nao aqui | ver abaixo |

Para o estado de **usuario** o analogo e' `machine_thread_state_initialize()`
(`osfmk/arm/status.c:464-471`): `bzero()` de todo o `arm_saved_state` e depois
`savestate->cpsr = PSR_USERDFLT;`. Zero e' estado invalido para quase tudo, entao o CPSR e'
reescrito explicitamente. Repare: **o kernel nunca deixa o CPSR sair do `bzero`**.

TLS/"UTCB" [FATO VERIFICADO] `osfmk/arm/cswitch.s`, `machine_load_context` e `Switch_context`
escrevem **tres** registradores CP15 a cada ativacao:

```asm
	mcr		p15, 0, r0, c13, c0, 4		// Write TPIDRPRW  (thread atual, so' kernel)
	...
	mcr		p15, 0, r1, c13, c0, 3		// Write TPIDRURO  (cthread self | numero da CPU)
	...
	mcr		p15, 0, r1, c13, c0, 2		// Write TPIDRURW  (TLS de usuario)
```

Ou seja: identidade da thread (`TPIDRPRW`), TLS somente-leitura (`TPIDRURO`) e TLS gravavel
(`TPIDRURW`) fazem parte do contexto trocado, **no mesmo instante** que os registradores.

#### APLICACAO NO ZEEBO-LLE (P1 e P4)

**P1 — mate o `apply_tbit` por faixa.** O lambda de `zeebo_lle_main.cpp:3471` decide Thumb por
`pc >= 0xb0100000 && pc < 0xb0120000` etc. Substitua por um campo `cpsr` real em `GuestThread`,
alimentado por duas fontes legitimas:

1. **Na primeira ativacao**: o OKL4 entrega o entry point via `L4_ExchangeRegisters` (syscall 0x0c,
   ja' tratada na linha ~3500). Aplique a regra do `sendsig_set_thread_state32`:
   `t.cpsr = (entry & 1) ? (BASE | PSR_TF) : BASE; t.pc = entry & ~1u;`.
2. **Numa syscall**: o CPSR do chamador e' o CPSR *corrente* do Unicorn no momento do
   `UC_HOOK_INTR`. Leia-o (`uc_reg_read(uc, UC_ARM_REG_CPSR, ...)`) e **salve-o** em
   `t.cpsr_at_svc`. E' o `SPSR_svc` que o hardware salvaria e que hoje ninguem salva (ver a tabela
   da secao 0 do documento). Na retomada, reponha `pc | (t.cpsr_at_svc & PSR_TF ? 1 : 0)`.

Isso resolve por construcao o caso citado no comentario da linha ~3445 ("instrucao ARM 0xe51ff004
lida como Thumb"): se o chamador era ARM, `cpsr_at_svc & 0x20 == 0`, e a retomada e' ARM — sem
saber em que regiao ele estava.

**P2 — largura da SVC.** Copie a regra do `trap.c`: o hook recebe `pc` ja' em `svc+4` *segundo a
convencao do Unicorn*; valide em vez de confiar. Com `cpsr_at_svc` em maos:

```
u32 svc_addr = (cpsr_at_svc & PSR_TF) ? (pc_hook - 2) : (pc_hook - 4);
u32 op; uc_mem_read(uc, svc_addr, &op, (cpsr_at_svc & PSR_TF) ? 2 : 4);
assert((cpsr_at_svc & PSR_TF) ? ((op & 0xFF00) == 0xDF00)      // Thumb SVC
                              : ((op & 0x0F000000) == 0x0F000000); // ARM SVC
```

**P4 — contrato de primeira ativacao.** Crie `GuestThread::activate_first_time(entry, stack, utcb)`
que preenche, nessa ordem e sem excecao: `pc`, `sp`, `cpsr` (modo USR + mascara de IRQ + bit T
derivado de `entry&1`), `r[0..12] = 0`, `lr = 0`, e o registrador de UTCB. **Sem campo herdado.**
No OKL4/ARM o ponteiro de UTCB e' lido de um registrador de thread (o analogo de `TPIDRURO`) —
confirme o registrador exato na secao 30.5 antes de fixar; se nao confirmado, marque
`utcb_reg = NAO VERIFICADO` no codigo em vez de chutar.

#### TESTE QUE PODE FALHAR

Dois controles negativos, ambos baratos:

1. **Coerencia T-bit x opcode.** Em toda retomada de syscall, antes do `uc_reg_write(PC)`:
   leia 4 bytes em `target_pc & ~1u`. Se `target_pc & 1` (Thumb) e os 16 bits lidos casarem
   com um encoding ARM *condicional impossivel* em Thumb (por exemplo `0xe51ff004` = `ldr pc,[pc,#-4]`),
   dispare `abort()`. Hoje, com o `apply_tbit` por faixa, esse assert **dispara** para qualquer
   chamador ARM fora das quatro janelas hardcoded (0xb0000000, 0xb0100000, 0xb0300000, 0x103dc000)
   — que e' exatamente o bug que o QW102 remendou faixa a faixa.
2. **Primeira ativacao sem heranca.** Preencha o `GuestThread` novo com `0xDEADBEEF` em todos os
   campos antes de `activate_first_time`. Se qualquer registrador ainda valer `0xDEADBEEF` quando o
   guest executar o primeiro bloco (cheque num `UC_HOOK_BLOCK` de uma unica vez), falhe. Isso
   captura o caso "thread nova herdou SP da thread anterior", que e' a variante silenciosa do P4.

---

### 30.4 Retorno de trap: PC e CPSR sao um par indivisivel

#### 30.4.1 ARM32: `srs` na entrada, `movs pc, lr` na saida

[FATO VERIFICADO] `osfmk/arm/locore.s`, `fleh_swi` (entrada de SVC), linhas 524-542:

```asm
swi_from_user:
	mrc		p15, 0, sp, c13, c0, 4		// Read TPIDRPRW
	add		sp, sp, ACT_PCBDATA		// Get User PCB
	...
	stmia	sp, {r0-r12, sp, lr}^		// Save user context on PCB
	...
	add		sp, sp, SS_PC
	srsia	sp, 	#PSR_SVC_MODE
```

Dois detalhes que o `zeebo-lle` nao faz hoje:

* `stmia sp, {r0-r12, sp, lr}^` — o `^` salva o **SP e LR bancados de usuario**, nao os do modo
  SVC. Modo e banco de registradores andam juntos.
* `srsia sp, #PSR_SVC_MODE` grava **`LR_svc` e `SPSR_svc` de uma vez** nos campos `SS_PC` e
  `SS_CPSR` do `arm_saved_state`. PC de retorno e CPSR de retorno sao gravados pelo **mesmo
  instrucao**, na ordem que a struct define. Nunca existe um instante em que um foi salvo e o outro
  nao.
* Consequencia importante para o P2: `LR_svc` ja' e' "o endereco da proxima instrucao" calculado
  **pelo hardware**, com a largura correta (ARM: `svc+4`, Thumb: `svc+2`). O XNU **nunca soma +4**
  no caminho normal de retorno de syscall. A unica aritmetica de PC no kernel esta nos caminhos de
  *reexecucao/skip* (`trap.c`, `systemcalls.c`), e la' ela consulta `PSR_TF`.

A saida, `thread_exception_return` / `load_and_go_user` (`locore.s:1851-1936`):

```asm
	add		r0, r9, ACT_PCBDATA		// Get User PCB
	ldr		r4, [r0, SS_CPSR]		// Get saved cpsr
	and		r3, r4, #PSR_MODE_MASK		// Extract current mode
	cmp		r3, #PSR_USER_MODE		// Check user mode
	movne	r0, r3
	bne		EXT(ExceptionVectorPanic)
	msr		spsr_cxsf, r4			// Restore spsr(user mode cpsr)
	...
	ldr		lr, [sp, SS_PC]			// Restore user mode pc
	ldmia	sp, {r0-r12, sp, lr}^		// Restore the other user mode registers
	nop					// Hardware problem
	movs	pc, lr				// Return to user
```

Tres fatos para roubar:

1. **Verificacao antes de retornar**: se o CPSR salvo nao esta em modo usuario, o kernel
   **entra em panico** (`ExceptionVectorPanic`) em vez de retornar para um estado incoerente.
   Este e' um assert de producao, nao de debug.
2. **`movs pc, lr`** copia `SPSR -> CPSR` **e** `lr -> PC` **na mesma instrucao**. Modo, bit T,
   flags de condicao e endereco mudam atomicamente. Nao existe janela em que o PC ja' e' o novo e o
   T ainda e' o velho — que e' precisamente a janela onde o `zeebo-lle` vive hoje.
3. O `nop` com o comentario `// Hardware problem` mostra o nivel de cuidado exigido nessa sequencia
   [INFERENCIA: erratum de core especifico; nao identifiquei qual — NAO VERIFICADO].

#### 30.4.2 ARM64: `ELR_EL1` + `SPSR_EL1` + `eret`, e a assinatura PAC do par

[FATO VERIFICADO] `osfmk/arm64/locore.s`, `Lexception_return_restore_registers` (linhas 1000-1010):

```asm
	mov 	x0, sp					// x0 = &pcb
	// Loads authed $x0->ss_64.pc into x1 and $x0->ss_64.cpsr into w2
	AUTH_THREAD_STATE_IN_X0	x20, x21, x22, x23, x24, el0_state_allowed=1
	...
	msr		ELR_EL1, x1			// Load the return address into ELR
	msr		SPSR_EL1, x2			// Load the return CPSR into SPSR
```

e o `ERET_CONTEXT_SYNCHRONIZING` na linha 1092, que restaura PC e PSTATE juntos.

O detalhe mais forte: em hardware com PAC, a Apple **assina criptograficamente o par**.
`osfmk/arm64/machine_routines_asm.s:1062-1073`, macro `COMPUTE_THREAD_STATE_HASH`:

```asm
	pacga	x1, x1, x0		/* pc hash, diversificado por &arm_saved_state_t */
	bic		x2, x2, PSR_CF
	pacga	x1, x2, x1		/* SPSR hash (gkey + pc hash) */
	pacga	x1, x3, x1		/* LR Hash (gkey + spsr hash) */
	pacga	x1, x4, x1		/* X16 hash */
	pacga	x1, x5, x1		/* X17 hash */
```

`{pc, cpsr, lr, x16, x17}` viram **um unico hash encadeado**, amarrado ao endereco da propria
struct. Alterar o PC sem alterar o CPSR (ou vice-versa) e' literalmente indetectavel de fazer: o
`eret` falha a verificacao. [FATO VERIFICADO] O `bic x2, x2, PSR_CF` com o comentario *"Mask off the
carry flag so we don't need to re-sign when that flag is touched by the system call return path"*
mostra que o **unico** bit do CPSR que o epilogo de syscall pode tocar e' o carry (convencao de erro
do BSD, `systemcalls.c:291`: `ss32->cpsr |= PSR_CF;`). Todo o resto do CPSR e' imutavel no retorno.

#### 30.4.3 Troca de espaco de enderecos amarrada ao retorno (P6)

[FATO VERIFICADO] Ainda em `load_and_go_user` (`locore.s:1926-1931`), sob `__ARM_USER_PROTECT__`:

```asm
	ldr     r3, [r9, ACT_UPTW_TTB]		// Load thread ttb
	mcr		p15, 0, r3, c2, c0, 0		// Set TTBR0
	ldr		r2, [r9, ACT_ASID]		// Load thread asid
	mcr		p15, 0, r2, c13, c0, 1		// Set CONTEXTIDR
	isb
```

e o simetrico na entrada (`locore.s:553-558`: TTBR0 = ttb do kernel, `CONTEXTIDR` = ASID 0).
O `pmap` da thread ja' foi trocado antes, no nivel C: `osfmk/arm/pcb.c:101`
(`machine_switch_context`) e `osfmk/arm/pcb.c:271` (`machine_stack_handoff`) chamam
`pmap_set_pmap(new->map->pmap, new)`. Ordem canonica: **pmap no switch, TTBR0/ASID no retorno,
`isb` antes de executar codigo de usuario**.

#### APLICACAO NO ZEEBO-LLE (P2, P1, P5, P6)

Concentre **todo** retorno ao guest numa unica funcao, o `exception_return` do projeto:

```cpp
static void guest_exception_return(uc_engine* uc, GuestThread& t) {
    // 1. par indivisivel: CPSR primeiro, PC depois, sempre os dois
    uc_reg_write(uc, UC_ARM_REG_CPSR, &t.cpsr);
    u32 pc = (t.cpsr & 0x20u) ? (t.pc | 1u) : (t.pc & ~1u);  // convencao BX do Unicorn
    // 2. assert de producao, estilo ExceptionVectorPanic
    assert((t.cpsr & 0x1Fu) == 0x10u /*USR*/ || (t.cpsr & 0x1Fu) == 0x13u /*SVC*/);
    // 3. banco de registradores completo da thread, inclusive SP/LR
    load_regs(uc, t);
    // 4. invalidacao de TB derivada do PC que estamos escrevendo (P5)
    uc_ctl_remove_cache(uc, t.pc & ~1u, 0x40);
    uc_reg_write(uc, UC_ARM_REG_PC, &pc);
}
```

Notas de ancoragem:

* **P5**: as chamadas `uc_ctl_remove_cache(uc, 0xb000c720, 0x100)` etc. (linhas ~3483-3485,
  3497, 3530, 3544, 3555 de `zeebo_lle_main.cpp`) sao enderecos **hardcoded**. O analogo no XNU e'
  `icache_invalidate_trap` (`locore.s:707-728`), que **calcula** a faixa a partir dos argumentos
  (`add r3, r0, r1`), valida contra `VM_MAX_ADDRESS`, e instala um recovery handler
  (`str r11, [r9, TH_RECOVER]`) antes de tocar em cache. Regra a importar: *a invalidacao e' funcao
  do estado que voce acabou de escrever, nunca uma constante*. Como o Unicorn reescreve PC/CPSR,
  invalide `[novo_pc-4, novo_pc+0x40)` e, no caso de handoff, tambem a faixa do PC antigo.
* **P6**: se/quando o `zeebo-lle` emular `L4_MapControl` (ramo `syscall == 0x14`, linha ~3327) com
  espacos separados, siga a ordem do XNU — trocar o "pmap" no switch de thread e programar
  TTBR0/ASID **no retorno**, imediatamente antes de escrever o PC, com um `isb` conceitual (no
  Unicorn: `uc_ctl_remove_cache` da faixa + `uc_ctl_flush_tlb` se o mapeamento mudou).
  A invalidacao seletiva de TLB por ASID no OKL4: **NAO VERIFICADO** nesta sessao.

#### TESTE QUE PODE FALHAR

*Teste do par indivisivel.* Adicione um contador `cpsr_writes` e `pc_writes`, incrementados por
wrappers `write_cpsr()`/`write_pc()` usados em todo o `zeebo_lle_main.cpp`, e proiba escrita crua
de PC (macro `#define uc_reg_write_pc_FORBIDDEN`). Ao fim de cada `UC_HOOK_INTR`:

```
assert(pc_writes == cpsr_writes);                 // nunca um sem o outro
assert(pc_writes <= 1);                            // exatamente um retorno por trap
```

**Hoje esse assert falha imediatamente**: o ramo `syscall == 0xb4` (linha ~3474) escreve PC sem
tocar em CPSR, e o comentario da linha ~3452 admite que setar CPSR "nao sobrevive" porque o PC e'
reescrito depois. O teste transforma esse comentario numa falha dura e reproduzivel.

*Teste de coerencia modo/CPSR (estilo `ExceptionVectorPanic`).* Antes de todo retorno, se
`(cpsr & 0x1F)` nao for USR nem SVC, `abort()` com dump de `tid/pc/cpsr`. Controle negativo:
force `cpsr = 0x00000000` num teste unitario e confirme que aborta — se nao abortar, o assert esta
no lugar errado (depois do write de PC, por exemplo).

---
### 30.5 seL4 e o proprio OKL4: o fastpath de IPC e o que ele **exige** do chamador

Esta secao compara o padrao Mach com a familia L4 — inclusive com o codigo do **proprio kernel que o
Zeebo roda**, em `refs/okl4-2.1.1-fix7`. A leitura dos arquivos foi feita nesta sessao (delegada ao
sub-agente `sel4-okl4-facts`, que produziu 61 fatos com caminho e linha); os pontos criticos abaixo
eu reverifiquei pessoalmente com `sed -n` nos arquivos citados.

**Licencas.** OKL4 2.1.1 / Pistachio: derivado do L4Ka::Pistachio, licenca BSD de 2 clausulas
(permissiva); ainda assim, **o objetivo aqui e' documentar comportamento observavel para emular, nao
importar codigo**. seL4: [FATO VERIFICADO] `LICENSE.md` do repo — *"kernel-level code is licensed
under GPLv2 and user-level code under the 2-clause BSD license"*, com cabecalhos
`SPDX-License-Identifier: GPL-2.0-only` em `src/fastpath/fastpath.c`, `src/api/syscall.c`,
`src/arch/arm/32/c_traps.c`. **Nao copie codigo do seL4 para o `zeebo-lle`.** A arquitetura pode ser
reimplementada; o texto do kernel, nao.

#### 30.5.1 OKL4 2.1.1 no ARM: a ABI real que o Zeebo usa

[FATO VERIFICADO] `arch/arm/libs/l4/src/ipc.spp`, label `L4_Ipc` (linhas 73-93) — o stub de usuario:

```asm
LABEL(L4_Ipc)
        stmfd   sp!, {r1,r3-r11, lr}
        mov     fp, #0xff000000
        ldr     fp, [fp, #0xff0]      /* ponteiro do UTCB */
        ...
        mov     ip, sp
        mov     sp, #SYSNUM(ipc)      /* SYSBASE 0xffffff00 + SYSCALL_ipc(0) */
        swi     SWINUM(ipc)           /* SWIBASE 0x1400 */
```

Confirma tres coisas que o `zeebo_lle_main.cpp` ja' descobriu empiricamente (comentarios das linhas
3063-3078: *"a identidade da syscall vem do SP, nao do imediato"*), agora com a fonte:

1. **O numero da syscall viaja em `sp`**, e o SP verdadeiro do chamador viaja em `ip` (r12).
   [FATO VERIFICADO] `arch/arm/pistachio/v5/src/traps.spp:244-256` (`arm_swi_syscall`) confirma do
   lado do kernel: `cmp lr, #(0xffffff00 + SYSCALL_ipc)` — o kernel **nao le o imediato do `swi`**,
   ele compara o valor que veio no SP de usuario.
2. **O UTCB e' lido de uma palavra fixa**, `*(u32*)0xff000ff0` (pagina `USER_UTCB_PAGE`
   `0xff000000`, offset `0xff0`). [FATO VERIFICADO] `v5/src/traps.spp:740,771` e
   `arch/arm/pistachio/v5/src/thread.cc:78-97` mostram o kernel **escrevendo** esse ponteiro no
   switch de thread (`str tmp5, [tmp1, #0xff0]`). Em ARMv6 (`v6/src/traps.spp:735-737`) o
   `TPIDRURW` e' **zerado** por seguranca — o UTCB **nao** esta em TLS.
3. MR0-MR5 viajam em `r3-r8`; `r0 = to_tid`, `r1 = from_tid`
   [FATO VERIFICADO `v5/src/traps.spp:261-276`].

**Contexto de syscall: quatro palavras.** [FATO VERIFICADO]
`arch/arm/pistachio/include/thread.h:136-150`:

```c
class arm_syscall_context_t { u32_t sp; u32_t lr; u32_t pc; u32_t cpsr; };
#define SC_SP 0 / SC_LR 4 / SC_PC 8 / SC_CPSR 12
#define ARM_SYSCALL_STACK_SIZE  16
```

**Este e' o estado minimo que o `zeebo-lle` precisa manter por thread para IPC.** Nao e' opiniao:
e' a struct do kernel que ele emula. E o retorno, `v5/src/traps.spp:1441-1465` (`syscall_return`),
mostra a ordem exata:

```asm
        ldr     r12,    [sp, #SC_CPSR]    /* Get user CPSR */
        ldmia   sp,     {sp}^             /* restaura SP banked de usuario */
        ...
        ldr     lr,     [sp, #SC_PC]      /* Get user PC */
        msr     spsr_cxsf,      r12
        ...
        moveqs  pc,     lr                /* SPSR->CPSR e lr->PC, juntos */
```

O `ipc_return_user` do fastpath (`v5/src/traps.spp:799-830`) faz a mesma sequencia. **O OKL4 e' tao
estrito quanto o XNU: o CPSR salvo e' reposto no SPSR e o par sai junto no `movs pc, lr`.**

**ARM vs Thumb no OKL4** [FATO VERIFICADO]:

* `arch/arm/pistachio/include/thread.h:69-82`: `CPSR_THUMB_BIT 0x20`,
  `ARM_USER_FLAGS_MASK 0xf8000020` (v5).
* `v5/src/traps.spp:680-694` (reply de exception IPC) converte **bit 0 do PC <-> bit T do CPSR**:
  `tst tmp1, #1 / orrne tmp2, tmp2, #0x20 / biceq tmp2, tmp2, #0x20`. Mesmissima regra do
  `sendsig_set_thread_state32` do XNU (secao 30.3.2).
* `v5/src/traps.spp:180-191` (`arm_undefined_inst_exception`) calcula o PC da falha com
  `sub lr,lr,#3 / tst r13,#CPSR_THUMB_BIT / addne lr,lr,#2` — largura por bit T, nunca por regiao.
* Contraponto honesto [FATO VERIFICADO + INFERENCIA]: `v5/src/traps.spp:1735-1742`
  (`arm_swi_exception`) le a instrucao `swi` com `ldr tmp5, [lr, #-4]` — assume 4 bytes, portanto
  **esta errado para SWI em Thumb**, exatamente como o seL4 admite no proprio comentario (abaixo).

**Troca de thread por continuacao — o OKL4 tambem faz.** [FATO VERIFICADO]
`pistachio/include/tcb.h:646,677`: `void switch_from(tcb_t*, continuation_t)` e
`void switch_to(tcb_t *dest, tcb_t *schedule) NORETURN`, com o comentario *"At the conclusion of
this function, execution will pass to the destination's continuation, stored in 'cont'"*.
`arch/arm/pistachio/v5/src/thread.cc:78-97` (`asm_switch_to`) declara clobber de
`r0-r3, r6-r11` e `memory`: **nenhum registrador de C e' salvo**. `v5/src/traps.spp:754-760` mostra
o fastpath guardando `fast_path_recover` em `tcb->cont`, e o destino sendo retomado pelo **cont
dele** (`ldr r11,[to_tcb,#OFS_TCB_CONT]; jump r11`, linhas 1138/1153).

> Conclusao com peso para o projeto: **o kernel que o Zeebo executa usa o mesmo padrao de
> continuation do Mach.** O `zeebo-lle` esta emulando um kernel de continuations com um emulador que
> assume retorno linear de syscall. O P3 nao e' um bug de detalhe; e' incompatibilidade de modelo.

#### 30.5.2 seL4: o fastpath e o contrato com o chamador

[FATO VERIFICADO] `src/fastpath/fastpath.c`, `fastpath_call()` (linha 19). O que ele **nao** faz:
nao chama `schedule()`, nao chama `activateThread()`, nao enfileira o destino, nao salva contexto de
C. Compare com `src/api/syscall.c:562-666` (`handleSyscall`), que **sempre** termina em
`schedule(); activateThread();`.

O que o fastpath exige do chamador (todas as condicoes testadas **antes** do comentario
`POINT OF NO RETURN`, linhas 167-171):

| Exigencia | Onde |
|---|---|
| mensagem curta, sem caps extras (`length <= 4`, `msgExtraCaps == 0`) | `fastpath_mi_check`, l.40-43 |
| nenhuma falha pendente (`seL4_Fault_NullFault`) | l.40-43 |
| cap e' endpoint com direito de envio | l.49-52 |
| ha' receptor bloqueado em `EPState_Recv` | l.62-64 |
| VTable valida e **ASID de hardware valido** (AArch32) | l.80-82, l.138-142 |
| prioridade do destino >= a do chamador (ou destino e' a maior) | l.126-129 |
| mesmo dominio de escalonamento; mesma CPU em SMP | l.145-147, l.160-165 |

Se qualquer uma falhar: `slowpath(SysCall)`. **Este e' o padrao a copiar**: o caminho rapido e' uma
*otimizacao com guarda*, nunca a unica implementacao.

A transicao propriamente dita [FATO VERIFICADO]:

* l.188-189: chamador vira `ThreadState_BlockedOnReply` (`thread_state_ptr_set_tsType_np`).
* l.226-227: destino vira `ThreadState_Running`, com o comentario *"Dest thread is set Running, but
  not queued."* — **rodando sem passar pela runqueue**: e' a doacao.
* l.195-206 (MCS): doacao **explicita** do sched context —
  `sc->scTcb = dest; dest->tcbSchedContext = sc; ksCurThread->tcbSchedContext = NULL;`.
* l.223: `fastpath_copy_mrs(length, ksCurThread, dest)` — so' os MRs em registrador.
* l.228: `switchToThread_fp(dest, cap_pd, stored_hw_asid)` — troca de MMU
  (`armv_contextSwitch_HWASID`), `ksCurThread = thread`, e `clearExMonitor_fp()` (um `strex` para
  **invalidar o monitor exclusivo** — detalhe que emulador esquece).
* l.230-232: `fastpath_restore(badge, msgInfo, NODE_STATE(ksCurThread))` — e `ksCurThread` **ja' e' o
  destino**.

E a saida, em `include/arch/arm/arch/32/mode/fastpath/fastpath.h` [FATO VERIFICADO]:

```c
	register word_t badge_reg asm("r0") = badge;
	register word_t msgInfo_reg asm("r1") = msgInfo;
	register word_t cur_thread_reg asm("r2") = (word_t)cur_thread->tcbArch.tcbContext.registers;
	... "mov sp, r2 ; add sp, sp, %[LR_SVC_OFFSET] ; ldmdb sp, {r2-lr}^ ; rfeia sp"
```

Leitura: **r0 e r1 (badge/msgInfo) sao o valor de retorno do RECEPTOR**, `r2-lr` vem do contexto de
usuario do receptor, e `rfeia sp` carrega **PC e CPSR juntos** de duas palavras consecutivas
(`registerset.h`: `NextIP = 15`, `CPSR = 16`). Na entrada, `src/arch/arm/32/traps.S:50-64` usa
`srsia #PMODE_SUPERVISOR` — a instrucao simetrica, que salva as duas de uma vez.

No caminho lento o mesmo efeito e' obtido escrevendo no TCB do receptor,
`src/kernel/thread.c:227-228` (`doNormalTransfer`):

```c
	setRegister(receiver, msgInfoRegister, wordFromMessageInfo(tag));
	setRegister(receiver, badgeRegister, badge);
```

**Nunca no chamador.** E' a mesma lei do `ipc_mqueue_post` do Mach (secao 30.2.2).

Admissao explicita de limitacao, que vale ouro como precedente [FATO VERIFICADO]
`src/arch/arm/32/traps.S:55-57`, comentario sobre `sub lr, lr, #4`:

> *"the FaultIP address, which in ARM mode is the NextIP - 4. NOTE: This is completely wrong and
> broken in thumb mode."*

Ou seja: **dois kernels reais da familia L4 (OKL4 e seL4) tem o mesmo bug de largura de SVC que o
P2 descreve**, e o seL4 documenta isso no fonte. O `zeebo-lle` nao pode copiar essa preguica, porque
o guest do Zeebo (BREW/AMSS) e' majoritariamente Thumb — e' justamente o caso que os dois kernels
declaram nao suportar.

#### APLICACAO NO ZEEBO-LLE (P1, P2, P3, P4, P6)

1. **Adote a struct do kernel real (P4).** `GuestThread` deve conter, no minimo, o
   `arm_syscall_context_t` do OKL4: `{sp, lr, pc, cpsr}` — quatro palavras — mais o ponteiro de
   UTCB. Nao invente campos; use os offsets `SC_SP/SC_LR/SC_PC/SC_CPSR` como documentacao.
2. **UTCB nao e' TLS (P4).** O ponteiro do UTCB corrente do guest vive em `*(u32*)0xff000ff0`.
   No handoff de `L4_Ipc`, o emulador **tem** de escrever esse endereco de memoria com o UTCB da
   thread que vai rodar, do mesmo jeito que `asm_switch_to` faz (`str %2, [r12, #0xff0]`). Se isso
   nao for feito hoje, o servidor destino le' os MRs do UTCB **do emissor** — um bug latente
   independente do P3. Verificacao rapida: `grep -n '0xff000ff0\|0xff0' tools/cpp/zeebo_lle_main.cpp`.
3. **Fastpath com guarda (P3).** Implemente `try_ipc_fastpath()` com a lista de quedas do seL4
   adaptada: (a) existe thread destino? (b) ela esta em estado de espera por este endpoint/tid?
   (c) mensagem cabe nos MRs em registrador (`r3-r8`)? (d) mesmo espaco de enderecos ou mapeamento
   valido? Se qualquer uma falhar -> caminho lento (enfileirar e bloquear). Isso substitui a flag
   `did_handoff` por uma decisao explicita e auditavel.
4. **Valores de retorno vao para o destino (P3).** `r0 = sent_from` e MR0-MR5 em `r3-r8` devem ser
   escritos **no contexto do destino**, nunca no CPU antes do switch —
   cf. `v5/src/traps.spp:1420-1436` (`ipc_syscall_return`: recarrega MRs do UTCB e poe
   `tcb->sent_from` em `r0`) e `v5/src/traps.spp:1126-1148` (*"Save ONLY valid registers - avoid
   trashing preserved MRs"*).
5. **Espacos de enderecos (P6).** O analogo de `switchToThread_fp` no OKL4 v5 e' o FCSE PID
   (`v5/src/traps.spp:764-765`: `mov tmp3,tmp3,LSL #23; mcr p15,0,tmp3,c13,c0`) e em v6 o
   Context ID/ASID (`v6/src/traps.spp:752`, CP15 c13 c0 op2=1, mais c2 c0 0 = TTBR). Se o Zeebo usa
   MSM7201A (ARM1136, v6), **o caminho v6 e' o relevante**: a troca de espaco e' TTBR0 + CONTEXTIDR,
   e deve acontecer no switch, antes de reescrever o PC.

#### TESTE QUE PODE FALHAR

1. **Teste do UTCB do destino.** Antes de todo handoff, grave `utcb_before = mem32(0xff000ff0)`.
   Depois do switch, `assert(mem32(0xff000ff0) == dst.utcb && mem32(0xff000ff0) != utcb_before)`
   (quando `dst.utcb != caller.utcb`). **Se o `zeebo-lle` hoje nunca escreve `0xff000ff0`, este
   teste falha na primeira troca real** — e o que ele revela e' um bug distinto do PC=0x14.
2. **Teste do guarda do fastpath.** Force artificialmente `length = 8` MRs (> os 6 que cabem em
   `r3-r8`) num teste dirigido. O emulador deve cair no caminho lento. Se ele fizer handoff assim
   mesmo, os MRs 6-7 chegam corrompidos no servidor — falha silenciosa que so' aparece horas depois.
   O controle negativo torna a falha imediata.
3. **Teste de largura de SVC em Thumb (o bug que os dois kernels admitem).** Injete no guest, num
   teste unitario, uma sequencia Thumb `svc #0x14` seguida de instrucao conhecida. Verifique que o
   emulador retoma em `svc+2` e nao em `svc+4`. Hoje o hook usa `pc` (ja' `svc+4` pela convencao do
   Unicorn) sem checar largura (comentarios das linhas 3475, 3494, 3538, 3549): o teste falha, e
   falha exatamente no caso que o BREW/AMSS mais exercita.

---

### 30.6 Resumo: qual tecnica ataca qual problema

| Tecnica (fonte) | Problema | Regra de uma linha |
|---|---|---|
| `thread_block_parameter` + `/*NOTREACHED*/` (`sched_prim.c:3744-3790`, `3063`) | **P3** | Quem bloqueia nunca executa o epilogo da syscall. |
| `Switch_context` com `teq r1,#0` (`arm/cswitch.s`) e `switch_from/switch_to` do OKL4 (`tcb.h:646,677`) | **P3, P4** | Com continuation nao se salva registrador nenhum; o estado vive no descritor. |
| `thread_syscall_return` le `TPIDRPRW` (`arm/locore.s:1821`) | **P3** | O valor de retorno e' escrito na thread que **esta** no CPU, achada pelo ponteiro de thread corrente. |
| `ipc_mqueue_post` escreve `receiver->ith_*` (`ipc_mqueue.c:823-836`); `thread.c:227-228` do seL4 | **P3** | A mensagem e' entregue no descritor do receptor, nunca em registrador de CPU. |
| `AST_HANDOFF` + doacao de quantum (`sched_prim.c:3501-3508`); `switchToThread_fp` sem `schedule()` | **P3** | Handoff e' *tentativa*: se falhar, bloqueia normalmente. |
| `trampact & 1` -> `PSR_TF` (`unix_signal.c:193-204`); `tst tmp1,#1 / orrne #0x20` (`v5/traps.spp:680-694`) | **P1, P4** | O modo ARM/Thumb entra pelo bit 0 do endereco e vive no CPSR — nunca por faixa. |
| `pc += (PSR_TF && !IS_THUMB32(...)) ? 2 : 4` (`arm/trap.c:397,726-730`) | **P2** | Largura da instrucao vem do bit T + do opcode lido, nao de constante. |
| `srsia` / `movs pc, lr` (`arm/locore.s:539,1936`); `srsia` / `rfeia sp` (seL4 `traps.S:53`, `fastpath.h`) | **P1, P2** | PC e CPSR sao salvos e restaurados **na mesma instrucao**. |
| `COMPUTE_THREAD_STATE_HASH` com `pacga` (`arm64/machine_routines_asm.s:1062-1073`) | **P1, P2** | Apple **assina** `{pc, cpsr, lr, x16, x17}` juntos; so' o carry pode mudar no retorno. |
| Checagem de modo + `ExceptionVectorPanic` (`arm/locore.s:1913-1917`) | **P1, P4** | Assert de producao antes de retornar ao guest. |
| `machine_stack_attach` com 5 campos (`arm/pcb.c:236-242`); `arm_syscall_context_t` de 4 palavras (OKL4 `thread.h:136-150`) | **P4** | Primeira ativacao preenche um conjunto fechado e explicito; nada e' herdado. |
| `icache_invalidate_trap` calcula a faixa (`arm/locore.s:707-728`) | **P5** | Invalidacao e' funcao do estado escrito, nunca endereco hardcoded. |
| `pmap_set_pmap` no switch + TTBR0/CONTEXTIDR no retorno (`arm/pcb.c:101,271`; `locore.s:1926-1931`); FCSE/ASID do OKL4 (`v5/traps.spp:764`, `v6/traps.spp:752`) | **P6** | Espaco de enderecos troca junto com a thread e e' programado imediatamente antes do PC. |
| UTCB em `*(u32*)0xff000ff0` escrito no switch (`v5/traps.spp:740,771`, `thread.cc:78-97`) | **P4, P3** | No OKL4 o UTCB **nao** e' TLS; e' memoria, e o switch tem de atualiza-la. |

#### O que ficou NAO VERIFICADO

* Qual erratum motiva o `nop` antes de `movs pc, lr` em `osfmk/arm/locore.s:1935`
  (comentario `// Hardware problem`).
* Invalidacao **seletiva** de TLB por ASID no OKL4 2.1.1 (secao 30.5.1 cita a programacao de
  CONTEXTIDR/TTBR, mas nao o caminho de invalidacao seletiva).
* Uso real de `arm_switch_stack_t` (`arch/arm/pistachio/include/thread.h:87-99`) — a struct existe
  mas nao foi encontrada em uso nesta versao; parece residual.
