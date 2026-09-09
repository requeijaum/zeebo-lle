# Zeebo LLE Emulator — ROADMAP (rev 2026-09-08, QW20-QW22 + causas raiz do Passo 13)

Low-level emulation of the Zeebo: boot the REAL firmware from the NAND dump on an
emulated Qualcomm MSM7201A (ARM11 apps core + ARM9 modem coprocessor + QDSP5), no HLE of BREW.
Esta revisão consolida o fechamento do lote QW20-QW22 e a investigação do Passo 13 com o
código-fonte OKL4 2.1.1 como referência (`refs/okl4-2.1.1-fix7/`, mapa completo em
`notes/boot-investigation/okl4-source-reference-map.md`):
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
- [ ] Compatibilidade restante: clipping homogêneo do near-plane e interpolação perspectiva (QW10/QW11 em execução), além de `GL_OES_draw_texture`.
- [ ] Caminhos guest reais: observar o retorno do `eglGetProcAddress` do firmware e registrar apenas o VA vivo; resolver `IEGLSurfaceManip` somente após QueryInterface/objeto vivo. Não usar trampolim, string ou vtable sintética do Zeebx.

### Fase 10: Subsistema QDSP5 (Áudio e Multimídia) — acoplamento ONCRPC e streaming
- [x] Comando-plane mapeado (FINDINGS 2zz–3c): dispatcher `0x16e8cba0`, 4 task engines (VOICEPROC, VFE, JPEG, AUDPP), enfileiramento SMD/ONCRPC `0x17571748`, packet frame (+0x20 proc, +0x80 payload).
- [x] `UnifiedAudioSink` disponível (Fase 8) como mixer PCM multi-stream.
- [x] **Acoplamento oficial do `Qdsp5Dispatcher` ao `UnifiedSMDBridge`**: IDs oficiais `prog::AUDMGR` (`0x30000013`) e `prog::ADSPRTOSATOM` (`0x3000000a`) substituindo o ID provisório `0x30000060`.
- [x] **Hook de consumo e retorno RPC**: captura em `0x16e8cb96`/`0x16e8cba0` alimenta `qdsp_disp_->feed_raw` com memória guest Core 0 (`guest.read`). Conclusão aciona respostas nos canais de retorno `0x31000013` (`AUDMGR_CB`) e `0x3000000b` (`ADSPRTOSMTOA`) via `on_completion`.
- [ ] Backend ao vivo `SDL_OpenAudioDevice` no `UnifiedAudioSink` para streaming contínuo durante o loop de emulação além dos testes de dump WAV.
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
  - A investigação causal consolidou: o travamento inicial era mascarado pela corrupção de registradores salvos na pilha (`L4_KernelInterface`, corrigido em `4224919`) e pelo misload de firmware do CLI (`d137813`). Com isso saneado, o Core 0 executa ~1.47M de instruções reais do Iguana, processa 96 pools de 1MB e trava em `0xb000d708` (`size_log2=56` / descritores whole-space); `bi_execute` e `0xb000aa94` continuam não alcançados.
  - Gate pendente: harness com guest vivo que observe MRs efetivamente transformados e avanço por bytes/endereços até `bi_execute`; instruction-count não é critério de sucesso.
- [x] **Passo 7: Integração VFS EFS2 com Iguana / BREW Loader e Catálogo de Applets (Concluído `f1b03fa` e `645f332`)**:
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
- [x] **Passo 10: Despacho Automático AEECShell / Ciclo de Vida Z-Wheel (274755) (Concluído `fccca5e`)**:
  - Descoberto que o payload de 64 KiB de `274755` (@0x3a92000, FNV-1a `0x544a6f30`) são metadados de gnode do VFS com assinatura `"274755"` e referências a assets (`slidemodel.qxm`), enquanto o código executável do ZeeboApp reside embutido em `0:APPS` no manipulador Thumb `@0x10532344`.
  - Implementado `ZeeboLLESystem::dispatch_zwheel_app_start()`: instancia scratch applet + vtable gráfica (`0x28`), despacha `EVT_APP_START` (`0x1f96`) sob Unicorn ao manipulador pré-mapeado com retorno real `r0 = 1` (sucesso) e roteia chamada para o `SoftRasterizer`, reproduzindo frame RGB565 com soma `2013081600`.
  - Integrado à CLI `--efs2-run=274755` e adicionado o teste automatizado `test-efs2-zwheel` no Makefile (agora 8 alvos de CI 100% verdes).
- [x] **Passo 11: Loop Interativo de Eventos Z-Wheel e Integração de Entrada Contínua (Concluído `79ec1eb`)**:
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
  - **Execução comprovada:** somente Z-Wheel/274755 completa `EVT_APP_START`, retorna `r0=1` e gera `pixel_sum=2013081600`.
  - **Carga comprovada, execução ainda não:** `reksio.mod` e `tectoy.mod` são `loaded_only`; bytes injetados e progresso genérico do Core 0 não contam como execução do applet.
  - Gate atual: 22/22 asserções; `pass` exige milestone específico por PC/retorno/bytes/pixels e `vram_blank is False`.
- [ ] **Passo 13: Execução do BootInfo (`bi_execute`) e Transição para Servidores Iguana (Naming/Pager)**:
  - Parser host-only comprovado contra a cópia `1.1.2_APPS.bin`: BootInfo no offset `0x57000`, magic `0x1960021d`, 10 `BI_TAG_VIRT_POOLS` e 5 `BI_TAG_PHYS_POOLS`; mutações dos bytes alteram/rejeitam o parse como esperado.
  - Saneamento de pilha ABI no `L4_KernelInterface` (`4224919`), parser posicional CLI (`d137813`), correção de PC-resume QW17 (`a88a9bd`) e correção do stub MapControl QW19 (`475ee1c`) — o `mempool_init` atravessa os 96 blocos de 1 MiB no boot real.
  - Bloqueio atual (QW26): a função de criação de thread (0xb0007360+, casável com `thread_create`) chama `ThreadControl` (0xb000c798) e `SpaceControl` (0xb000c944) — stubs com frame pós-svc (`pop`) que caem no `else` do handler (retomada via `lr`, pulando o epílogo) → corrompe callee-saved → panic `SpaceControl != 1` (r3=0xf4/linha 244). Fix = retomar 0x08/0x18 em `pc`+`SP=ip` (padrão QW19).
  - Experimento: com QW23+QW24 o boot alcança `bi_execute` (0xb00001fc, r0=0xb0d00000), aplica 98+ maps físicos reais (0 WRITE_PROT, 0 whole-space espúrio) e avança até o thread_create.
  - Próximo gate: QW26; depois re-observar rumo a `extensions_init@0xb00017b8`/`iguana_server_loop@0xb000aa94` e ao primeiro server, sem forçar registradores.
- [ ] **Passo 14: Shims de IPC, Threading e Handoff para o BREW AppMgr**:
  - Emulação ou despacho honesto de syscalls do OKL4: `L4_ThreadControl` (`0x0c`), `L4_Ipc` (`0x00`), `L4_ExchangeRegisters` (`0x10`).
  - Handoff para o processo de espaço de usuário do `AEECShell` / BREW em `0x10137000` / `0x10c874f4`.

---

## Próximos Passos Priorizados

### P0 — Cadeia crítica de boot real

1. **Passo 13 — BootInfo/`bi_execute` (CONCLUÍDO)**
   - Provado e atravessado por execução real: BootInfo @ file offset `0x57000`, 10 `VIRT_POOLS` e 5 `PHYS_POOLS`.
   - `bi_execute` concluído com sucesso (`r0=0`), `extensions_init` executado, 756 chamadas `L4_MapControl` aplicadas (318 blocos `[aliased]` na RAM), Core 0 entrou no `iguana_server_loop` em `0xb000aa94` e ultrapassou 8,27 milhões de instruções orgânicas.
2. **Passo 14 — Despacho IPC no Iguana Server Loop & Boot do BREW AppMgr** (bloqueio atual = QW30)
   - O `iguana_server_loop` (`0xb000aa94`) aguarda IPC no laço `bl 0xb000c800` (`L4_Ipc` wait em `0xb000c834`).
   - Tags de threads registradas no BootInfo identificam os alvos a serem despachados:
     - `ig_naming` (VA `0xb0100000`, tag 7, ref 6)
     - `quartz_servers` (VA `0xb0300000`, tag 7, ref 13)
     - `AMSS` (VA `0x10137000`, tag 7, ref 23)
   - QW28 (estrutura e tipos MsgTag) e QW29 (tabela de threads e captura via ExchangeRegisters) foram CONCLUÍDOS.
   - Bloqueio atual = QW30: Chaveamento cooperativo em `L4_Ipc` / `ThreadSwitch` para despachar threads filhas dos servidores e entregar requisições de registro de interfaces/memsections.
   - Handoff para o processo de espaço de usuário do `AEECShell` / BREW em `0x10137000` / `0x10c874f4`.
   - Gate final: Inicialização do launcher BREW AppMgr (`ZeeboApp`), Z-Wheel preview/fábrica e execução de applets de jogos (ex: Double Dragon).

### Quick wins independentes

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
| QW18 | **concluído** | Eliminação de escritas legadas em `sp+0/4/8` no handler de interrupção 0xb4 de `zeebo_lle_main.cpp:2223` | baixo | Fechado por análise causal sem alteração de código: trap 0xb000c738 intercepta antes e SP codifica trap-id em região segura de interrupção |
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
| QW30 | **em andamento** | Scheduler Cooperativo L4 (Chaveamento de Contexto no `L4_Ipc`/`ThreadSwitch`) | médio | comutar execução para as threads dos servidores (`ig_naming`, `quartz_servers`, `AMSS`) quando a thread atual ceder no IPC wait (`pick_next_thread` em `zeebo_l4_thread.h`) |

### P1 — Infraestrutura após o Passo 13

1. **Checkpoint v3 de máquina completa**
   - Primeiro vertical slice: CPU/memória + MMU + IRQ/timers, com validação integral antes da restauração; depois GPU/GL, EFS/VFS e filas/eventos.
   - Gate: save→run→restore→rerun produz os mesmos bytes, pixels, registradores e eventos.
2. **Tempo híbrido ARM11/ARM9/periféricos**
   - Contador relativo/dívida entre os dois cores; sync-on-access nas janelas IPC/SMD; deadlines absolutos para timers, IRQ, GPU e futuro QDSP5.
   - Gate: variar quantum sem alterar a sequência observável de bytes/eventos nos harnesses.
3. **JSON-RPC tipado sem quebrar NDJSON**
   - Envelope `id/method/params/result/error` e notificações assíncronas de break/watch/probe; compatibilidade temporária com comandos atuais.
4. **GDB RSP ARM11**
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
- **Frente A — cadeia crítica (Passo 13):** QW17/QW19 destravaram o `mempool_init`; o bloqueio atual tem 2 causas raiz casadas com o fonte OKL4 2.1.1: `KIP[0xc4]` zerado (panic do `thread_init`) e decode do `PhysDesc` com gran errada (mapas físicos no-op). Próximos: QW23 (KIP thread_bits) e QW24 (gran 64B do phys) por TDD; o experimento KIP[0xc4]=18 já provou que o boot alcança `bi_execute` e roda código de server.
- **Frente E — GL estrutural:** concluída (QW9-QW11 integrados).
- **Frente de hardening curto:** QW14/QW15/QW16/QW20 concluídos; `--strict-unmapped` permanece opt-in e QDSP5 não foi alterado.
- **Depois do Passo 13:** Passo 14 e P1 na ordem checkpoint → tempo híbrido → JSON-RPC → GDB; só promover objetos, extensões e applets alcançados pelo boot real.
- **QDSP5:** congelado até liberação explícita; executar seus testes, mas não editar `tools/cpp/qdsp5/`.

Gate de toda frente: alvo afetado RED→GREEN, `make check`, `test-bootinfo-real` quando houver afirmação sobre BootInfo, QDSP5 sem alterações, `run_lle_cputests.sh` 12/12, `git diff --check` e clone limpo compilável.
