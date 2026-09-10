# Core1 — estado real do boot (medido, 2026-09-10)

Nota de correção. Registra o que foi **medido** nesta sessão e corrige afirmações
desatualizadas de `okl4_source_para_boot.md` e `ig_naming_init_order.md`.

Regra aplicada (MORE_INFO §7): toda afirmação abaixo tem medição associada.
Onde não houve controle negativo, está marcado explicitamente.

---

## 1. O caminho de IPC está MORTO no boot do Core1

Instrumentei o hook de syscall (contador bruto, antes do `switch`), boot padrão
`--headless --boot-appmgr --seconds=8`:

```
[SVC] n=1 syscall=0x14 pc=b000c940
...
[SVC] n=8 syscall=0x14 pc=b000c940
total: 8 syscall=0x14      (L4_MapControl)
```

**8 syscalls no boot inteiro, todos `0x14` (L4_MapControl), todos do mesmo PC.
ZERO `L4_Ipc` (0x00). ZERO `L4_ExchangeRegisters` (0x0c).**

Consequência direta: todo o bloco `case 0x00` de `zeebo_lle_main.cpp` — handoff
cooperativo (QW32), `pick_next_thread`, e a **injeção sintética de `MR0=1/MR1=0x16`**
(~linha 2854) — é **código morto neste caminho de boot**. Nunca executa.

Também não executa o registro de servers por sniffing de faixa de IP
(`register_service("ig_naming", ...)`, ~linha 2952), pois depende de
ExchangeRegisters.

## 2. Correções às notas existentes

### `okl4_source_para_boot.md` §"Fix em 2 partes", item 1 — DESATUALIZADO
A nota afirma que o T-bit foi corrigido "SÓ no 0x0c" e que o branch `0x00`
continua com `apply_tbit(pc)` cru. **Falso hoje.** O guard foi generalizado para
dentro do próprio helper:

```cpp
// zeebo_lle_main.cpp:3063
bool caller_is_ig_naming_arm = (pc >= 0xb0100000u && pc < 0xb0120000u);
// :3069
auto apply_tbit = [&](u32 v) -> u32 {
    return (caller_is_kernel_stub || caller_is_ig_naming_arm) ? (v & ~1u) : (v | 1u);
};
```

Vale para TODOS os syscalls, inclusive o `0x00`. O fix (A) está **APLICADO**.

### `ig_naming_init_order.md` §4 — reclassificado
- Item (A) "ROOT CAUSE, highest confidence" (T-bit no 0x00): **já corrigido** (acima).
- Item (B) "latent, would surface after A is fixed" (nenhuma thread emissora real):
  não é latente — é **inalcançável**. O boot morre antes do primeiro IPC.

A ordem de bloqueios da nota (T-bit → scheduler) está superada: **o obstáculo atual
está ANTES dos dois**.

## 3. Onde o boot realmente morre

Laço infinito de page-table walk pós-`creating root server (000a8001)`.
Causa medida em sessões anteriores: a tabela de tamanhos de página do OKL4
`{12,16,20,26,32}` em `0xf000efc8` (`.rodata`) é servida do shadow Split I/D
**zerado**, produzindo shift 0 → máscara `0xffffffff` → `sl=0xb0000007` → walk infinito.

Servindo essa faixa do pristino (probe `notes/boot-investigation/patches/01-*`):

| | insns | ponto de parada |
|---|---|---|
| sem probe | ~7,0M | laço em `f0003df4..f0004080` |
| com probe | ~58,9M | `Assertion !"Failed to create root server TCB" — pistachio/src/thread.cc:1273` |

Isto **converge com §1**: o kernel não chega a criar threads, por isso nenhum IPC
existe. A assertion do TCB é exatamente o ponto onde threads passariam a ser criadas.

⚠ **Controle negativo AINDA NÃO RODADO** para o probe (exigência MORE_INFO §7).
O ganho 7M→59M prova que a variável mudou, não que a correção seja a certa.

## 4. Hipóteses REFUTADAS por medição (não reabrir)

| # | Hipótese | Como caiu |
|---|---|---|
| 1 | `find_kernel_heap`/memdesc mal configurado escolhe o heap | Este build **não usa** esse caminho. Erro de método: apliquei o corpus OKL4 como prova do fluxo desta firmware — exatamente o que MORE_INFO §5.1 adverte sobre o kernel MSM |
| 2 | Heap `(f0000000,f0200000)` se sobrepõe à imagem ⇒ limiar linear separa | `.rodata` e pilha vivem no **mesmo** `PT_LOAD` (`va=f0000000 filesz=0001a324 memsz=0001e2c0`). Tentativa de limiar → regressão para 16.717 insns, `pc=0` |
| 3 | A lista livre do alocador está vazia / nunca inicializada | Li `[pool]` no ponto errado: `f0002c94` é o ramo de **sucesso**, e `[pool]=0` é o estado **depois** de desenfileirar o último nó. A lista tinha nós |
| 4 | Split I/D restaurando a pilha como código causa o laço | Assimetria era real, mas `r8` continuou 0 com a pilha coerente |
| 5 | `7` em `b0000007` é corrupção | É o campo **rights** de um fpage L4 (`orr r2,r2,#7` em `f0016a8c`) |
| 6 | Scheduler/IPC ausente é o bloqueio (QW46) | 8 SVCs no boot, **todos** `0x14` (MapControl), zero L4_Ipc — o caminho é inalcançável. **Ressalva**: o hook `UC_HOOK_INTR` está registrado só no Core0 (`zeebo_lle_main.cpp:2740`); a contagem descreve o Core0 |
| 7 | **complete fpage mal detectada** causaria a assertion do TCB | **REFUTADA por medição.** A divergência de código é REAL (nosso `is_whole_space()` usa `size_log2>=32`; o OKL4 codifica complete como `size==1 && base==0`, confirmado em `refs/okl4-2.1.1-fix7/pistachio/include/fpage.h:137-138` e `:206-207`) — mas é **inócua neste firmware**. Instrumentei o dispatcher (`[FPG]`): das **371 fpages** emitidas no boot, **ZERO** complete e **ZERO** whole-space. `size_log2` observados: 20(124×) 12(78×) 14(37×) 21(21×) 13(21×) 22(20×) 23(19×) 16(17×) 15(10×) 17(9×) 19(6×) 18(6×) 25(3×) — nenhum `size==1`. Logo `is_whole_space()` é **código morto** neste boot e corrigi-lo **não pode** destravar o TCB |

> **Lição de método (repetida, agora evitada a tempo):** o corpus OKL4 provou que
> existe uma divergência de ABI, **não** que ela é exercida por este firmware.
> Medir antes de corrigir evitou um segundo "conserto" de variável vizinha.
> A divergência continua valendo como dívida técnica — só não é a causa do TCB.

## 4.1. Segfault pré-existente em `--seconds=20`

Descoberto em 2026-09-10 enquanto instrumentava as fpages. **Não é regressão
das mudanças de hoje** — reproduzido no worktree do commit `744a227`, anterior
a todo o trabalho desta sessão, com as ROMs reais presentes:

```
seconds=5   -> exit 0
seconds=12  -> exit 0
seconds=20  -> exit 139 (SIGSEGV)
```

O emulador **crasha no host** ao rodar ~20s de boot. Como o probe da `.rodata`
leva o boot a ~73M instruções, esse limite passou a ser alcançável. Ainda **não
diagnosticado**: falta rodar sob ASAN/gdb para localizar o acesso inválido.
Item aberto — não confundir com a assertion do TCB, que é do *guest*.

## 5. Cadeia do alocador (fatos, sem conclusão)

```
f000a7f0  mov r7, #0x400          ; tamanho 1KB
f000a7f4  ldr r0, [pc, #0x408]    ; pool = f001a508
f000a800  bl  f0002b7c            ; alocador
f0002c84  str ip, [r5]            ; desenfileira (ramo de sucesso)
f0002c94..f0002cb8                ; zera o bloco (4x str r1,[r2],#4)
```

Escritas na cabeça da lista `f001a508`:
```
[f001a508] <- f0000000  pc=df602d80     <- espelho de relocacao REX de f0002d80
[f001a508] <- f0000400  pc=f0002c84
...
[f001a508] <- f000e000  pc=f0002c84     <- pagina da .rodata
```
`f0002d80` (`str r4,[r2]`) é **free/insert ordenado**, não init de pool.
Quem chama esse free com base `f0000000` segue **não medido** (build quebrou por
disco cheio antes da captura).

## 6. Armadilha de ambiente

`/tmp` é **tmpfs de 4,8G**. Traços de lockstep (`/tmp/long_*.txt`, ~486MB cada)
encheram o disco e o build falhou com:
```
fatal error: error writing to /tmp/ccXXXX.s: Não há espaço disponível no dispositivo
```
Isso **se parece com erro de código e não é**. Antes de investigar falha de build
súbita: `df -h /tmp`. Traços de lockstep são regeneráveis — apagar sem dó.

## 7. Próximo passo

Atacar a assertion do TCB (`thread.cc:1273`) com o probe como **muleta declarada**,
rodando o controle negativo que falta. Duas frentes convergem para lá: é onde o
kernel para com o probe, e é onde o caminho de IPC (§1) passaria a existir.
