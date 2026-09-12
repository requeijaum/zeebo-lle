# O modelo de CPU estava errado: ARM1176 → ARM1136

**Data:** 2026-09-12 · **Branch:** `linux-boot` · **Commit da correção:** `53d1a09`
**Commit do instrumento:** `1437cb9` (`tools/cpp/test_linux_mmu_contract.cpp`)

---

## TL;DR

O emulador declarava a CPU do Zeebo como **ARM1176** (part `0xB76`). O hardware
real é **ARM1136** (part `0xB36`). A prova veio de um log de boot do Linux
rodando no console físico, que imprime o `MIDR` lido do silício.

A correção foi aplicada em 18 ocorrências / 15 arquivos. **Não houve ganho
funcional** — e isso era esperado: medi antes de trocar e a ISA observável dos
dois modelos é idêntica. O valor da mudança é remover uma identidade falsa, não
consertar um sintoma.

---

## 1. A evidência

Fonte: log de boot publicado por Fausto "TripleOxygen", `pastebin.com/raw/pdVwuLUV`
(cópia local em `/tmp/zlinux_full.txt`, 168 linhas). Linux `2.6.29-zeebo`,
build #85, 07/ago/2011, bootloader próprio "Zeeboot v0.1", máquina `halibut`.

A linha que decide a questão:

```
CPU: ARMv6-compatible processor [4117b362] revision 2 (ARMv6TEJ), cr=0x00c5387f
```

Esse `[4117b362]` é o **MIDR lido do próprio silício** pelo kernel — não é
documentação, não é suposição, não é o que alguém achou que a CPU era. É o
registrador de identificação do chip, impresso pelo Linux rodando no console.

Decodificação (`arch/arm/kernel/setup.c` imprime `read_cpuid_id()`):

| Campo | Bits | Valor | Significado |
|---|---|---|---|
| Implementer | 31:24 | `0x41` | ARM Ltd. |
| Variant | 23:20 | `0x1` | r1 |
| Architecture | 19:16 | `0x7` | ARMv6 (definido por CP15 c0) |
| **Part number** | **15:4** | **`0xB36`** | **ARM1136** |
| Revision | 3:0 | `0x2` | p2 |

→ **ARM1136 r1p2.**

## 2. O que estava no código

`zeebo_dynarmic_core.h` declarava `midr = 0x410FB767` e todo o resto configurava
`UC_CPU_ARM_1176`:

```
0x410fb767 → part 0xB76 = ARM1176   (o que declarávamos)
0x4117b362 → part 0xB36 = ARM1136   (o que o silício reporta)
```

Distribuição do erro, medida em `a665028^` (antes de qualquer trabalho desta
rodada): **16 ocorrências de `UC_CPU_ARM_1176` em 14 arquivos**, mais o `midr`
do motor recompilado. Após eu criar `test_linux_mmu_contract.cpp` (que também
citava o valor antigo), eram **18 ocorrências em 15 arquivos** no momento da
correção.

## 3. Isto já era suspeitado — e já havia sido investigado

Ponto importante de honestidade: **a dúvida não é nova**. O `ROADMAP.md` já
tinha um item aberto, *"Incoerência de identidade de CPU entre os dois
backends"*, que registrava:

- o TRM que possuímos localmente é `DDI0211K_arm1136_r1p5_trm.pdf` — **ARM1136**,
  descrito como "Core0" (ROADMAP linha 49);
- o plano `.hermes/plans/2026-09-09_etapa3-dynarmic-integration.md:326` já
  avisava em caixa alta: *"ATENÇÃO: part=0xB36 (ARM1136), NÃO 0xB76 (ARM1176).
  Valor exato = capturar do baseline, não hardcodar de cor"*, com o valor
  marcado como **"CONFIRMAR"**;
- e havia um **experimento negativo já realizado**: trocar para
  `UC_CPU_ARM_1136_R2` + `midr=0x4107B362` deu resultado **idêntico em tudo** —
  mesma primeira divergência de boot (#23726), mesmos valores
  (`r3 = 0x10090001` vs `0`), mesmas contagens (596.725 vs 1.168.764 instruções).
  O experimento foi **revertido** e a pergunta ficou aberta.

O que esta rodada acrescenta não é a suspeita, é a **prova**: antes havia um TRM
sugerindo 1136 e um valor não confirmado; agora há o MIDR lido do silício. A
pergunta *"qual é a CPU historicamente correta do Zeebo"*, que o ROADMAP deixou
explicitamente em aberto, está **respondida**.

## 4. Medição antes de trocar

Não troquei às cegas. Instrumento em `/tmp/behav.cpp`: mesmo programa ARM
executado sob os dois modelos do Unicorn, comparando resultado e erro.

| Instrução | ARM1136 | ARM1176 | Igual? |
|---|---|---|---|
| `uqadd8 r0,r0,r0` | `0x00000000` OK | `0x00000000` OK | sim |
| `rev16 r0,r0` | `0x00000000` OK | `0x00000000` OK | sim |
| `ldrex r0,[r1]` | READ_UNMAPPED | READ_UNMAPPED | sim |
| `setend be` | OK | OK | sim |
| `clz r0,r0` | `0x00000020` | `0x00000020` | sim |
| `smuad r0,r0,r0` | `0x00000000` OK | `0x00000000` OK | sim |
| `mcr p15 wfi` | OK | OK | sim |
| **`fmrx r0,fpsid`** | **`0x410120b4`** | **`0x410120b5`** | **NÃO** |

Uma única diferença observável, o FPSID, e mesmo assim só no dígito de revisão.
**A ISA é idêntica.** Consequências:

1. a troca é de **baixo risco** (não muda semântica de execução);
2. a troca **não iria, sozinha, consertar bug nenhum** — o que é coerente com o
   experimento negativo anterior do ROADMAP;
3. portanto o benefício é de **corretude de identidade**, não de comportamento.

## 5. Limite honesto: erramos em 1 bit, de propósito

O Unicorn não expõe nenhum modelo com variant 1 **e** revision 2 ao mesmo tempo:

| Modelo do Unicorn | MIDR | Problema |
|---|---|---|
| `UC_CPU_ARM_1136_R2` | `0x4107b362` | variant 0 (real é 1) |
| `UC_CPU_ARM_1136` | `0x4117b363` | revision 3 (real é 2) |
| `UC_CPU_ARM_1176` | `0x410fb767` | part errado |
| **alvo real** | **`0x4117b362`** | — |

Adotei **`UC_CPU_ARM_1136`** (`0x4117b363`): acerta implementer, variant,
architecture e **part**, erra **um bit** na revisão. É a aproximação mais
próxima disponível sem fork do Unicorn.

Note que a escolha anterior do ROADMAP (`UC_CPU_ARM_1136_R2`, `0x4107b362`)
errava o **variant**; `UC_CPU_ARM_1136` é estritamente melhor — está a 1 bit do
alvo em vez de 2 campos.

O que importa na prática é o **part number**: é por ele que software detecta
família de CPU. O critério M4 do teste valida o part; a diferença residual de
revisão é **impressa** pelo teste, não escondida.

## 6. O erro que eu cometi, e o teste que o pegou

Fiz a troca **pela metade**: mudei o `midr` do motor recompilado (Dynarmic) e os
`uc_ctl_set_cpu_model` do código de produção, mas **esqueci os testes**, que
mantinham `UC_CPU_ARM_1176` hardcoded. Resultado imediato:

```
  ok    SCTLR (c1,c0,0) no reset     interpretado=0x00050078  recompilado=0x00050078
  ok    CTR   (c0,c0,1) no reset     interpretado=0x01dd20d2  recompilado=0x01dd20d2
  FALHA MIDR  (c0,c0,0) no reset     interpretado=0x410fb767  recompilado=0x4117b362
==== CP15 no reset: MOTORES DIVERGEM ====
```

`check-fast` foi a RED. `test_jit_cp15_reset` existe exatamente para impedir que
os dois backends tenham identidades diferentes — pré-requisito para o lockstep
ter qualquer valor — e cumpriu o papel. Corrigido nos 15 arquivos; ambos os
motores agora reportam `0x4117b363`.

Segunda vez nesta sessão que um gate do projeto pega um erro meu antes do
commit (a primeira foi o selftest pegando encoding ARM montado à mão errado).

## 7. Validação

| Gate | Resultado |
|---|---|
| `test_jit_cp15_reset` | motores concordam (`0x4117b363` nos dois) |
| `test_jit_lockstep` | PASS — ambos calculam `r0 = 2` |
| `test_linux_mmu_contract` | M1, M2, M3, M4 PASS |
| Boot BREW/AppMgr (Zeetris, headless 3s) | roda sem derail (~1,5k frames, ~17k chamadas IGL) |

Nota sobre os números do boot: frames e chamadas IGL **variam entre execuções**
(medi 1544/16984 e 1571/17281 em duas corridas) porque o laço é limitado por
tempo de parede, não por contagem de instruções. Só a **ausência de derail** é
o critério aqui; não tratar esses números como determinísticos.
| `make check-fast` | **PASS** |

## 8. O que NÃO mudou (não confundir com progresso)

Nenhum sintoma conhecido foi resolvido por esta correção, e nenhum deles foi
atribuído a ela:

- Zeetris continua **não interativo** (seletor `ctx+0x904f` = 0 em 120/120);
- `0x12001a38` (start 0→1) continua **nunca chamada**;
- `PC=0x14` no `--boot-appmgr` orgânico continua;
- a divergência de boot **#23726** continua no mesmo lugar — o experimento
  anterior do ROADMAP já havia demonstrado que a família de CPU não a move;
- ISHELL continua stub.

## 9. Contexto de descoberta

A divergência **não** foi encontrada procurando por ela. Apareceu como efeito
colateral do pivô para boot de kernel Linux: ao usar o log do TripleOxygen como
oráculo de MMU (`test_linux_mmu_contract.cpp`), o mesmo log traz a linha do MIDR.

Isso reforça o valor do log como fonte: ele é um **retrato do hardware real em
execução**, e já rendeu, além do MIDR:

- `PAGE_OFFSET`/`PHYS_OFFSET` confirmados (delta `0xb0000000` exato em 6 regiões);
- base da RAM `0x10000000` verificada contra hardware, não suposta;
- **alias do framebuffer**: `fbram` VA `0xc4c00000` → PA `0x12e00000`, a mesma PA
  da região `fb` (VA `0xc2e00000`) — dois mapeamentos da mesma memória física,
  relevante para o backend de GPU;
- `SCTLR` real no boot: `cr=0x00c5387f` (MMU on, caches on, **V=1** high vectors
  em `0xffff0000`, U=1, XP=1).

## 10. Verificação complementar

Busca pelos quatro valores de MIDR como palavra literal (little-endian) em
`nand/1.1.2_AMSS.bin`: **0 ocorrências** para `0x4117b362`, `0x4117b363`,
`0x410fb767` e `0x4107b362`. O firmware não compara MIDR com literal embutido —
ao menos não em palavra crua e não alinhada a essa codificação. Isso é
consistente com a ausência de mudança de comportamento observada, e reproduz o
mesmo resultado que o ROADMAP já havia registrado.

## 11. Pendências

- [x] **`ROADMAP.md` atualizado nesta auditoria**: o item *"Incoerência de
      identidade de CPU"* foi de `[ ]` para `[x]` (RESOLVIDA), com a resposta
      definitiva e o ponteiro para este documento. A frase do experimento
      negativo que afirmava *"o código segue em `arm1176`"* ficou desatualizada
      com `53d1a09` e recebeu nota de atualização — a conclusão daquele
      experimento (família de CPU não move a divergência #23726) segue válida.
- [x] **Comentários desatualizados no código, achados nesta auditoria e corrigidos:**
      `test_linux_mmu_contract.cpp:40-41` ainda dizia *"O nosso emulador usa
      0x410FB767 (part 0xb76 = ARM1176) e UC_CPU_ARM_1176 em ~12 lugares"* —
      descrição do estado **anterior** a `53d1a09`, que ficou lendo como se fosse
      o estado atual. Também o "~12 lugares" era estimativa; o número medido é 18.
- [ ] O `Cp15Ids` traz `ctr = 0x01DD20D2` e `reset_sctlr = 0x00050078`, valores
      que vêm do QEMU e são idênticos nos três modelos — logo não foram afetados.
      Mas o log real dá `cr=0x00c5387f` **depois** do boot do Linux (não é valor
      de reset); não há conflito, e não se deve confundir os dois.
- [ ] Core1 (`UC_CPU_ARM_926`, ARM926EJ-S) **não** foi tocado e continua coerente
      com o TRM `DDI0198E_arm926ejs_r0p5_trm.pdf`.

## 12. Fontes

- `https://pastebin.com/raw/pdVwuLUV` — log de boot Linux 2.6.29-zeebo
  (TripleOxygen), cópia em `/tmp/zlinux_full.txt`.
- `docs/remote/DDI0211K_arm1136_r1p5_trm.pdf` — TRM do ARM1136 (Core0).
- `ROADMAP.md` — item "Incoerência de identidade de CPU"; experimento negativo
  1136_R2; valores do QEMU `target/arm/tcg/cpu32.c`.
- `.hermes/plans/2026-09-09_etapa3-dynarmic-integration.md:326` — alerta
  `part=0xB36, NÃO 0xB76` com valor marcado "CONFIRMAR".
- `tools/cpp/test_linux_mmu_contract.cpp` — critério M4.
- Commits: `1437cb9` (instrumento), `53d1a09` (correção).
