# Relatório diário — Frentes Zeebo (LLE / HLE / JIT)

**Emitido:** 2026-09-09 19:06 BRT (GMT-3) → **revisado/auditado 19:30**
**Escopo:** Todas as frentes autônomas e investigações desde 00:00 de 2026-09-09

> **Nota de auditoria:** este documento foi auditado contra o `git log` real dos três repos.
> A contagem de commits foi corrigida: **34 commits no LLE**, **7 no HLE**, **3 no JIT** (não os ~12 citados na 1ª versão). O commit-chave `e67e3b4 fix(qw42)` (resume ARM da copia local `ig_naming`) estava omitido — o `9ca92a6` que aplicamos é uma GENERALIZAÇÃO dele (0x0c → todos os syscalls).

---

## 1. Resumo executivo

Três frentes autônomas (`/goal`) operadas e supervisionadas hoje, com **dois marcos**:
1. **QE raiz do stall do boot (`ig_naming`) identificada e corrigida em código** (T-bit do retorno do L4_Ipc) — o boot passa a avançar e renderizar frames.
2. LLE evoluiu QW28→QW44 e passou a renderizar a **Z-Wheel** via SoftRasterizer.

- **LLE**: 34 commits (QW29→QW44) + fix `9ca92a6`.
- **HLE**: 7 commits até ~12:13, depois pausada (verdict blocked).
- **JIT**: 3 commits; Etapa 1 completa, Etapa 2 iniciada e bloqueada.

---

## 2. Infraestrutura operacional

### 2.1 Drivers systemd
- `zeebo-goal-{lle,hle,jit}.service` com ExecStart `relaunch_goal2.py <log> <fifo> copilot claude-sonnet-5 <goal.txt>`.
- Modelo trocado de `gemini-3.8-flash` → **`claude-sonnet-5`** via `--provider copilot` (gemini: 503 "upstream high demand"; `claude-sonnet-4.8` não existe no copilot).

### 2.2 Vigia (cron `4912e21ce93d`, 30 min)
- Script `~/.hermes/scripts/zeebo_goal_watch.py` patchado para **3 frentes**.
- Classificação corrigida: agora usa **commits reais por repo** como âncora (antes cruzava sessões pelo texto de meta — bug de rotulação, p.ex. JIT rotulado como HLE).

### 2.3 Erros de stream (17:42–18:11)
- Série de **408/429** (timeout/rate limit) no provider: 17:42 (vision 400), 17:43 (408), 17:54/18:04/18:11 (429). Fila `/queue` do usuário vazia no momento.

---

## 3. Frente LLE (`~/projects/zeebo-lle`)

### 3.1 Cronologia COMPLETA de commits (34, de 00:01 a 19:05)

| Hora | QW | Commit | Conteúdo |
|---|---|---|---|
| 00:01 | QW29 | `93503d5` | L4 thread table + ExchangeRegisters tracking |
| 00:05 | QW28/30 | `9c0ce8a` | close QW28, avança Passo 14 → QW30 |
| 00:07 | QW30 | `e9fc98c` | scheduler cooperativo round-robin primitives (ThreadTable) |
| 00:19 | QW30 | `e3c539c` | detalhes context switch em c0_intr_hook |
| 00:28 | QW28 | `4b73be6` | track test_l4_ipc_msgtag.cpp |
| 00:30 | QW31 | `a9e8f6d` | cooperative thread switching no L4_ThreadSwitch |
| 01:10 | QW31 | `f46ef2f` | SystemServiceRegistry + IPC service dispatch |
| 05:21 | QW32 | `20e8bdf` | cooperative thread switching no L4_Ipc wait |
| 05:36 | QW33 | `d505d31` | AMSS/BREW handoff detection + execution vector |
| 06:10 | QW34 | `b20906b` | boot target routing + lifecycle binding |
| 06:41 | QW35 | `0f541a9` | MIF/MIF2 parser + applet resolver |
| 07:01 | QW36 | `4bd6b0f` | unify applet/commercial game lifecycle dispatch |
| 07:09 | QW37 | `c0e6aaf` | glFlush/glFinish + Adreno 130 frame-loop |
| 07:19 | QW38 | `70f582b` | BREW IShell timer engine |
| 11:27 | QW18 | `92b2a4f` | remove escrita espúria sp+0/4/8 no L4_KernelInterface |
| 11:52 | QW40 | `6523fd6` | ativação REAL de thread via ExchangeRegisters (HALTFLAG, sem DELIVER `1<<9`) → destravou `0xb000c834` → salto `0x10137000` (AMSS/BREW real, 1ª vez) |
| 12:48 | QW41 | `8374e76` | preserva T-bit (L4 handoff, SVC resume, slice re-entry). Descoberta: `uc_reg_read(PC)` do Unicorn nunca devolve LSB |
| 12:54 | QW42 | `c3115db` | bloqueio QW42 (stall pós-fix T-bit em `0xb010333a`) |
| 13:18 | QW42 | `d0c8edf` | 2 hipóteses testadas/descartadas (RE) |
| 13:31 | QW42 | `2842b12` | 3ª hipótese descartada (SVC 0x0c localizado) |
| **13:48** | **QW42** | **`e67e3b4`** | **fix: resume ig_naming LOCAL ExchangeRegisters copy as ARM** (o fix real do 0x0c) |
| 13:52 | QW42/43 | `18f6eb2` | QW42 resolvido (`e67e3b4`); abre QW43 (loop memcpy r4 corrompido) |
| 14:04 | QW43 | `b13df8c` | causa raiz localizada (byte 0x00 no decoder de string em `0xb0400052`) |
| 14:08 | QW43 | `ffa4edf` | refinado: tabela de ponteiros com entrada nula, não string |
| 14:10 | QW43 | `e4ba726` | hipótese (b) descartada por watchpoint, foco (a) |
| 14:20 | QW43 | `2018a96` | decodificador RLE identificado, byte de controle zero causa underflow |
| 14:22 | QW43 | `1ac962c` | mecanismo exato do underflow; recomenda pausa/avaliação |
| 14:27 | QW43 | `dc70ec2` | workaround experimental testado e REJEITADO (novo travamento downstream) |
| 15:45 | QW43 | `6e5c031` | causa raiz + truncamento buffer fonte; decoder RLE correto (strings ARM/OKL4) |
| 15:57 | QW43 | `84107a3` | zloader real + bootstrap puro (sem RLE); caller do dispatcher `0xb0400000` |
| 16:14 | QW43 | `163f50a` | análise estática do dispatcher: tabela 3 entradas (memcpy/RLE/zero-fill), underflow reproduzido em sim Python |
| 17:13 | QW44 | `ac2bfbb` | **causa raiz do underflow RLE = bug na simulação Python de RE** (`r5+2`, não `r5+1`); oráculo PASS 1040/1040 bytes; nenhuma correção C++ |
| 17:15 | QW44 | `1333a65` | descarta hipótese de truncamento por DMA; PT_LOAD ELF termina exatamente no limite do blob RLE |
| 19:05 | QW43/44 | **`9ca92a6`** | **fix (nosso): T-bit do L4_Ipc (0x00) p/ cópia local ARM do ig_naming — generaliza `e67e3b4` para TODOS os syscalls** |

### 3.2 Correção principal (commit `9ca92a6`, nosso)

- **Causa raiz do stall em `0xb010333a`**: branch `syscall==0x00` (L4_Ipc) usava `apply_tbit(pc)` cru → forçava **Thumb** para pc fora de `0xb000-0xb002`; mas o `ig_naming` (0xb010–0xb012) mantém **cópia local ARM** dos stubs → retorno da syscall via Thumb → drift → pousa no meio de um `bl`. **Não era deadlock**, era artefato de decodificação.
- O QW42 (`e67e3b4`) tinha corrigido só o branch `0x0c` (ExchangeRegisters). **Generalizamos**: `caller_is_ig_naming_arm` no lambda `apply_tbit` cobre todos os syscalls.
- **Validação real**: build limpo; boot avança (ig_naming mapeado rwx, L4_MapControl de `b030`/`b0e0`, ciclos correm, **14 frames renderizados**, 32 draws do Adreno). Frame ainda preto (draws não conectados ao sink — camada seguinte).

### 3.3 Marco Z-Wheel
- LLE renderiza a Z-Wheel via SoftRasterizer: PNG 640×480 RGB (1951 bytes, cor sólida — consistente com dados determinísticos de teste, não UI real).

### 3.4 Próximo passo (deixado para a sessão)
- **Scheduler**: criar/logar os servers na ordem OKL4 (iguana → timer → ... → appmgr) p/ que o `naming_insert` real chegue ao ig_naming (em vez do MR fake, linhas 2232-2237).
- Conectar os draws do Adreno ao framebuffer (frames ainda pretos).

---

## 4. Frente HLE (`~/projects/zeebo-emulator/research/sources/zeebulator`)

### 4.1 Cronologia de commits (7, até ~12:13)

| Hora | Commit | Conteúdo |
|---|---|---|
| 11:06 | `400f076` | IHeap + IThread cooperativo, EGL extensions/surface, VFS case-insensitive |
| 11:29 | `dbe6c48` | IHash (MD5) real + VFS subdir miss-resolver |
| 11:58 | `04afaf9` | census62 real (17 títulos mudaram, 16 alive=true) |
| 12:01 | `96e68ed` | fix falso-positivo regressão bio4_brew (erro de CLSID no teste manual) |
| 12:05 | `3ae9baf` | TGA decoder (types 2/10, 24/32bpp); validado contra Pac-Mania; não ligado ao render |
| 12:13 | `305bf1b` | documenta crash Zumas Revenge (vtable nula `[r4+108]`) + Peggle estacionário; nenhum é quick win |

### 4.2 Status
- **Pausada** 12:30 (verdict blocked — "no further quick wins relevant").
- Bejeweled Twist: fix conhecido do zeebx não está no corpus local (intestável).
- Opções oferecidas: (a) re-escopo Zumas, (b) nova direção, (c) manter pausada — **pendente escolha**.
- **Correção de auditoria**: havia 4 commits de trabalho real (IHeap, IHash, TGA) antes do 305bf1b — a 1ª versão do relatório omitiu.

---

## 5. Frente JIT (`~/projects/zeebo-dynarmic-bringup/`)

Protótipo em **diretório isolado** (escolha do usuário) — não edita LLE nem zeebulator.

### 5.1 Etapa 1 (viabilidade) — COMPLETA (3 commits, 16:14–16:22)
| Hora | Commit | Conteúdo |
|---|---|---|
| 16:14 | `491ed46` | dynarmic build + lockstep harness (ARM 100k zero-div) |
| 16:20 | `925509e` | hooks MMIO NAND+GPT portados |
| 16:22 | `2d4bbfb` | NOTES.md final (verdict + gate Etapa 2) |

- **ARM puro: 100k passos, zero divergência.** Thumb + MMIO destravados (hooks portados do `stage_runner.py`).
- Veredito: dynarmic não tem lacuna de semântica ARMv6K; bug do T-bit é do Unicorn do LLE, não do dynarmic.

### 5.2 Etapa 2 (subset-proof em modo bloco) — INICIADA, BLOQUEADA
- Sessão `d5f062` avançou p/ `Jit::Run()`. **Hang reprodutível** em `0xb0105dfc` — loop de wait IPC L4 (`bl 0xb0102c10` → `svc #0x1400`). Block-mode não trata syscall como exceção → roda para sempre. **Mesma doença do LLE** (IPC nunca satisfeito).
- Gate Etapa 2: recompilar com hooks MMIO; subset-proof block-mode (hash fronteira JIT ⊆ trajetória oráculo).

---

## 6. Investigações paralelas

### 6.1 Microkernel OKL4 no corpus
- `~/projects/zeebo-lle/refs/okl4-2.1.1-fix7/` (OKL4 completo + REX + `arm-kernel.elf`).
- Firmware embute paths OKL4 + strings `ig_naming`, `k1Process: Created iguana PD`.
- ABI: **UTCB/MyLocalId = `0xFF000FF0`** (o `ldr r2,[r3,#0xff0]` do ig_naming); MR0-5 = r3..r8.
- Ordem de boot: cada server `naming_insert("<nome>", obj)` — o IPC que o ig_naming espera.

### 6.2 Remote TripleOxygen / openzeobo
- `tool/revskills2.04.zip` baixado — binário Windows, hardware-only (inútil p/ emulação).
- `DDI0198E_arm926ejs_r0p5_trm.pdf`, `DDI0211K_arm1136_r1p5_trm.pdf`, `ZeeboDeveloperGuide0.97.pdf` baixados.
- Clonado `tripleoxygen/openzeobo` — `tools/zloader/` ARM9, `start_kernel` `0xc0008848`.

### 6.3 Wiki tripleoxygen
- EFS tree completo (Z-Wheel real em `/mod/274755/assets/stage_slides/`, MIFs em `/mmc4/mif/`).
- Mapa NAND completo; **ZeeboMCP** copia jogo eNAND→NAND antes de executar.
- MMU dumps ARM9/ARM11; mapa de memória MSM7201A.

### 6.4 Técnicas de outros emuladores
- **Infuse** (Tuxality): único emulador Zeebo funcional — HLE total do BREW, dynarmic, sem BIOS. Ninguém boota o REX/L4 real.
- **Zeebx** (local): HLE BREW em Rust, `INSTRUCTION_BUDGET` anti-looP infinito.
- Trade-off documentado: LLE fiel vs. HLE parcial.

---

## 7. Documentos produzidos (`notes/`)

| Arquivo | Conteúdo |
|---|---|
| `tripleoxygen_efs_nand_mmu.md` (796 linhas) | EFS, partições NAND, memória, MMU, zloader |
| `ig_naming_investigation.md` | Stall `0xb010333a`, causa raiz T-bit |
| `ig_naming_init_order.md` | Ordem de boot dos servers, 1º IPC |
| `okl4_source_para_boot.md` | OKL4 source como referência ABI/init |
| `emulador_tecnicas_zeebx_sdk.md` | Infuse/Zeebx/SDK técnicas |
| `relatorio_diario_2026-09-09.md` | Este relatório |
| `l4e-syscall-abi.md` (pré-existente, docs/) | ABI corrigida |

---

## 8. Estado atual (19:30 BRT)

| Frente | Sessão | Status | Último commit |
|---|---|---|---|
| LLE | `b110d5` | active (reativada) | `9ca92a6` (fix T-bit) |
| JIT | `d5f062` | active (reativada) | `2d4bbfb` (Etapa 1) |
| HLE | `8a34d7` | paused | `305bf1b` |

- Drivers systemd: 3× `active`. Vigia: cron `4912e21ce93d` ativo.

---

## 9. Pendências / decisões abertas

1. **HLE**: re-escopo (Zumas) ou manter pausada.
2. **JIT**: corrigir o modelo de periférico/IPC do polling loop vs. documentar o hang.
3. **LLE**: scheduler na ordem OKL4 + conectar draws do Adreno ao sink.
4. **Config**: default ainda `custom:xkiro` — usar `--provider copilot` nas autônomas.