# Zeebo LLE Emulator — ROADMAP: Double Dragon jogável (revisão auditada)

## Objetivo e estado real — base auditada `9c7ae29`

**Objetivo aberto:** no executável LLE, iniciar Double Dragon, atravessar splash/menu,
entrar numa fase, controlar o personagem e ouvir música/efeitos produzidos pelo jogo.
BREW AppMgr e Z-Wheel permanecem objetivos de boot; não substituir o emulador por Infuse.

**Correção explícita dos relatos anteriores:** os commits `d530d4c`, `7c146f6` e
`9c7ae29` NÃO provam jogos executando com som e imagem. `--applet=` copia bytes;
`dispatch_applet_start()` ignora o handler solicitado e chama sempre o harness
`dispatch_zwheel_app_start()` em `0x10532344`. O applet/objeto/vtable são scratch do host.
O hook e o loop pintam azul; SDL abrir um dispositivo não prova PCM do guest.
As alegações de Double Dragon e outros sete jogos “resolvidos” estão RETRATADAS.
O teste `test-roms-external`, baseado nessa mensagem PASS, é um falso gate de execução.

### Evidência reproduzida nesta revisão

- Build: `make zeebo_lle_main` retornou 0 (alvo atualizado; não foi clean build).
- Execuções separadas de DD real e arquivo inválido com `--applet=... --seconds=0.05
  --dump-frames=...`: ambos exit 0, entry AEEMod_Load não resolvido e mesmo PASS.
  Ambos produziram 18 PPMs; primeiro frame tem **uma única cor** e SHA-256
  `2547e8be48601dcf4d09e16428c9b96d5844bd4b5f2af12f8c8b51dfc3fd756d`.
  Isso prova que o gate atual independe do jogo, NÃO compatibilidade comercial.
- Logs e controle negativo: `/tmp/zeebo-dd-audit-scjbdxlq/{double_dragon,negative_control}.log`
  e `results.json` (temporários; receita: arquivo não executável como controle negativo).
- Código causal: `tools/cpp/zeebo_lle_main.cpp` — `dispatch_applet_start`,
  `dispatch_zwheel_app_start`, `run_zwheel_interactive`, `zwheel_stub_hook`;
  `tools/cpp/zeebo_brew_loader.h` — `inject_bytes` / resolução de entry.

### Documentação normativa local — não buscar na web

`docs/remote/` (não commitado; verificado 2026-09-10) contém a referência de arquitetura:

- `DDI0211K_arm1136_r1p5_trm.pdf` — ARM1136 r1p5 TRM, 934 pp. **Core0** (ARM1136EJ-S, part `0xB36`).
- `DDI0198E_arm926ejs_r0p5_trm.pdf` — ARM926EJ-S r0p5 TRM, 264 pp. **Core1** (AMSS/ARM9).
- `ZeeboDeveloperGuide0.97.pdf` — 137 pp., guia oficial de desenvolvedor do Zeebo.
- `memory_map.ods`, `openzeebo/`, `revskills2.04.zip` — material de RE da comunidade.

Usar os TRMs como fonte normativa para semântica de instrução, CP15, modos e bancos de
registradores em vez de inferir comportamento por tentativa. Isso é **referência de ABI /
comportamento**: não copiar código de terceiros.

### Mídia disponível — não procurar outro dump

- Fonte de trabalho read-only: `/home/rafaelfrequiao/.Tuxality/Infuse/brew/`.
  Contém árvore `mod/` + `mif/` e assets; não é imagem NAND nem prova de execução LLE.
- Segunda cópia local: `/media/rafaelfrequiao/8C5F-19E51/zeebo/ROMs/` (isso não implica procedência independente).
  O ZIP de Double Dragon passou em `ZipFile.testzip()`; seus **cinco arquivos** são
  byte-idênticos aos correspondentes da árvore Infuse (comparação integral).
- Double Dragon: App ID/diretório **274754**; AEECLSID **0x0102F789**, NÃO 274754.
  Referência já existente: `../zeebo-lab/notes/2026-08-31_infuse-vs-zeebulator-dd.md:23-27`.
- `mod/274754/ddragonz.mod`: 462748 bytes; SHA-256
  `5485a189fc3f22652dcd94c4a1b242ff2b0f1271ce1b52f96aee5b4727e1a55f`.
  Assets obrigatórios: `data.ggz`, `sound.ggz`, `ddragonz.sig`, `mif/274754.mif`.
- O `.7z` em Downloads/temp falhou na extração/CRC; não é necessário para este plano.
  A busca por nome nos dirents da NAND não prova ausência universal de conteúdo nem
  justifica procurar “cartucho”: jogos Zeebo são distribuídos digitalmente.
- Referências A/V existentes em `../zeebo-lab/assets/`: `dd-theme-oracle-01.wav`,
  `dd-menu-oracle-01.wav`, `dd-gameplay-oracle-01.mp4`. Antes de usar como gate,
  registrar hash, procedência/emulador e sequência de input; não atribuir toda captura ao Infuse.

### Estratégia de armazenamento e fidelidade

Preferir **overlay de arquivos com base read-only + saves separados**, sem modificar
NAND nem userdata do Infuse. Montar `fs:/mod/274754/`, `fs:/mif/274754.mif` e os caminhos
`fs:/mmc4/` que forem observados; resolver caminhos relativos pelo contexto do módulo.
Isto é PROPOSTA: a cópia `--applet=` atual não oferece open/read/seek/stat nem VFS guest.

Manter CPU/firmware do LLE. O backend host de arquivos precisa ser conectado à fronteira
real do firmware (ou à emulação de armazenamento); um backend host isolado é só infraestrutura.
Interceptação BREW de IFile/IGL, caso usada para acelerar bring-up, deve ser identificada
como modo híbrido experimental, nunca confundida com boot LLE completo. Não implementar
um segundo runtime BREW inteiro nem transplantar código de emuladores de licença incompatível.

Low-level emulation of the Zeebo: boot the REAL firmware from the NAND dump on an
emulated Qualcomm MSM7201A (ARM11 apps core + ARM9 modem coprocessor + QDSP5), no HLE of BREW.
Histórico de infraestrutura abaixo: JIT (`c28d66a`), carga EFS2 (`adcb631`), UX (`07033e1`) e UARTs. Esses componentes não fecham o boot BREW nem a execução de jogos; o estado auditado e os gates no topo prevalecem sobre contagens históricas:
- QW17 (`a88a9bd`)/QW19 (`475ee1c`): convenção de PC-resume e restauração de frame callee-saved
  nos stubs de syscall — o mempool_init (96×1 MiB) atravessa e o boot avança além.
- **Causa raiz 1 (validada por experimento)**: `thread_init` (0xb00070c8, casado com `thread.c`)
  panica no assert (linha 134, `rfl_insert_range(131075, 1)`) porque o KIP sintético tem
  `KIP[0xc4]` (thread_bits) = 0. Com `KIP[0xc4]=18` (poke via control-port, boot orgânico):
  **`bi_execute` (0xb00001fc) é alcançado** e o boot roda código de server (0xb0400000+).
- **Causa raiz 2**: o decode `PhysDesc::phys_base() = (raw>>6)<<10` (gran 1KB do OKL4 fix7)
  produz endereços 16× maiores (MR cru 0x10081000 → 0x100810000, >4GB) → o handle_map_control
  classifica os mapas físicos do bi_execute como "whole-space" e não os aplica → páginas rwx=0 →
  memset do BSS falha com `UC_ERR_WRITE_PROT`. O Zeebo usa gran 64B (`<<6`).

## What is now KNOWN & VERIFIED (evidence from execution & disassembly)

1. **Kernel = L4e (OKL4 2.1.1 lineage) + Iguana + REX RTOS on top:**
   - Standalone OKL4 kernel boots cleanly to thread scheduler / idle thread (`pc=0xf0002ea4`, commit `e9d646c`).
   - Syscall trampoline table at `0x00d06d9c`:
     - `0x00d06d9c`: `ldr pc, [pc, #-4]` -> `0x16e9ab20` (thread switch / yield / `L4_ThreadSwitch`, `SVC #0x6`).
     - `0x00d06da4`: `ldr pc, [pc, #-4]` -> `0x17478927` (synchronous IPC handler / `L4_Ipc`, `SVC #0x1400`).
     - `0x00d06dac`: `ldr pc, [pc, #-4]` -> `0x16e0d079` (thread timer handler).
   - Syscall invoker thunk at `0x00d0cae0` prepares `r0-r2`, saves frame pointer in `ip` (`r12`), branches via `bl 0x00d06d9e`.

2. **AMSS RTOS (REX) Scheduler Loop & Event Wait:**
   - Stable execution past **5,000,000 instructions** (`err=ok`) deterministically pausing in idle loop at `0x16ef0b2c/2e`.
   - Event wait mechanism in `0x1730f442..0x1730f488`: wait mask check `0x00180000`.
   - Free-running virtual tick timer MMIO at `0xc5000108` ensures REX software timer expiration and prevents lockups.
   - Mode stack registry at `0x1755d264`: System (1KB, `0x179fe058`), Abort (400B, `0x179fdec8`), Supervisor (208B, `0x179fddf8`), IRQ (536B, `0x179fdbe0`).

3. **ONCRPC / SMD Inter-Core Communication Channel:**
   - `0x1755d1dc` is the SMD channel state structure (`struct smd_half_channel`):
     - `+0x00`: State = `0x01` (`SMD_SS_OPENING`)
     - `+0x01`: Busy / Mutex flag = `0x00` (free)
     - `+0x02`: Error / Abort flag = `0x00` (clean)
     - `+0x03`: Channel link status = `0x02` (`SMD_SS_OPENED`)
   - Triple query helpers:
     - `0x16ef0a9c`: reads `+0x01` (busy lock)
     - `0x16ef0a82`: reads `+0x03` (link status)
     - `0x16ef0aa2`: reads `+0x02` (abort flag)
   - Packet queue head at `0x17571748`: `+0x00` head, `+0x04` tail, `+0x08` count, `+0x1c` initialized (`0x00000001`).
   - Router dispatch loop (`0x16ef0b28..0x16ef0b48`):
     - `r0 >= 3` (`SMD_SS_FLUSHING/OPENED` fully connected): invokes packet consumer `0x16e8cb96`.
     - `r0 < 3`: branches to channel reset/handshake `0x16e8cb88` and waits for peer.

4. **QDSP5 Multimedia Acceleration Subsystems (via ONCRPC):**
   - The modem AMSS hosts the command dispatcher for all MSM7201A DSP engines:
     - **Voice DSP (`QDSP_VOICEPROCTASK`)**: Upstream command queue `UPVOCPROCQUEUE` (`0x16ea9cb0`).
     - **Video Front End (`QDSP_VFETASK`)**: Scale (`VFECOMMANDSCALEQUEUE`), Table (`VFECOMMANDTABLEQUEUE`), and Command (`VFECOMMANDQUEUE`) queues (`0x16ea9ce8..0x16ea9d88`).
     - **JPEG Hardware Codec (`QDSP_JPEGTASK`)**: Action (`UPJPEGACTIONCMDQUEUE`) and Config (`UPJPEGCFGCMDQUEUE`) queues (`0x16ea9dd0..0x16ea9e18`).
     - **Audio Post-Processor (`QDSP_AUDPPTASK`)**: Multi-band EQ and DAC output queue `UPAUDPPCMD2QUEUE` (`0x16ea9e68`).
   - Unified packet framing: 1280 bytes (`0x500`) max payload, procedure ID at offset `+0x20`, data starting at `+0x80`.

5. **Hardware Map (MSM7201A):**
   - VIC `0xC0000000`, GPT `0xC0100000`, DMOV/ADM `0xA9700000`, MDDI `0xAA600000`, CLK `0xA8600000`, UART1 `0xA9A00000`, NAND `0xA0A00000`, Timer `0xC5000000`.
   - NAND ID `0x5580b1ad` verified byte-exact.

---

## What is MISSING & Blocking Full Boot (The Gap Analysis)

To achieve the ultimate goal — booting the real firmware end-to-end to launch a game — the following concrete layers are still missing:

1. **SMD / SMSM Inter-Core Handshake & RPC Peer Emulation [PROTOTIPADO E VALIDADO]:**
   - *Current status:* Concluído em `tools/cpp/zeebo_smd_bridge.cpp`.
   - *Verified:* Inicialização de SMSM compartilhado no SMEM `0x01F00000`, transição de enlace SMD (`0x1755d1dc`) de `2` (`SMD_SS_OPENED`) para `3` (`SMD_SS_FLUSHING`), e injeção de pacotes RPC estruturados de 1280 bytes com `Procedure ID` (`0x1b59`) na fila `0x17571748` com sincronização de nós de lista encadeada e contadores ativos.

2. **Dual-Core ARM11 (Apps) + ARM9 (Modem) Shared Memory (SMEM) Fabric [PROTOTIPADO E VALIDADO]:**
   - *Current status:* Concluído em `tools/cpp/zeebo_dual_core.cpp` (commit `df01bcd`).
   - *Verified:* Execução simultânea intercalada de ARM11 (`UC_CPU_ARM_1176`, OKL4 L4e em `0xf001c000`) e ARM9 (`UC_CPU_ARM_926`, AMSS em `0x00a00000`) compartilhando SMEM (`0x01F00000`), ProcComm, interrupções A2M/M2A (`0xC0100400`) e timer GPT (`0xC5000000`). Executou 100.000 instruções por núcleo com `err=ok`.

3. **NAND OS Loader Bridge (Flash -> DRAM relocation) [PROTOTIPADO E VALIDADO]:**
   - *Current status:* Concluído em `tools/cpp/zeebo_nand_relocator.cpp`.
   - *Verified:* Leitura direta da cópia da NAND (`1.1.2.bin` bloco 0x12) via descritores DMA do hardware DMOV (`DMOVModel` / `NandController`), decodificação do cabeçalho ELF (`0x464c457f`), extração do entrypoint `0x00a00000` e mapeamento das 18 seções `PT_LOAD` em tempo de execução sem arquivos ELF pré-extraídos.

4. **Adreno 130 3D / 2D Display Engine & MDDI Bridge [PROTOTIPADO E VALIDADO]:**
   - *Current status:* Concluído em `tools/cpp/zeebo_mddi_display.cpp`.
   - *Verified:* Controlador virtual de interface serial MDDI em `0xAA600000` via `uc_mmio_map`, com respostas fiéis de versão do núcleo (`0x00000102`), comandos de inicialização de enlace (`CMD_POWER_UP`), status de link ativo (`STAT_LINK_ACTIVE`), e processamento de listas primárias de DMA (`MDDI_PRI_PTR`) para transferência de quadros de vídeo RGB565 em resolução nativa de 640x480.

---

## Staged Roadmap & Next Milestones

### Phase 1 — AMSS Active Packet Ingestion & Baseband Handshake [PROTOTIPADO E VALIDADO]
- [x] Mapeamento completo do despachante ONCRPC e tabelas de tarefas QDSP5 (`b55dae0`).
- [x] Implementar injeção de pacotes na fila `0x17571748` com estrutura de 1280 bytes (`procedure ID`, payload `+0x80`) — validado em `zeebo_smd_bridge.cpp`.
- [x] Implementar transição de estado SMD (`0x1755d1dc`: estado `2` -> `3`) simulando resposta do ARM11 para disparar os callbacks `blx r2` registrados pelo modem — validado em `zeebo_smd_bridge.cpp`.

### Phase 2 — SMEM & Dual-Core Harness Architecture
- [x] Unificar os runners `zeebo_boot.cpp` e `zeebo_kernel_boot.cpp` em um único processo C++ com dois contextos Unicorn (`uc_open(UC_ARCH_ARM, UC_MODE_ARM)` para ARM1176JZ e ARM926EJ-S) — validado em `zeebo_dual_core.cpp` (`df01bcd`).
- [x] Mapear o espaço SMEM compartilhado (`0x01F00000`, 2MB) com estruturas ProcComm, controle de versão e interrupções inter-core A2M (`0xC0100400`) — validado em `zeebo_dual_core.cpp` (`df01bcd`).
- [x] Conectar os eventos de interrupção A2M (escrita em `0xC0100418` / `MSM_A2M_INT`) ao vetor de interrupção VIC do ARM9 para acordar threads REX — validado em `zeebo_dual_core.cpp`.

### Phase 3 — Second-Stage NAND Relocator
- [x] Conectar o modelo de hardware DMOV DMA (`DMOVModel`) e o controlador de NAND (`NandController`) para leitura direta de partições — validado em `zeebo_nand_relocator.cpp`.
- [x] Parsear dinamicamente os cabeçalhos ELF e tabelas `PT_LOAD` direto da memória DMA lida da NAND (`1.1.2.bin`), eliminando dependência de arquivos ELF pré-extraídos — validado em `zeebo_nand_relocator.cpp`.

### Phase 6 — Gamepad & Input Subsystem (Z-Pad / Keysense) [PROTOTIPADO E VALIDADO]
- [x] Mapear leitura do teclado/gamepad do host (SDL2) para as filas de evento ou registradores de GPIO/Keypad do MSM7201A (`KEYPAD_BASE 0xA9A00000`, `INT_KEYSENSE #28`).
- [x] Integrar subsistema `UnifiedInput` ao laço de eventos do mestre `zeebo_lle_main.cpp`.

### Phase 5 — System Integration & Master Orchestrator [PROTOTIPADO E VALIDADO]
- [x] Integrar no orquestrador o handoff `0:APPS`, o dispatcher L4e com `L4_MapControl` funcional e a ponte SMD/ONCRPC; a semântica completa de `L4_Ipc`, `L4_ThreadControl` e `L4_ExchangeRegisters` permanece no Passo 14.
- [x] Atualizar `zeebo_dual_core.cpp` para carregar os 14 segmentos `PT_LOAD` do `1.1.2_APPS.bin` real na RAM física de 96MB (`0x10000000..0x16000000`), janelas virtuais L4e (`0xf0000000`) e Iguana (`0xb0000000..0xb2000000`), motor de syscalls (`UC_HOOK_INTR`) e validar execução concorrente estável de 1,2 milhão de instruções (600k por núcleo) com `1.1.2_AMSS.bin`.
- [x] Conectar sinalização de interrupção real `INT_KEYSENSE` (IRQ #28) no controlador de interrupções VIC do ARM11 (`0xC0000000`) ao disparar eventos do Z-Pad / SDL2.
- [x] Mapear e carregar integralmente os 14 segmentos `PT_LOAD` do super-ELF real `1.1.2_APPS.bin` com coexistência das janelas físicas (`0x10000000..0x16000000`, 96MB) e virtuais (L4e `0xf0000000`, Iguana `0xb0000000`, BREW `0x10137000+`).
- [x] Configurar o protocolo bidirecional ProcComm (`APP_COMMAND`, `APP_STATUS`, `MDM_COMMAND`, `MDM_STATUS`) e estados SMSM (`0x0000002b`) na SMEM (`0x01F00000`) dentro do `zeebo_lle_main.cpp`.
- [x] Carregar a cadeia de boot da cópia da NAND via hardware DMOV DMA (`load_apps_dmov`, `load_amss_dmov`) diretamente no orquestrador `zeebo_lle_main.cpp`.
- [x] Integrar dispatcher nativo de syscalls L4e (`UC_HOOK_INTR`) no Core 0 do `zeebo_lle_main.cpp`, permitindo que o Iguana OS despache chamadas de sistema (`svc #0x14` / `MAP_CONTROL`) e mantenha execução concorrente estável além de 600.000 instruções por núcleo.
- [x] Migrar o harness de dual-core (`zeebo_dual_core.cpp`) para carregar o binário real `nand/1.1.2_APPS.bin` (eliminando o kernel sintético `arm-kernel.elf`) com mapeamento físico `0x10000000` e janelas virtuais `0xf0000000` / `0xb0000000`.
- [x] Unificar todos os subsistemas em `tools/cpp/zeebo_lle_main.cpp` com orquestração dual-core, SMEM, campainhas A2M/VIC, NAND/DMOV, MDDI, Adreno 130 e display sink SDL2.
- [x] Carregar e bootar a partição de firmware real `1.1.2_APPS.bin` no ARM11 a partir de `0x10000000`.
- [x] Carregar e bootar a partição de firmware real `1.1.2_AMSS.bin` no ARM9 a partir de `0x00a00000`.
- [x] Observar transição completa do microkernel OKL4 (`0xf0000000`) para o espaço de usuário do Iguana (`pc=0xb0000028`) com ambos os núcleos executando 200.000 instruções sem falhas (`err=ok`).

### Fase 7: Suíte de Conformidade de CPU e Validação Cruzada (Testkit Conformance)
- [x] Construir sonda LLE de conformidade (`zeebo_lle_mod_probe`) com interface compatível ao `mod_probe` do `zeebulator`.
- [x] Executar e validar 12 testes de CPU de `/home/rafaelfrequiao/projects/zeebo-emulator/testkit/cputests/` sob ARM11 Unicorn: 12/12 PASS (`alu`, `callret`, `condflags`, `controlflow`, `interwork`, `ldmstm`, `loadstore`, `media`, `memory`, `muldiv`, `shifter`, `thumb2branch`).
- [x] QW6 (`a447dde`, `1ed62ec`): primeiro vetor transacional ARM/Thumb com INIT/FINAL completos e traço ordenado de prefetch/load/store; valores de load vêm de `UC_HOOK_MEM_READ_AFTER`, não de zeros pré-read. O runner falha por mudança de ordem/endereço/valor mesmo quando o estado final coincide.
- [x] Testar a execução do módulo limpo `zbtest.mod` (construído via SDK oficial BREW) no LLE e mapear o ponto de despacho para `AEEMod_Load`.

### Fase 8: Estado Reproduzível e Áudio
- [x] Baseline de Save State Dual-Core (`ZeeboSaveStateManager` em `zeebo_save_state.h`, formato v2): contextos Unicorn (`uc_context_save`), contadores/PCs e regiões mapeadas de memória.
- [ ] Elevar para checkpoint de máquina completa (v3 chunked): hash da cópia de NAND, serializer simétrico, `validate` antes de mutar, estado de MMU/IRQ/timers/GPU-GL/EFS-VFS/eventos e `post_load` para caches/callbacks. O v2 atual **não** prova restauração determinística dos dispositivos.
- [ ] Gate de checkpoint: salvar em A → rodar N eventos → coletar hashes de bytes/pixels/registradores → restaurar A → repetir N eventos e exigir identidade.
- [x] Incorporar mixer de áudio multi-stream (`UnifiedAudioSink` em `zeebo_audio_sink.h`) com controle de canais e vtable HLE/LLE limpa evitando problemas de ciclo de vida e interworking.

### Fase 9: Subsistema Gráfico (Adreno 130 / IGL) — acoplamento LLE e rasterização
- [x] Evidência de firmware: 3D usa fachada OpenGL ES 1.x ATI-Imageon; o produtor observável no ARM11 é a vtable IGL/IEGL, não um ring PM4 A2xx.
- [x] `IGpuRasterizer`, `SoftRasterizer`, `IglHook` e `IglGuestBridge` integrados ao orquestrador e ao sink SDL2 640×480 RGB565.
- [x] Resolução determinística de objetos/vtables vivos (`obj[0]`) com validação contra segmentos PF_X; nenhum VA ou vtable fabricado.
- [x] ABI legada separada e validada: IGL=80, IEGL=28, sem `this` nos métodos GL/EGL; `eglGetProcAddress=8`, `eglSwapBuffers=26`. IEGL11 nova permanece separada (`SwapBuffers=25`) e ainda não é ativada sem objeto vivo comprovado.
- [x] Pipeline fixed-function: modelview/projection, viewport, Triangles/TriStrip/TriFan, arrays habilitados explicitamente, current color, texturas RGBA8/RGB565, duas unidades, repeat, bilinear, depth LESS/depth-write e blend SRC_ALPHA/ONE_MINUS_SRC_ALPHA (`b7019fa`).
- [x] Robustez: smokes retornam falha real, índices esparsos são compactados (`100005→7` leituras), MVP coerente e bridge normaliza o bit Thumb dos VAs; slots não modelados continuam no wrapper/firmware sem corrupção de R0 (`33924a6`, `7f1844f`).
- [x] Gates por pixels: `gpu_smoke`, `igl_smoke`, display, transform e bridge fazem parte de `make check`; clear `0xF800/0x001F`, textura `0x001F`, depth `0x07E0`, blend `0x8010`, bilinear `0x8410`.
- [x] Quick wins GL QW4/QW5/QW7 (`e078422`, `9dfdc9e`): alpha-test; culling com defaults GLES CCW/`GL_BACK`; `glTexParameterx` por textura (NEAREST/LINEAR, REPEAT/CLAMP); oito depth funcs e fatores usuais de blend. Gate `gl_quickwins_smoke`: 13/13 pixels/comportamentos PASS. `glFrontFace` não foi inventado porque nenhum slot vivo foi observado; MIN_FILTER permanece estado-only até existir LOD/minificação.
- [x] ATITC clean-room em `glCompressedTexImage2D` slot 15 (QW9, `94a449c`): RGB methods 0/1, alpha explícito/interpolado e crop 6×6; oracle independente e gate 15/15 por pixels/FNV.
- [x] Compatibilidade restante: clipping homogêneo do near-plane e interpolação perspectiva (QW10/QW11 em `gl_clip_smoke.cpp`, testado no gate GPU), além de `GL_OES_draw_texture`.
- [ ] Caminhos guest reais: observar o retorno do `eglGetProcAddress` do firmware e registrar apenas o VA vivo; resolver `IEGLSurfaceManip` somente após QueryInterface/objeto vivo. Não usar trampolim, string ou vtable sintética do Zeebx.

### Fase 10: Subsistema QDSP5 (Áudio e Multimídia) — acoplamento ONCRPC e streaming
- [x] Comando-plane mapeado (FINDINGS 2zz–3c): dispatcher `0x16e8cba0`, 4 task engines (VOICEPROC, VFE, JPEG, AUDPP), enfileiramento SMD/ONCRPC `0x17571748`, packet frame (+0x20 proc, +0x80 payload).
- [x] `UnifiedAudioSink` disponível (Fase 8) como mixer PCM multi-stream.
- [x] **Acoplamento oficial do `Qdsp5Dispatcher` ao `UnifiedSMDBridge`**: IDs oficiais `prog::AUDMGR` (`0x30000013`) e `prog::ADSPRTOSATOM` (`0x3000000a`) substituindo o ID provisório `0x30000060`.
- [x] **Hook de consumo e retorno RPC**: captura em `0x16e8cb96`/`0x16e8cba0` alimenta `qdsp_disp_->feed_raw` com memória guest Core 0 (`guest.read`). Conclusão aciona respostas nos canais de retorno `0x31000013` (`AUDMGR_CB`) e `0x3000000b` (`ADSPRTOSMTOA`) via `on_completion`.
- [x] Backend SDL de saída implementado em `UnifiedHostAudio` (`d530d4c`); abertura com dummy comprovada.
- [ ] Streaming de PCM originado pelo jogo, sincronização e encerramento seguros: ainda sem prova de som de DD/AppMgr/Z-Wheel. QDSP5 permanece congelado; auditar integração host sem expandir engines.
- [ ] Q2/Q3/Q4: JPEG (libjpeg-turbo), VFE, VOICE — expansão pós-áudio funcional.

### Fase 11: Execução Guest Real e Resolução dos 5 Gargalos Estruturais
- [x] **Eliminação de saltos artificiais**: remoção de `core0_.entry = 0x1013a000` hardcoded; avanço autêntico por `uc_emu_start` nos dois núcleos.
- [x] **MAP_CONTROL funcional**: decodificação de MRs UTCB e mapeamento via `uc_mem_map`.
- [x] **Injetor direto de applets BREW**: flag CLI `--applet=<path>` e rotina `load_applet` alocando janela e carregando binários `.mod`/`.bar` em `0x12000000`.
- [x] **Item 1 (AMSS / Core 1 REX scheduler — commit `9fa10db`)**:
  - Vetor de reset real encontrado via `nand/find_arm9_reset.py`: `e_entry` PA `0x00a00000` traduzido para VA `0xf0000000` (preâmbulo `b 0xf0000024` → `msr cpsr_fc, #0xd3` SVC/IRQ/FIQ off → `ldr sp, =0x00a197f8`).
  - Mapeadas janelas virtuais no Core 1 (`0xf0000000` kernel/REX, `0xb0000000` task).
  - Implementado slide-detector em `c1_code_hook` para abortar NOP-slides.
  - **Resultado real comprovado**: NOP-slide eliminado; Core 1 executa branches reais (`0xf0000000 → 0xf0017544 → 0xf0017890`) e estabiliza na barreira/loop de espera do REX (`b 0xf0017890`).
- [x] **Item 2 (MAP_CONTROL / Cópia prévia de páginas — commit `99399d3`)**:
  - `map_one` em `zeebo_l4_mmu.h` instrumentado com probe de conteúdo nas páginas com permissão de execução (`UC_PROT_EXEC`).
  - Alerta imediato e limpo (`[MMU/WARN]`) quando página mapeada está virgem (0x00/0xFF).
- [x] **Item 3 (Core 0 loop de poll em 0xb000d4a8 — commit `99399d3`, resolução `47d6e6c`)**:
  - Diagnóstico inicial: adicionada sonda em `c0_code_hook` observando `[r0 + 0xc8]`.
  - Descoberta definitiva: loop em `0xb000d4a8` era o bit-scan de `l4e_min_pagesize()` sobre o campo `PageInfo` da KIP (`KIP[+0xc8]`), e não um poll inter-core.
  - Resolução orgânica: `build_kip()` agora popula legitimamente `KIP[0xc8] = 0x01111006` (páginas 4K/64K/1M/16M ARMv6 | rwx). O bit-scan encerra organicamente e o Core 0 avança para `0xb000d6b8` rumo ao pipeline de `MAP_CONTROL`.

### Fase 12: Arquitetura Avançada de Memória e Transição de Modos dos Núcleos
- [x] **VTLB LUT & PhysPool Aliasing (commits `596cb30` e `feb5886`)**:
  - Implementado `VtlbLut` (2^20 páginas, 8MB host) para tradução e acessos $O(1)$.
  - `PhysPool` de 96MB backing-store host (`apps_pool_mem_`) integrado ao `ZeeboLLESystem` e mapeado via `uc_mem_map_ptr`.
  - `handle_map_control` integrado com `map_one_aliased()` estilo PCSX2/Dolphin, permitindo mapeamento de múltiplos VAs para o mesmo espaço físico sem clonagem de páginas.
  - `BrewLoader` acoplado com `bind_lut()` para injeção e leitura direta via VTLB.
- [x] **Core 1 REX Memory Bring-Up & MMU/Remap Transition (commit `a79af76`)**:
  - Tabela de regiões de RAM do AMSS semeada em `0x00a1d73c` (`base=0x00a00000, teto=0x00c00000, attr=0x0f`), checagem `0xf0017448` retornando 0 (sucesso).
  - Panic dead-loop `0xf0017890` eliminado (0 hits).
  - Transição de relocação pós-CP15 diagnosticada e resolvida: mapeada janela `REX_RELOC_BASE=0xdf600000` (16MB) e espelhados os segmentos de código no delta `0xef600000`, permitindo que `mov pc, r0` em `0xf001774c` salte de `0xf0017750` para `0xdf617750` e avance até `0xf0017b04` (loop de configuração CP15).
- [x] **Resolução Determinística de gpIGL/gpIEGL sem Símbolos Hardcoded (commit `3c0a6a2`)**:
  - Constatado que `1.1.2_APPS.bin` é stripped (`e_shnum=0`) e as vtables são povoadas em runtime via `ISHELL_CreateInstance`.
  - Implementado `IglGuestBridge::resolve_from_object(uc, obj_va, is_igl)` resolvendo diretamente de `obj[0]=&vtable`.
  - Validação estrutural pura `validate_vtable()` checando alinhamento e se >=75% dos slots apontam para segmentos executáveis reais (`PF_X`). Testado sob Unicorn em `tools/cpp/gpu/igl_guest_bridge_test.cpp` (12/12 PASS).
- [x] **Core 0 — fechamento do BootInfo/`bi_execute` (Passo 13 — CONCLUÍDO)**:
  - QW1 parcial (`f196f14`, `b5d0766`, `433ba9c`) criou parser host-only e gate separado `test-bootinfo-real`.
  - QW17/QW19 destravaram o `mempool_init` (96 blocos de 1 MiB atravessados). **QW23 (`b57c591`)** resolveu o panic do `thread_init` (KIP[0xc4]=18); **QW24 (`2c0873d`)** resolveu o decode do `PhysDesc` (gran 64B) — com ambos, o boot alcança `bi_execute` e aplica mapas físicos reais (0 WRITE_PROT).
  - **QW26 (`7b570e2`)**: `ThreadControl` (0x08) e `SpaceControl` (0x18) retomados no epílogo (`pc` + `SP=ip`), eliminando panic `SpaceControl != 1`.
  - **QW27 (`fc4a811`)**: `ExchangeRegisters` (0x0c), `ThreadSwitch` (0x04) e `Schedule` (0x10) com restauração de frame e trap-stack (`SP=ip`), eliminando salto para PC=0x00000000.
  - **Resultado (Passo 13 Concluído)**: Core 0 completou integralmente `bi_execute`, `extensions_init` e entrou no `iguana_server_loop` (`0xb000aa94`), executando 756 chamadas MapControl e mais de 8,27 milhões de instruções orgânicas.

- [ ] **Passo 14: Iguana Server Loop & Despacho IPC rumo ao BREW AppMgr, Z-Wheel e Jogos**:
  - Tratar mensagens IPC de entrada no `iguana_server_loop` (`0xb000aa94` / `0xb000c834` L4_Ipc wait) para ativação dos servidores do sistema:
    - **`ig_naming`** (VA `0xb0100000`, Tag 07 @ fileoff `0x57160`): registro de nomes de objetos e serviços Iguana.
    - **`quartz_servers`** (VA `0xb0300000`, Tag 07 @ fileoff `0x573a0`): subsistemas de display/drivers.
    - **`AMSS`** (VA `0x10137000`, Tag 07 @ fileoff `0x575d4`): entrada do ambiente BREW / modem / AEECShell.
  - Handoff para o processo de espaço de usuário do `AEECShell` / BREW em `0x10137000` / `0x10c874f4`.
  - Boot do launcher `ZeeboApp` (`AEEAppletNew`, strings `fs:/mif/brewappmgr.mif`, `fs:/mod/brewappmgr/appmgr{ls,ln}.bar` embutidas em `0:APPS` offset `0x46e2e8`, VA `0x105322e8`), depois Z-Wheel e jogos (ex: Double Dragon).
- [x] **Core 1 CP15 Init Loop & Refinamento do Slide-Detector (commit `7bc384c`)**:
  - `0xf0017b04` é o loop de inicialização de CP15 (`bl 0xf0015d7c; cmp r4, #0xd; ble ...; mcr p15`). Falso positivo eliminado.
  - Refino aplicado ao `c1_code_hook`: o detector agora **decodifica a instrução ARM corrente** e zera a `slide_run` sempre que a insn é control-flow real (B/BL, BX/BLX, escrita de `Rd=PC` em data-proc/ldr, LDM/POP com PC na lista).
  - Blank-detector endurecido: exige run de 64 blanks consecutivos (`slide_blank_run`) em vez de um único word isolado.
  - Adicionado `c1_unmapped_hook` para acomodar acessos periféricos/MMIO do modem ARM9 sem travamentos de memória.
  - **Resultado real**: Core 1 atravessa todo o loop CP15 e o dispatch de init do REX (`0xf0017b04 → 0xf0018480`), processando a tabela `0xf0019e78`. Fronteira seguinte: callback de terminação/espera do subsistema REX. CPU conformance 12/12 mantido.
- [x] **Partição 0:EFS2APPS & Acesso DMA NandController (commit `09d0d54`)**:
  - MIBIB @`0x60810` e geometria de partições confirmadas: `0:APPS` no bloco `0xe6` (`0x1cc0000`), `0:EFS2APPS` no bloco `0x191` (`0x3220000`).
  - Harness `zeebo_efs2apps.cpp` 10/10 PASS provando acesso via DMOV/NandController byte-a-byte idêntico ao dump.
  - Provado que o launcher `ZeeboApp` (`AEEAppletNew`, strings `fs:/mif/brewappmgr.mif`, `fs:/mod/brewappmgr/appmgr{ls,ln}.bar`) reside embutido diretamente no ELF de **`0:APPS`** (offset de arquivo `0x46e2e8`, VA `0x105322e8`, manipulador de eventos Thumb em `0x10532344`), enquanto `0:EFS2APPS` armazena dirents e recursos de módulos (`.mod`, `.bar`, `.ini`). EFS acessado localmente pelo ARM11 sem dependência de RPC de arquivos com o modem.
- [x] **Item 4 (Loader BREW / Dispatch de Applets — commit `da9d5f4`)**:
  - Criada classe modular `BrewLoader` (`tools/cpp/zeebo_brew_loader.h`), integrando injeção de `.mod` e resolução de `AEEMod_Load` via ELF `e_entry`.
  - Tratamento honesto de símbolos ausentes/não mapeados.
- [x] **Item 5 (VTable IGL / Guest Machine — commit `da9d5f4`)**:
  - Implementado `tools/cpp/gpu/igl_guest_bridge.h` conectando chamadas de vtable `gpIGL`/`gpIEGL` do espaço virtual do guest à `GuestMachine`, despachando para `IglHook` e `SoftRasterizer`.
  - Atualização do display sink sincronizada com `mark_dirty()` nas chamadas gráficas.

---

### Fase 13: Bring-Up do Shell ZeeboApp / Z-Wheel e Integração EFS2APPS (Fase Atual)
- [x] **Engenharia Reversa do Launcher ZeeboApp e Z-Wheel**:
  - Ponto de entrada de eventos BREW do ZeeboApp localizado em `0x10532344` (Thumb): decodifica eventos em relação à constante base `K = 0x1f92`.
  - `EVT_APP_START = 0x1f96` (`K + 4`) desvia para `0x1053241e`, invocando o método da interface gráfica `[applet + 0x2c]->vtbl[0x28](1)` seguido da checagem de retorno BREW.
  - Também identificados os ramos de `EVT_APP_SUSPEND` (`0x1053235c`) e `EVT_APP_START_BACKGROUND` (`0x10532360`).
  - ClassID oficial identificado pelo corpus local e Zeebx: **AEECLSID `0x01070798` ("TECTOY")**, pacote App ID `274755` (`mif/274755.mif`, `mod/274755/`).
- [x] **Execução e Renderização Real da Z-Wheel sob Unicorn (commits `163a64a`, `d36d36d`)**:
  - Implementado harness executável `tools/cpp/zeebo_zwheel_harness.cpp` (`make test-zwheel`).
  - Carrega os segmentos de código de `0:APPS` (`1.1.2_APPS.bin`), monta a estrutura do applet com vtable gráfica em `[applet + 0x2c]` conectada ao `SoftRasterizer`/`IglHook`.
  - Disparou `EVT_APP_START` (`0x1f96`) no manipulador Thumb `0x10532344`: retorno `r0 = 1`, retorno limpo ao sentinela `0x1000fffe`.
  - 1 chamada gráfica capturada no slot 10 (`0x28`) com `arg=1`.
  - Framebuffer RGB565: soma de pixels 0 → 2013081600, centro `0x1999` — **pixels reais gerados e validados por execução**.
- [x] **Subagentes em Paralelo (`deleg_5e8643b1` e `deleg_1e6303ae`) — Core 1, EFS2 e Integração SDL2**:
  - **Passo 1 Concluído (commit `c1b4cb4`)**: Subagente `sa-0` implementou com sucesso o isolamento de RAM/heap do REX com arquitetura Split I/D (`zeebo_rex_harness.cpp`, `zeebo_lle_main.cpp`). O particionador `0xf0002cd4` executa 2.048 writes de heap no shadow sem corromper `.text` do AMSS; free-list preenchida em 2047/2047 blocos de 1KB; a re-entry em `0xf000e6d4` atravessa sem `UC_ERR_INSN_INVALID` e `rex_sched` `0xf0013b84` decodifica código pristino com sucesso (`make test-rex` exit 0).
  - **Passo 3 Concluído (commit `5223647`, `8bb4fb7`, `06b61e5`)**: Subagente `sa-1` e harness integraram o pipeline gráfico da Z-Wheel (vtable slot 10 / `0x28` com `arg=1` → SoftRasterizer) na janela SDL2 (640x480) de `zeebo_lle_main.cpp`. Criadas flags `--zwheel-preview` (modo interativo) e `--zwheel-preview-headless` (modo CI/teste). Validação em `make test-zwheel-preview`: frame RGB565 apresentado ao display sink com soma de pixels **2013081600** (idêntica ao harness determinístico) e dump comprovado `/tmp/zeebo_zwheel_rendered.ppm` (pixel `0x1999`).
  - **Passo 2 Mapeado (Auditoria EFS2 Data Extents e Spare OOB)**: Subagente `sa-0` da segunda rodada mapeou os nós de arquivos da Z-Wheel (`0x435`: `.qxt`, `.qxm`, `.qxa`) e identificou que o mapeamento de payload de dados `inode -> data page` do EFS2 reside nos 64 bytes de metadados OOB por página (`1.1.2_spare.bin`, 2112 bytes/página).
  - **Interface CLI & Telemetria Concluídas (commit `06b61e5`)**: Implementado `--help` completo com paridade de opções aos emuladores HLE (Zeebx/Zeebulator), telemetria em tempo real `--fps` (MIPS de Core 0 e Core 1 + FPS de vídeo), controle de boot `--boot-appmgr` (FIRSTAPP:0, padrão jailbreak) e `--boot-zwheel` (FIRSTAPP:3, padrão fábrica), suporte a injeção externa (`--applet=<caminho.mod>` e `run <caminho.mod>`), dump contínuo de quadros em PPM (`--dump-frames=<DIR>`) e limite de tempo de execução real (`--seconds=<N>`).
- [x] **Arquitetura de Memória e Sistema de Arquivos Auditados via Corpus/Hardware Real**:
  - **Descoberta de MMU ARM9**: O dump de MMU L1 do hardware real comprova que `VA 0xf0000000 = PA 0x00a00000 (SECTION)` é SRAM física interna de dados. O código executável reside em seções físicas dedicadas (`0x16e00000..0x17b00000`). O heap em `0xf0000000` deve ter backing store de RAM física independente.
  - **Descoberta de Particionamento NAND vs eNAND (`/mmc4`)**: A NAND interna (128 MB) armazena exclusivamente o SO (BREW/Rex/L4) e o Z-Wheel (`274755`). Todos os jogos comerciais ficam na eNAND externa (`fs:/mmc4/`). O Z-Wheel é o aplicativo central autêntico presente no dump da NAND.
  - **Subsistema ZeeboNet e Schemas SQLite Mapeados (commits `0db8cfa`, `be2da3d`)**: Documentados em `notes/ZWHEEL_SQLITE_SCHEMAS.md` os schemas DDL de `tt_game_info` (`GAMEINFO`, `TITLETEXT`, `DBINFO`), `tt_prefs.db` (`PREFSINFO`), `tt_dlqueue.db` (`DLITEMINFO`) e cache DSL `ASSETS`. Mapeado o cliente embutido da loja na Z-Wheel, o destino direto em `fs:/mmc4/` e o daemon `ZeeboMCP` (`fs:/zmcp.dat`).
- [x] **Estrutura de Arquivos EFS2APPS Mapeada (69.634 dirents)**:
  - Formato binário comprovado contra o dump `nand/1.1.2.bin`: `0x69 [inode u32][reclen u8][type u8][parent_ref u32][pad 00][name (reclen-5)]`.
  - Semântica de hierarquia decodificada: `parent_ref = (parent_inode << 8) | tag`.
  - Inode raiz `0x6064` indexa diretórios de primeiro nível (`"mod"`, `"mif"`, `".efs_private"`, `"err"`, etc.).
  - Módulos e assets indexados sob IDs numéricos da BREW (`mod/274755/`, `mif/274755.mif`).
- [x] **Core 1 REX MMU & Clobber de Código Diagnosticado (commit `d71d27d` e lotes `deleg_4d905a0d`, `deleg_fb1369dd`)**:
  - Ativação de MMU (`mcr p15` com `SCTLR.M=1` em `0xf0017718`) estabilizada via `UC_TLB_VIRTUAL`.
  - Causa do `UC_ERR_INSN_INVALID` identificada: `0xf0002cd4` inicializa free-list de 2MB em `0xf0000000`. Em `0xdf613b20`, o código relocado faz re-entry em VA absoluto `0xf000e6d4`. O clobber de heap sobrescreve essas instruções se o backing store físico não for desacoplado.
- [x] **Isolamento de Memória do Heap REX no Core 1 (Passo 1 / Fase 13 - Concluído `c1b4cb4`)**:
  - Provedor de backing store físico de RAM de dados para `0xf0000000..0xf0200000` separado do `.text` preservado do AMSS via Split I/D. O particionador conclui e o ARM9 avança até `rex_sched` (`0xf0013b84`) decodificando código pristino.
- [x] **Parser C++ EFS2APPS e extents (Passo 2 / Fase 13 — concluído em `d53f6c4`)**:
  - `zeebo_efs2_fs.h` indexa 69.634 dirents em O(1), lê clusters de 512 B e blocos indiretos; gate atual 33/33.
- [x] **Acoplamento do ZeeboApp ao Loop Principal do Core 0 e Display SDL2 (Passo 3 / Fase 13 - Concluído `5223647`, `06b61e5`)**:
  - Integrado o pipeline gráfico da Z-Wheel ao loop principal de `zeebo_lle_main`, conectando o ponto de despacho de applets da BREW ao pipeline de display SDL2 e criando a interface de controle CLI/telemetria.
- [x] **Destravar IPC do Iguana OS no Core 0 - Resolução de `L4_MapControl` (Passo 4 / Fase 13 - Concluído `6fe15b6`)**:
  - Identificada a causa raiz de `UC_ERR_NOMEM` no Iguana OS / OKL4 2.1.1: descritores de fpage com `size_log2 >= 32` (4GB) e `phys_base >= 0x100000000` representam operações de controle sobre todo o address space (flush/unmap global/concessão de permissões de espaço), e não mapeamento físico de RAM.
  - Implementado `Fpage::is_whole_space()` e tratamento limpo no dispatcher L4e em `tools/cpp/zeebo_l4_mmu.h`.
  - Cobertura em `tools/cpp/test_l4_mmu.cpp` com caso 8 validado (`ALL TESTS PASSED`). Core 0 agora avança sem crash na inicialização de pools de memória do Iguana.

- [x] **Mapeamento de Teclas Z-Pad/SDL2 e Despacho Contínuo de EVT_KEY_* (Passo 5 / Fase 13 - Concluído `46e68a5`)**:
  - Implementado em `tools/cpp/zeebo_brew_loader.h` o mapa de constantes de evento BREW (`EVT_KEY_PRESS = 0x0100`, `EVT_KEY_RELEASE = 0x0101`, `EVT_KEY = 0x0102`) e códigos AVK Zeebo (`AVK_UP = 0xFF52`, `AVK_DOWN = 0xFF54`, `AVK_LEFT = 0xFF51`, `AVK_RIGHT = 0xFF53`, `AVK_SELECT = 0xFF0D`, `AVK_CLR = 0xFF08`, etc.).
  - Implementado `BrewLoader::dispatch_event` executando o `HandleEvent` Thumb do applet sob Unicorn com captura honesta de `r0`.
  - Integrado no loop interativo de `tools/cpp/zeebo_lle_main.cpp`: encaminhamento contínuo de cada evento de teclado/joystick SDL2 para o manipulador do applet.
  - Criados `tools/cpp/zeebo_input_harness.cpp`, `zeebo_input_stub.s` e alvo `test-input` no `Makefile`.
  - Provado por execução real: 32 eventos de tecla despachados e 32 consumidos com `r0 = 1`. Suíte completa de 23/23 testes verdes.

- [x] **Parser C++ EFS2APPS `zeebo_efs2_fs.h` e Extração de Extents (Passo 2 / Fase 13 - Concluído `d53f6c4`)**:
  - Implementado `tools/cpp/zeebo_efs2_fs.h`: classe `efs2::Efs2Filesystem` (header-only, determinístico) capaz de abrir `1.1.2.bin`, mapear `0:EFS2APPS` (`0x3220000`), escanear e indexar dirents com marcador `0x69` em $O(1)$ por `(parent_inode, name)` e por `inode`.
  - Suporte a leitura de clusters de dados de 512B (`0x3220000 + cluster*512`) e resolução encadeada de blocos indiretos `u32` com terminador `0xFFFFFFFF`.
  - Criado harness `tools/cpp/test_efs2_fs.cpp` e alvo `test-efs2-fs` no Makefile.
  - Provado por bytes reais do dump: 69.634 dirents recuperados; dirent `reksio.mod` em `0x32606ef` validado (inode `0x265e4`, reclen 15, parent `0x4abef`); cluster `0x6d11` verificado com FNV-1a `0xa0f4d11f`; bloco indireto em `0x3b1d400` encadeado para 128 clusters (64 KiB) com FNV-1a `0xd9339103`. 18/18 testes PASS.
- [ ] **Passo 6: Diagnóstico e Avanço do Boot User-space no Iguana OS / Core 0 (reaberto após QW12/QW13, atualizado pós-investigação causal)**:
  - O commit histórico `770eb1a` implementou `write_mr(uc, utcb_base, index, val)` e o echo dos descritores em `handle_map_control` (`MR[i*2] = phys_desc`, `MR[i*2+1] = fpage`).
  - A afirmação anterior de que esse echo atendia de forma load-bearing à convenção Iguana/OKL4 não é distinguível no teste black-box atual: entrada e saída são byte-idênticas mesmo sem `write_mr`. QW13 prova a transação de map no Unicorn, não o efeito do write-back sobre o guest.
  - A investigação causal consolidou: o travamento inicial era mascarado pela corrupção de registradores salvos na pilha (`L4_KernelInterface`, corrigido em `4224919`) e pelo misload de firmware do CLI (`d137813`). O stall `0xb000d708` (`size_log2=56`) capturado em `path-c-corrected` (commit `ea1b48f`) foi posteriormente ultrapassado pelas correções de frame de stub QW19/QW26/QW27 (`475ee1c`, `fc4a811`); no HEAD atual o Core 0 executa ~7M+ instruções reais, atravessa `mempool_init`/`bi_execute`/`extensions_init` e entra no `iguana_server_loop` (`0xb000aa94` / `0xb000c834` L4_Ipc wait). A hipótese de que a escrita extra em `sp+0/4/8` do handler 0xb4 causava o d708 foi REFUTADA por TDD host-only (ver QW18 / `test_l4_kip_trap.cpp`): o SP no intr hook é o SP da trap (`mvn sp,#0x4b` = `0xFFFFFFB4`), scratch nunca lido.
  - Gate pendente: harness com guest vivo que observe MRs efetivamente transformados e avanço por bytes/endereços até `bi_execute`; instruction-count não é critério de sucesso.
- [x] **Passo 7: Parser/catálogo EFS2 e injeção host (`f1b03fa`, `645f332`) — NÃO VFS guest completo**:
  - Integrado o parser `efs2::Efs2Filesystem` ao `ZeeboLLESystem` em `tools/cpp/zeebo_lle_main.cpp`.
  - Adicionado `BrewLoader::inject_bytes()` em `tools/cpp/zeebo_brew_loader.h` para injeção de payloads materializados direto da memória.
  - Implementados métodos `ZeeboLLESystem::efs2_ls()`, `efs2_extract()` e `load_applet_from_efs2()`: varrem os 69.634 dirents da partição `0:EFS2APPS` (`0x3220000`), resolvem dirents por `(parent_inode, name)` e extraem a cadeia de clusters do bloco indireto, injetando via `BrewLoader`.
  - Adicionadas flags CLI `--efs2-ls[=filtro]` (lista dirents, ex: `.mod`) e `--efs2-run=<arquivo>` (extrai e injeta em Core 0).
  - Catálogo de blocos indiretos comprovados por bytes expandido:
    - `reksio.mod`: bloco indireto `@0x3b1d400`, 64 KiB, FNV-1a `0xd9339103`.
    - `274755` (Z-Wheel / ZeeboApp, CLSID `0x01070798`): bloco indireto `@0x3a92000`, 64 KiB, FNV-1a `0x544a6f30`, assinatura ASCII `"274755"`.
    - `tectoy.mod`: bloco indireto `@0x6026200`, 64 KiB, FNV-1a `0xf7c3c740`, assinatura ASCII `"tectoy.claro.com.br"`, dirent `inode=0x7ff13, parent=0x1fae8`.
  - Harness `test_efs2_fs.cpp` expandido para **33/33 testes PASS**. Suíte completa verde.
- [x] **Passo 8: Diagnóstico Preciso da Rotina de FPage e Mempool (Concluído `645f332` / Análise)**:
  - Desmistificado `0xb000d4a8`: não se trata de loop de polling de produtor externo, mas de rotina determinística `l4e_min_pagesize()` / CTZ (*Count Trailing Zeros*) que varre `KIP[0xc8]` (`PageInfo = 0x01111006`) e calcula `log2(min_pagesize) = 12` (páginas de 4 KiB), armazenando em `0xb0041284`.
  - Causa raiz do travamento em `mempool_init`: a rotina de decomposição `0xb000d4dc` itera aumentando `size_log2` a partir de 12. Quando os limites virtual e físico repassados via BootInfo (`0xb0d00000`) não estão estritamente alinhados ou extrapolam a memória convencional, a rotina não encontra uma fpage cobrindo o bloco, resultando em avanço nulo `r0 = 0` em `0xb000d6dc: add r4, r4, r0` e prendendo o loop.
- [x] **Passo 9: Investigação de `mempool_init` e Comportamento Real de MapControl (Concluído `e0ea94b` / Análise)**:
  - Isolada a execução de `0xb000d4dc`: em ambiente isolado, `0xb000d4dc` compõe fpages de 1 MiB (`size_log2 = 20`) com avanço positivo (`r4` de `0xb0d00000` para `0xb0e00000`).
  - No emulador completo com múltiplos ciclos (`--cycles=60`), o Iguana invoca repetidas vezes `L4_MapControl` via UTCB (`0xdff00000`) para registrar e mapear pools do sistema, executando o bit-scan `l4e_min_pagesize()` / CTZ de `KIP[0xc8]` de forma contínua e sem travamentos.
  - Ajustado o log do endereço `0xb000d4a8` em `zeebo_lle_main.cpp` para refletir estritamente o algoritmo real de CTZ (`[Core0/CTZ]`), eliminando qualquer interpretação espúria de polling.
- [ ] **Passo 10: Boot/ciclo de vida real da Z-Wheel — parcial: harness isolado (`fccca5e`)**:
  - Descoberto que o payload de 64 KiB de `274755` (@0x3a92000, FNV-1a `0x544a6f30`) são metadados de gnode do VFS com assinatura `"274755"` e referências a assets (`slidemodel.qxm`), enquanto o código executável do ZeeboApp reside embutido em `0:APPS` no manipulador Thumb `@0x10532344`.
  - Implementado `ZeeboLLESystem::dispatch_zwheel_app_start()`: instancia scratch applet + vtable gráfica (`0x28`), despacha `EVT_APP_START` (`0x1f96`) sob Unicorn ao manipulador pré-mapeado com retorno real `r0 = 1` (sucesso) e roteia chamada para o `SoftRasterizer`, reproduzindo frame RGB565 com soma `2013081600`.
  - Integrado à CLI `--efs2-run=274755` e adicionado o teste automatizado `test-efs2-zwheel` no Makefile (agora 8 alvos de CI 100% verdes).
- [ ] **Passo 11: Loop de jogo/entrada — parcial: preview host e despacho de eventos (`79ec1eb`)**:
  - Implementado `ZeeboLLESystem::run_zwheel_interactive()`: após `dispatch_zwheel_app_start()` persiste o scratch do applet (manipulador `0x10532344`, applet, pilha) e re-arma o roteamento gráfico slot 10 → `SoftRasterizer`, mantendo um loop que (a) apresenta o framebuffer RGB565 no `HostVideoSink`/tela SDL2 à taxa de quadros e (b) drena eventos de teclado/gamepad SDL2 mapeados para AVK BREW via `dispatch_zpad_to_brew` (`EVT_KEY_PRESS`/`EVT_KEY_RELEASE`, retorno real `r0=1` sob Unicorn).
  - Wired em `main`: `--efs2-run=274755` com `--seconds=N` (N>0) ou modo GUI entra no loop; `--seconds=0 --cycles=1` headless preserva o comportamento de 1 frame estático (`test-efs2-zwheel` intacto).
  - Adicionado `test-efs2-zwheel-loop` ao Makefile; suíte de 9 alvos de CI 100% verde. Validado com execução real (`--seconds=1`: 13066 frames apresentados, loop encerrado organicamente).
- [x] **Passo 12: Primitivas de Debugging Estruturado para Agentes de IA Autônomos (Concluído `38fa980`)**:
  - Implementadas primitivas RPC via TCP NDJSON/JSON-RPC no `ZeeboControlServer` (`zeebo_lle_main.cpp` + `zeebo_debug_scripting.py`):
    - `backtrace`: unwinding de pilha sob Unicorn (frames com PC, LR, SP, FP e filtros de segmentos `0:APPS`/`0:AMSS`).
    - `peek` / `poke`: leitura e escrita atômica (1, 2, 4, 8 bytes) com invalidação automática de cache de tradução JIT (`uc_ctl_remove_cache`) em páginas PF_X.
    - `vram_stat`: telemetria de framebuffer RGB565 (resolução, soma de pixels `pixel_sum`, pixel central, flag `blank`, draw calls).
    - `set_hook`: injeção dinâmica de ações e desvios sem necessidade de recompilar C++.
  - Validado via teste automatizado de cliente Python sobre instância viva com `--headless`.
  - QW2 (`e906492`, `0594592`): deduplicação opt-in no cliente Python por máscara+janelamento, contagem de omitidos e invalidação após `poke` somente quando `ok=true`; 25/25 asserções.
  - QW3 (`0b8e446`, `03bdcc1`, `2044d4f`): `ProbeRegistry` read-only com `probe.list/get` para MMU, BootInfo, IRQ, GPU e acessos não mapeados; `PageInfo=0x01111006` decodifica `min_page_log2=12`; debug-agent vivo 22/22.
  - QW8 parcial: `unmapped.unknown` mantém log estruturado e limitado de core/PC/endereço/largura/direção/valor, sem afirmar MMIO; o modo default preserva auto-map-and-continue.
  - QW12 (`7d658cb`, `f3b2ed1`): sonda Python client-only para `mempool_init`/`bi_execute`, 49/49, breakpoints limpos e nenhuma evidência fabricada em falha de UTCB/registrador. Execução viva classificada `blocked`: `fpage=0xb0d00206`, `r4=0xb0d00000`, parada em `0xb000d6dc`; `bi_execute` não atingido.
  - QW14 (`2781e18`): `--strict-unmapped` opt-in pausa no primeiro acesso desconhecido, preserva PC/página não mapeada e expõe evento estruturado; keypad conhecido não dispara e o default permanece 22/22.
- [x] **Passo 15: Harness Autônomo e Classificação Honesta de Apps (`aa3fa5c`, endurecido em `640147b`)**:
  - `tools/cpp/zeebo_debug_agent.py` oferece `peek`, `poke`, `trace`, VRAM, backtrace, catálogo e relatórios via TCP NDJSON/JSON-RPC.
  - **Prova restrita ao harness:** rotina do firmware em `0x10532344` retorna `r0=1` com objeto scratch e clear azul do host. Não comprova Z-Wheel completa, carrossel ou jogo.
  - **Carga comprovada, execução ainda não:** `reksio.mod` e `tectoy.mod` são `loaded_only`; bytes injetados e progresso genérico do Core 0 não contam como execução do applet.
  - Gate atual: 22/22 asserções; `pass` exige milestone específico por PC/retorno/bytes/pixels e `vram_blank is False`.
- [x] **Passo 13: infraestrutura BootInfo — histórico consolidado na Fase 12**; não duplicar o bloqueio antigo QW26 já corrigido.
- [ ] **Passo 14: boot completo até BREW AppMgr** — permanece aberto. `L4_ThreadControl=0x08`, `L4_Ipc=0x00`, `L4_ExchangeRegisters=0x0c`. Handoff inicial para `0x10137000` não prova AEECShell/AppMgr funcional.

---

### Fase 14: JIT, carga de applets e UX — PARCIAL; execução genérica reaberta
- [x] **Etapa 3 JIT Dynarmic Integrada e Validada (commit `c28d66a`)**:
  - `zeebo_dynarmic_core.h/.cpp` integrando `dynarmic::A32::UserConfig` para ARM1136EJ-S (ARMv6, part `0xB36`) com suporte a Thumb, SVC e MMIO interceptado.
  - Modo `--jit` (AB-testing com Unicorn shadow nas primeiras fatias) e `--jit-solo` (JIT autônomo total) no Core 0.
  - **RETRATAÇÃO PARCIAL (2026-09-10)**: a alegação "boot de 11.4M instruções validado sem regressão"
    NÃO se sustenta. O contador de instruções subia porque o backend girava em falso sobre a MESMA
    instrução: falhas do Dynarmic caíam em `ExceptionRaised`/`InterpreterFallback` de **corpo vazio**,
    não consumiam ticks e o laço principal reiniciava no mesmo PC. Contador alto era sintoma do
    defeito, não prova de execução. Ver `33a83dd` e os quatro defeitos abaixo.

- [x] **Backend recompilado: quatro defeitos corrigidos com RED confirmado (2026-09-10)**:
  - `b1638d8` — **gancho de código disparava na tradução, não na execução**. `MemoryBridge::on_code`
    era chamado 1x por bloco traduzido; criado `on_code_exec` (por instrução) + `halt_from_hook()`.
    Teste: laço de 8 voltas ⇒ 8 execuções vs 1 tradução (`test_jit_code_hook.cpp`, 5/5).
  - `33a83dd` — **falha silenciosa**. `ExceptionRaised`/`InterpreterFallback` vazios; `run()` não saía
    do laço na falha; Core0 não consultava `halted`; `state`/`reg`/`backtrace`/`peek` liam o **Unicorn**
    sob `--jit`, reportando PC=0 de um motor que não executa nada. Passa a relatar
    `[Core0/JIT] parada em pc=... (fallback de interpretador)` (`test_jit_fault_report.cpp`, 8/8).
    RED: sem as saídas de laço o teste **trava** (exit 124) — o RED honesto aqui é o travamento.
  - `24d7d26` — **`CPS` não implementada**. O opcode em `0xf0003adc` é `0xf10800c0` = `cpsie if`
    (kernel habilitando IRQ/FIQ no boot). O Dynarmic decodifica CPS mas **delega ao interpretador**
    (`arm_CPS -> InterpretThisInstruction`), que este projeto não acopla. Semântica do encoding A1
    implementada na nossa camada, sem tocar em fonte de terceiro. Boot: 114.281 ⇒ 1.168.798 instruções.
  - `5471a8b` — **CP15 não guardava estado**. Toda escrita MCR era descartada (`CompileSendOneWord`
    ⇒ `NopFn`) e toda leitura fora de MIDR/CTR devolvia zero. O boot escreve TTBR0 (c2) e o controle
    do sistema (c1) e os lê de volta; o JIT lia 0. Banco indexado por `(opc1,CRn,CRm,opc2)` — indexar
    só por CRn faria TTBR0/TTBR1 se sobrescreverem (`test_jit_cp15.cpp`, 7/7; RED: 4 falhas lendo 0).
  - **Resultado negativo registrado**: a hipótese de que os backends divergiam por **flags** foi
    REFUTADA pelo lockstep (`test_jit_lockstep.cpp`) — NZCV concordam. Não reinvestigar.

- [x] **Divergência de boot entre backends — ZERADA (`28fdf79`); resta implementar `LDM_usr`/`RFE`**:
  - Ferramenta: `--trace-core0=<arq> --trace-limit=N` grava a trajetória do Core0 pelos **dois**
    backends (o gancho `c0_code_hook` é comum) e `tools/cpp/diff_traces.py` acha a primeira
    divergência exata de PC/flags/registradores. **Não usar o log periódico** para isso: ele amostra
    a cada 10 mil instruções, e a "divergência de 8 bytes no ciclo 2" antes relatada era artefato
    da amostragem, não defeito real.
  - Estado: primeira divergência recuou de #16723 (CP15) para **#23726**, em
    `ldreq r3,[r5]` @ `0xf000a1d0` — r3 = `0x10090001` no interpretado, `0` no recompilado.
    Carga condicional de memória; defeito distinto do CP15.
  - **O boot continua NÃO fechando.** Contador maior (1,17M no JIT vs 596k no interpretado) **não**
    é prova de correção — pode significar executar lixo por mais tempo. O que sustenta progresso
    aqui é a divergência ter recuado, não o contador ter subido.

- [x] **Varredura AMPLIADA de lacunas do Dynarmic + fontes do tripleoxygen/OpenZeebo (2026-09-10)**:
    Revarredura completa, indo além do `InterpretThisInstruction`:

    - **Fallback real** (a lacuna que nos afeta): continuam sendo **exatamente 6** em A32
      (`arm_LDM_usr` x2, `arm_LDM_eret`, `arm_STM_usr`, `arm_CPS`, `arm_RFE`, `arm_SRS`)
      e **zero** em Thumb. Nada novo apareceu.
    - **`UndefinedInstruction` NÃO é lacuna**: são ~centenas de sítios em ASIMD/NEON, mas
      representam *encodings inválidos* — recusar é o comportamento correto. Não confundir
      os dois ao contar "o que falta".
    - **Tabela de decodificação A32**: 261 entradas. Checadas as instruções ARMv6/ARM11
      sensíveis — `MCRR`, `MRRC`, `LDREX`, `STREX`, `SWP`, `SWPB`, `SETEND`, `SEV`, `WFI`,
      `WFE`, `CLREX`, `BKPT`, `MRS`, `LDC`, `STC`, `CDP`, `MCR`, `MRC` estão **todas presentes**.
      `MSR`/`PLD` existem com sufixo (`arm_MSR_imm`/`arm_MSR_reg`, `arm_PLD_imm`/`arm_PLD_reg`) —
      um grep ingênuo pelo nome puro dá falso negativo. Ausentes de fato: apenas `DBG` e `SMC`,
      **irrelevantes** aqui (depuração e TrustZone).
    - **Contagem no boot real** (400k instruções, interpretado): `CPS` 18x, `LDM_usr` 1x,
      `RFE` 1x; `STM_usr`, `SRS`, `SETEND`, `SWP/SWPB`, `LDREX/STREX`, `WFI/WFE/SEV`, `PLD`
      e `BKPT` = **ZERO**. Nenhum acesso a coprocessador != CP15 (sem VFP/NEON no boot).
    - **Conclusão**: após `559dcf9` não há lacuna de instrução conhecida pendente. A
      divergência em `#181306` **não** era instrução não traduzida — era assimetria
      VTLB↔Unicorn, corrigida em `45e6bb1`.
    - **Revarredura em traço 7,5x maior (3.000.000 de instruções, 2026-09-10)**: repetida
      a contagem no traço longo para vencer a limitação "zero só vale até onde o traço
      alcança". Resultado **idêntico**: `CPS` 18x, `LDM_usr` 1x, `RFE` 1x; todas as demais
      (`STM_usr`, `SRS`, `SETEND`, `SWP/SWPB`, `LDREX/STREX`, `PLD`, `BKPT`) seguem em
      **zero**, e nenhum acesso a coprocessador != CP15. **Thumb: 0 instruções** — o Core0
      roda 100% em ARM no boot, então a cobertura integral de Thumb do Dynarmic é
      irrelevante aqui.
    - **Core1 (ARM926EJ-S) não usa Dynarmic**: é `uc_open` puro (Unicorn), logo lacunas de
      tradução do Dynarmic **não o afetam**. Toda esta análise vale só para o Core0.
    - **Paridade dos backends em 3.000.000 de instruções: divergência ZERO** (antes o
      máximo medido era 400.000).

### Auditoria de emuladores de terceiros (2026-09-10) — o que se confirmou na fonte

Pesquisa delegada sobre PCSX2/Dolphin, Azahar/Citra, Ryujinx/Eden, emuladores de
feature phone e QEMU. **Só entram aqui afirmações que eu verifiquei na fonte real**;
o resto foi descartado. Nada de código de terceiros foi copiado.

- **Nenhum emulador consultado resolve o nosso problema de instrução privilegiada —
  todos o EVITAM por serem HLE.** No Azahar (`src/core/arm/dynarmic/arm_dynarmic.cpp:83`)
  o `InterpreterFallback` é literalmente `UNREACHABLE_MSG` ("Should never happen"):
  como o kernel do 3DS é reimplementado em C++, o guest nunca executa modo supervisor
  e as 6 instruções nunca aparecem. Confirmado baixando o arquivo. Nós somos LLE e
  **temos** que executá-las — foi o que fizemos em `559dcf9`, e a decisão de
  implementar na nossa camada (em vez de delegar) está validada.
- **Delegar ao interpretador seria um beco sem saída em host ARM64**: no Dynarmic,
  `backend/arm64/emit_arm64_a32.cpp:37` faz `ASSERT_FALSE("Interpret should never be
  emitted.")`, enquanto só o backend x64 (`backend/x64/a32_emit_x64.cpp:1133`) chama
  `InterpreterFallback`. Verificado no fonte local. Nosso host é x86_64 hoje, mas a
  interceptação pré-tradução que adotamos é a única portável para handhelds ARM64.
- **CP15 "reads ignored" não existe na API do Dynarmic — o idioma é ponteiro-sumidouro**:
  o Citra/Azahar (`arm_dynarmic_cp15.cpp:29-49`) devolve `&state.cp15_flush_prefetch_buffer`,
  `&state.cp15_data_sync_barrier` etc. para escritas que devem ser NOP, e
  `std::monostate{}` para o resto. Confirma independentemente a limitação que
  encontramos sozinhos e valida nosso `emulate_reads_ignored_cp15`.
- **O Dynarmic já traz um comparador Unicorn↔JIT** (`tests/A32/fuzz_arm.cpp`), sob
  licença **permissiva** (0BSD) — diferente de PCSX2/Dolphin/Citra/QEMU, que são
  copyleft. Detalhe aproveitável: `fuzz_arm.cpp:429` normaliza o PC porque "Qemu
  doesn't do Thumb transitions??" — classe de falso-positivo que pode nos morder ao
  comparar Unicorn (QEMU) contra Dynarmic. Irrelevante hoje (boot é 100% ARM), mas
  registrado para quando houver Thumb.
- **Não existe emulador público do Qualcomm MSM7201A.** Há apenas um RFC de 2026-05
  na lista qemu-devel, sem código. O parente mais próximo é o `qemu-calypso` (baseband
  TI Calypso, dois cores + firmware não-patcheado), que **não declara licença** — tratar
  como todos-os-direitos-reservados, só arquitetura. O `zeebo-lle` não tem precedente
  público rodando AMSS/L4/REX.
- **Critério de aceitação adotado do qemu-calypso**: a plataforma só está emulada quando
  **mais de um** dump de firmware boota sem hacks específicos. Distingue "emulo o
  MSM7201A" de "fiz este dump andar". Vale como gate futuro do boot LLE.
- **Estratégia validada por FirmWire (BSD-3)**: rodar firmware real e **stubar
  explicitamente** as partes intratáveis (RF/L1/DSP). Aplica-se diretamente ao Zeebo,
  que é "um celular sem rádio".

### config.page_table do Dynarmic — verificado, e a decisão de NÃO ligar agora

Batelada de pesquisa (`deleg_7d00ff0c`) recomendou como item nº1 eliminar a
"segunda fonte de verdade" passando nossa LUT ao `config.page_table` do Dynarmic
(modelo Citra/Azahar). Verifiquei na fonte, e a parte factual **confere**:

- `interface/A32/config.h:158` — `std::array<std::uint8_t*, NUM_PAGE_TABLE_ENTRIES>* page_table`,
  `PAGE_BITS = 12`, `NUM_PAGE_TABLE_ENTRIES = 1 << 20` (`:156-157`).
- Semântica default (`absolute_offset_page_table = false`, `:165`):
  `page_table[addr >> bits][addr & mask]` — **idêntica** à da nossa `VtlbLut`
  (`zeebo_l4_mmu.h:183-190`, mesmo 4KB/2^20, `translate()` faz `base + (va & PAGE_MASK)`).
- A invariante "MMIO nunca tem ponteiro" **já vale** aqui: `zeebo_l4_mmu.h:416` só
  chama `lut->map()` quando o Unicorn aceitou o host_ptr; MMIO nunca entra na LUT.

Também descobri **código morto**: `DynarmicCore::enable_page_table()` existe
(`zeebo_dynarmic_core.cpp:566`, declarada em `.h:111`) e **nunca é chamada**.
Hoje todo acesso do JIT vai por callback.

**Decisão: NÃO ligar agora.** O `config.page_table` é um fast-path inline no
código recompilado: ele serve loads/stores direto do ponteiro de página, sem
passar pela nossa bridge (`zeebo_lle_main.cpp:1271-1329`). É justamente na bridge
que vive o vigia VTLB↔Unicorn que capturou a causa raiz de `#181306`
(`READ16 ... origem=VTLB uc_diz=...`). Ligar o fast-path hoje **cegaria o
lockstep de memória** — nossa única defesa contra falha silenciosa — em troca de
performance que não é o gargalo (o boot para por panic do Core1, não por lentidão).

Impedimento adicional, menor: nossa LUT é `std::vector<u8*>` (`zeebo_l4_mmu.h:253`)
e a API exige `std::array` — mudança de tipo, não de arquitetura.

**Pré-condição para reconsiderar**: o boot chegar ao AppMgr e a performance virar
gargalo medido. Aí a troca certa é ligar `page_table` **e** mover o vigia para um
modo de verificação opcional, não removê-lo.

*(Nota: a premissa do subagente de que temos "duas fontes de verdade" está
desatualizada — isso foi corrigido em `45e6bb1`. Hoje a VTLB só é atualizada
quando o Unicorn adota o mesmo host_ptr.)*

### qemu-ios (devos50) — o precedente mais próximo que existe

Auditado no fonte (branch `ipod_touch_2g`, clonado e lido; **GPLv2** — só arquitetura,
não copiar). É o parente técnico mais próximo do zeebo-lle encontrado até agora:

- **`hw/arm/ipod_touch_2g.c:544` → `ARM_CPU_TYPE_NAME("arm1176")`** — a MESMA CPU que
  configuramos. iPod touch 1G/2G são ARMv6 (S5L8900/S5L8720), SoC móvel da mesma era do
  MSM7201A. E, diferente de Azahar/Citra/Ryujinx, **é LLE de verdade**: boota bootloader
  e kernel reais sem modificar os binários.
- **27 periféricos modelados** para chegar à interface gráfica. Dá a escala honesta do
  que falta: nosso alvo não é "mais uma correção", é um conjunto de periféricos.
- **Padrão do periférico desconhecido (adotar)**: existe um device chamado literalmente
  `ipod_touch_unknown1.c` (56 linhas, `0x3D700000`) — um bloco de MMIO cuja função o
  autor NÃO descobriu, mas que virou device nomeado: loga todo offset acessado, devolve
  `0` por padrão e só tem valor mágico onde o firmware exigiu (`0x140→0x2`, `0x144→0x3`).
  **Dar nome e endereço ao desconhecido, em vez de esperar entendê-lo, é o que destrava
  o boot.** 6 dos 27 periféricos logam todo acesso.
- **Duas políticas OPOSTAS para registrador desconhecido, deliberadamente**:
  `ipod_touch_chipid.c` usa `hw_error(...)` (**aborta ruidosamente**) porque um chip ID
  errado corromperia silenciosamente todo o boot; o `unknown1` devolve `0` e segue. A
  lição não é "escolha uma", é **falhar alto onde o valor importa e seguir quieto onde
  não importa**. Nossa política atual de MMIO desconhecido não faz essa distinção.
- **Começar de um estágio de boot mais tardio é decisão legítima**: o autor NÃO
  conseguiu rodar o bootrom (salta para código fundido no silício, ausente do dump) e
  **deliberadamente pulou para o iBoot**. Precedente direto para nossa dúvida entre boot
  LLE fiel desde o início vs. entrar depois.
- **Escolher a versão mais antiga do firmware por ter menos segurança** foi decisão
  explícita dele (iPhoneOS 1.0, sem trust cache) — evitou ter de driblar crypto.
- Referência adicional citada por ele: **openiBoot** (reimplementação de bootloader), que
  foi como entendeu periféricos não documentados.

- [ ] **CP15 exercitado pelo boot vs. o que modelamos (levantado 2026-09-10)**:
    Inventário dos `MCR p15` realmente executados no boot, por (CRn,CRm,opc1,opc2):

        c1,c0   x3      c2,c0   x3      c3,c0   x1
        c7,c5   x4      c7,c6   x1      c7,c10  x156     c7,c14  x3842
        c8,c7   x6      c10,c0  x8      c13,c0  x3

    O grosso (`c7` = cache/barreiras, `c8` = TLB) é manutenção e pode ser no-op sem dano.
    **`c13,c0` merece atenção**: é o par FCSE PID (opc2=0) / **Context ID** (opc2=1) /
    Thread-Process ID (opc2=2). O firmware escreve Context ID **1** em `#76234`, depois
    Thread ID 0 e Context ID **2** em `#118306` — ou seja, **troca de contexto de processo**,
    20 instruções antes do `LDM_usr` de `0xf000bcac`. Se o Context ID influencia tradução de
    endereço (FCSE) e não o modelamos, a divergência posterior pode vir daí. **A verificar.**

- [x] **Fontes do tripleoxygen/OpenZeebo já presentes em `docs/remote/openzeebo/` (2026-09-10)**:
    Confirmado: o material do tripleoxygen (`github.com/tripleoxygen/openzeebo`, GPLv2) já
    está no projeto. Achados de hardware que **não estão modelados** hoje:

    - `asm/arm11_jtag.S` — o "ARM11 enabler": no hardware real, ligar o ARM11 exige
      escrever em **`0xa9000254` = 0x57** (pull-up nos pinos MODE) e
      **`0xa900026c` = 0b11** (modo ARM11-only). Base `0xa9000000` é o bloco GPIO.
      Nenhum dos dois offsets é modelado no nosso MMIO.
    - `tools/zloader/notes.txt` — mapa de **MPUs** não documentado no TRM:
      `0xa0b00000` (NAND, enable em +0x0), `0xa0e00000` (Peripheral, +0x400),
      `0xa8240000` e `0xa8250000` (+0x800). Mais setup de UART1/GPIO
      (clock `0xa86000e0=0x30`, gpio45/46) — útil para saída de console real.
    - Sequência de boot do ARM11 em ROM, com **`MCR p15,0,r0,c15,c2,4`** =
      *Peripheral Port Memory Remap* (TRM p.3-164). O boot que traçamos **não** executa
      `c15,c2` — provavelmente porque nosso ponto de entrada é posterior a essa ROM.
    - **Licença**: OpenZeebo é **GPLv2**, mesma restrição do QEMU. Ler/entender endereços e
      sequências (fatos de hardware) é livre; **copiar código** não.

- [x] **Inventário do que o Dynarmic NÃO traduz — COMPLETO: exatamente 6 A32 (`a60d92b`)**:
  - O Dynarmic delega ao interpretador apenas **6 instruções A32**, todas de modo privilegiado —
    verificado em `third_party/dynarmic/.../translate/impl/`:
    `arm_CPS`, `arm_RFE`, `arm_SRS` (`status_register_access.cpp`) e
    `arm_LDM_usr`, `arm_LDM_eret`, `arm_STM_usr` (`load_store.cpp`).
    **Thumb não tem nenhuma** (`InterpretThisInstruction` = 0 ocorrências nos tradutores Thumb).
    Como este projeto não acopla interpretador, cada uma PARA o Core0 sob `--jit`.
  - Varredura linear do AMSS acha 173.430 candidatos, mas isso é **ruído**: dados e código Thumb
    são lidos como palavras ARM (há ASCII entre os achados). O número útil vem do **traço de
    execução**, não da varredura. Executadas de fato no boot interpretado (596k instruções):
    apenas **4 endereços distintos**.

  | PC | opcode | instrução | situação |
  |---|---|---|---|
  | `0xf0003adc` | `0xf10800c0` | `cpsie if` | **resolvido** em `24d7d26` |
  | `0xf0003b04` | `0xf10c00c0` | `cpsid if` | **resolvido** em `24d7d26` |
  | `0xf000bcac` | `0xe9dd7fff` | `ldmib r13,{r0-r14}^` | **pendente** |
  | `0xf000bcb8` | `0xf8bd0a00` | `rfeia r13!` | **pendente** |

  - As duas pendentes são adjacentes e formam a sequência clássica de **retorno de tratador de
    exceção**: restaura o banco de registradores de **usuário** (sufixo `^`, bit S) e depois
    recarrega PC+CPSR de uma vez. Sem elas não há retorno de IRQ/SVC, então o boot não pode
    progredir além do primeiro tratador — implementar as duas **juntas**, com acesso ao banco
    de registradores de usuário (não ao banco do modo corrente).
  - O boot interpretado as alcança na instrução **#118326**. O JIT ainda **não** chega lá
    (executa as 18 CPS, zero RFE/LDM_usr): a divergência #23726 o desvia antes. Ou seja, são
    bloqueios **reais e já enfileirados**, mas a divergência de memória vem primeiro.
  - Referência normativa disponível localmente (não buscar na web): `docs/remote/` traz
    `DDI0211K_arm1136_r1p5_trm.pdf` (934 pp., ARM1136 = Core0) e
    `DDI0198E_arm926ejs_r0p5_trm.pdf` (264 pp., ARM926EJ-S = Core1).

- [x] **Estado de RESET do CP15 corrigido — lição vinda do FONTE do QEMU (`test_jit_cp15_reset.cpp`)**:
  - Método: em vez de só interrogar o binário, li `target/arm/tcg/cpu32.c` do QEMU. Como o
    Unicorn é fork do QEMU, **essas definições são as que o nosso motor interpretado usa** — logo
    servem de especificação para o recompilado, sem copiar uma linha de código.
  - Valores do QEMU para a família (idênticos em 1136, 1136_r2 e 1176):

        ctr          = 0x01dd20d2
        reset_sctlr  = 0x00050078   <-- NÃO é zero (bits W/P/D/L ligados)
        midr         = 0x4107b362 (1136_r2) / 0x4117b363 (1136) / 0x410fb767 (1176)

  - **Defeito encontrado**: nosso banco CP15 nascia inteiramente **zerado**. Uma leitura de
    `SCTLR` (c1,c0,0) antes da primeira escrita devolvia `0`, enquanto o interpretado devolvia
    `0x00050078`. É a mesma classe do defeito de `5471a8b`, mas de outra origem: lá o banco não
    guardava o que era **escrito**; aqui não tinha o valor **inicial**. O boot do AMSS faz
    read-modify-write de SCTLR, então partindo de 0 todos os bits de reset se perdiam.
  - Também corrigidos, pelo mesmo motivo: `ctr` (era `0x1D152152`, valor sem procedência) e
    `midr`, que agora acompanha o modelo realmente configurado no Unicorn (`arm1176`).
  - RED confirmado: com o código anterior, **3 asserções falham** (SCTLR `0x00050078` vs `0`,
    CTR `0x01dd20d2` vs `0x1d152152`, MIDR `0x410fb767` vs `0x4107b362`). GREEN: 3/3.
    `make check` = exit 0, sem falhas.
  - **Resultado honesto sobre o boot: a divergência NÃO se moveu.** Continua em **#23726**, com
    os mesmos valores (`r3 = 0x10090001` vs `0`) e as mesmas contagens (596.725 vs 1.168.764
    instruções). A correção elimina uma divergência real de estado entre os backends, mas **não
    é a causa** da parada do boot. Registrado assim para não virar falso progresso.
  - Isto **resolve parcialmente** a incoerência de identidade de CPU descrita acima: os dois
    backends agora respondem `arm1176`. Continua **em aberto** qual é a CPU correta do Zeebo — o
    TRM que temos é do ARM1136 r1p5, o que sugere que o certo seria alinhar tudo à família 1136,
    não ao 1176. Trocar exige refazer os traços (muda o oráculo).

- [x] **Resultado NEGATIVO — a família de CPU (1136 vs 1176) NÃO afeta a divergência do boot**:
  - Experimento: troquei o Core0 para `UC_CPU_ARM_1136_R2` no Unicorn **e** o `midr` do Dynarmic
    para `0x4107B362`, alinhando os dois backends à família ARM1136 (a do TRM que temos).
  - Resultado: **idêntico em tudo** — mesma primeira divergência (#23726), mesmos valores
    (`r3 = 0x10090001` vs `0`), mesmas contagens (596.725 vs 1.168.764 instruções).
  - Conclusão: a escolha entre 1136 e 1176 **não influencia** o defeito do boot. O experimento foi
    revertido (o código segue em `arm1176`, agora coerente nos dois backends). A pergunta de qual
    é a CPU historicamente correta continua aberta, mas deixou de ser prioridade — não é o
    caminho para destravar o boot.

- [ ] **Incoerência de identidade de CPU entre os dois backends (parcialmente resolvida)**:
  - Levantado ao investigar o que o QEMU teria a ensinar (o Unicorn é um **fork do QEMU**, então
    o modelo de CPU dele *é* o modelo do QEMU; ver seção de licença abaixo).
  - Medido: `MIDR` que cada modelo do QEMU/Unicorn reporta (`mrc p15,0,Rd,c0,c0,0`):

        arm1136     MIDR=0x4117b363
        arm1136-r2  MIDR=0x4107b362   <-- identico ao que declaramos
        arm1176     MIDR=0x410fb767   <-- o que realmente configuramos
        arm926      MIDR=0x41069265

  - **A incoerência**: `zeebo_dynarmic_core.h:37` declara `midr = 0x4107B362` (família ARM1136),
    mas todo o código configura o Unicorn como `UC_CPU_ARM_1176`, que reporta `0x410fb767`.
    Os dois backends respondem **identidades de CPU diferentes** para o mesmo firmware.
  - Ocorrências de `UC_CPU_ARM_1176` para o Core0: `zeebo_lle_main.cpp:1185`, `zeebo_boot.cpp:337`,
    `zeebo_dual_core.cpp:203`, além de harnesses e de `test_jit_lockstep`/`test_jit_unaligned_ldr`.
    (Core1 usa `UC_CPU_ARM_926`, coerente com o ARM926EJ-S.)
  - **Não corrigir no escuro**: o valor `0x4107B362` está marcado como *"CONFIRMAR"* no plano
    `.hermes/plans/2026-09-09_etapa3-dynarmic-integration.md:326` — nunca foi confirmado contra
    hardware real. Busca por ambas as constantes na `nand/1.1.2_AMSS.bin` deu **0 ocorrências**,
    então o firmware não compara MIDR com literal embutido (pelo menos não em palavra crua).
  - Impacto plausível, **não medido**: identidade de CPU divergente muda caminho de código no boot
    (ARM1136 e ARM1176 diferem em VMSA/TLB/CP15). Pode ou não ter relação com a divergência
    #23726 — **não afirmar** relação sem medir.
  - Próximo passo: decidir qual é a CPU real do Zeebo (o TRM em `docs/remote/` é do **ARM1136
    r1p5**, o que favorece a família 1136), alinhar os DOIS backends ao mesmo modelo e observar
    se a divergência do boot se move. Trocar o modelo do Unicorn muda o comportamento do
    oráculo — refazer os traços depois.

- [x] **QEMU como fonte de aprendizado: decidido — ler/compreender sim, copiar não (`6c94135`)**:
  - **Restrição legal (decidir antes de copiar qualquer linha)**: QEMU é **GPLv2**. Copiar código
    dele para este projeto tornaria o resultado uma obra derivada sob GPLv2. O `Dynarmic` que
    usamos é **licença permissiva** (estilo ISC/0BSD: "permission to use, copy, modify, and/or
    distribute ... with or without fee"). Este repositório **não tem arquivo LICENSE** — ou seja,
    a licença do próprio projeto está indefinida. **Não copiar fonte do QEMU** enquanto isso não
    for decidido explicitamente pelo dono do projeto.
  - **Já dependemos do QEMU indiretamente**: o Unicorn é um **fork do QEMU** e o pacote instalado
    (`libunicorn 2.1.1`) declara `GPL-2` / `LGPL-2+` no copyright. Ligamos com `-lunicorn` em
    praticamente todos os binários. Isso é **linkagem**, não cópia de fonte — situação diferente,
    mas que reforça a necessidade de definir a licença do projeto.
  - **O que o QEMU NÃO oferece**: nenhuma máquina que sirva de referência para o SoC do Zeebo.
    `qemu-system-arm -machine help` lista 107 máquinas; as únicas Qualcomm são BMCs Cortex-A7
    (`qcom-dc-scm-v1-bmc`, `qcom-firework-bmc`), sem relação com o MSM do Zeebo. Não há modelo de
    MSM7xxx para copiar — a parte específica do console (QDSP5, GPU, EFS2, BREW) continua sendo
    trabalho original nosso.
  - **O que o QEMU JÁ nos dá, sem copiar nada**: os modelos de CPU. Como o Unicorn é fork do
    QEMU, `uc_ctl_set_cpu_model()` seleciona exatamente as definições de CPU do QEMU — incluindo
    `arm1136`, `arm1136-r2`, `arm1176` e `arm926`. Foi assim que a incoerência de MIDR acima foi
    medida. **Aprender comportamento observando o binário é legítimo e não cria obra derivada**;
    copiar o fonte é que cria.
  - **O fonte do QEMU serve como ESPECIFICAÇÃO, sem ser copiado.** `target/arm/tcg/cpu32.c`
    contém os valores de reset de cada CPU (`midr`, `ctr`, `reset_sctlr`, `reset_auxcr`, features).
    Como o Unicorn é fork do QEMU, esses são **exatamente** os valores que o nosso motor
    interpretado usa — ou seja, são a especificação contra a qual o recompilado tem de bater.
    Ler um valor numérico de referência e implementá-lo por conta própria **não é obra derivada**;
    copiar a implementação seria. Foi assim que o defeito de `reset_sctlr` foi encontrado.
  - Arquivos do QEMU com maior valor para este projeto (para consulta futura):
    `target/arm/tcg/cpu32.c` (definições/reset das CPUs ARM32),
    `target/arm/cpu.h` (bits de SCTLR e flags de feature).
  - Uso recomendado: tratar QEMU/Unicorn como **oráculo executável** (comparar comportamento), o
    fonte do QEMU como **fonte de valores de referência**, e o TRM em `docs/remote/` como
    **referência normativa** — escrevendo a implementação por conta própria, exatamente o método
    já usado em `CPS`, `CP15` e no reset do CP15.

- [x] **Resultado NEGATIVO — LDR desalinhado NÃO é a causa da divergência #23726**
      (`test_jit_unaligned_ldr.cpp`; registrado para ninguém reinvestigar):
  - Hipótese: a instrução que diverge seria `ldreq r3,[r5]` com **r5 = 0x00000002**, uma carga de
    endereço **desalinhado**. (A premissa em si era falsa: `r5` vale `0xf401ffc0` e está
    alinhado — eu havia lido a coluna errada do traço. O teste continua válido como regressão,
    mas a motivação original não existia.) O TRM do ARM1136 (DDI0211K, p.210) diz que o bit U vale 0 no reset
    e que nesse modo o processador "treats unaligned loads as rotated aligned data accesses".
    Seria uma explicação limpa: interpretado rotacionando, recompilado lendo literal.
  - **Refutada por medição.** Controle que separa os dois modelos: memória `11223344 55667788`,
    `ldr` de `+2`. O modelo rotacionado daria `0x33441122`; o Unicorn como ARM1176 devolve
    **`0x77881122`** = leitura **literal**. Ler byte-a-byte do endereço cru — como a VTLB faz —
    reproduz o oráculo. Nada a consertar aqui.
  - **Ressalva sobre o oráculo (medida depois)**: o Unicorn devolve `SCTLR = 0` no reset (bit
    U = 0) e **mesmo assim** faz leitura literal. Ou seja, ele **não modela** o comportamento
    rotacionado descrito no TRM — não é que o firmware tenha habilitado U=1, é que o oráculo
    ignora esse bit. Consequência honesta: os dois backends **concordam entre si**, que é o que o
    teste trava, mas nenhum dos dois foi provado fiel ao silício aqui. Se o boot algum dia
    depender de rotação, este é um ponto onde emulador e hardware real podem divergir.
  - Cuidado metodológico: com memória zerada além da palavra, "rotação" e "leitura literal" dão
    **o mesmo resultado**. O primeiro teste que escrevi não distinguia os dois e teria
    "confirmado" a hipótese errada; foi preciso um caso com **duas palavras não nulas adjacentes**.
  - O teste ficou no `check` como **regressão** (trava a concordância dos motores em carga
    desalinhada, para que uma futura "correção" não introduza rotação indevida). Ele **passa** —
    não reproduz o defeito do boot.
  - **A causa real segue ABERTA** e é de outra natureza: os dois motores executam a mesma
    instrução, no mesmo PC, com as mesmas flags (`nzcv=0x60000000`, Z=1 nos dois), lendo o
    **mesmo endereço**, e obtêm valores diferentes (`0x10090001` vs `0`). Divergem no **conteúdo
    da memória**, não na semântica da instrução. (O endereço real é `r5 = 0xf401ffc0`, não `0x2`
    como registrei antes por ler a coluna errada do traço — ver a entrada de caracterização
    abaixo.)
  - Próximo passo: comparar o **conteúdo da memória** entre os backends (não só
    registradores) e rastrear quem escreveu — ou deixou de escrever — a região lida.

- [x] **Divergência #23726 — RESOLVIDA (`f4bac04`): escrita perdida + assimetria de mapeamento**:
  - **CORREÇÃO de um erro anterior deste ROADMAP**: eu havia registrado que a carga era
    `ldreq r3,[r5]` com **`r5 = 0x00000002`** (ponteiro absurdo, sugerindo memória corrompida).
    **Estava errado** — eu lia a coluna errada do traço. O formato real da linha é
    `# pc nzcv opcode r0..r15`, portanto `r3` é a **coluna 7** e `r5` a **coluna 9**.
    O valor real é **`r5 = 0xf401ffc0`**, um ponteiro perfeitamente alinhado e plausível.
    (Isto também derruba a motivação original da hipótese de acesso desalinhado.)
  - Fatos estabelecidos sobre `0xf401ffc0`:
    - **Não é periférico**: não cai em nenhuma janela conhecida (`MSM_CSR` `0xc0100000`,
      `SMEM` `0x01f00000`, `MDDI` `0xaa600000`, `ADRENO` `0xa0000000`, `VIC` `0xc0000000`),
      logo `is_core0_peripheral()` responde falso e a leitura vai para a memória normal.
    - **Não vem da imagem**: o deslocamento `0x401ffc0` na janela AMSS (`0xf0000000`) está
      **além do fim** de `nand/1.1.2_AMSS.bin` (21.626.880 B = `0x14a0000`). Não é conteúdo
      carregado de arquivo.
    - **Nenhuma escrita do Core0** para essa região aparece no traço antes de #23725.
    - Fora do contexto do boot, os **dois** backends leem `0x00000000` ali (`diff_memory.py`,
      que usa a sonda `peek` do servidor de controle). O conteúdo estático é igual.
  - Conclusão parcial: o valor `0x10090001` que o interpretado enxerga é **produzido em tempo de
    execução por algo que não é o Core0** — candidatos: Core1 (ARM926), DMA, injeção de SMD/RPC,
    ou um mapeamento de VTLB que só existe em um dos caminhos. É uma região vizinha de outras em
    uso ativo (`0xf4090000`, `0xf401fe40` aparecem como base em passagens seguintes pelo mesmo
    trecho de código), ou seja, `0xf401xxxx` é uma área viva, não lixo.
  - **Ferramenta nova**: `tools/cpp/diff_memory.py` — sobe o emulador nos dois backends e compara
    o conteúdo do mesmo endereço via `peek`. Serve para separar "divergência de semântica de
    instrução" de "divergência de estado de memória".
  - **CAUSA RAIZ ENCONTRADA — a escrita se perde silenciosamente no caminho recompilado.**
    O vigia `--watch-writes` mostrou que **os dois backends emitem as mesmas escritas**:

        core0 pc=0xf000a32c addr=0xf401ffc0 valor=0x00000001
        core0 pc=0xf000a35c addr=0xf401ffc0 valor=0x10090001   <- interpretado E recompilado

    Mas no recompilado a leitura seguinte do MESMO endereço devolve `0`:

        READ addr=0xf401ffc0 valor=0x00000000 origem=UC

    Ou seja: **o defeito não está na semântica do store nem na do load** — a escrita
    simplesmente não persiste. (Isto corrige a suspeita anterior de que o Core0 "não escrevia":
    ele escreve; o dado é que se perde.)
  - Mecanismo: `bridge.write32` (`zeebo_lle_main.cpp:1299`) faz

        s->vtlb_.write_u32(addr, val);          // retorna bool  — DESCARTADO
        if (s->core0_.uc) uc_mem_write(...);    // retorna uc_err — DESCARTADO

    `0xf401f000` **não está mapeado**, então as duas falham e ninguém percebe: o firmware
    prossegue como se tivesse gravado, e relê zero. Falha silenciosa clássica.
  - Teste `test_jit_lost_write.cpp` trava o contrato, com controle positivo (página mapeada:
    escreve e relê o valor) e o caso real (`0xf401ffc0`: escrita **rejeitada** e leitura falha
    junto). 5/5.
  - **CORRIGIDO — a assimetria era o mapeamento sob demanda.** O motivo de `0xf401f000` não
    estar mapeado no recompilado:
    - No interpretado, o store parte do **código emulado**, dispara `UC_HOOK_MEM_WRITE_UNMAPPED`
      e o `c0_unmapped_hook` faz *"map dynamically to continue discovery"* (`uc_mem_map` da
      página) — a escrita então **acontece**.
    - No recompilado, a escrita passa por `bridge.write32` → `uc_mem_write()`, que é **API
      externa e não dispara hooks**. O handler nunca roda, ninguém mapeia, e a escrita se perde
      com `UC_ERR_WRITE_UNMAPPED` (7) — retorno que era descartado.
    - Conserto: dar **paridade** ao caminho recompilado — ao receber `UC_ERR_WRITE_UNMAPPED`,
      mapear a página e repetir a escrita, exatamente o que o interpretado já fazia. Não é
      silenciar o sintoma: é replicar a política de descoberta que o outro backend usa.
  - Teste `test_jit_unmapped_asymmetry.cpp` (7/7) reproduz a assimetria em engine isolado, sem
    firmware: prova que `uc_mem_write()` falha e **não** chama o hook, enquanto o mesmo store
    executado como código emulado chama o hook, mapeia e persiste o valor.
  - **RESULTADO MEDIDO — a divergência recuou de #23726 para #67395** (2,8× mais fundo), e o
    recompilado passou de 23.726 para **118.326 instruções** antes de parar. Agora ele para
    exatamente em `0xf000bcac` = `ldmib r13,{r0-r14}^` (`LDM_usr`), uma das instruções que o
    Dynarmic não traduz e que eu havia catalogado como pendente — ou seja, o boot avançou até o
    próximo obstáculo **já conhecido e inventariado**.
  - Nova primeira divergência (#67395): `pc=0xf0009de0`, precedida de `0xee1a0f10` =
    **`MRC p15, 0, r0, c10, c0, 0`** (registrador de TLB lockdown). `r0` = `0xc0` no
    interpretado contra `0` no recompilado.

- [x] **CP15 `c10` (TLB lockdown) — divergência do boot ZERADA (mas ver RESSALVA do TRM abaixo)**:
  - Diagnóstico: `r0` **já valia `0xc0` antes** do `MRC` (instrução #67392 = `mov r0,#0xc0`), e
    **nenhuma escrita a `c10` ocorre antes** no traço. Ou seja, `0xc0` não é conteúdo do
    registrador nem valor de reset: o interpretado simplesmente **preserva** `r0`, enquanto o
    nosso banco CP15 genérico devolvia o slot (zero, nunca escrito) e **sobrescrevia** o destino.
  - Confirmação no oráculo em engine limpo: `MRC c10,c0,0` devolve **`0`**, não `0xc0` — o que
    já descarta a hipótese de "valor de reset" que eu poderia ter assumido do traço.
  - Fonte do QEMU (`target/arm/helper.c`): `TLB_LOCKDOWN` (crn=10, crm 0/1, `CP_ANY`) é
    `ARM_CP_NOP`, e `cpregs.h` define: *"no change to PE state: writes ignored, reads ignored"*.
  - **Limitação da API do Dynarmic**: `Coprocessor::CompileGetOneWord` **não consegue expressar**
    "reads ignored" — o retorno `std::uint32_t*` e o `Callback` **sempre escrevem** no registrador
    de destino (`emit_arm64_a32_coprocessor.cpp`), e `std::monostate` gera exceção de
    coprocessador. Como não alteramos código de terceiro, a instrução é tratada **na nossa
    camada** (`try_execute_cp15_reads_ignored`), no mesmo padrão já usado para `CPS`: intercepta
    antes da tradução e só avança o PC, deixando os registradores intactos.
  - Teste `test_jit_cp15_tlb_lockdown.cpp` (4/4) trava o contrato contra o oráculo, com **controle
    que distingue "preserva" de "devolve constante"**: duas sementes diferentes (`0xc0` e
    `0xa5a5a5a5`) têm de produzir resultados diferentes — um registrador com conteúdo próprio
    devolveria o mesmo valor nas duas.
  - **RESULTADO MEDIDO — divergência ZERADA**: `diff_traces.py` agora reporta *"sem divergência
    nas 118.326 instruções comparadas"*. Os dois backends executam **exatamente as mesmas
    instruções com os mesmos registradores** do reset até o ponto de parada.
  - O recompilado para em `0xf000bcac` = `0xe9dd7fff` = `ldmib r13,{r0-r14}^` (P=1,U=1,S=1,L=1),
    ou seja **`LDM_usr`** — uma das 6 instruções inventariadas que o Dynarmic não traduz. O
    interpretado segue dali para `0xf000bcb8` = `rfeia r13!` (**`RFE`**) e retorna do tratador.
  - **Próximo passo**: implementar `LDM_usr` e `RFE` na nossa camada (mesmo padrão de `CPS` e
    `c10`). São o par de retorno de tratador de exceção e o **último obstáculo conhecido** entre
    o backend recompilado e a continuação do boot.

- [ ] **RESSALVA DO TRM: o `c10` NÃO é "reads ignored" no silício — dívida técnica aberta**:
  - Os TRMs foram convertidos para markdown (`docs/remote/md/`) e o **DDI0211K §3.3.22** contradiz
    o modelo do QEMU: o TLB Lockdown Register é *"32-bit **read/write** register"*, com campos
    reais — `[0] P` (preserve), `[28:26] Victim` (0-7, incrementa sozinho após table walk que
    escreve na região de lockdown), `[25:1]` e `[31:29]` SBZ/UNP. **Reset = 0.**
  - O QEMU trata como `ARM_CP_NOP` ("reads ignored") porque **não modela lockdown de TLB** — é uma
    simplificação do emulador, não o comportamento do ARM1136. Nossa implementação copiou essa
    simplificação.
  - **O firmware usa o registrador de verdade**: há **8 escritas** (`MCR p15,0,rX,c10,c0,0`) em
    `0xf0009de4`..`0xf0009e64`, e a sequência em `0xf0009dd4` é um **read-modify-write** clássico:

        e3a03102  mov r3,#0x80000000        ; base
        e1a037c3  asr r3,r3,#15             ; MVA
        ee1a0f10  MRC p15,0,r0,c10,c0,0     ; LE o c10
        e3800001  orr r0,r0,#1              ; liga o bit P (preserve)
        ee0a0f10  MCR p15,0,r0,c10,c0,0     ; escreve de volta

  - **Por que o boot converge mesmo assim**: preservar `r0` faz o RMW produzir `0xc0|1 = 0xc1`, que
    é o que o interpretado também produz — os backends concordam. Mas `0xc0` **não é conteúdo
    legítimo do `c10`**: os bits 6-7 caem em `[25:1]` SBZ/UNP, que um `c10` real não guardaria.
    É `r0` remanescente do escopo anterior, que o modelo do QEMU deixa passar. **Os dois backends
    concordam sobre um valor que o silício não produziria.**
  - **Consequência honesta**: a paridade entre backends está correta e a divergência foi realmente
    zerada, mas ambos podem estar divergindo do hardware real neste ponto. Como o Unicorn é o nosso
    único oráculo executável, ele não consegue revelar esse erro — só o TRM revela.
  - **Correção devida** (não urgente: não bloqueia o boot): modelar `c10` como registrador real com
    reset 0, mascarando SBZ/UNP na escrita e devolvendo o conteúdo guardado na leitura. Isso vai
    **reintroduzir a divergência** contra o Unicorn em `#67395` — o teste terá de comparar contra o
    **TRM**, não contra o oráculo. Requer decidir explicitamente que o TRM ganha do Unicorn quando
    os dois discordam.

- [x] **TRMs convertidos para markdown (`docs/remote/md/`) — pesquisáveis por grep**:
  - `arm1136_trm.md` (1,4 MB, 26.590 linhas) do DDI0211K r1p5 — **Core0**, ARM1136EJ-S.
  - `arm926ejs_trm.md` (346 KB, 6.489 linhas) do DDI0198E r0p5 — **Core1**, ARM926EJ-S (AMSS).
  - Conversão por `markitdown` 0.1.6 (41 s e 11 s). Completude verificada de ponta a ponta (capa →
    glossário final), não apenas por tamanho do arquivo.
  - **Não versionar os `.md`**: são derivados de PDFs de terceiros (ARM, "Non-Confidential /
    Unrestricted Access", mas com copyright). São ferramenta local de consulta, como os PDFs.
  - Ganho concreto: o TRM passou a ser **grep-ável**, e foi assim que a contradição do `c10` acima
    apareceu. Em 934 páginas de PDF ela não teria sido encontrada por leitura.
  - Confirmações úteis para `LDM_usr`/`RFE` (bloqueio atual), do próprio TRM:
    - Regra do `^` (bit S): *"For all STMs and LDMs that **do not load the PC**, stores or restores
      the **User mode banked registers** instead of the current mode registers"*; e *"For LDMs that
      **do** load the PC, indicates that the **CPSR is loaded from the SPSR**"*. Ou seja, o mesmo bit
      S seleciona duas semânticas distintas conforme o PC esteja na lista — é exatamente a divisão
      `LDM_usr` vs `LDM_eret` do Dynarmic. Nosso `0xf000bcac` (`ldmib r13,{r0-r14}^`) **não** tem PC
      na lista → é banco de USUÁRIO, sem tocar CPSR.
    - `RFE` e `SRS` estão na lista de instruções que **não podem ser executadas condicionalmente**
      (são incondicionais, campo `cond` = `0b1111`). Isso vale para o decodificador: não tente
      avaliar condição nelas.
  - **Aviso de método**: ao extrair registradores do traço, conferir o índice das colunas contra
    uma linha crua antes de tirar conclusão — foi exatamente esse descuido que gerou o
    diagnóstico errado de "ponteiro 0x2" e a caçada inútil ao acesso desalinhado.
- [ ] **Ciclo de vida real por módulo (reaberto; `adcb631` oferece apenas carga + harness fixo)**:
  - EFS2 usa catálogo de blocos conhecidos, não extração universal. `--applet=` copia bytes host.
  - Resolver formato/relocações/entry de DD, criar instância com CLSID correto e usar HandleEvent do objeto retornado; jamais reutilizar `0x10532344` para todo jogo.
- [x] **Melhorias de Usabilidade Estilo Dolphin/RPCS3/RetroArch (commit `07033e1`)**:
  - Título dinâmico de janela: `Zeebo LLE | Dynarmic JIT | C0: X MIPS | C1: Y MIPS | FPS: Z [RODANDO/PAUSADO]`.
  - Janela redimensionável 4:3 com VSync e letterboxing automático (`SDL_RenderSetLogicalSize(640, 480)`).
  - Gamepad hotplug dinâmico com mapa completo Z-Pad (D-Pad, A, B, 1, 2, L, R, Home).
  - Hotkeys: F11 (fullscreen toggle), PAUSE/Break (pausar/retomar emulação), F12 (screenshot instantâneo PPM 640x480).
- [x] **Roteamento Unificado de UARTs {1, 2, 3}**:
  - Endereços `0xA9A00000` (UART1 - Console), `0xA9B00000` (UART2 - Modem IPC), `0xA9C00000` (UART3 - Diag/Aux) mapeados em Core0 e Core1 com buffers de linha e identificação `[UART#N]`.

---

### Core1 (ARM926/AMSS) — estado do boot em 2026-09-10

Fonte detalhada: `notes/core1_boot_estado_real.md`. Resumo para não repetir trabalho:

**Onde o boot está.** O kernel OKL4 imprime o banner completo (`Initializing KIP...` →
`root-servers: utcb_area/kip_area` → `creating root server (000a8001)`) e então entra num
laço infinito de page-table walk. **Causa raiz provada (QW49)**: a `.rodata` do kernel é
servida do shadow Split I/D zerado. Com o probe que serve essa faixa do pristino, o laço
morre e o kernel avança ~9x, até uma assertion **dele mesmo**:
`Failed to create root server TCB` (`pistachio/src/thread.cc:1273`) — QW50, alvo atual.

**O probe é muleta declarada, não correção.** A correção estrutural (QW51) continua aberta.

**REGRA — investigação de LLE roda em INTERPRETADOR PURO.** Sem `--jit`/`--jit-solo`.
O JIT acrescenta divergência de backend a um problema que ainda é de emulação low-level;
ele tem worktree próprio (`zeebo-dynarmic-bringup`) e o lockstep é assunto separado.
O teste `test_rodata_probe_control` **falha** se detectar Dynarmic na saída.

**Hipóteses REFUTADAS por medição — não reabrir sem evidência nova:**

| # | Hipótese | Como caiu |
|---|---|---|
| 1 | `find_kernel_heap`/memdesc mal configurado escolhe o heap | Este build **não usa** esse caminho. Erro de método: apliquei o corpus OKL4 como prova do fluxo desta firmware — exatamente o que `MORE_INFO.md` §5.1 adverte quanto ao kernel MSM |
| 2 | Heap `(f0000000,f0200000)` sobrepõe a imagem ⇒ limiar linear separa | `.rodata` e pilha no MESMO `PT_LOAD`; tentativa → regressão 16.717 insns/`pc=0` |
| 3 | Lista livre do alocador vazia / nunca inicializada | Li `[pool]` no ponto errado: `f0002c94` é o ramo de **sucesso** e `[pool]=0` é o estado **depois** de desenfileirar o último nó |
| 4 | Split I/D restaurando a pilha como código causa o laço | Assimetria real, mas `r8` continuou 0 com a pilha coerente |
| 5 | O `7` em `b0000007` é corrupção | É o campo **rights** de um fpage L4 (`orr r2,r2,#7` em `f0016a8c`) |
| 6 | Scheduler/IPC é o bloqueio (QW46) | 8 SVCs no boot, todos MapControl, zero IPC — inalcançável |

**Etapa de carregamento que faltava (corrigida, `0d12c99`)**: o loader do super-ELF do AMSS
só gravava no VA; **11 dos 18 `PT_LOAD` ficavam com o PA vazio** (`shnum=0`, `phnum=18`).
Teste `test_elf_pa_load.cpp`. Correto por si, mas **não** destravou o boot.

**Armadilha de ambiente**: `/tmp` é tmpfs de 4,8G; traços de lockstep (~486MB cada) enchem
o disco e o build falha com `error writing to /tmp/ccXXXX.s: Não há espaço disponível` —
**parece erro de código e não é**. Checar `df -h /tmp` antes de investigar.

---

### Fase 15: Double Dragon — runtime, assets, imagem e som (ABERTA)
- [ ] **Desacoplamento do Stub de Tela Azul e Renderização Real do BREW / Z-Wheel**:
  - Substituir o stub de `clear_color(0.1f, 0.2f, 0.8f)` pelo processamento de command buffers e chamadas reais de `IBitmap` / `IDisplay` do BREW.
  - Carregar assets visuais (`slidemodel.qxm`, `.bar`, `.bmp`) da NAND `0:EFS2APPS` para exibição na interface do Z-Wheel.
- [ ] **Decodificação de Logs via Protocolo Qualcomm Diag (USB / SMD)**:
  - Capturar frames HDLC na interface de diagnóstico USB / SMD para extrair logs `DIAG_MSG_F` do BREW AppMgr e L4 diretamente no console do emulador.
- [ ] **PCM do guest até o host**:
  - Backend SDL já existe (`d530d4c`); faltam prova de produção/consumo de amostras do jogo, callbacks, sincronização e validação audível. Não basta abrir device.
- [ ] **Módulo/arquivos/timers reais de DD**: executar a cadeia e gates abaixo antes de declarar renderização comercial.

---

## Próximos Passos Priorizados — vertical slice Double Dragon **[HISTÓRICO]**

> Esta lista, a tabela DD-QW1–6 e os candidatos associados foram consolidados e
> substituídos por QW99 Parte 4. Permanecem aqui somente como registro de proveniência;
> em caso de divergência, valem os gates DD0/DD1a/DD1b–DD6 de QW99.

### P0 — Cadeia crítica (não confundir com quick wins)

1. **DD0 — Gates confiáveis e identidade do módulo.** Rejeitar arquivo inválido;
   registrar App ID, CLSID, hash, formato, base, entry e PC executado. Estado inicial
   `loaded_only`. O teste negativo desta revisão deve deixar de receber PASS de jogo.
2. **DD1 — Loader real + instância BREW.** `ddragonz.mod` começa com ARM cru
   (`04e02de5...`), mas `resolve_mod_entry` só aceita ELF (`zeebo_brew_loader.h:279`).
   Reutilizar RE/documentação/testkit existentes para confirmar ABI de AEEMod_Load,
   base/relocação/RW/ZI/imports e objetos IShell/IModule. Executar o entry real,
   obter módulo → CreateInstance(`0x0102F789`) → applet → HandleEvent real.
   **Não** tratar entry de módulo como HandleEvent; não basta `entry=load_va`.
   Gate: PCs/retornos/buffers observados dentro do módulo correto; nenhuma chamada
   ao handler fixo `0x10532344` usada como substituto. Um probe isolado é progresso
   de loader, não prova de boot orgânico.
3. **DD2 — Assets/VFS alcançáveis pelo guest.** Conectar backend de arquivos à
   interface real identificada no firmware; `open/read/seek/stat/close`, caminhos
   relativos, EOF/erros, permissões e saves separados. Gate: DD lê seus próprios
   `data.ggz`/`sound.ggz`, bytes retornados conferem com arquivos host e retirar
   um asset provoca falha identificável, não sucesso simulado.
4. **DD3 — Execução contínua, callbacks e input.** O loop atual descarta timers
   expirados (`zeebo_lle_main.cpp:989-991`) e repinta azul, sem rodar o módulo.
   Conectar timers one-shot, callbacks de mídia, eventos e tempo ao contexto real.
   Gate: sequência de teclas avança splash → menu → fase; PCs continuam no jogo,
   callbacks retornam corretamente e tecla solta não fica presa.
5. **DD4 — Primeiro frame real.** Resolver objetos IDisplay/IBitmap e, se usados,
   IGL/IEGL por chamadas/retornos vivos. Reusar rasterizador existente; não inventar
   o tipo de `[applet+0x2c]` nem supor um framebuffer linear sem prova. O caminho 2D
   é prioridade junto do 3D: o laboratório já documenta DrawText/DrawRect no DD.
   Gate: splash e menu reconhecíveis, sequência de frames produzida pelo guest,
   sem clear azul substituto ou cópia de screenshots do oráculo.
6. **DD5 — Som do jogo.** Rastrear abertura/leitura de sound.ggz → chamadas de mídia
   → buffers/comandos → PCM → SDL. O layout AUDPP atual está marcado como hipótese;
   confirmar contra tráfego real antes de expandir QDSP5. Gate: captura PCM não
   silenciosa com procedência guest, música contínua e efeitos reagindo ao input;
   validar escuta no dispositivo real. WAV externo tocado à parte não conta.
7. **DD6 — Jogável integrado.** Partida de pelo menos 5 minutos com movimento,
   ataques, dano, música/efeitos, pausa/retomada e saída limpa. Medir FPS do jogo
   (não repaints do host), velocidade relativa ao oráculo, latência de entrada e
   underruns de áudio; guardar vídeo/PCM/log e comandos reproduzíveis. Repetir em
   cold boot. Só então marcar Double Dragon jogável e ampliar catálogo.

**Boot LLE obrigatório em paralelo:** Passo 13 (BootInfo) tem marcos históricos;
Passo 14 (serviços → AEECShell → AppMgr) permanece aberto. Não foi fechado por
JIT ou injeção de .mod. Reobservar o primeiro bloqueio real com o debugger atual;
fechar IPC/scheduler/serviços necessários, sem forçar PC ou retorno-sucesso.
O caminho direto assistido ao módulo pode facilitar diagnóstico, mas seu resultado
não fecha o caminho de boot pela NAND. AppMgr e Z-Wheel precisam de gates próprios.

### Quick wins acionáveis (candidatos; estimativas relativas, não promessas de prazo)

| Ordem / ID | Estado | Escopo e ganho | Esforço / dependência | Prova para concluir |
|---|---|---|---|---|
| 1 — DD-QW1 | pendente no código; documentação corrigida | Remover PASS universal; separar carga, entry, instância, frame e PCM. Corrigir `test-roms-external` e teste Reksio. | baixo; causa já reproduzida | Arquivo inválido falha; DD permanece `loaded_only` até executar; mutante que reintroduz handler fixo deixa teste vermelho. |
| 2 — DD-QW2 | pendente | Identidade explícita do pacote: App ID separado de CLSID, manifesto com hashes/paths; impedir aceitar o scan heurístico MIF como autoridade. | baixo-médio; bytes disponíveis | MIF real hoje retorna erroneamente `0x01000100` e mod vazio (probe compilado, exit 1). Exigir `0x0102F789` por metadado validado/override explícito; arquivo truncado ou CLSID inválido é rejeitado. Parser estrutural definitivo exige confirmar layout, não outro scan mágico. |
| 3 — DD-QW3 | pendente | Probe limitado do módulo ARM cru: disassembly do entry, classificação explícita e trace com orçamento de instruções; reaproveitar testkit/mod_probe. | baixo-médio para diagnóstico; loader completo é DD1 | Provar PCs do próprio DD, ABI/retorno ou primeira dependência faltante; reconhecer formato desconhecido sem chamá-lo de jogo executado. Não despachar AEEMod_Load como evento. |
| 4 — DD-QW4 | pendente | Backend host de arquivos read-only + overlay de saves, usando árvore Infuse já validada; eliminar cópias/extracões repetidas. | médio; camada host isolada, ligação guest é DD2 | Testes open/read/seek/EOF, bytes dos assets, caminhos relativos, traversal/symlink fora da raiz recusados; hashes da fonte intactos após escrita no overlay. |
| 5 — DD-QW5 | pendente | Corrigir lifetime do SDL: parar/fechar callbacks antes de destruir dispatcher e recursos usados pela fonte. | baixo; fora de `qdsp5/` | Teste de callback ativo durante teardown e init/shutdown repetidos com ASan; nenhum acesso após destruição. `~ZeeboLLESystem:516` hoje só flush UART; membros `:3592-3593` destroem dispatcher primeiro. |
| 6 — DD-QW6 | pendente | Serializar produção/mixagem no orquestrador ou passar PCM por fila limitada; honrar frequência negociada do SDL; observar frames/amostras/underruns com origem. | médio; auditar todas as entradas sem editar engines congeladas | Teste concorrente de feed/mix/close, silêncio em underrun, 22.05→44.1 kHz e frequência host alternativa corretos; teste sintético rotulado como teste, nunca áudio de DD. |

**Ordem de execução:** DD-QW1/2/3 primeiro (destravamento do módulo); DD-QW4 e
DD-QW5/6 podem andar em paralelo. Em seguida DD1→DD2→DD3, com imagem e áudio
integrados assim que houver chamadas reais. Não priorizar mais jogos antes de DD6.

### Candidatos antigos reconciliados

- **QW-AUD1: parcial.** Saída SDL implementada (`d530d4c`), áudio do jogo não provado;
  segurança/clock no DD-QW5/6 e validação real no DD5. Congelamento de `qdsp5/`
  preservado; este plano não altera `QDSP5_TODO` nem engines.
- **QW-GFX1 / QW45: fase DD4, não quick win garantido.** Não há objeto/buffer guest
  resolvido que torne isso um simples blit. Contador Adreno e pixels não pretos,
  isoladamente, aceitam o stub azul e portanto não são gate de jogo.
- **QW-UART1: adiado.** Não forçar clock/enable só para obter logs; modelar o contrato
  observado quando bloquear o guest. Não garante que firmware de produção envie UART.
- **QW-DIAG1: pesquisa adiada.** `0x3000000a` e `0x30000013` são programas RPC
  ADSPRTOSATOM/AUDMGR, não identificação comprovada de canal Diag. Identificar
  transporte/framing/comandos reais antes de propor sniffer; não inventar IDs.

---

### QW43 — ATUALIZAÇÃO: causa raiz é truncamento do buffer FONTE, não bug no decoder (2026-09-09)

Reimplementei o decodificador RLE completo em Python (literais + run-length de repetição do
último byte + back-reference com offset, disassembly completo de `0xb0400044-0xb040009a`) e
rodei sobre o stream real capturado em runtime (`ZEEBO_DUMP_STREAM2`, fonte=`0xb04155b4`,
1056 bytes lidos). Resultado da decodificação: **os primeiros ~250 bytes do stream comprimido
são dados reais e decodificam corretamente para strings ASCII legíveis do kernel ARM/OKL4**
("spinlock", "arm.ss", "Invalid argument" — nomes de arquivo/mensagens de debug típicas do
kernel), confirmando que o algoritmo RLE está correto e bem entendido. A partir do byte 250,
**o restante do buffer fonte é zero puro** (`all(b==0 for b in data[250:1056]) == True`) — não
há mais dados comprimidos válidos ali. O decoder consome o padrão `ctrl=0x00,extra=0x00`
(zero real do buffer, não um valor de controle intencional) e sofre underflow em `subs r4,#1`
com `r4=0`, exatamente como documentado antes — mas agora sabemos que a causa é **o stream de
entrada estar truncado/zerado prematuramente**, faltando ~55 bytes (985/1040 destino escrito)
para completar a tabela de strings. O guard de término real do laço (`cmp r1,r2; blo` em
`0xb0400096-0xb0400098`, r2 constante `0xb04155b4` = mesmo endereço do buffer fonte, ou seja,
destino e limite de fonte coincidem por design) só é verificado POR SÍMBOLO completo, nunca
por byte — não é o bug em si, mas explica por que o decoder não para graciosamente ao ficar
sem dados.

Hipótese revisada (linha com o apontamento do usuário para o zloader): o buffer fonte
comprimido em `0xb04155b4` foi copiado da NAND/AMSS para essa posição de RAM por uma rotina
de carregamento anterior (possivelmente relacionada a `load_amss_dmov`/`DMA_PAGE_BUF` já
mapeados no emulador, linhas ~1928/1961 de `zeebo_lle_main.cpp`) — se essa cópia for
incompleta (menos bytes copiados do que o comprimento real do blob comprimido na NAND), o
decoder RLE recebe um stream truncado e cai no mesmo padrão de zero observado. Investigação
do `firmware/openzeebo-zloader.bin` (projeto openzeebo, código-fonte em
`~/projects/zeebo/research/openzeebo-repo/tools/zloader/`) mostrou que esse zloader é um
carregador ARM9 de boot que copia AMSS/APPS da NAND para RAM (via `memcpy` em `main.c`, sem
qualquer rotina de descompressão) — ou seja, o zloader real da Qualcomm NÃO contém o
decodificador RLE; esse decodificador pertence ao próprio AMSS/kernel OKL4 carregado (código
fechado da Qualcomm, sem fonte disponível — buscas em `refs/okl4-2.1.1-fix7` por
`rle|lzss|lz77|decompress` não encontraram nada relevante).

Próximo passo recomendado: rastrear o código que copia o blob comprimido para
`0xb04155b4` (watchpoint em escritas nesse endereço, análogo ao `ZEEBO_WATCH_SLOT` já usado)
para confirmar se a cópia é truncada por um tamanho incorreto (ex.: comprimento fixo/errado
usado no `memcpy`/DMA em vez do tamanho real do blob comprimido na NAND).

**Watchpoint em `0xb04155b4-0xb04159d4` (2026-09-09, sessão seguinte) — resultado**: existem
dois writers na região. (1) `pc=0xb04000a4` escreve 120 words (480 bytes, mas o padrão real
via `objdump`/emulador mostra ~240 bytes de payload útil) trazidos de endereços de código
`0xb0401xxx` — **para de escrever após ~240-250 bytes**, exatamente onde o decoder RLE
encontra o padrão `ctrl=0x00,extra=0x00` e sofre underflow. (2) `pc=0xb0400068` é o próprio
loop de literais do decoder RLE reescrevendo a MESMA região com zeros ao decodificar run-length
de repetição (ele usa a região fonte como buffer de trabalho/destino simultaneamente — decoder
"in-place", fonte e destino compartilham o mesmo range de memória por design, o que é normal
em decoders RLE compactos, não um bug).

**Disassembly do zloader real (2026-09-09)**: `arm-none-eabi-objdump -D -b binary -m arm
--adjust-vma=0xa0000000 firmware/openzeebo-zloader.bin` (2.356 linhas, `/tmp/zloader_disasm.txt`).
Entry em `0xa0000028` (setup de registradores r7-r10, stack em modo SVC/0xd3, zera uma região
de BSS via loop `str r0,[r1],#4 / cmp r1,r2 / ble`, seta flag em `0xb8000108+0x10c` — registrador
de hardware MSM — e salta via `blx r4` para o código de aplicação carregado). Rotina de cópia
em `0xa00000a0-0xa00000c0`: `ldm/stm` de blocos de 4 words (`{r3,r4,r5,r6}`) em loop condicional
(`bhi`), completando com halfword/byte residual — **memcpy genérico simples**, sem qualquer
lógica de descompressão (sem shifts de 3 bits + escape byte, sem run-length, sem back-reference
por offset). Busquei especificamente o padrão do decoder RLE identificado no AMSS (`lsrs r3,r3,
#29` equivalente ao `lsls/lsrs #0x1d` visto no Thumb do AMSS) — encontrei DOIS usos desse shift
no zloader (`0xa0000690`, `0xa00006e8`), mas o contexto é **extração de bits de status/flags de
um registro de erro/exceção** (checagem `tst r2,#2`, dispatch para rotinas de log/printf via
`bl 0xa000169c`), não descompressão de dados — **falso positivo, mesmo padrão de shift usado
para propósito diferente**. Outro candidato (`bl 0xa0001358`) é uma rotina de I/O bit-a-bit
(usa registrador de hardware `0xb8000000` + delay loop `mov ip,#100`) — bit-banging de
GPIO/UART, não descompressão.

**Conclusão (revisão da hipótese do zloader)**: o `firmware/openzeebo-zloader.bin` é
confirmado como um bootloader ARM9 minimalista (init de hardware/stack/BSS + memcpy simples +
jump para o código de aplicação/AMSS) — **não contém nenhuma rotina de descompressão RLE**.
Isso é consistente com a análise anterior do código-fonte C do projeto openzeebo
(`main.c` usa apenas `memcpy`, sem chamadas a rotinas de unpack). A hipótese do usuário levou a
uma verificação rigorosa e NEGATIVA: **a descompressão RLE não acontece no zloader** — ela
acontece dentro do próprio AMSS/kernel OKL4 já carregado em RAM (código fechado da Qualcomm,
sem fonte disponível), como já havia sido mapeado via disassembly do runtime do emulador
(`0xb0400044-0xb040009a`). O zloader real provavelmente só copia os blocos comprimidos da NAND
para RAM sem processá-los — a descompressão ocorre depois, já em contexto do AMSS.

O achado mais relevante permanece: **o writer #1 (pc=`0xb04000a4`) para de preencher o buffer
fonte após ~240-250 bytes**, e é essa cópia truncada — não o zloader — que causa o underflow no
decoder RLE do AMSS. Próximo passo: identificar o CALLER de `0xb04000a4` (a rotina que decide
quantos bytes copiar) para achar por que ela usa um tamanho menor que o necessário (~1040 bytes
esperados vs. ~240-250 copiados).

**Caller de `0xb04000a4` identificado**: `lr=0xb040001c`, dentro do próprio dispatcher ARM já
mapeado em `0xb0400000-0xb0400034` (entradas de 16 bytes, `ldm sl!,{r0,r1,r2,r3}` / `bx r3`).
5 chamadas capturadas: `src=0xb04151a4→dst=0xb04155b4,len=0xec`; depois
`src=0xb04151b4→dst=0xb04155c4,len=0xdc`; `...len=0xcc`; `...len=0xbc`; `...len=0xac` — **src e
dst avançam em passos de 0x10 (16) e `len` decresce em 0x10 a cada chamada**, indicando que o
próprio dispatcher está iterando sobre a tabela de 16 bytes e chamando essa cópia por entrada,
com `len` sendo o "tamanho restante da tabela", não o tamanho do blob comprimido isolado. Ou
seja, **não é uma cópia NAND→RAM feita pelo zloader** — é lógica interna do dispatcher/AMSS já
carregado, disparada após o handoff do zloader. O primeiro `len=0xec=236` bate com precisão com
o ponto onde os dados reais do stream comprimido terminam (byte ~240-250, confirmado pela
decodificação RLE bem-sucedida das strings "spinlock"/"arm.ss"/"Invalid argument"). Isso sugere
que **236 bytes é o tamanho ESPERADO e correto da primeira entrada copiada** — o problema não é
truncamento por parâmetro errado nessa chamada, e sim que o CONSUMIDOR (decoder RLE) espera
consumir até preencher 0x410 (1040) bytes de destino, mas o produtor só fornece dados para uma
fração disso nesta tabela/entrada, e o restante do buffer fica com zero residual (não
inicializado com stream comprimido válido) — possivelmente porque a tabela tem MAIS ENTRADAS
com deslocamentos aumentando (implicando MAIS regiões de dados comprimidos: até
`0xb04151e4→0xb04155f4`, i.e., offset 0x40=64 bytes acumulados de tabela) e essas próximas
entradas nunca são alcançadas/copiadas por outra razão (loop externo termina cedo, guard
incorreto, ou índice de iteração truncado) — não confirmado; requer mais uma rodada de captura
completa das ~15+ chamadas restantes até o dispatcher esgotar a tabela.

**Resumo executivo QW43 (estado atual, 2026-09-09)**: (1) zloader real (openzeebo,
`firmware/openzeebo-zloader.bin`) NÃO contém rotina de descompressão — é bootstrap ARM9 puro
(confirmado por disassembly binário via `arm-none-eabi-objdump`); (2) a rotina RLE que trava
está dentro do AMSS/OKL4 já carregado (código fechado, sem fonte); (3) o algoritmo RLE está
corretamente entendido e decodifica ~240 bytes reais em strings de debug legíveis do kernel
ARM; (4) a causa do underflow é a produção incompleta do stream fonte por um mecanismo de
cópia em 16-byte chunks interno ao próprio dispatcher (não ao zloader) — ainda não se sabe por
que esse mecanismo para de fornecer dados válidos após a primeira entrada da tabela.



**QW44 — causa raiz do "underflow" identificada: era um bug na SIMULAÇÃO PYTHON de RE, não no firmware/dispatcher (2026-09-09)**: reimplementando o disassembly Thumb de `0xb0400044-0xb040009a` byte-a-byte com disciplina estrita (sem atalhos que a sessão QW43 introduziu por engano), a decodificação dos 252 bytes reais do blob em `0xb04151a4` (ELF estático, `nand/1.1.2_APPS.bin`, MD5 intacto) produz EXATAMENTE os 1040 bytes esperados (`entry1.len=0x410`), consumindo 250/252 bytes de entrada, SEM qualquer underflow — e decodifica corretamente as strings ASCII do kernel `"spinlockarm.s"` e `"Invalid argument"` (evidência independente: essas strings não existem em nenhuma forma no blob comprimido bruto, só aparecem se o RLE for decodificado certo). Dois erros da simulação anterior (QW43) foram corrigidos: (1) o loop de back-reference (`0xb0400078` `subs r6,#1`/`strb`/`bne`, seguido de `0xb040007e adds r1,r1,r5`) copia **r5+2** bytes, não r5+1 como a sessão anterior assumiu — só r5+2 fecha exatamente em 1040/1040 sem sobra/falta; (2) o loop de literais deve ser pulado inteiramente (0 iterações) quando o campo bruto de contagem decrementado (`r4` após `subs r4,#1`) chega a 0, sem ramificações especiais adicionais. O erro de offset acumulado da simulação QW43 (r5+1) desalinhava progressivamente a leitura de control-bytes até coincidir por acaso com um zero real do BSS estático perto do fim do blob, produzindo um "control byte 0x00 espúrio" que nunca existiu como problema real — o "underflow" documentado em QW42/QW43 era um artefato de simulação, não um bug do firmware nem do dispatcher `0xb0400000` em si. CONCLUSÃO IMPORTANTE: como o firmware roda sob Unicorn executando o código ARM/Thumb REAL do guest (não há reimplementação C++ do decoder RLE em `zeebo_lle_main.cpp` — o `c0_code_hook`/`c0_intr_hook` só intercepta syscalls/SVC/T-bit, nunca decodifica RLE em software), o Unicorn EXECUTANDO o dispatcher já decodifica correto por construção, desde que (a) os bytes do blob fonte estejam de fato mapeados/intactos em `0xb04151a4` no momento da chamada e (b) nenhum hook anterior (ex. a heurística já revertida do QW43 que forçava `r4=0`) esteja artificialmente interceptando/alterando o fluxo. Nenhuma correção de C++ foi necessária ou aplicada — nada no emulador precisa mudar para este dispatcher especificamente. Oráculo determinístico da simulação corrigida, executável sem tocar em `zeebo_lle_main`: `tools/cpp/rle_dispatch_oracle_qw44.py` (saída `PASS`, 1040/1040 bytes, strings decodificadas corretamente). Reafirma a regra do ROADMAP (QW42-QW43): nunca forçar registradores/valores para "consertar" um sintoma sem entender a causa raiz real — aqui a causa raiz nem estava no firmware. Próximo passo: se o stall real em `0xb010333a`/downstream persistir na execução viva (via control-port, não terminal), investigar se algum OUTRO ponto do boot ainda intercepta/corrompe o buffer fonte antes do dispatcher rodar (ex. timing de cópia DMA do blob em `0xb04155b4`/`0xb04151a4`, não o algoritmo RLE em si, que está provado correto por este oráculo).**

**QW44 — hipótese de truncamento por DMA/carga DESCARTADA por parse do Program Header ELF**: verifiquei se o carregamento (`load_apps()` em `zeebo_lle_main.cpp`) trunca o blob fonte do RLE antes do dispatcher rodar. O Program Header do ELF real mostra que o segmento `PT_LOAD` que cobre `0xb0400000` tem `p_filesz=0x152a0`, terminando exatamente em `vaddr+filesz=0xb04152a0` — o MESMO limite do fim do blob comprimido (`0xb04151a4+0xfc=0xb04152a0`). `load_apps()` copia esse segmento inteiro via `uc_mem_write(core0_.uc, target, d.data()+off, fs)` direto do arquivo ELF, sem qualquer DMA/NAND truncando o conteúdo (o caminho `load_apps_dmov()` só lê o cabeçalho ELF via DMA para validar a partição; o conteúdo real vem de `load_apps(fallback_path)`, que lê o arquivo local inteiro). CONCLUSÃO: os 252 bytes de entrada não são um truncamento de carga — é exatamente o tamanho real do blob comprimido no firmware, por design, tal como QW44 (decoder) já havia provado ser suficiente para produzir os 1040 bytes de saída sem underflow. Hipótese de "stream truncado por DMA" (aventada em `6e5c031`) fica descartada; nada precisa mudar no carregamento do ELF.**

| Ordem | Estado | Quick win | Esforço | Prova obrigatória |
|---|---|---|---:|---|
| QW1 | **parcial honesto** | Parser/harness BootInfo por bytes reais | baixo-médio | PASS: magic/tags/10 pools virtuais/5 físicos + mutações; pendente: nenhuma prova firmware-derived das 96 fpages |
| QW2 | **concluído** | Deduplicação de trace no cliente Python + invalidação SMC após `poke ok=true` | baixo | 25/25 + debug-agent 22/22 |
| QW3 | **concluído** | `ProbeRegistry` read-only (`probe.list/get`) para MMU/BootInfo/IRQ/GPU/unmapped | baixo-médio | PageInfo vivo `0x01111006→12`; RPC e debug-agent 22/22 |
| QW4 | **concluído** | Alpha-test + culling com defaults CCW/`GL_BACK` | baixo | pixels discard/CW/CCW/default; sem slot `glFrontFace` inventado |
| QW5 | **concluído** | `glTexParameterx`: nearest/linear e repeat/clamp por textura | baixo | textura 2×2 nas bordas; MIN_FILTER state-only documentado |
| QW6 | **concluído** | Primeiro vetor ARM11 transacional | baixo-médio | INIT/FINAL completos + traço ordenado P/L/S com valores reais via `READ_AFTER`; CPU 12/12 |
| QW7 | **concluído** | Oito depth funcs e fatores usuais de blend GLES1 | baixo-médio | matriz de pixels; quickwins GL 13/13 |
| QW8 | **parcial honesto** | Log limitado `unmapped.unknown` com core/PC/endereço/largura/direção/valor | baixo-médio | log passivo validado; falta pausa/erro opt-in sem confundir RAM inválida com MMIO |
| QW9 | **concluído** | ATITC RGB/RGBA em `glCompressedTexImage2D` slot 15 (`94a449c`) | médio | 15/15: RGB methods 0/1, alpha explícito/interpolado, crop 6×6, pixels/FNV do oracle independente |
| QW10 | **concluído** | Clipping homogêneo `z+w>=0` antes do divide (`707229f`) | médio | 19/19 checks em `gl_clip_smoke`: inside/outside/crossing com interpolação de vértices e culling de w~0/não finito |
| QW11 | **concluído** | Interpolação perspectiva de cor/textura; depth como `z/w` afim em screen space (`707229f`) | médio | cor e textura normalizadas por 1/w; depth afim linear em tela sem duplo denominador; 5 mutações RED |
| QW12 | **concluído** | Script Python `mempool/bi_execute` usando breakpoints/probes/dedup (`7d658cb`, `f3b2ed1`) | baixo | 49/49; evidência viva ordenada termina `blocked` em `stuck_add`; nenhum trace C++ |
| QW13 | **bloqueado (guest vivo)** | Regressão transacional de `L4_MapControl` (`7355364`, `a6c1967`) | baixo | regiões/ordem/permissões/escrita/nil/whole-space provados; echo de MR não prova writeback load-bearing |
| QW14 | **concluído** | `--strict-unmapped` opt-in (`2781e18`) | baixo-médio | primeiro acesso desconhecido pausa com evento estruturado; keypad não dispara; default 22/22 |
| QW15 | **concluído** | Gate transacional QW6 + crossing real de página (`ba3f534`) | baixo | dois vetores PASS; crossing word/half em `0x00102000`, SCTLR.A=0; CPU 12/12 |
| QW16 | **concluído** | Fechar pequenos desvios GLES: clamp de `glAlphaFuncx` para [0,1], validação de enum de alpha-func e de combinações `glTexParameterx` (pname/param) | baixo | `gl_alpha_sampler_smoke` 17/17 estado+pixel; ref>1/<0 clampado, func/pname/param inválidos não mutam estado e retornam false (caminho firmware); sem slot `glFrontFace` inventado; MIN_FILTER mipmap rejeitado (limitação LOD documentada) |
| QW17 | **concluído** | Correção do PC-resume no `c0_intr_hook` (`target_pc = pc`, evitando pular 1 instrução) (`a88a9bd`) | baixo | microteste `test_intr_pc_resume.cpp` com asserção RED e GREEN integrada em `make check`; elimina avanço duplo em syscalls 0xb4/0x00/0x0c |
| QW18 | **concluído (código + TDD)** | Remoção das escritas espúrias em `sp+0/4/8` no handler 0xb4 (`L4_KernelInterface`) de `zeebo_lle_main.cpp` | baixo | Confirmado por TDD host-only load-bearing `test_l4_kip_trap.cpp` (bytes reais do stub 0xb000c720; `mvn sp,#0x4b`): no intr hook o SP é o SP DA TRAP (`0xFFFFFFB4`), scratch nunca lido — a escrita era espúria e NÃO corrompia r4. GREEN com o caminho atual; RED só ao mutar o alvo para `ip+0/4/8` (frame do chamador → r4=0xc). Hipótese de causa do stall d708 REFUTADA; código-morto removido. Boot real inalterado (já ultrapassava d708 no HEAD atual) |
| QW19 | **concluído** | Retomada no `pop` (`svc+4`) para stub de `MapControl` (syscall 0x14) em `c0_intr_hook` (`475ee1c`) | baixo | TDD reproduzindo restauração do frame de registradores (`r4-r8, sb, sl, fp`) no stub `0xb000c930` (`test_mapcontrol_frame_resume.cpp`); elimina corrupção de r4 que causava `size_log2=56` |
| QW20 | **concluído** | Alinhamento do alvo `make clean` e remoção de artefatos de teste não rastreados (`83fff7f`) | baixo | gate estático `test_clean_hygiene.py` (23 alvos root + 10 gpu cobertos pelo clean do próprio Makefile); roda no check sem destruir artefatos; mutação negativa real (remover 1 nome → RED) |
| QW21 | **concluído (análise)** | Teste de regressão para convenção de retorno de `L4_ExchangeRegisters` (syscall `0x0c` / `0x140c`) | baixo | stub 0xb000c758 comparado byte-a-byte com `exchangeregisters.spp`; 8 callers reais existem, mas bp vivo não hitou no boot atual (panica no thread_init antes); fix do case 0x0c (sp=ip + outputs em ip+0x30) necessário quando o bi_execute destravar |
| QW22 | **concluído** | Teste de limite de fpage whole-space em `zeebo_l4_mmu.h` sem overflow de 32 bits (`280e421`) | baixo | auditoria sem gap de produção (guard s>=32 já correto); fortalecimento de contorno em `test_l4_mmu.cpp`: size_log2∈[32,63] → size_bytes()==2^32 exato + is_whole_space(); 63 explícito; sub-contorno 31 (==2^31, não whole-space) |
| QW23 | **concluído** | Inicializar `KIP[0xc4]` (thread_bits=18) no `build_kip` (`b57c591` + teste `7cc0c19`/`984393a`) | baixo | TDD: teste de integração (`test_kip_thread_bits.py`) — RED: boot panica no `thread_init` (0xb0007184) com KIP=0; GREEN: KIP[0xc4]==18 e o assert do thread_init não dispara (marco específico, não o hang genérico) |
| QW24 | **concluído** | Corrigir decode do `PhysDesc::phys_base()` para a gran do Zeebo (`(raw>>6)<<6`, 64B) em `zeebo_l4_mmu.h` (`2c0873d`) | baixo-médio | TDD host-only com MRs crus reais (0x10081000→0x10081000; 0x10000000→0x10000000; 0xFFFFFFFF→0xFFFFFFC0); RED 5 checks; guard `phys_base()>=4GB` removido (código morto sob o decode certo — critério whole-space = is_whole_space()); encoders de teste alinhados ao guest; boot: 98+ maps físicos `[aliased]` reais, 0 WRITE_PROT |
| QW25 | **absorbido (QW24)** | Revisar a heurística "phys_base ≥ 4GB = controle de AS" do `handle_map_control` | baixo | removido dentro do QW24 — era código morto sob o decode correto; critério whole-space ficou exclusivamente `fpage.is_whole_space()` (size_log2>=32) |
| QW26 | **concluído** | Retomar os stubs de syscall com frame `ThreadControl` (0x08, 0xb000c798) e `SpaceControl` (0x18, 0xb000c944) em `pc`+`SP=ip` (epílogo), como o QW19 fez para MapControl (`7b570e2`) | baixo | padrão QW19/microteste (`test_threadspace_frame_resume.cpp`, RED: epílogo pulado corrompe callee-saved e descarta writeback; GREEN: retomada em pc+SP=ip restaura frame e writeback); elimina panic `SpaceControl != 1` (linha 244) |
| QW27 | **concluído** | Retomar stubs de syscall com frame `ExchangeRegisters` (0x0c), `ThreadSwitch` (0x04) e `Schedule` (0x10) em `pc`+`SP=ip` (`fc4a811`) | baixo-médio | TDD isolado `test_exregs_frame_resume.cpp` (bytes reais do firmware); GREEN exit 0; RED com `buggy` (exit 1); elimina salto para PC=0x00000000; boot avança de 378k para 8.27M instruções e entra no `iguana_server_loop` (`0xb000aa94` / `0xb000c834` L4_Ipc wait) |
| QW28 | **concluído** | Servidores Iguana / IPC dispatch inicial & MsgTag em `iguana_server_loop` | médio | tratar mensagens IPC de entrada no server loop rumo à inicialização do EFS/VFS e BREW (`test_l4_ipc_msgtag.cpp`) |
| QW29 | **concluído** | Tabela de Threads L4 & Captura de Ativação via `ExchangeRegisters` | baixo-médio | registrar SP/IP/ThreadID em `thread_start` (0x0c) para chaveamento de threads nos servidores (`zeebo_l4_thread.h` + teste TDD) |
| QW30 | **concluído** | Scheduler Cooperativo L4 (Chaveamento de Contexto no `L4_Ipc`/`ThreadSwitch`) | médio | comutar execução para as threads dos servidores (`ig_naming`, `quartz_servers`, `AMSS`) quando a thread atual ceder no IPC wait (`pick_next_thread` em `zeebo_l4_thread.h` e chaveamento no c0_intr_hook case 0x04) |
| QW31 | **concluído** | Entrega de registros de serviços/buffers no `ig_naming` e `quartz_servers` | médio | protocolar o envio de registro de interfaces/memsections no `iguana_server_loop` rumo ao handshake com AMSS (`0x10137000`) (`test_l4_ipc_dispatch.cpp`) |
| QW32 | **concluído** | Handshake do Iguana com AMSS (`0x10137000`) e comutação em `L4_Ipc` wait | médio-alto | despachar IPC cooperativo no `c0_intr_hook` case 0x00 para permitir avanço dos servidores e AMSS (`zeebo_lle_main.cpp`) |
| QW33 | **parcial; boot aberto** | Handoff inicial AMSS, não AEECShell/AppMgr comprovado | médio-alto | Exigir criação real de applet pelo firmware; entry `0x10137000` não fecha o marco |
| QW34 | **parcial; gate pendente** | Seleção FIRSTAPP no CLI | médio | Provar AppMgr/Z-Wheel selecionado executando pelo boot, não só log de opção |
| QW35 | **reaberto (parser incorreto)** | MIF/MIF2 estrutural e identidade BREW | médio | MIF DD real deve resolver CLSID `0x0102F789`; hoje retorna `0x01000100` por scan não estruturado — DD-QW2 |
| QW36 | **reaberto (falso PASS)** | Lifecycle de jogo por entry/instância/handler real | alto | DD1/DD3; handler fixo `0x10532344` não é Double Dragon nem Reksio |
| QW37 | **parcial (infraestrutura)** | Flush/finish no rasterizador, sem prova de draw guest | médio-alto | DD4; retirar contador artificial/clear azul do gate |
| QW38 | **parcial (fila host)** | Timers one-shot sem execução de callbacks no loop | médio-alto | DD3/DD5; callback real e mídia têm que completar no guest |
| QW39 | **pendente; vertical slice DD1–DD6** | Double Dragon com imagem e áudio reais | alto | Partida controlável, música/efeitos e sequência A/V registrada |
| QW40 | **concluído (código + TDD)** | Ativação real de thread via `L4_ExchangeRegisters` usando o control REAL do firmware (`0x11e` = RECV\|SEND\|SP\|IP\|HALTFLAG, **sem** o bit DELIVER) em `zeebo_l4_thread.h`/`zeebo_lle_main.cpp` | alto | **Achado por execução real (boot 90s/~11.4M instr):** apesar de QW32-34 registrarem "concluído", o boot real ficava PRESO PERMANENTEMENTE em `0xb000c834` (`L4_Ipc` wait do `iguana_server_loop`) — nenhum handoff de thread jamais ocorria porque o código só ativava threads no bit `DELIVER` (`1<<9`), nunca setado pelo firmware real. Evidência: `ZEEBO_SYSCALL_HIST=1` mostrou `[EXREGS] dest=0x80008001/0x8000c001/0x80010001 control=0x11e` para `ig_naming`/`quartz_servers`/AMSS-BREW, nenhum com DELIVER. Fonte OKL4 2.1.1-fix7 (`exregs.cc`): HALTFLAG setado + HALT limpo = resume/start de thread halted. TDD `test_exregs_activation.cpp` (RED contra produção antiga: control=0x11e não ativa; GREEN com o fix). **Resultado real após fix:** o handoff ocorre de fato — Core0 sai de `0xb000c834`, PC salta para `0x10137000` (AMSS/BREW real) — mas trava em seguida em `0x1039322e` com instrução ARM/Thumb malformada (ver bloqueio QW41) |
| QW41 | **parcial (código + TDD); novo bloqueio isolado** | Core0 executa código AMSS/BREW real a partir de `0x10137000` | alto | Causa raiz identificada e corrigida em 3 pontos: (1) handoff de thread (`L4_Ipc`/`L4_ThreadSwitch`) não setava o T-bit a partir do IP alvo; (2) retorno de SVC não restaurava o T-bit quando o chamador está fora dos stubs L4 fixos (`0xb0000000-0xb0020000`, ARM) — SVC vindo de AMSS/BREW Thumb (ex. `0x103dcd18`) sempre entra em exceção ARM (hardware real) e o handler nunca restaurava o modo; (3) **achado adicional decisivo**: `uc_reg_read(PC)` no Unicorn nunca retorna o LSB setado, então a cada nova fatia (`uc_emu_start` do loop principal) o T-bit era perdido mesmo que a fatia anterior estivesse rodando em Thumb — corrigido reconstituindo o bit a partir do CPSR real antes de cada `uc_emu_start`. Prova por execução real: Core0 agora executa milhares de instruções Thumb genuínas de `0x10137000` até pelo menos `0x1039322e` (`cbz r4,...` roda como loop legítimo, não mais decode ARM inválido); em execuções subsequentes o boot avança ainda mais, travando em endereços posteriores do range de `ig_naming` (`0xb010333a`), variação atribuída a timing de scheduling cooperativo, não a regressão do fix. TDD `test_svc_thumb_resume.cpp` (RED contra CPSR-write direto/buggy; GREEN com bit0-no-PC). Suite completa + clean-hygiene verdes. **QW42 — 2 hipóteses testadas e descartadas**: (a) perda de T-bit no save de contexto do handoff cooperativo (`cur->ip=cur_pc` sem preservar LSB) — corrigido, stall não mudou; (b) classificador binário de T-bit no retorno de SVC substituído por `pc < 0x10000000` (kernel/libs=ARM, app=Thumb) — RE confirmou que a rotina em `0xb0102cb8` (chamada por BL de dentro do ig_naming, faz `svc #0x1400`/L4_Ipc) é ARM legado fora dos stubs fixos, então o classificador antigo do QW41 (`0xb0000000-0xb0020000`=ARM, resto=Thumb) marcava seu retorno como Thumb incorretamente — hipótese certa quanto ao mecanismo, mas o fix simples por faixa de endereço causou REGRESSÃO (trava bem mais cedo, em `0xb000cda4`, ~145K instruções) porque há SVCs do próprio AMSS/BREW (Thumb, `pc>=0x10000000`) que retornam para stubs específicos dependentes do comportamento antigo — a classificação correta não pode ser só por faixa de PC do chamador, precisa considerar também o destino/stub específico. Ambas tentativas revertidas com segurança (`git checkout HEAD --`); HEAD permanece em `c3115db`.
| QW42 | **concluído** | Fix da cópia LOCAL ARM do stub de trap-stack do `L4_ExchangeRegisters` (0x0c) dentro do `ig_naming` (`e67e3b4`) | médio | TDD `test_exregs_ig_naming.cpp` RED com classificador genérico antigo, GREEN com o fix restrito à faixa `[0xb0100000,0xb0120000)`; boot real avança p/ `0xb0358bb4`/`0xb03ba634`/memcpy `0xb0400064`; stall `0xb010333a` era artefato de modo errado (PC 2 B dentro de `bl`), não off-by-2 |
| QW43 | **concluído (análise/RE)** | Causa raiz do "underflow" RLE em `0xb0400000` = bug na SIMULAÇÃO PYTHON de sessão anterior (`r5+1`→`r5+2`), NÃO no firmware/dispatcher | médio | Oráculo Python decodifica 250/252 bytes → 1040/1040 exatos (strings kernel `"spinlockarm.s"`/`"Invalid argument"`); workaround de core testado e REJEITADO/revertido; zloader real (openzeebo) NÃO descomprime (memcpy puro); nenhuma correção C++ necessária |
| QW44 | **concluído** | Guard T-bit generalizado p/ TODOS os syscalls quando o retorno cai na cópia local ARM do ig_naming (`9ca92a6`); hipótese de truncamento por DMA descartada via Program Header ELF | médio | boot avança além do stall, `ig_naming` mapeado rwx, **14 frames / 32 draws do Adreno renderizados**; frame ainda **PRETO** (draws não conectados ao sink) |
| QW45 | **pendente; DD4, não quick win** | Conectar produtor guest ao framebuffer | alto/RE | Frame reconhecível do jogo + origem dos comandos; rejeitar azul constante e contagem artificial |
| QW46 | **REFUTADO por medição (2026-09-10)** | ~~Scheduler real na ordem OKL4 + retirar a injeção sintética de IPC~~ — o alvo não existe neste caminho de boot | — | **Instrumentando o hook de syscall: 8 SVCs no boot inteiro, TODOS `0x14` (L4_MapControl), do mesmo PC `b000c940`. ZERO `L4_Ipc` (0x00), ZERO `L4_ExchangeRegisters` (0x0c).** Todo o `case 0x00` — handoff cooperativo QW32, `pick_next_thread`, injeção sintética `MR0=1/MR1=0x16` — é **código morto** aqui, assim como o registro de servers por sniffing de faixa de IP (`register_service`, ~:2952). Não há o que "retirar": o boot do Core1 morre **antes** do primeiro IPC, no laço de page-table walk (ver QW49). Reabrir só depois que o Core1 passar do TCB. Nota: `notes/core1_boot_estado_real.md` |
| QW47 | **concluído** | Modelar as 3 UARTs do MSM7201A (UART1 `0xA9A00000` console, UART2 `0xA9B00000`, UART3 `0xA9C00000`) + captura de TX FIFO no stderr como console de boot (`aea313d`); corrige colisão KEYPAD_BASE (0xA9A00000 era UART1 real) | baixo-médio | TX FIFO acumulado → console em stderr; read-hook fornece TX_READY (UART_SR=0x0C); boot preservado (frame 14/32 draws, `make check` verde) |
| QW49 | **concluído (causa raiz provada com controle negativo)** | Laço infinito de page-table walk do Core1 pós-`creating root server`: a `.rodata` do kernel é servida do shadow Split I/D **zerado** (`5267476`, `339c0b2`) | alto | A tabela de tamanhos de página do OKL4 `{12,16,20,26,32}` em `0xf000efc8` cai dentro da janela do heap REX; o laço de zeragem apaga a cópia do shadow ⇒ `ldr r1` lê 0 ⇒ size 0 ⇒ `lsl r8` shift 0 ⇒ máscara `0xffffffff` ⇒ `sl=0xb0000007` ⇒ walk infinito. **Controle negativo (MORE_INFO §7) rodado**: baseline 8,2M insns / `rodata` 73,3M (8,93x, alcança TCB) / `control` (faixa vizinha `f0012000`, mesma mecânica) 8,3M (1,01x, NÃO alcança TCB). Teste `test_rodata_probe_control.cpp` no `check`, com guarda de **interpretador puro** (falha se detectar Dynarmic; controle positivo: com `--jit` acusa 2 ocorrências) |
| QW50 | **CAUSA MEDIDA — `obj@f001a52c == 0`** | `allocate_tcb` (`0xf0007008`) e chamada **uma unica vez** e o ponteiro do objeto alocador de TCB esta **ZERADO**: `[TCB] allocate_tcb ENTRA obj@f001a52c=0x00000000`. **O pool NAO e o problema** — no mesmo boot `pool_alloc` (`f0002b7c`) atende varios pedidos com sucesso (`f0001000`, `f0004000`, `f000b000`, `f000c000`), inclusive apos `creating root server`. **Isto REFUTA minha propria hipotese unificada** ("alocador e TCB sao um bug so"): sao dois, e o probe da `.rodata` so destrava o primeiro. Proximo: achar quem deveria construir `f001a52c` (BSS, alem do `filesz` `0xf001a324`) e por que nao roda | alto | Instrumentacao `ZEEBO_TCB_PROBE=1`. `--watch-writes` e **cego no Core1** — registra a faixa e nao reporta nada; usar hook proprio |
| QW50-desasm | concluido | `allocate_tcb` = `0xf0007008`, chamada em `0xf0016b8c`; o panic dispara pelo `beq 0xf0016d14` em `0xf0016bec`. O `mov r2,#0x4f0; add r2,#9` = **1273** confirma `thread.cc:1273`. **A implementação real NÃO é a free-list do corpus**: `allocate_tcb` carrega um objeto em `0xf001a52c` e chama um alocador (`bl 0xf00067e4`), com um 2º símbolo em `0xf001a54c`. **Os três endereços estão além do `filesz` do seg1 (`0xf001a324`) ⇒ vivem em BSS**, e `0xf001a52c` fica a 36 bytes do `pool head 0xf001a508` que já rastreamos. Ou seja: é o **mesmo alocador de `f0002b7c`** cuja semeadura o probe de `.rodata` destrava. Hipótese unificada: não são dois bugs (alocador + TCB), é **um só**. | alto | Substitui a leitura anterior. **REFUTA** a hipótese do elfweaver (`num_tcbs`/`free_tcb_idx`/`tcb_array` de `data.cc`): esses símbolos são de outra variante de build; este binário usa alocador por pool. Ver `notes/core1_boot_estado_real.md` |
| QW50-antigo | superada | Assertion do próprio kernel: `!"Failed to create root server TCB"` em `pistachio/src/thread.cc:1273` | alto | Alcançada com o probe de `.rodata` (73,3M insns) — território novo, nunca antes atingido. É onde threads passariam a existir, o que explicaria os zero IPCs do QW46. Corpus em `refs/okl4-2.1.1-fix7/pistachio/src/thread.cc` |
| QW52 | **aberto — bloqueio arquitetural** | **SMEM não é compartilhada.** `zeebo_lle_main.cpp:2215-2216` faz dois `uc_mem_map` anônimos (um por core) e inicializa com dois `uc_mem_write` separados; o comentário `// Shared SMEM 2MB` é **falso**. Provado empiricamente: Core0 escreve `0xDEADBEEF`, Core1 lê `0x00000000`. ⇒ SMD/SMSM/ProcComm são **fisicamente impossíveis**; toda "comunicação" é o host injetando `dummy_payload(16, 0x42)` e respondendo `PCOM_CMD_SUCCESS`. Fix: `uc_mem_map_ptr` com o MESMO backing de host (já usado para APPS_RAM em `:2248`) | alto | Apps/Modem compartilham DDR no MSM7201A (diferente de EE/IOP do PS2, que têm RAM separada e copiam por DMA no SIF) |
| QW53 | **aberto — bloqueio arquitetural** | **Zero entrega de IRQ/FIQ.** O VIC (`0xc0000000`) é `uc_mem_map` — RAM comum. Doorbell A2M só faz `vic_status0 |= (1<<int_num)` via `uc_mem_write` (`:3487-3491`): nunca troca CPSR para modo IRQ, nunca vetoriza para `0x18`. O único `UC_HOOK_INTR` filtra `intno==2` (SVC **síncrono**). O CPSR do Core1 nasce `0xD3` = **IRQ+FIQ mascarados** (`:2604`). GPT é contador que só avança quando lido (`:3907-3910`), sem tick. ⇒ emulador é 100% polling + valores forjados | alto | O PCSX2 não bootaria nada sem INTC/DMAC entregando IRQ em pontos de ciclo determinísticos |
| QW54 | **aberto** | **Syscalls: 4 reais faltando, não 24.** Por desassemblagem linear (capstone, só segmentos `PF_X`, imediato no range `SWIBASE+n`), os wrappers vivem em `0xb0102a5c..0xb0102f10` (seg7). Dos não implementados, o firmware só contém: `0x1c`, `0x20` cache_control, `0x24` security_control, `0x28` lipc. Os outros 20 (KPUTC, GETTICK, mutex, interrupt_control...) **não aparecem**. O `default: res_r0=0` é silencioso — devolve "sucesso" para o que não modela | médio | `cache_control` importa num ARM926 com Split I/D (QW51). Scan por máscara de palavra dá milhares de falsos positivos em dados — foi descartado |
| QW55 | **aberto** | **Hardware ausente por completo**: RTC, GPIO/TLMM, SDCC/MMC, clock/reset controller, PMIC PM7540 + barramento SBI/SSBI. Stubbados: MDDI (não faz DMA da linked-list), Adreno 130 (conta draws, não rasteriza), UART (status fixo `0x000C`), QDSP5 (sem hook no PACKET_CONSUMER). Modelados de fato: só NAND/EBI2 e ADM/DMOV (canal NAND) | médio | Auditoria de 4 frentes paralelas, 2026-09-10 |
| QW51 | **aberto** | Fix estrutural do Split I/D: separar `.rodata` (nunca do shadow) de `.bss`/pilha (nunca do pristino) — hoje o probe é **muleta declarada**, não correção | alto | Limiar linear de endereço **não** resolve: `.rodata` (`f000efc8`) e pilha (`sp≈f00196xx`) vivem no MESMO `PT_LOAD` (`va=f0000000 filesz=0001a324 memsz=0001e2c0`) e o super-ELF tem `shnum=0` (sem section headers). Tentativa por topo de imagem (memsz/filesz) → regressão para 16.717 insns/`pc=0`. Caminho: inferir `.rodata` por comportamento (lida como constante, escrita só pelo laço de zeragem) |
| QW48 | **proposto** | Resolver a re-entrada indevida da 2ª imagem no `__scatterload` (quartz_servers via `lr=0xb0302dd1` ou tabela corrompida por aliasing) — o kernel Iguana completa o scatterload corretamente, o travamento é host | alto | kernel termina reach `0xb0410070` __rt_entry (já verificado); 2ª imagem completa sem `ctrl=0x00`/loop; boot avança para `iguana_server_loop` pós-scatterload |

**QW42 — 3ª hipótese testada e descartada, com localização exata do SVC**: instrumentação `ZEEBO_DEBUG_SYSCALL_NEAR` (temporária) confirmou que o ÚNICO SVC disparado na faixa `0xb0102000-0xb0104000` antes do stall é `syscall=0x0c` (L4_ExchangeRegisters) em `pc=0xb0102c2c` — não `0x00`/L4_Ipc como hipotetizado antes. Confirmado por disassembly (Capstone) que `0xb0103338` é um `bl 0xb0102cb8` (função ARM que por sua vez faz `svc #0x1400`); o retorno real é reconstruído incorretamente pelo classificador `apply_tbit` genérico, mas a correção precisa (detecção de formato por bytes, halfword em `pc-2`==`0xDFxx`⇒Thumb) aplicada SÓ no branch do case `0x0c` (`else`, sem `did_handoff`) causou REGRESSÃO para o stall antigo `0x10137000` — ou seja, esse mesmo case/branch é usado pelo caminho normal que já FUNCIONA para chegar até dentro do `ig_naming`; a heurística "errada" (`apply_tbit` por faixa fixa) coincidentemente acerta esse caso mais comum, então substituí-la ali quebra o handoff que já funcionava. Revertido com segurança (`git checkout HEAD --`); HEAD confirmado em `d0c8edf`, suite/boot idênticos ao baseline. Conclusão: o bug do QW42 não está isolado num único ponto de retorno de SVC — é necessário identificar e diferenciar CADA call site específico (não por case de syscall nem por faixa de PC do chamador), possivelmente rastreando o LR do chamador de `0xb0102cb8` (visto no trace: `lr=0xb0046fa8`, fora de qualquer stub conhecido) para achar de onde realmente vem essa chamada.

**Bloqueio residual (QW42 — RESOLVIDO em `e67e3b4`)**: causa raiz encontrada e corrigida. `ig_naming` embute uma CÓPIA LOCAL ARM do stub de trap-stack do `L4_ExchangeRegisters` dentro de `0xb0100000-0xb0120000`, distinta do stub fixo `0xb000c758`. O classificador genérico `apply_tbit(pc)` do QW41 tratava qualquer `pc` fora de `0xb0000000-0xb0020000` como Thumb — correto para AMSS/BREW mas ERRADO para essa cópia local ARM. Prova por `ZEEBO_FULL_TRACE` + Capstone: o T-bit vira de 0→1 exatamente na transição de retorno do SVC `0xb0102c28→0xb0102c2c`, corrompendo a decodificação e eventualmente travando em `0xb010333a` (2 bytes dentro de um `bl` legítimo, não um bug de off-by-2 do handler — era efeito colateral do modo errado). Fix: no case específico `syscall==0x0c` (L4_ExchangeRegisters), tratar também a faixa `[0xb0100000,0xb0120000)` como ARM, sem alterar o comportamento fora dela (preserva o handoff cooperativo real). TDD `test_exregs_ig_naming.cpp` (RED com classificador genérico antigo; GREEN com o fix restrito à faixa) integrado a `make check`. Boot real agora avança de fato: passa por `0xb0358bb4`, `0xb03ba634`, entra num loop `ldrb/strb/subs/bne` em `0xb0400064` (memcpy Thumb válido).

**QW43 — workaround experimental testado e REJEITADO (revertido)**: implementei uma heurística no `c0_code_hook` — ao detectar `r4==0xffffffff` em `0xb0400062` (checagem pós-decremento do loop RLE), forçar `r4=0` e desviar para `0xb0400070` (mesmo destino usado pelo codec para "0 literais legítimo"), sem alterar mais nada. Resultado real (`/tmp/boot_qw43fix1.log`): o loop infinito de fato foi evitado — o boot AVANÇOU e chegou a produzir `[APPSBL] Performing handoff jump: bx r2 -> 0x10000000` (sinal de progresso real na cadeia de boot). PORÉM a execução trava logo depois num NOVO travamento em `pc=0x0045005c` (região de buffer de DMA de página, `DMA_PAGE_BUF=0x00450000`, não código válido) com `UC_ERR_INSN_INVALID` — ou seja, o decodificador RLE produziu uma saída incompleta/errada (porque interrompemos a descompressão no meio, sem realmente saber quantos bytes ainda faltavam), e código posterior tenta pular para um endereço de dados corrompido como consequência. CONCLUSÃO: o workaround não é seguro — ele evita o sintoma (loop infinito) mas não resolve a causa (dados de saída da descompressão ficam incorretos), gerando um bug diferente e potencialmente mais difícil de depurar mais adiante. Revertido integralmente; HEAD limpo. Isso reforça que o byte de escape zero provavelmente indica STREAM DE ENTRADA REALMENTE INCOMPLETO/CORROMPIDO na imagem NAND (`1.1.2_AMSS.bin`/`1.1.2_APPS.bin`) usada — não um caso de EOF legítimo do formato. Recomendação: (1) verificar integridade/hash das imagens NAND contra uma fonte alternativa antes de investir mais tempo em RE de baixo nível deste codec; (2) se as imagens forem a única cópia disponível e estiverem corretas, o próximo passo é reconstruir com precisão o algoritmo de descompressão completo (não só o ponto de falha) para simular corretamente o efeito de um "corte" de stream, o que exige engenharia reversa completa do codec (esforço significativo, ainda sem retorno garantido). |

**QW44 — Core1 destravado até o kernel OKL4 falar (`395685f`, `0c3ca70`)**: duas causas encadeadas, ambas *falhas silenciosas*.

1. **`uc_ctl_set_cpu_model` ignorado em silêncio** (`395685f`). A chamada vinha DEPOIS de `uc_ctl_tlb_mode()`; nessa ordem o Unicorn devolve `UC_ERR_ARG` e descarta o modelo — e o retorno não era checado. **O Core1 nunca foi ARM926**: rodava no CPU default, que rejeita `mrc p15,0,apsr_nzcv,c7,c14,3` (*test-and-clean dcache*, exclusiva do ARM9) com `UC_ERR_INSN_INVALID`, travando em `0xf001833c`. Medido isoladamente: ARM926 executa; ARM946 e ARM1176 rejeitam. Fix: `set_cpu_model` ANTES de `tlb_mode`, com os dois `uc_err` checados e abortando o boot. Regressão coberta por `test_uc_cpu_model_order` (controle negativo embutido: reprova a ordem antiga). Core1: 37.262 insns travadas → 810.000+ contínuas.

2. **Console do kernel OKL4 espelhado** (`0c3ca70`). O `putchar` do kernel (`0xf000e6e0`) grava byte a byte num ring buffer em `0xf001da68`, índice em `0xf001da64`, wrap `0x800`. Espelhar essas escritas transformou falha silenciosa em diagnóstico do próprio kernel:

```
kmem_init (f0000000, f0200000) [2M]
OKL4 - (provider: Open Kernel Labs) built on Apr 10 2008 15:12:43 using gcc 3.4.4
Initialized tracebuffer @ 00004000
Initializing kernel space @ 00000000...
Initializing KIP...
Assertion r != 0 failed in file pistachio/arch/arm/src/init.cc, line 130
--- KD# assert ---
```

**Dado negativo importante**: o dead-loop `0xf000e710` NÃO era espera de MMIO nem inicialização de dispositivo faltando — é o handler `ent0` da tabela de dispatch em `0xf0019e78` (índice `[0xf001da60]=0`), usado como stub de panic após a asserção. O `r0=1` do teste em `0xf000e704` vem de `mov r0,#1` **literal** em `0xf00143bc`, não de leitura de dispositivo. Vigia de escrita próprio foi necessário: `--watch-writes` não cobre o Core1 (vigiar TODA a memória deu 0 escritas = instrumento inválido).

> ⚠ **SUPERADA (2026-09-10).** O boot do Core1 avançou muito além desta asserção: hoje
> imprime o banner completo até `creating root server (000a8001)` e a barreira atual é a
> assertion do TCB em `thread.cc:1273` (QW50), alcançada com o probe de `.rodata` (QW49).
> O parágrafo abaixo fica como registro histórico do caminho percorrido.

**Próxima barreira (era aberta; hoje SUPERADA)**: a asserção `r != 0` em `init.cc:130`. No corpus OKL4 2.1.1-fix7 (referência, não cópia) as asserções desse arquivo com essa forma são `ASSERT(ALWAYS, r)` sobre o retorno de `kspace->add_mapping(...)` (linhas 392/404/453) — ou seja, **um mapeamento de página do kernel está falhando**. Note que o arquivo do corpus não bate linha-a-linha com o binário (build diferente), então a linha 130 não é diretamente localizável; a identificação do `add_mapping` é por forma da asserção, não por linha. Investigar `add_mapping`/`lookup_mapping` do kernel space é o próximo passo do Core1.

### P1 — Infraestrutura após o Passo 13

1. **Modelo de interrupção real (IRQ/timer)** [da auditoria 2026-09-09]
   - O Core1 (ARM9/AMSS) não tem `UC_HOOK_INTR`/`uc_intr`: o doorbell A2M só seta o bit no VIC (0xc0000000), nunca entregue ao core (CPSR sobe mascarado). GPT (0xc5000000) é só um ticker hack (sem match/compare/IRQ). Timer/IRQ-driven scheduling não funciona.
   - Gate: `uc_intr`/UC_HOOK_INTR no Core1 com salvamento de contexto IRQ e vetor 0x18; timer deve gerar tick IRQ; verificação por execução viva de um loop que espira IRQ.
2. **Guard `is_peripheral` no aliasing L4** [da auditoria 2026-09-09]
   - `map_one_aliased` não valida se `fpage.vaddr()` cai numa faixa de periférico já mapeada (0xa0000000+, 0xc0000000+...); `uc_mem_map_ptr` → UC_ERR_MAP → `uc_mem_protect` sobre MMIO, e no sucesso o VTLB registra VA de periférico → pool RAM.
   - Gate: fpage cujo VA é periférico não mapa para aliasing da pool; não registra no VTLB.
3. **Checkpoint v3 de máquina completa**
   - Primeiro vertical slice: CPU/memória + MMU + IRQ/timers, com validação integral antes da restauração; depois GPU/GL, EFS/VFS e filas/eventos.
   - Gate: save→run→restore→rerun produz os mesmos bytes, pixels, registradores e eventos.
4. **Tempo híbrido ARM11/ARM9/periféricos**
   - Contador relativo/dívida entre os dois cores; sync-on-access nas janelas IPC/SMD; deadlines absolutos para timers, IRQ, GPU e futuro QDSP5.
   - Gate: variar quantum sem alterar a sequência observável de bytes/eventos nos harnesses.
5. **JSON-RPC tipado sem quebrar NDJSON**
   - Envelope `id/method/params/result/error` e notificações assíncronas de break/watch/probe; compatibilidade temporária com comandos atuais.
6. **GDB RSP ARM11**
   - Adaptador sobre pause/step/breakpoints/memória/registradores existentes; NDJSON/Python continua sendo a API principal dos agentes.

### Itens deliberadamente não classificados como quick win

- `eglGetProcAddress`, `IEGLSurfaceManip`, IEGL11/IGLES11 e strings GL: dependem de objeto/VA/retorno real observado no firmware; não fabricar trampolim, vtable, interface ou string do Zeebx.
- Checkpoint completo, scheduler e GDB RSP: têm alto valor, mas são mudanças transversais; executar como P1 com gates próprios, não vendê-los como correções pequenas.
- `libco`/corrotinas, árvore dinâmica completa do ares, BML/icarus, GUI debugger do higan e dependência integral de nall: rejeitados para a arquitetura fixa baseada em Unicorn.
- Código Ymir/higan: GPL; transferir somente arquitetura/comportamento documentado e reimplementar. ares é permissivo no núcleo, mas dependências exigem auditoria/atribuição antes de cópia literal.
- Áudio/JPEG/VFE/QDSP5: aguardar liberação explícita do QDSP5.
- Backend GL host/ubershader: otimização posterior; primeiro fechar correção do backend software e boot guest.

---

## Regras de Higiene e Verificação
- **Clean-room Absoluto:** `a1Sim` permanece estritamente como oráculo caixa-preta (proibido descompilar).
- **Integridade da NAND:** Leitura exclusiva na cópia de trabalho; dump original preservado com `chmod a-w`.
- **Validação por Execução Real:** Todo avanço deve ser demonstrado por código executável com commits atômicos e registros auditados em `notes/FINDINGS.md`.
- **Rastreabilidade de build:** todo commit deve deixar o HEAD compilável — nenhum `#include`/alvo de Makefile pode apontar para arquivo não versionado. (Violado em `431461b`/`f51cccd`; corrigido em `0a5abd4`.)

---

## Estratégia de execução paralela

- **Concluído:** QW2–QW7, QW9, QW12, QW14–QW17, QW18 (análise), QW19, QW20, QW21 (análise) e QW22; todos integrados com TDD, mutações load-bearing e revisão independente (spec + quality). QW1/QW8 permanecem parciais honestos.
- **Frente A — cadeia crítica (Passo 13/14):** QW40 concluído (ativação real de thread por HALTFLAG, sem DELIVER), QW41 (preservação de T-bit), QW42 (`e67e3b4`: cópia local ARM do ExchangeRegisters), QW43 (underflow RLE = bug da simulação Python, não do firmware), QW44 (`9ca92a6`: guard T-bit generalizado). O boot real avança, renderiza 14 frames/32 draws do Adreno, mas o frame é PRETO. Próximos: QW45 (conectar draws ao framebuffer) e QW46 (scheduler real na ordem OKL4).
- **Frente de hardening curto:** QW14/QW15/QW16/QW20 concluídos; `--strict-unmapped` permanece opt-in e QDSP5 não foi alterado.
- **Depois do Passo 13:** Passo 14 e P1 na ordem checkpoint → tempo híbrido → JSON-RPC → GDB; só promover objetos, extensões e applets alcançados pelo boot real.
- **QDSP5:** congelado até liberação explícita; executar seus testes, mas não editar `tools/cpp/qdsp5/`.

Gate de toda frente: alvo afetado RED→GREEN, `make check`, `test-bootinfo-real` quando houver afirmação sobre BootInfo, QDSP5 sem alterações, `run_lle_cputests.sh` 12/12, `git diff --check` e clone limpo compilável.


**QW43 — sessão de análise estática (2026-09-09, sem execução de binários) — 3 achados confirmados por disassembly+simulação Python, hipóteses (a)/(b) documentadas**

Metodologia: 100% análise estática. Nenhum binário compilado (`arm-none-eabi-*`, `zeebo_lle_main`)
foi executado nesta sessão. Todo o trabalho usou `python3` + `capstone` (ARM/Thumb) lendo
diretamente `nand/1.1.2_APPS.bin` (ELF real, intacto, MD5 preservado) e simulação manual do
decoder RLE em Python puro replicando byte-a-byte a lógica Thumb desassemblada.

**1. Caller do dispatcher 0xb0400000 — CONFIRMADO por parse do Program Header ELF**
O segmento PT_LOAD #4 do ELF real cobre exatamente essa região: `p_offset=0x41000,
p_vaddr=0xb0400000, p_filesz=0x152a0, p_memsz=0x244d8, flags=W+X (0x80000007)`. O prólogo em
`0xb0400000-0xb0400034` (ARM puro) é um dispatcher genérico de tabela de "procedimentos
instalados": lê ponteiro base via `add r0,pc,#0x28` (PC-relative, literal pool em `0xb0400038`
contém dois offsets relativos que resolvem para `sl=0xb041516c` / `fp=0xb041519c` — a JANELA da
tabela real é `[0xb0415174, 0xb04151a4)`, 3 entradas de 16 bytes cada, EXATAMENTE
`(0x1a4-0x174)/16 = 3` — o loop termina por `cmp sl,fp; beq` de forma limpa e determinística
quando a tabela se esgota, **não por corrupção nem truncamento**.

**2. Estrutura da tabela de 3 entradas (16B: src,dst,len,fn) — bytes lidos diretamente do ELF**
```
entry0 @0xb0415174: src=0xb04151a4 dst=0xb04155b4 len=0xfc   fn=0xb040009c  (ARM,  memcpy 4/2/1-word chunks)
entry1 @0xb0415184: src=0xb04155b4 dst=0xb04151a4 len=0x410  fn=0xb0400040  (Thumb, o "decoder RLE" já mapeado)
entry2 @0xb0415194: src=0xb04152a0 dst=0xb04155b4 len=0xef24 fn=0xb04000c4  (ARM,  memset-zero em loop STM)
```
`entry2.fn=0xb04000c4` é disassembly-confirmado como rotina `mov r3,r4,r5,r6,#0` + `stmhs {r3-r6}`
em loop condicionado por `subs r2,#0x10` — **um memset/zero-fill**, não uma segunda fonte de dados
comprimidos. O "len" gigante (`0xef24`=61220) é o TAMANHO A ZERAR, coerente com limpar um buffer
BSS grande, não um payload RLE adicional.

**3. Byte real após o blob de 252 bytes — CONFIRMADO: são zeros do próprio ELF estático, não runtime**
`entry0` copia exatamente `len=0xfc=252` bytes de `src=0xb04151a4` (o blob comprimido real, bytes
confirmados na sessão anterior como strings ARM/OKL4 decodificáveis) para o buffer de scratch
`dst=0xb04155b4`. Inspecionando o ELF estático logo após o fim desse blob real
(`0xb04151a4+0xfc = 0xb04152a0`), os 64 bytes seguintes são **`0x00` no próprio arquivo do
firmware** — ou seja, não existe MAIS dado comprimido real reservado para esse stream em lugar
nenhum do binário estático; qualquer leitura além do byte 252 (pelo decoder de `entry1`, que
tenta produzir `len=0x410=1040` bytes de SAÍDA a partir de só 252 bytes de ENTRADA) cai
necessariamente em zeros do BSS estático.

**4. Simulação Python do decoder Thumb (`0xb0400040-0xb0400096`) contra os 252 bytes reais**
Reimplementei a lógica exata instrução-a-instrução (control byte: bits[0:2]=contagem de
literais via `lsls/lsrs #0x1d`, bits[4:7]=contagem de fill/backref via `asrs #4`, bit[3]=modo
fill-vs-backref via `lsls #0x1c`/`bmi`, extra bytes lidos quando os campos vêm zero do control
byte). Rodando contra os 252 bytes reais do firmware (sem forçar nada, sem pular bytes):
- Consumo do input: **252 de 252 bytes reais** (o decoder usa TODO o blob real disponível).
- Saída produzida: 962 bytes de um alvo de 1040 (`0x410`).
- Parada: falta o próximo control-byte/byte-extra exatamente no limite do blob real — ou seja,
  a simulação reproduz de forma consistente o "underflow" já documentado: **o decoder precisa
  de mais bytes de controle do que os 252 reais fornecem para preencher os 1040 bytes de saída
  esperados**.

**Hipótese (a) — bug real de tamanho/razão de compressão no firmware/dispatcher (favorecida)**
O par `(len_entrada_real=252, len_saída_esperada=1040)` implica uma razão de compressão ~4.1x.
Isso é plausível para um decoder RLE bem alimentado, mas os últimos ~10-15 tokens de controle
decodificados pela simulação ficam com padrões degenerados (ex.: sequências longas de `0x01`
repetido, fills grandes) que sugerem os ÚLTIMOS bytes reais do blob de 252 já estão sendo
consumidos em um regime de "esticar" a saída via fills grandes, não mais dados literais novos —
consistente com o decoder ter sido escrito para operar com um limite (`len=0x410`) que o
PRODUTOR (quem grava o dispatch table / o zloader upstream) não estava efetivamente honrando
com dados reais suficientes nesta imagem NAND específica. Sem o código-fonte do encoder (AMSS
fechado), não é possível provar se 0x410 é o `len` correto esperado pelo firmware real rodando
em hardware, ou se nossa leitura do campo `len` da entrada (offset+8, 4 bytes) está semanticamente
errada (ex.: talvez não seja "tamanho de saída" mas outro parâmetro ainda não identificado).

**Hipótese (b) — leitura incorreta do papel do `len` do dispatch table pelo nosso RE (não descartada)**
Como não confirmamos via nenhuma fonte independente (OKL4 upstream não cobre este dispatcher —
é código AMSS fechado da Qualcomm) o significado exato do 3º word de cada entrada de 16 bytes,
é possível que `len` não seja "bytes de saída esperados pelo decoder" e sim outro campo (ex.:
tamanho do buffer de trabalho, ou tamanho MÁXIMO permitido, não o tamanho que o decoder deve
necessariamente preencher até o fim antes de retornar). Nesse caso o "underflow" seria um
artefato da nossa suposição sobre a ABI da tabela, não um bug real do firmware. Não foi possível
descartar via análise estática pura — requer ou (i) mais dispatch tables similares em outras
partes do firmware para comparar padrões de `len` vs. saída real observada em execução (que
sessões futuras podem investigar sem tocar neste blob específico), ou (ii) uma fonte externa
sobre o formato deste codec proprietário (inexistente publicamente).

**Conclusão QW43 (honesta, sem promover falso progresso)**
1. O caller do dispatcher 0xb0400000 está confirmado por bytes do ELF: é um mini-dispatcher de
   3 procedimentos instalados (memcpy → decode RLE → zero-fill), não uma tabela truncada nem
   corrompida — o loop de 3 entradas termina exatamente onde deveria.
2. O "underflow" documentado nas sessões anteriores foi REPRODUZIDO por simulação Python pura
   (sem executar nenhum binário) contra os bytes reais do firmware: o decoder de fato precisa
   de mais bytes de controle do que os 252 bytes reais disponíveis para preencher os 1040 bytes
   de saída declarados pela entrada da tabela.
3. As duas hipóteses (a) bug/limite real do firmware nesta imagem NAND específica vs. (b) nosso
   entendimento do campo `len` da ABI do dispatch table estar errado permanecem ambas em aberto;
   nenhuma prova estática decisiva as separa nesta sessão.
4. Recomendação: não investir mais tempo tentando "consertar" o decoder sem antes decidir entre
   (a)/(b) — um workaround forçado (como o já testado e revertido em sessão anterior) mascara o
   sintoma sem resolver a causa raiz, e o próprio ROADMAP já documenta esse risco. Próximo passo
   de maior valor: procurar OUTRAS tabelas de dispatch do mesmo formato em `0xb0000000+` cujo
   par (len_entrada, len_dst) possa ser cruzado com saída REALMENTE observada em execução viva
   (via debug agent / control-port, não terminal direto) para decidir (a) vs (b) com evidência
   cruzada, antes de qualquer nova tentativa de correção.

### QW56 — Split I/D corrompe a BSS/`.data` do kernel  **[PARCIALMENTE INVALIDADO — ver QW58]**

> ⚠ **Esta secao foi refutada em parte.** O clobber do `.bss` descrito aqui e' real,
> mas a conclusao "causa raiz do panic do TCB" e' **FALSA**, e a guarda aplicada era
> **assimetrica** (so' no read path), o que **destruiu o pool do kmem**. Ler a QW58
> antes de confiar em qualquer numero desta secao. O texto abaixo fica como registro
> do raciocinio original.

**Provado por medicao** (interpretador puro): `shadow[f001a538]=0x00000100` vs
`uc[f001a538]=0x00000000`. O `init_tcb_allocator` roda, escreve certo, e a RAM do
Unicorn e' revertida a zero por `c1_heap_read_hook` (`zeebo_lle_main.cpp:3946`).

- Janela `REX_HEAP_VA_BASE=0xf0000000 + 0x200000` cobre `.text`+`.data`+`.bss` do kernel.
- Toda leitura na janela serve shadow/pristino **e faz `uc_mem_write` por cima**.
- `.bss` no pristino = zeros (sem filesz) => variaveis globais do kernel sao apagadas.
- Sintoma visivel: panic `Failed to create root server TCB` (thread.cc:1273).
- **`ZEEBO_PROBE=rodata` e' remendo do mesmo defeito**, nao a causa.

**Proximo passo**: teste RED que prove a corrupcao (escreve global na janela, le de
volta, exige o valor escrito) e so' entao restringir a janela ao `.text` executavel.

**Rebaixa**: QW50 deixa de ser "causa localizada" — a causa e' esta.

**CORRECAO APLICADA** (`c1_heap_read_hook`): guarda `if (off >= REX_KERNEL_FILESZ) return;`
com `REX_KERNEL_FILESZ = 0x0001a324` (filesz do seg1). O pristino so' e' servido onde
ha' lastro no arquivo; `.bss` passa a viver na RAM do Unicorn como deveria.

**Controle negativo (obrigatorio, §7)** — mesmo binario, so' a guarda muda:

| guarda | onde o boot para |
|---|---|
| desativada | `creating root server (000a8001)` -> panic TCB (thread.cc:1273) |
| ativada    | avanca; nova parada em `tracebuffer.cc:116` |

**Sem `ZEEBO_PROBE`** — o boot passa do TCB com o probe desligado. O remendo virou no-op.

**Teste**: `test_split_id_bss_clobber` (RED comentando a guarda: exit 1, `lido=0x0`;
GREEN com ela: exit 0). Controle positivo embutido: se o shadow nao registrar a
escrita, o teste aborta com exit 2 em vez de concluir.

**Efeito colateral**: `test_rodata_probe_control` ficou obsoleto nas premissas (1) e (3)
— exigia que o boot travasse no TCB. Rebaixadas a INFO; os dois controles negativos
seguem valendo.

### QW88 — Anatomia da tabela de inicialização CRT em `0xb0400000` e o conflito entre Task 1 e Task 2  **[MEDIDO com probe dinâmico]**

Aprofundando a causa levantada no QW86-87, a rotina em `0xb0400000` (segmento 4 do `1.1.2_APPS.bin`) foi totalmente desassemblada e analisada contra os binários e em tempo de execução (`ZEEBO_TBL_DBG`).

**Estrutura da rotina (`0xb0400008..0xb0400034`):**
Ela não é um descompressor genérico isolado, mas um despachante de inicialização de CRT/runtime com uma tabela iterativa `{src, dst, len, handler}`:

    b0400008  add ip, pc, #0x28      ; ip = b0400038
    b040000c  ldm ip, {sl, fp}       ; sl = início da tabela, fp = fim
    b040001c  cmp sl, fp             ; terminou a tabela?
    b0400020  beq b0410070           ; SIM -> salta para o entry point real do serviço
    b0400024  ldm sl!, {r0,r1,r2,r3} ; r0=src, r1=dst, r2=len, r3=handler
    b0400028  sub lr, pc, #0x14      ; lr = b040001c (retorno fixo)
    b0400034  mov pc, r3             ; chama handler

**A tabela em `0xb0415174..0xb04151a4` possui exatamente 3 entradas:**
1. **Entrada 0 (`h=0xb040009c`)**: `memcpy` ARM de 252 bytes (`len=0xfc`) de `0xb04151a4` para `0xb04155b4`.
2. **Entrada 1 (`h=0xb0400040`)**: descompressor LZ/RLE Thumb in-place — lê 252 bytes de `0xb04155b4` e descomprime 1.040 bytes (`len=0x410`) gravando de volta em `0xb04151a4`. O buffer descomprimido contém strings como `"spinlockarm.s"` e `"Invalid argument"`, além de parâmetros para a etapa seguinte.
3. **Entrada 2 (`h=0xb04000c4`)**: `memset(0)` ARM de 61.220 bytes (`len=0xef24`) a partir de `0xb04155b4` (limpeza de BSS).

**O que o probe dinâmico (`ZEEBO_TBL_DBG`) comprovou:**
- **Task 1 (`SP=0xb0046f7c`)**:
  - Executa a Entrada 0 (`memcpy` 252B).
  - Executa a Entrada 1 (descompressão LZ 1040B).
  - Executa a Entrada 2 (`memset` zero 61KB).
  - Em `0xb040001c`, `cmp sl, fp` encontra `sl == fp` (`Z=1`).
  - O salto `beq b0410070` é tomado com sucesso e a Task 1 atinge o corpo do serviço em `0xb0410070`!
- **Task 2 (`SP=0xb0327e3c`)**:
  - Uma segunda task re-executa `0xb0400000`.
  - Como o emulador possui um único espaço plano de memória para o Core0, a área `0xb04151a4` já havia sido sobrescrita pelos 1.040 bytes da descompressão da Task 1.
  - A Entrada 0 copia dados já descomprimidos (`01 00 00 00...`) para `0xb04155b4`.
  - A Entrada 1 tenta descomprimir dados que já foram expandidos; o parser RLE consome tamanho zero e o laço interno em `0xb040006c` (`subs r4, #1`) sofre sob underflow para `r4 = 0xFFFFFFFF`, travando o Core0 em 4 bilhões de iterações.

**Estratégia de resolução definida (Diretiva: mais rápido e fácil primeiro, depois o estrutural):**
1. **Fase Rápida/Fácil (Próxima etapa)**: Repristinar o buffer original de 252 bytes em `0xb04151a4` (obtido da imagem limpa do ELF) quando uma nova task re-executar o vetor de inicialização, ou preservar/restaurar no chaveamento de contexto. Isso neutraliza o underflow e permite medir se a Task 2 também completa o init e avança para `b0410070`.
2. **Fase Estrutural/Difícil**: Implementar separação real de address space (múltiplas instâncias de `uc_engine` por L4 space ID ou comutação dinâmica de TLB/page tables baseada no `sid` do `handle_map_control`).

### QW89-QW96 — Plano auditado: quick win de BOOT primeiro, correção estrutural depois

Esta seção substitui a formulação imprecisa de que existiriam "quatro bloqueios
imediatos" equivalentes. A auditoria do código e uma nova execução viva mostraram que
eles pertencem a etapas diferentes:

| item | estado exato | bloqueia o ponto atual? | falta concreta |
|---|---|---|---|
| Address spaces/tasks do Core0 | **bloqueio atual provado** | **SIM**: Core0 para na segunda execução do scatterload | selecionar memória por `sid`/PD e preservar contexto por thread |
| SMEM/SMSM/ProcComm | **defeito arquitetural provado** | ainda **não provado como a próxima barreira**; o Core0 para antes | mesmo backing host mapeado nos dois cores; completion após o STR guest |
| IRQ/FIQ/VIC/GPT | **subsistema ausente provado** | ainda **não provado como a barreira atual**; o caminho observado progride por polling/cooperativo | VIC ativo, pending/mask/ack, entrada/retorno IRQ e timer compare→IRQ |
| BREW/game loader | **gap posterior, parcialmente implementado** | **NÃO bloqueia o firmware em `b040`;** bloqueará o jogo depois do AEECShell | loader raw `.mod`, CreateInstance real, VFS e handler do objeto real |

#### QW89 — testemunha que faltava: identidade da segunda execução

Execução reproduzida (interpretador puro):

    ZEEBO_TBL_DBG=1 ZEEBO_SYSCALL_HIST=1 \
      ./zeebo_lle_main --headless --boot-appmgr --seconds=45

Os três servidores são criados pelo firmware:

    ig_naming       tid=0x80008001  sp=b0147f24  ip=b0100000
    quartz_servers  tid=0x8000c001  sp=b0327f24  ip=b0300000
    AMSS/BREW       tid=0x80010001  sp=b0e1ff0c  ip=10137000

O primeiro scatterload roda antes de `ThreadTable.current_tid` ser estabelecido:

    tid=0           sp=b0046f7c  lr=b000c3fc  -> entradas 0,1,2 -> Z=1

Depois, a reentrada problemática foi identificada sem inferência por pilha:

    tid=8000c001    sp=b0327e3c  lr=b0302dd1  -> entradas 0,1 -> underflow

Logo, a segunda execução pertence especificamente a **`quartz_servers`**, e o LR
Thumb `b0302dd1` retorna ao código do próprio quartz. Não é uma continuação acidental
do primeiro laço. O firmware está iniciando o mesmo runtime/scatterload em outro PD,
mas o emulador lhe entrega a área gravável já transformada pela primeira execução.

A raiz no código é composta, não apenas "um `uc_open`":

1. `handle_map_control(..., sid, ...)` imprime `sid`, mas todos os `map_one` caem no
   mesmo `core0_.uc`; não existe tabela de mapas por `sid`.
2. `ThreadInfo` guarda apenas `tid/sp/ip/flags`; não guarda `space_id`, registradores
   gerais, CPSR nem contexto Unicorn completo.
3. `MapControl` de query ecoa os MRs da requisição em vez de consultar um mapa por PD;
   fpage nil/unmap é ignorada.
4. O scheduler cooperativo troca somente PC/SP. Isso não é contexto L4 completo e
   precisa ser corrigido junto da separação de espaços para o fix estrutural.

#### QW90 — Quick win diagnóstico (primeiro ataque)

**Objetivo limitado:** fazer `quartz_servers` atravessar a segunda inicialização e
revelar a próxima fronteira. Isto NÃO será chamado de boot concluído nem de correção
de MMU.

Implementação mínima, sempre atrás de flag explícita de investigação:

1. Em `load_apps`, antes de descartar o vetor do ELF, salvar os **252 bytes pristinos**
   `[b04151a4,b04152a0)` e seu hash. Não copiar bytes de outro binário nem regenerar
   a saída do decoder.
2. No `c0_code_hook`, detectar a segunda entrada em `b0400000/b0400008` somente quando
   `current_tid==0x8000c001` e o SP estiver na pilha `b032xxxx`; restaurar os 252 bytes
   **antes** da entrada 0. Não pular instrução, não alterar `r4`, PC ou retorno.
3. Logar `restore_count`, `tid`, SP e hash antes/depois. Exigir exatamente uma
   restauração; repetição inesperada é falha.
4. Remover/desligar a flag após a medição. O mecanismo corrompe o estado privado do
   primeiro PD no mapa plano e, portanto, é deliberadamente descartável.

Gates obrigatórios:

| gate | baseline/controle | experimento |
|---|---|---|
| instrumento positivo | primeira execução chega a `Z=1`/`b0410070` | continua chegando |
| causalidade | flag OFF: segunda execução chega à entrada 1 e `r4=ffffffff` | flag ON: segunda execução conclui entradas 0/1/2 e chega a `Z=1` |
| progresso externo | `MapControl=756`, 100% da janela em `b0400064-6e` | contador/evento externo novo OU PC fora de `b040` sustentado por janela |
| controle inválido | APPS inválido não passa do loader/ELF | não pode receber restore/PASS |
| regressão | `make check`, interpretador puro | mesmos testes + teste RED que reproduz os bytes sobrescritos |

**STOP:** se só o contador de instruções aumentar, ou se a Task 2 sair do laço mas
produzir PC/dados inválidos, o quick win falhou. Registrar a nova fronteira e não
empilhar outro patch de PC/registrador.

#### QW91 — Fix estrutural de address spaces (segundo ataque, difícil)

Fazer em quatro entregas verificáveis, não em um rewrite único:

1. `AddressSpaceTable`: mapa `(sid,va)->(phys,size,perms,attr)` com query e unmap reais.
2. Associar `tid -> sid` durante `SpaceControl`/`ThreadControl`/BootInfo; salvar/restaurar
   contexto completo (`uc_context`) por thread, não só PC/SP.
3. Fazer a troca de espaço **fora** de code/intr hooks (fila `pending_space_switch`,
   drenada depois de `uc_emu_start`, como o fix reentrante QW79). Primeiro protótipo:
   page-bank por `sid` no mesmo engine; se remap em massa for instável/caro, promover
   para um `uc_engine` por PD com hooks/dispositivos compartilhados.
4. Compartilhar apenas páginas físicas explicitamente comuns; writable private de
   `quartz_servers` deve divergir do root, enquanto páginas realmente concedidas devem
   manter alias byte-idêntico.

Gate estrutural: duas PDs mapeiam o mesmo VA para backings distintos e mantêm bytes
independentes; duas VAs/PDs que mapeiam o mesmo PA compartilham bytes; switch A→B→A
restaura registradores+CPSR+memória; o scatterload completa nos dois PDs **sem restore
especial de `b04151a4`**. Variar quantum não pode alterar a sequência observável.

#### QW92 — SMEM/SMSM/ProcComm (rápido depois da fronteira Core0)

O comentário `// Shared SMEM 2MB` é falso: `setup_memory_maps()` usa dois
`uc_mem_map` anônimos e duas inicializações. Correção:

1. Criar backing host de 2 MiB com lifetime do sistema.
2. Mapear o mesmo ponteiro em Core0 e Core1 via `uc_mem_map_ptr`.
3. Corrigir ProcComm: hoje o hook escreve `CMD_DONE` antes de o STR original terminar,
   podendo ser sobrescrito pelo próprio guest. Enfileirar completion e aplicá-la depois
   da fatia, como QW79.
4. Remover `dummy_payload(16,0x42)` do caminho de produção; conservar somente fixture
   explicitamente sintética.

Gate: write guest bidirecional visível byte a byte, estados SMSM/SMD produzidos pelos
cores, request→completion exatamente uma vez e nenhum pacote host contado como RPC guest.
Este item é necessário para modem/serviços/áudio, mas só será promovido a bloqueio do
boot quando uma espera viva em SMEM/doorbell for capturada.

#### QW93 — IRQ/FIQ/VIC/GPT (depois de capturar a primeira dependência)

Hoje VIC e GPT são RAM/ticker: setar bit em `0xc0000000` não entra em modo IRQ, não
salva CPSR/LR e não vetoriza para `0x18`. Implementar:

1. modelo VIC por core: pending, enable/mask, acknowledge/EOI e prioridade mínima;
2. injeção ARM correta quando CPSR.I permite, com SPSR/LR_irq e vetor `0x18`;
3. GPT com contador, compare e geração determinística de IRQ;
4. teste de retorno real pelo handler e repetibilidade com quanta diferentes.

Não implementar FIQ, todas as linhas e todos os timers de uma vez: começar pela linha
que uma captura viva provar necessária. Gate positivo: loop guest é interrompido,
handler reconhece/limpa e retorna ao PC exato. Gate negativo: IRQ mascarada permanece
pending sem executar handler.

#### QW94 — BOOT orgânico até AppMgr (fronteira B)

`--boot-appmgr` hoje só altera `boot_firstapp_` e mensagens de log; não escreve
`flixfile.dat` nem muda o fluxo do guest. Fechar BOOT exige:

1. Core0 e Core1 continuarem vivos após os fixes acima;
2. AEECShell ser alcançado organicamente pelo thread `0x80010001`;
3. `FIRSTAPP:0` ser observado/fornecido na fronteira real de arquivo/configuração;
4. PC e eventos dentro do AppMgr real, sem scratch applet e sem handler fixo.

Gate B: cold boot alcança um evento real do AppMgr e o controle com 0:APPS/configuração
corrompida falha identificavelmente. Isso ainda não significa jogo carregado.

#### QW95 — módulo comercial: LOAD e primeira instrução (fronteiras L/I)

O caminho atual é apenas infraestrutura:

- `resolve_mod_entry()` aceita ELF; `ddragonz.mod` é módulo ARM cru;
- `BrewSymbols.ishell_create_va/aeemod_load_va/aeeclscreate_va` permanecem zero;
- `dispatch_applet_start()` ignora `handler_va` e chama sempre o ZeeboApp
  `0x10532344` com objeto/vtable scratch;
- `run_zwheel_interactive()` pinta azul no host;
- o parser MIF varre qualquer u32 parecido com CLSID e não resolve DD de forma estrutural;
- `--boot-appmgr` não seleciona FIRSTAPP no guest.

Ordem: parser MIF real (`CLSID=0x0102F789`) → formato/relocs/imports/ZI do `.mod` cru →
`AEEMod_Load` → `ISHELL_CreateInstance`/`AEEClsCreateInstance` → objeto+HandleEvent reais →
primeiro PC dentro da faixa carregada do DD. Arquivo inválido, MIF truncado e CLSID errado
devem falhar antes da execução. O estado deve distinguir `loaded_only`, `instanced` e
`executed`.

#### QW96 — lista completa após primeira instrução do jogo (F/N/A)

1. **VFS/armazenamento:** overlay read-only para `fs:/mmc4/mod/274754`, MIF e assets;
   open/read/seek/stat/close na fronteira guest real; remover `data.ggz` deve falhar.
2. **Frame guest:** resolver objeto vivo IDisplay/IBitmap/IGL/IEGL e ligar ao
   SoftRasterizer; clear azul/contador de draw não contam. Controle inválido não gera frame.
3. **Input/tempo:** eventos e callbacks/timers no objeto DD real; o loop atual descarta
   callbacks expirados. Sequência deve avançar splash→menu→fase e key-up não pode prender.
4. **Áudio:** requisição guest → SMD/ONCRPC real → buffer/codec → mixer → SDL/WAV;
   sem dummy, com request/completion correlacionados. QDSP5 continua congelado até liberação.
5. **Periféricos sob demanda:** syscalls reais ainda ausentes `0x1c/0x20/0x24/0x28`;
   RTC, GPIO/TLMM, SDCC/MMC, clocks/reset e PMIC entram somente quando um trace vivo os exigir.

Marcos não intercambiáveis:

    B = AppMgr orgânico
    L = módulo DD carregado e instanciado
    I = primeiro PC dentro do DD
    F = primeiro frame produzido pelo DD
    N = input altera estado do DD
    A = PCM originado pelo DD

Somente `B+L+I+F+N+A`, com controle negativo e uma sessão reproduzível de partida,
permite marcar Double Dragon jogável. Nenhum desses marcos é fechado por bytes copiados,
frame azul, SDL aberto ou contador maior.

### QW81-QW87 — Causa raiz do Core0: **um unico address space para todas as tasks**  **[MEDIDO — bloqueio arquitetural]**

Com o teto de 45s removido (QW78-80), rodei ate 180s: `insns=640.944.226`, sem
crash, mas **`MapControl` congelado em 756** e o PC sempre em `b04000xx`.

**QW81-QW83 — onde o tempo e gasto (histograma nao-filtrado, STATS §1)**

Bucket de 64KB: `b040` absorve **100% do crescimento** (50M por janela); todo o
resto congelado. Histograma fino dentro do bucket:

    b0400064: 32.164.200   ldrb r6,[r0]
    b0400066: 32.164.200   adds r0,#1
    b0400068: 32.164.199   strb r6,[r1]
    b040006a: 32.164.199   adds r1,#1
    b040006c: 32.164.199   subs r4,#1
    b040006e: 32.164.199   bne  b0400064

O laco **interno** de copia literal roda ~32 milhoes de vezes — mas o probe no
laco **externo** (`b040004a`) so disparou **32 vezes**, e progredindo
(`parados=0`). As duas medidas nao batiam; nao concluí antes de reconciliar.

**QW84 — o contador estoura**

    entrada#1..#8   r4 = 1, 4, 2, 2, 1, 1, 1, 1     (sadio)
    entrada#59      r4 = 0xFFFFFFFF (4.294.967.295)  <<<

`subs r4,#1` com `r4=0` faz **underflow**: o laco passa a copiar ~4 bilhoes de
bytes. Na entrada#59 o `dst` **volta ao inicio** (`0xb04151a4`, o mesmo do #1).

**QW85 — por que o comprimento veio 0**

    call#1  src=0xb04155b4 dst=0xb04151a4 len=1040  bytes=72 01 65 b4 55 41 b0 23
    call#2  src=0xb04155b4 dst=0xb04151a4 len=1040  bytes=01 00 00 00 01 00 00 00
    CTRL (b0400040): 01c08fe2 nas duas  -> a leitura de memoria esta sadia

A **mesma** descompressao roda duas vezes com argumentos identicos; na segunda a
fonte ja foi sobrescrita. A conta fecha: `dst + len = 0xb04151a4 + 0x410 =
0xb04155b4 = src`. **O destino termina exatamente onde a fonte comeca** —
descompressao **in-place** com buffers adjacentes, layout legitimo e comum.

> **Isso nao e bug do descompressor.** In-place e correto rodando **uma vez**.
> A pergunta certa nao era "por que o LZ trava", e sim **"por que roda duas vezes"**.

**QW86 — nao e laco: sao duas tasks**

    call#1  lr=0xb040001c  sp=0xb0046f7c
    call#2  lr=0xb040001c  sp=0xb0327e3c   <- SP completamente diferente

Pilhas distintas = contextos distintos (um laco reusaria o SP). O chamador e um
**interpretador de tabela de init**:

    b0400000  b   b0400008           ; entry point da task
    b0400008  add r0, pc, #0x28      ; base da tabela (PC-relativo)
    b040000c  ldm r0, {sl, fp}       ; sl=inicio, fp=fim
    b040001c  cmp sl, fp             ; fim da tabela?
    b0400020  beq b0410070           ; sim -> segue o boot
    b0400024  ldm sl!, {r0,r1,r2,r3} ; entrada: src, dst, len, handler
    b0400028  sub lr, pc, #0x14      ; lr = b040001c  (CONSTANTE por construcao)
    b0400034  bx  r3                 ; chama o handler (descompressor)

`sub lr,pc,#0x14` explica o LR identico nas duas chamadas: e constante, **nao**
indica o mesmo chamador dinamico.

**QW87 — a causa raiz, confirmada por medicao E por estrutura**

Log de mapeamentos: **63 paginas fisicas mapeadas em mais de um space L4**.

    phys=0x10200000 -> space=0x80000100 (va 0x10200000 e va 0xb0f00000)
                    -> space=0x80010001 (va 0x10200000)

No codigo: **um unico `uc_open` para o Core0** (linha 1228). Um `uc_engine` = um
espaco de enderecos. `handle_map_control` recebe `sid` mas **o sid nao seleciona
espaco nenhum** — todo mapeamento cai no mesmo `uc`.

O L4 cria multiplos address spaces; o emulador os **colapsa num unico mapa
plano**. Tasks que deveriam estar isoladas enxergam a memoria uma da outra.

**Cadeia completa, do sintoma a raiz:**

| # | Fato medido | QW |
|---|---|---|
| 1 | Core0 "travado" em `b0400064`, 32M insns/janela | QW83 |
| 2 | laco interno roda com `r4 = 0xFFFFFFFF` | QW84 |
| 3 | `r4` estourou porque o comprimento lido foi **0** | QW84 |
| 4 | leu 0 porque a **fonte ja estava sobrescrita** | QW85 |
| 5 | sobrescrita porque a descompressao e **in-place** (`dst+len == src`) | QW85 |
| 6 | in-place executada **duas vezes**, SPs distintos = **2 tasks** | QW86 |
| 7 | as tasks compartilham memoria: **1 address space** no emulador | QW87 |

O item 5 e comportamento legitimo do firmware. **O defeito e o item 7.**

Isto e a mesma raiz do bloqueio ja registrado ("SMEM nao compartilhada",
mapeamentos `[aliased]`), vista de outro angulo: **o emulador nao modela address
spaces separados.**

**Fix necessario (nao implementado):** um `uc_engine` por task, ou TLB virtual
comutada por `sid` em `handle_map_control`. Mudanca estrutural — parei aqui para
decidir o caminho.

### QW78-QW80 — SIGSEGV: invalidacao de TB nao e' reentrante em code hook  **[CORRIGIDO]**

O `exit=139` estava catalogado como "segfault de host pre-existente". **Nao era.**
Era um teto: qualquer investigacao que precisasse passar de ~45s do Core0 morria
ali.

**QW78 — backtrace (gdb, `-O0 -g`):**

    #1 tb_invalidate_phys_range_arm   libunicorn
    #3 uc_ctl                         libunicorn
    #4 ZeeboLLESystem::c1_code_hook   <-- nosso codigo
    #5 helper_uc_tracecode            libunicorn

`c1_code_hook` roda **via `helper_uc_tracecode`**, ou seja, de dentro do
translation block em execucao. Chamar `uc_ctl(TB_REMOVE_CACHE)` dali invalida e
libera o proprio TB que esta executando; o retorno cai em memoria liberada.

A faixa era sempre sadia (4 bytes) — **o problema nunca foi o argumento, foi o
reentrance**. Por isso so aparecia as vezes: depende de o TB invalidado ser
exatamente o que esta em execucao, o que fica mais provavel quanto mais tempo
roda (batia com "pre-existente com `seconds=20`").

**QW79 — causalidade provada (controle negativo, 2 pares alternados):**

    COM invalidacao (controle):  exit=139, exit=139   <- sempre
    SEM invalidacao (teste):     exit=0,   exit=0     <- nunca

    Core0 COM: insns=7.044.226
    Core0 SEM: insns=105.944.226 / 106.644.226   (15x mais longe)
    L4_MapControl = 756 nos dois                 (boot nao regrediu)

**Mas remover a invalidacao NAO e o fix.** Ela existe porque o Split I/D do
Core1 escreve no heap REX; sem invalidar, o Unicorn seguiria executando a
traducao **antiga** de codigo automodificado. O boot nao regredir aqui e sorte,
nao corretude.

**Fix correto — adiar, nao remover:** enfileirar as faixas durante o hook e
aplica-las **fora** do `uc_emu_start`, onde `uc_ctl` e reentrante:

    queue_tb_invalidate(pc, pc+size)   // no hook
    drain_tb_invalidate(core1_.uc)     // apos uc_emu_start, contexto seguro

**QW80 — controle POSITIVO do fix** (senao "nao crasha" poderia significar
apenas "a fila nunca e usada" — bug mudo no lugar de crash):

    drains=8192  invalidacoes=12.037.608  fila_max=4096  saturou=2

12 milhoes de invalidacoes **realmente aplicadas**. A fila trabalha.

> ⚠ **Defeito que eu mesmo introduzi:** escrevi `if (q.size() < 4096)`, que
> **descarta silenciosamente** ao encher — exatamente o bug mudo que eu dizia
> querer evitar. `saturou=2` mostrou que acontecia de verdade. Removido o teto;
> pico real medido = **4664** (o teto de 4096 descartava ~568 invalidacoes).
> Custo de memoria e irrelevante (8 bytes/par, drenado a cada fatia).
> **Nunca descartar trabalho de correcao para respeitar um limite arbitrario.**

**Validacao final** (`-O2`, sem instrumentacao):

    run1 exit=0  insns=157.410.000  MapControl=756
    run2 exit=0  insns=155.810.000  MapControl=756
    run3 exit=0  insns=158.810.000  MapControl=756
    make check   exit=0  ALL TESTS PASSED

De 7 milhoes de instrucoes (com crash) para ~157 milhoes (estavel): **22x**.
O teto de 45s deixou de existir.

### QW77 — Nao existe bug ARM/Thumb; Core0 atravessa o memset e entra na descompressao  **[MEDIDO com controle]**

**A previsao do QW76 foi testada e confirmada.** Se o gargalo era throughput
(nao deadlock), mais tempo deveria produzir um PC novo:

    seconds=12  ->  pc=0xb000aff4  insns=4.966.776   (ainda no memset de 5 MB)
    seconds=45  ->  pc=0xb040006c  insns=7.044.226   (fase NOVA)

O Core0 **atravessou** o memset e avancou.

**Onde ele chegou:** `0xb0400040` (seg4, `va=0xb0400000 off=0x41000`) e' um
**veneer ARM->Thumb** canonico, seguido de um **descompressor LZ em Thumb**:

    b0400040  add  ip, pc, #1     ; veneer: LSB=1
    b0400044  bx   ip             ; troca para Thumb
    b0400048  adds r2, r1, r2     ; --- Thumb daqui em diante ---
    b040004a  ldrb r3, [r0]       ; le byte de controle
    b0400064  ldrb r6, [r0]       ; copia literal
    b0400068  strb r6, [r1]
    b040006c  subs r4, #1
    b040006e  bne  b0400064
    b0400082  ldrb r4, [r0]       ; referencia para tras (match LZ)
    b0400096  cmp  r1, r2
    b0400098  blo  b040004a       ; ate' preencher o destino

**Medicao do T-bit, com controle negativo:**

    LZ (b0400040-b04000a0):  T=1: 989.650   T=0: 36
    CTRL memset (ARM):       T=1: 0         T=0: 6.150.015
    PCs no LZ: b0400040 44 48 4a 4c 4e 50 52 54 56 58 5a 5c 5e   <- passo 2

O veneer (`40`,`44`) aparece com passo 4 (ARM); de `48` em diante o passo e' 2
com `T=1` (Thumb). O controle na funcao ARM da `T=0` puro — o instrumento
**distingue** os dois casos.

> **O "passo de 4" que me enganou** (`64,68,6c`) era artefato de **amostragem**
> do traco periodico, nao evidencia de decodificacao errada. Medindo *todas* as
> instrucoes, o passo 2 aparece. **Traco amostrado nao prova tamanho de
> instrucao.**

**Conclusoes:**

1. **Nao existe bug de ARM/Thumb no emulador.** `apply_tbit` entrega `T=1` onde
   deve e `T=0` onde deve. A auditoria que o marcou como suspeito fica encerrada.
2. **O endereco `0xb0400064/68/6c` do ROADMAP antigo estava CERTO.** Eu o havia
   declarado errado no QW64 — retratado tambem.
3. O Core0 progride por fases: setup -> memset de 5 MB -> **descompressao LZ**.

**Pendente:** ~~`exit=139` com `seconds=45`~~ **[RESOLVIDO no QW78-80: invalidacao de TB nao reentrante em code hook]**

### QW73-QW76 — Core0: o "travamento" e' THROUGHPUT, nao deadlock  **[MEDIDO]**

Retomada apos a retratacao QW70-72, agora sobre o binario correto
(`nand/1.1.2_APPS.bin`).

**QW73 — `0x80000000` esta mapeada; quem chama o memset**

    n=1       lr=0xb0043000  dst_err=0  CTRL_err=0  faixas: b0=1
    n=100000  lr=0x8093bf70  dst_err=0  CTRL_err=0  faixas: 80=80888 b0=19112
    n=500000  lr=0x80a56770  dst_err=0  CTRL_err=0  faixas: 80=480888 b0=19112

`dst_err=0` com controle `CTRL_err=0` em `0xb0043000` (sabidamente mapeada):
a faixa `0x80000000` **esta mapeada**. A hipotese "escrita em regiao nao
mapeada" morre aqui. A faixa `b0` congela em 19112 e a `80` cresce sem parar:
o alvo mudou de buffers pequenos de setup para um buffer grande.

Chamadores (topo da pilha, estaveis): `0xb00049b8` e `0xb00058d0`. Ambos com o
mesmo idioma:

    ldmda r3, {r2, r3}    ; carrega par (inicio, fim)
    sub   r2, r3, r2      ; tamanho = fim - inicio
    add   r2, r2, #1
    ldr   r0, [r5/r6, #8] ; destino
    bl    0xb000af88      ; memset

**QW74 — o tamanho (com uma correcao de instrumento no meio)**

Primeiras chamadas, todas sadias: `4096, 4096, 104, 12, 4, 12`.

> ⚠ **Erro meu:** o filtro era `n<=6 || n%20000==0` e o total nunca chegou a
> 20000, entao vi 6 linhas e reportei **"6 chamadas"** como se fosse o total.
> Era so' o que eu tinha mandado imprimir. Refeito com escala log
> `(n & (n-1))==0`, que cobre qualquer volume: total real **>256**.
> **Filtro mal escolhido mente por omissao.**

Com o instrumento corrigido, o alvo real aparece:

    *** memset GRANDE dst=0x80800000 size=0x00500000 (5242880 bytes) lr=0xb00049b8
    *** memset GRANDE dst=0x80800000 size=0x00500000 (5242880 bytes) lr=0xb00058d0

**5 MB zerados**, pelos dois sitios.

**QW75 — nao ha entrada anomala no laco**

    n=400000  iteracoes=399720  portas: 0xb000afd8=280

Apenas **280 entradas**; todo o resto sao iteracoes normais. Ninguem salta para
dentro do laco: uma unica chamada itera centenas de milhares de vezes.

**O laco esta correto** (`b000af88..b000b004`, memset ARM desenrolado):

    b000afdc  tst   ip, #3        ; Z=1 se ip multiplo de 4
    b000afe0  streq r3, [lr], #4  ; caminho A: 3 stores condicionais
    b000afe8  subeq ip, ip, #4    ;            + ip -= 4
    b000aff0  subne ip, ip, #1    ; caminho B: ip -= 1
    b000aff4  cmp   ip, #0
    b000aff8  str   r3, [lr], #4  ; sempre 1 store
    b000affc  bne   b000afdc

Caminho A zera 4 words e desconta 4; caminho B zera 1 e desconta 1. Coerente,
e termina em `ip==0`.

**QW76 — a conta fecha**

    0x500000 / 4        = 1.310.720 words
    caminho rapido      = 4 words/iteracao
    => ~327.680 iteracoes por chamada de 5 MB

Casa com o observado: 399.720 iteracoes ≈ 1,2 chamadas de 5 MB; e o
`ip=0x132aa4` (1.256.100) que eu tinha achado suspeito e' simplesmente o
contador **descendo** de 1.310.720.

**Conclusao:** o Core0 **nao esta travado nem corrompido** — esta zerando 5 MB,
uma instrucao interpretada por vez, e progride (`insns=3.185.753`). Nao ha bug
para consertar aqui. A pergunta vira orcamento de tempo: quantos segundos para
atravessar o memset e alcancar o proximo marco.

### QW70-QW72 — RETRATACAO: QW64-69 estavam ERRADOS (binario errado)  **[CORRECAO]**

> ⚠ **As secoes QW64-QW66 e QW67-QW69 abaixo estao FACTUALMENTE ERRADAS.**
> Toda a conclusao "Core0 executa Thumb em modo ARM" veio de eu desassemblar
> **o binario errado**. Mantidas como registro do erro; **nao usar como referencia**.

**O erro:** o Core0 (APPS/Iguana) e' carregado de `nand/1.1.2_APPS.bin`. Eu
desassemblei `nand/1.1.2_AMSS.bin` — o binario do **Core1** — com um offset
(`0x00af0000`) que nem corresponde a esse arquivo. Os bytes lidos eram de outro
programa, em outro lugar. Como lixo raramente decodifica em ARM e quase sempre
produz *algo* em Thumb, montei uma narrativa inteira em cima disso.

**O que o arquivo CORRETO mostra** (`1.1.2_APPS.bin`, seg2 `va=0xb0000000 off=0x30000`):

    b000c930  push {r4-r8, sb, sl, fp, lr}
    b000c938  mvn  sp, #0xeb
    b000c93c  svc  #0x1414          <- svc ARM genuino
    b000c940  pop  {r4-r8, sb, sl, fp, pc}

    b000c73c  cmp  r4, #0           <- NAO e' "blx r1"

    b000afdc  tst   ip, #3          <- o "laco": memset ARM desenrolado
    b000afe0  streq r3, [lr], #4
    b000afe8  subeq ip, ip, #4
    b000aff4  cmp   ip, #0
    b000affc  bne   b000afdc

**Cai por terra:**

| Afirmacao publicada | Status |
|---|---|
| "Core0 executa Thumb em modo ARM" | **FALSO** — e' ARM executando ARM |
| "`b000c73c` e' `blx r1` (Thumb)" | **FALSO** — e' `cmp r4,#0` |
| "`mov r8,r8` = nop canonico Thumb" | eram bytes do AMSS lidos fora de lugar |
| "CPSR restaurado sem bit T = causa raiz" | **FALSO** — T=0 esta' CORRETO |
| "premissa do QW41/QW43 esta errada" | **FALSO** — a faixa `b0000000-b0020000` E' ARM |

`T=0` em 4194304/4194304 amostras nunca foi bug: era o valor certo.
Os fixes QW66 e QW71 foram inertes porque **nao havia nada para consertar**.

> **O que me pegou:** o detector do QW71 leu `0xef00` em `pc-2` e respondeu
> `thumb=0`. Eu tratei como "detector falhou" e fui verificar — era o detector
> **certo** contradizendo minha premissa. Divergencia entre instrumento e
> hipotese: desconfie da hipotese primeiro.
>
> **Regra nova:** antes de desassemblar, confirmar de QUAL arquivo o codigo foi
> carregado e por qual `PT_LOAD`. Um offset plausivel num binario errado produz
> desassemble que *parece* coerente.

**O que sobrevive (medido, independe do desassemble):** o Core0 gira em
`b000afdc..b000affc` com passo de 4 bytes. Agora sabemos que e' um **memset real**.

**QW72 — o memset funciona:**

    n=1     ip=0x00000400  lr=0xb0043000    (4KB)
    n=256   ip=0x00000004  lr=0xb0043ff0    terminou
    n=512   ip=0x00000004  lr=0xb0044ff0    nova invocacao

`ip` decresce monotonicamente e conclui. Nao ha' travamento — a funcao e'
**chamada muitas vezes** com alvos diferentes.

Mas as amostras tardias mudam de escala:

    n=32768   ip=0x00132aa4  lr=0x80835570   <- ~5 MB numa chamada
    n=262144  ip=0x00052aa4  lr=0x80bb5570
    n=524288  ip=0x00092aa4  lr=0x80ab5570

`lr` aponta para `0x80000000+`, faixa que **nao aparece em nenhum `uc_mem_map`**
que auditei (`b0000000`, `f0000000`, `f4000000`, `ffff0000`, `10000000`).

**Nao concluo causa raiz aqui.** Falta medir: se `0x80000000` esta' mapeada,
quem chama o memset (o return address, nao o `lr` do laco — aqui `lr` e' o
ponteiro de destino), e se as invocacoes crescem sem limite.

### QW67-QW69 — Core0: o bit T se perde no RETORNO do kernel para o Iguana  **[❌ RETRATADO — ver QW70-72: binario errado]**

Continuacao do QW64-66. Duas hipoteses minhas refutadas antes de achar o ponto real.

**Hipotese A (minha, da mensagem anterior) — `e_entry` com LSB=1 perdido: REFUTADA.**

    e_entry do APPS = 0x10000000, LSB=0

O dump em `0x10000000` desassembla como ARM valido e idiomatico — boot de
primeiro estagio legitimo, exatamente como deve ser:

    b 0x10000014 ; msr cpsr_fc,#0xd3 ; mcr p15,0,r0,c1,c0,0 (SCTLR)
    ldr sp,[pc,#0x1c] ; add r0,pc,#4 ; bl 0x100168b0 ; b 0x10000038

Nao ha' bit perdido. Nada a corrigir ali.

**Hipotese B — propagacao do T entre slices: JA' ESTA' CORRETA.** O codigo
(linha ~1600) reconstitui o T do CPSR: `start_addr0 = core0_.entry | ((cpsr0 >> 5) & 1u)`.
Se o CPSR chega com T=0, ele propaga T=0 fielmente. Nao e' o defeito.

**Onde esta' de fato — cadeia completa de saltos do Core0 (so' 3 transicoes):**

| Faixa | PC | CPSR | T |
|---|---|---|---|
| `00 -> 10` | `0x10000000` | `0x400001d3` | 0 | entry ARM, correto |
| `10 -> f0` | `0xf00124a4` | `0x800001d3` | 0 | entra no kernel |
| `f0 -> b0` | `0xb000c73c` | `0x60000010` | 0 | **volta ao Iguana — AQUI** |

Desassemble de `0xb000c73c`:

    ARM  : capstone nao produz UMA instrucao valida
    THUMB: 0xb000c73a  adds r0, r6, #0
           0xb000c73c  blx  r1          <<< nosso PC
           0xb000c73e  mov  r8, r8      (nop canonico Thumb)

O padrao `ldr r0,[r6] / ldr r1,[r0,#4] / adds r0,r6,#0 / blx r1` e' despacho de
metodo virtual. Os `mov r8,r8` sao assinatura inequivoca de Thumb.

> **Causa raiz:** o kernel devolve o controle ao Iguana em `0xb000c73c` com
> `cpsr=0x60000010` (modo User, **T=0**), mas o destino e' codigo **Thumb**.
> O CPSR do usuario e' restaurado **sem o bit T**. Esperado: `0x60000030`.

Consistente com todas as medicoes anteriores: T=1 nunca aparece em 4M amostras,
passo de 4 bytes em codigo Thumb, e o "laco" `b000afdc..affc` e' lixo decodificado.

**Fix ainda NAO aplicado — de proposito.** A licao do QW66 (patch inerte que
teria virado falso "causa raiz corrigida") exige controle negativo antes de
qualquer correcao. O fix deve restaurar o T no retorno kernel->usuario e ser
comparado no mesmo binario contra o comportamento atual.


### QW64-QW66 — Core0 executa codigo Thumb em modo ARM  **[❌ RETRATADO — ver QW70-72: binario errado]**

**O endereco no ROADMAP estava errado.** O Core0 nao esta' no laco
`0xb0400064/68/6c`. O perfil por janela mostra a regiao real:

| Janela (1M insns) | PCs distintos | Faixa |
|---|---|---|
| 1 | 7422 | `10000000..f001704c` |
| 2 | **9** | `b000afdc..b000affc` |
| 3 | **9** | `b000afdc..b000affc` |
| 4 | 527 | `b0003974..b000d718` |

**Descoberta central — o codigo e' Thumb, executado como ARM:**

    ARM   (desassemble): stmdals / andlt / adcsmi / <?>        -> lixo
    THUMB (desassemble): bl / cmp / bne / pop {r4-r7,pc} / ldr -> coerente

Medicao direta do `CPSR.T` em `0xb0000000+`:

    THUMB (T=1) =       0
    ARM   (T=0) = 4194304      <- 100%

Confirmacao independente: o hook nunca ve `0xb000afda` (2-alinhado) mas ve
`b000afdc`/`b000aff4` — **passo de 4 bytes**, assinatura de modo ARM.

**Hipotese testada e REFUTADA:** `apply_tbit` (linha ~3069) classifica o modo por
**faixa de PC**, e `[b0000000,b0020000)` e' declarada "stub ARM" embora contenha
codigo Thumb do Iguana. Item que a auditoria ja' listava como suspeito. Troquei a
fonte de verdade para o `CPSR.T` do guest e comparei **no mesmo binario**:

    com fix    : T=1=0  T=0=4194304
    controle   : T=1=0  T=0=4194304      (identico — fix sem efeito)

> **O controle negativo salvou a conclusao.** Se eu tivesse commitado o fix sem
> comparar, teria reportado "causa raiz corrigida" com base num patch inerte.

**Reinterpretacao:** T=1 nunca aparece em 4M de amostras — nem antes nem depois de
syscalls. Logo o problema **nao** e' a retomada limpar o bit T; e' que o Core0
**nunca entrou em Thumb desde o inicio**. A causa esta' no **ponto de entrada**
do Core0 (como o PC inicial e' definido), nao em `apply_tbit`.

Fix revertido (inerte). `apply_tbit` continua suspeito por classificar por faixa,
mas **nao** e' o que prende o Core0.

**Proximo passo:** auditar como o PC inicial do Core0 e' setado e se algum `BX`
deveria ter feito a transicao ARM->Thumb.


### QW60-QW63 — Estado real do Core1 apos o boot do kernel  **[MEDIDO — sem bug fatal encontrado]**

Investigacao para responder "ate' onde o Core1 chega?". Resultado: **ele nao esta'
travado**, mas o metodo que usei primeiro deu resposta errada duas vezes. Registro
os dois erros porque ambos sao armadilhas reutilizaveis.

**Medicoes (interpretador puro, sem Dynarmic):**

| O que | Valor |
|---|---|
| Perfil por janela de 2M insns | janela 1: 3516 PCs distintos; janelas 2-6: **772, identicas** |
| Topo do walker `f0003df4` | `r6=0xf400f800` constante, `repetidos=32681 mudou=87` (99.7%) |
| Janela de page table `0xf4000000` | mapeada, `err=0`, valor **0x00000000** |
| Controle positivo `0xf000f800` | mapeada, `err=0`, valor `0x202c7825` (heap real) |
| Escritas do Core1 | total=131072, `pgtable(f4)=889`, `heap(f0)=125938` |
| Escrita em `0xf400f800` | 5x, `pc=0xf000498c`, **val=0x0**, size=4 |
| `L4_MapControl` | 755 total, 173 VAs distintos |

**`0xf000498c` desassemblado:**

    f0004988  mov r1, #0
    f000498c  str r1, [r6]     <- zera a entrada de propósito

E' **invalidacao legitima** de entrada de page table. O zero que o walker le
e' o zero que o proprio kernel escreveu.

**ERRO 1 (meu) — "escrita perdida no alias".** Suspeitei que `0xf4000000` e
`0xf0000000` fossem `uc_mem_map` independentes e que as escritas de page table
se perdessem, igual ao bloqueio da SMEM. **Refutado pela medicao**: ha' 889
escritas na janela e a entrada lida e' escrita pelo proprio kernel com valor 0.

**ERRO 2 (meu) — "laco fechado".** Janelas identicas (772 PCs, contagens iguais)
me levaram a concluir laco travado. **Refutado**: ha' **278 `L4_MapControl`
concluidas DEPOIS** do ponto de estabilizacao (478 antes), e o Core0 avanca
(6844226 -> 7044226 insns).

> **Licao de metodo:** contar PCs distintos mede **diversidade de codigo**, nao
> **progresso**. Codigo de servico (walker, alocador) tem baixa diversidade por
> natureza e roda indefinidamente sem estar travado. O discriminador correto e'
> o **efeito externo** (syscalls concluidas), nunca o perfil interno.
> Isto tambem contradiz minha leitura anterior de que o Core1 "progride no setup":
> ele progride, mas em regime estacionario processando uma fila.

**Achado aproveitavel:** ~21783 iteracoes de walker por 2M instrucoes (~1% do
tempo) reconsultando **a mesma entrada invalidada**. Ineficiencia real, nao bug
fatal — candidato a otimizacao, nao a correcao de boot.

**Pendente honesto:** nao ha' referencia para dizer se 755 `MapControl` com 173
VAs distintos e' o esperado ou se ha' reprocessamento. Comparar com o corpus OKL4.


### QW57 — `Assertion trace_buffer failed` (tracebuffer.cc:116) **[RESOLVIDO — era artefato do QW56]**

Nao era fronteira nova: era **consequencia da guarda assimetrica do QW56**, que
destruia o pool do `kmem`. Com a guarda simetrica (QW58) o assert desaparece.
Ver QW58 para a medicao.

### QW58 — [AUTO-CORRECAO] a guarda do QW56 era assimetrica e quebrou o pool do kmem

**O commit `6760e3b` estava errado.** A guarda foi aplicada so' no caminho de
LEITURA (`c1_heap_read_hook`), mas o caminho de ESCRITA (`c1_mem_hook`) continuou
gravando no shadow em toda a janela. Resultado: acima de `REX_KERNEL_FILESZ` a
escrita ia para o shadow e a leitura vinha da RAM crua do Unicorn — **escrita e
leitura em memorias diferentes**, e todo o `.bss` virou lixo.

**Medicao** (mesmo binario, unica variavel = a guarda; probe em `f0002b7c`/`f0002c60`,
`b7c` como controle positivo do instrumento):

| config | pool alloc ok | insert (`f0002d80`) | `head` | onde parou |
|---|---|---|---|---|
| sem guarda (pre-`6760e3b`) | 63/64 | 41 | `0x00000000` | panic TCB (thread.cc:1273) |
| guarda ASSIMETRICA (`6760e3b`) | **0/8** | **0** | `0x00000000` | assert tracebuffer.cc:116 |
| guarda SIMETRICA (QW58) | 63/64 | 41 | `0xf000f800` | panic TCB (thread.cc:1273) |

**Correcao**: aplicar a mesma condicao no write path (`off < REX_KERNEL_FILESZ`),
mantendo escrita e leitura coerentes.

**Conclusao que invalida o QW56**: a guarda e' **neutra** para o panic do TCB. Com
ela, sem ela, ou simetrica, o kernel para no mesmo ponto. O clobber do `.bss` e'
um defeito real do nosso modelo, mas **nao e' a causa do panic do TCB**.

**Licao (gate §7 do MORE_INFO)**: o controle negativo do QW56 estava correto nos
fatos e errado na leitura. "Sem guarda para no panic A; com guarda avanca ate' B"
foi lido como progresso, quando B so' era alcancado porque a estrutura por baixo
tinha sido destruida. **Avancar de panic nao e' progresso** — e' o "contador maior
!= correcao" na sua forma mais convincente.

### QW59 — [ERRO DE INSTRUMENTO] o probe do panic do TCB era falso positivo

O probe imprimia `>>> ponto do panic thread.cc:1273 alcancado` em `pc == 0xf0016bec`.
Desassemblado: `f0016bec` e' o **`beq f0016d14`**, isto e', o TESTE — executado em
todo boot, com ou sem falha. O panic real e' o destino `f0016d14`; o `printf` de
thread.cc:1273 esta' em `f0016bac` (`mov r2,#0x4f0` + `add r2,r2,#9` = 1273).

Medido com o pool sao (guarda simetrica), instrumentando os guardas de verdade:

    [QW59] allocate_tcb RETORNOU r0=0xf000ce1c (tcb)     <- SUCESSO
    [QW59] f000c3a4 RETORNOU r0=0x00000001  => ok        <- 1o guarda passa
    [QW59] f000c240 RETORNOU r0=0x00000001  => ok        <- 2o guarda passa

**Ou seja: nao ha panic do TCB.** `allocate_tcb` devolve um TCB valido e os dois
guardas passam. O "panic" que persegui era o rotulo mentiroso do meu proprio probe.

**Estado real do Core1** (histograma nao-filtrado, `STATS_TECHNIQUES.md` §1):
o Core1 executa >4M instrucoes e emite **491 `L4_MapControl`** com **140 VAs
distintos** progredindo (`b0e00000`, `b0e01000`, ... `phys=0x10055000`+). Nao esta'
travado — esta' fazendo setup de espaco de enderecamento. A ultima linha do console
(`creating root server`) e' apenas o ultimo `printf`, nao o ponto de parada.

**Aplicacao do `STATS_TECHNIQUES.md` §1**: o histograma que eu usava filtrava
`pc >= 0xf0000000 && pc < 0xf0020000` ANTES de contar — poder zero contra a
hipotese "o hook e' cego fora da faixa". Trocado por `hi[pc >> 28]` cobrindo os
16 nibbles + janela fina. Foi o que revelou o trafego real.

### QW97-QW98 — Auditoria cruzada Zeebx/LLE e primeiras correções **[VERIFICADO]**

Auditoria integral das bases atuais: LLE (183 fontes, 30.473 linhas C/C++/Python)
e Zeebx `64844bd` (47 fontes, 30.627 linhas Rust/Python). Zeebx está sincronizado
com `origin/master`; `cargo test --all-targets` passou **273/273** testes (4
ignorados). É GPL-2.0-only: usar apenas contratos observáveis e fatos de RE; não
copiar implementação para o LLE.

**Achados críticos confirmados no LLE:**

1. `handle_map_control` ignora `space_id`; o único address space causa a colisão
   CRT QW88. O contexto de thread também perde R0-R12/CPSR e não guarda o SID.
2. A SMEM era mapeada duas vezes com backing anônimo independente; nenhum protocolo
   Core0↔Core1 poderia convergir. Corrigido em QW97 com um único buffer host-backed
   mapeado por `uc_mem_map_ptr` nos dois engines e teste bidirecional real.
3. O parser MIF procurava qualquer `0x010xxxxx` byte a byte, aceitava falsos
   positivos e rejeitava ClassIDs fora dessa faixa. Corrigido em QW98: magic
   `0x0011`, tabela `n+1` de limites, seção de applet de 20 bytes e campos-zero
   estruturais; teste inclui DD `0x0102f789`, ClassID fora da faixa, decoy,
   não-applet e truncamento.
4. O loader ainda aceita apenas ELF; Double Dragon é MOD ARM cru. O dispatch ainda
   chama o handler fixo da Z-Wheel `0x10532344`; isso continua barrado como evidência.
5. VIC/GPT ainda são RAM/polling sem entrada de exceção IRQ; ProcComm ainda dá ACK
   sintético. Corrigir após isolamento de espaços/contextos.
6. Bugs adicionais: revogação `rwx=0` vira leitura em `zeebo_l4_mmu.h`; writes JIT
   de 8/16 bits perdem store unmapped (não atacar enquanto o foco for interpretador);
   payload SMD >0x380 colide com o queue node; loader ELF/relocator requer hardening.

**Otimização QW97:** `getenv()` saiu do hook por-instrução dos dois cores e passou a
ser cacheado uma vez. Em três execuções de 3 s, a mediana subiu de 710 para 1000
slices (**+40,8%**); no gate longo de 45 s, Core0 chegou a **187.244.226** instruções
contra mediana anterior de **157.410.000** (**+19,0%**), interpretador puro, exit 0.
O próximo hotspot medido é a releitura de opcode por instrução no slide-detector do
Core1; deve ser tornado opt-in ou migrado para hook de bloco sem perder o controle
positivo do detector.

### QW99 — Plano crítico em quatro partes **[AUTORITATIVO; substitui QW39, QW97-QW98 e os planos DD anteriores]**

Baseline auditada em `aa7a914` (`3e3e563` = baseline de código anterior ao plano), com
`make -C tools/cpp check` verde. O objetivo permanece **Double Dragon com imagem, input
e som produzidos pelo guest**. Este plano separa modelo unitário, integração
guest-visible, efeito observado no boot e marco comercial; um nível não pode ser
promovido ao seguinte por contador, log, payload injetado, frame host ou áudio sintético.

#### Parte 1 — Base confiável e arquitetura testável (P0)

Objetivo: impedir gates verdes sobre artefatos antigos. Esta parte não autoriza uma
refatoração ampla antes de mover a fronteira de boot.

- [x] Remover do índice os binários ignorados `tools/cpp/{zeebo_boot,zeebo_elf,
  zeebo_harness,zeebo_kernel_boot,zeebo_partition}` e os três
  `tools/__pycache__/*.pyc`; estender `test_clean_hygiene.py` para reprovar qualquer
  saída de `git ls-files -ci --exclude-standard`. (Concluído em `2237b8b`/`aab47db`).
- [x] Em `tools/cpp/Makefile`, separar `check-fast`, `check-firmware` e `check-full`; o
  gate de firmware deve **falhar**, não virar verde, quando a NAND necessária estiver
  ausente. Exibir totais PASS/FAIL/SKIP e derivar build/check/clean de listas únicas. (Concluído em `2237b8b`).
- [x] **DD0 — gate honesto de módulo:** arquivo inválido falha antes da execução;
  `test-roms-external` deixa de emitir PASS de jogo; Reksio/DD permanecem `loaded_only`
  até um PC pertencente ao módulo realmente executar. Integrado em `7045164`, com
  mutante PASS-on-load vermelho.
- [x] Inventariar e encerrar worktrees/branches `agent/qw*` já integrados; nenhum
  resultado durável deve existir apenas em `/tmp`. Todos os commits e diagnósticos
  foram integrados na árvore principal (`3fd982a`, `db94caa`, `77b888c`, `92f927e`, `2a5f59e`).
- [x] Extrair `L4KernelShim/CoreScheduler`, `PeripheralBus`, `IntercoreFabric` e remover
  estado estático **somente conforme o caminho tocado exigir uma seam testável**. Concluído
  gradualmente conforme as seams foram necessárias (`zeebo_peripheral_bus.h` integrado em `f7938c5`,
  `zeebo_l4_mmu.h` em `3fd982a`). Não bloquear o scatterload nem DD1a por uma decomposição completa do god object.

Gate P0: clone limpo recompila tudo; nenhum arquivo ignorado está rastreado; NAND
ausente falha no tier correto; módulo inválido não recebe PASS; worktree principal
contém somente mudanças deliberadas.

#### Parte 2 — Periféricos e comunicação realmente alcançáveis pelo guest (P1, em paralelo)

Estado dos dez bugs: 1/2 (`cd7da2f`), 3 (`68a477d`), 4 (`3808767`/`b4a4d35`),
7 (`03b29c7`), 8 (`3e3e563`), 9 (`19a1a4b8`) e 10 (`18caa09`/`350984c`) estão
integrados com controles negativos. **5 e 6 foram reabertos pela auditoria e
corrigidos no caminho de produção**: Bug 6 em `128d80d`; Bug 5 em `0a88d98` +
`f7938c5`. O gate de acesso vivo pelo firmware continua separado abaixo. Os números
deste painel são os do backlog de dez bugs, não as subseções de
`AUDIT_2026-09-10.md`.

- [x] Bug 5: ligar leituras/escritas MMIO do guest **em `zeebo_lle_main`** aos modelos
  VIC/GPT por um decoder compartilhado com os testes. Escritas em ENABLE/MATCH/
  INTENABLE/ACK/EOI alteram o modelo. `f7938c5` fecha a divergência observada: página
  de timers em `0xc5000000`, sub-banco GPT em `0xc5000100`, COUNT em
  `0xc5000108` e ACK em `0xc500010c`; o mutante do offset antigo fica vermelho.
- [x] Modelar `pending`, `enabled` e `in_service`; não redeliver a mesma IRQ antes do
  EOI nem sobrescrever `LR_irq/SPSR_irq` (`0a88d98`/`f7938c5`).
- [x] Substituir `ticks = instruções Core0 + instruções Core1` por tempo virtual
  determinístico independente dos dois cores (`tick_slice()`).
- [x] Bug 6: resolver a colisão `PCOM_CMD_RESET_MODEM == PCOM_CMD_DONE == 1` usando
  estado shadow real (`pending_cmd/has_pending`) ou estado equivalente. Em `128d80d`,
  o Core1 é o único produtor da conclusão, fora de `UC_HOOK_MEM_WRITE` e após o STR
  guest; o mutante síncrono fica vermelho.
- [x] Criar testes de integração pelo caminho de produção: escrita guest MMIO → GPT →
  VIC → exceção → ACK/EOI; e Core0 escreve RESET_MODEM → SMEM → Core1 atende → Core0
  observa DONE/status. Os testes exercitam os adaptadores usados por
  `zeebo_lle_main`, não cópias isoladas.

Gate P1: além do teste unitário e do mutante vermelho, o firmware executa os acessos
MMIO/SMEM reais e a variável defeituosa muda. Modelo não conectado = item aberto.
VIC/GPT/ProcComm podem avançar em paralelo, mas **não são declarados bloqueadores do
boot** até um trace vivo mostrar o guest esperando por IRQ ou leitura cross-core.

#### Parte 3 — Fechar o boot orgânico e o scatterload (P2; fronteira imediata)

Os fixes reais de SID/contexto (`cd7da2f`) não destravaram o boot longo: o Core0
continua em `0xb0400064..0xb040006c`. Portanto, “um único address space” era defeito
real, mas não explicação suficiente para o estado atual. QW49 e
`notes/core1_boot_estado_real.md` ainda descrevem panic de TCB, enquanto QW59 registra
Core1 avançando por MapControl; essa contradição deve ser resolvida, não herdada.

- [x] Instrumentar **agora**, em cada entrada de `0xb0400000`: TID, SID ativo, SP/LR,
  backing físico e hash dos 252 bytes em `0xb04151a4`, antes/depois da ativação do SID.
  Medido em `7588e3e`: a segunda entrada usa SID `0x8000c001`, mas a página fonte foi
  registrada apenas em `0x80000100`; o decoder recebia bytes errados e o tamanho
  underflowava. `1ec165c` demonstrou uma correção provisória, ainda sob o gate abaixo.
- [x] Provar que `SpaceManager` troca o conteúdo executado, inclusive páginas
  registradas antes de a task receber SID; não aceitar somente LUT paralela ou teste
  sintético. `3fd982a` substitui o vínculo Pager→SID cru por Pager thread→thread_space,
  exercita o caminho real `activate()` sob Unicorn, preserva/restaura mappings estáticos
  entre trocas e mantém um mutante sem o vínculo em RED. O avanço real obtido por
  `1ec165c` permanece a testemunha externa do scatterload.
- [x] Reestabelecer a fronteira real do Core1 com trace não filtrado e controle
  negativo; reconciliar QW49/QW59 e atualizar `notes/core1_boot_estado_real.md`.
  QW99 confirmou ~272,5M instruções e zero hits nos endereços de panic; QW49 fica
  explicitamente histórico.
- [x] Aplicar janelas e histogramas não filtrados de `STATS_TECHNIQUES.md`; progresso
  exige efeito externo novo, não instruções/slices maiores. Aplicado no rastreador
  de scatterload (`7588e3e`), nas contagens de instruções do Core1 (`qw99-scatterload-core1-measurement.md`)
  e na investigação exata de proveniência de registradores/memória em `notes/boot-investigation/pc14-alias-vs-state-separation.md`.
- [ ] Levar o fluxo sem restauração host, salto forçado ou dispatch fixo até
  AEECShell/AppMgr e registrar a cadeia IPC/naming/quartz/AMSS que realizou a transição.
- [x] Manter Dynarmic fora deste gate; primeiro fechar o comportamento no intérprete.
  Interpretação pura mantida em todos os testes e execuções do boot.

Gate P2: AppMgr/AEECShell alcançado organicamente em execuções repetíveis, com
controle negativo e sem patches barrados. STOP: se somente instruções/slices crescerem,
não empilhar outro patch de PC/registrador. O marco é mudança de fronteira observável.

#### Parte 4 — Vertical slice do Double Dragon (P3)

DD0 está em P0. DD1a–DD3 podem avançar **em paralelo** como diagnóstico assistido,
sempre rotulados `hybrid/assisted`; não fecham boot orgânico nem o marco B.

- [x] **DD1a — primeiro PC assistido:** validar pacote, MIF/App ID/CLSID e executar o
  entry ARM cru de `ddragonz.mod` sob orçamento, registrando PC dentro do módulo.
  Resolução de entry cru e dispatch por módulo já existem (`68a477d` e sucessores);
  não reimplementar o falso gap “ELF-only” nem usar `0x10532344`. Concluído em `c86e4cb`/`542f5ce`,
  com 23 instruções executadas sob helper assistido. Integrado no Makefile sob target
  `test-dd1a` com mutação negativa que reprova fabricação de entrada.
- [ ] **DD1-runtime — objetos/imports:** resolver VAs reais de
  `ishell_create_va/aeemod_load_va/aeeclscreate_va` (VAs de referência de segmento 11 do APPS
  localizadas: ISHELL_CreateInstance @ `0x105c7fb4`, AEEAppletNew @ `0x105322f2`);
  obter IShell vivo, aplicar RW/ZI/relocações/imports/GOT e provar
  `CreateInstance(0x0102F789) → objeto → HandleEvent`. Probe assistido em `test_dd1a_diag`
  provou execução completa do `AEEMod_Load` (73 instruções reais com static_base `AEEHelperFuncs`
  suprido em `LB-4`, mock de MALLOC @ `+0x68` e `pIShell->AddRef`, instanciando a vtable
  do IModule com AddRef=`0x12002064`, Release=`0x120020ac`, CreateInstance=`0x12002078`,
  FreeResources=`0x120020a8`, retornando limpo em `0x12000030: bx lr` com `r0=0`).
  Primeiro PC sem sobreviver ao primeiro import é apenas DD1a.
- [ ] **DD1b — primeiro PC orgânico:** repetir DD1a/runtime pelo boot NAND, sem restore,
  salto, handler ou retorno forçado. Exigir cadeia IPC/naming/quartz/AMSS registrada.
- [ ] **DD2 — VFS:** primeiro identificar o limite guest `IFileMgr/OEMFS`; então oferecer
  `open/read/seek/stat/close`, caminhos e overlay de saves. Provar leitura byte-exata de
  `data.ggz`/`sound.ggz`; retirar asset deve causar falha identificável.
- [ ] **DD3 — loop:** implementar entrega BREW `ISHELL_SetTimer`/callbacks/eventos,
  distinta do GPT/L4; o game loop avança sem retorno forçado.
- [ ] **DD4 — frame:** primeira imagem escrita por comandos/objetos do jogo. Clear azul,
  padrão sintético, soma de pixels ou harness Z-Wheel não contam.
- [ ] **DD5 — input:** evento do controle altera estado observável do jogo.
- [ ] **DD6 — áudio e jogabilidade:** bloqueado até liberação explícita do freeze de
  `tools/cpp/qdsp5/`. Depois, provar PCM originado pelo guest e sessão de cinco minutos
  com imagem, input e som. ACK QDSP5, SDL aberto ou tom sintético não contam.

Gate final: evidência encadeada `guest → modelo → efeito no boot/jogo`, com origem do
frame e do PCM identificada. Marcos B (boot orgânico), L (loaded/instanced) e I
(primeiro PC) são independentes; resultado assistido pode fechar diagnóstico, nunca B.
Não priorizar outros jogos, expansão do rasterizador, Dynarmic ou QDSP5 especulativo
antes de DD1a–DD3.

**Ordem corrigida:** higiene P0 mínima; medir scatterload/Core1 imediatamente; bugs 5/6
e DD1a–DD3 em paralelo. Convergir depois em boot orgânico/DD1b, frame/input e, somente
após o stop/go do freeze QDSP5, áudio. Refatoração estrutural ocorre conforme necessária
para testes, não como frente infinita.
