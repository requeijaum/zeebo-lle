# Zeebo LLE Emulator — ROADMAP (rev 2026-09-07, after session 3c)

Low-level emulation of the Zeebo: boot the REAL firmware from the NAND dump on an
emulated Qualcomm MSM7201A (ARM11 apps core + ARM9 modem coprocessor + QDSP5), no HLE of BREW.
Decided by Rafael 2026-09-06. This revision is grounded in what sessions 2a-3c actually PROVED,
including the live execution past 5M instructions and full reverse-engineering of L4e syscalls,
ONCRPC routing, and QDSP5 hardware accelerator pipelines.

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
- [x] Implementar as 3 frentes no orquestrador `zeebo_lle_main.cpp`: handoff da partição `0:APPS` no APPSBL com salto `bx r2` para `0x10000000`, motor de syscalls L4e abrangente (`L4_Ipc`, `L4_ThreadControl`, `L4_ExchangeRegisters`, `L4_MapControl`, `L4_SpaceControl`) e injeção bidirecional SMD/ONCRPC na fila do AMSS `0x17571748` disparada por campainhas A2M `0xC0100400`.
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
- [x] Executar e validar a bateria completa de 11 testes de CPU de `/home/rafaelfrequiao/projects/zeebo-emulator/testkit/cputests/` sob o núcleo ARM11 (Unicorn/ARM1176): 11/11 PASS (`alu`, `callret`, `condflags`, `controlflow`, `interwork`, `ldmstm`, `loadstore`, `media`, `muldiv`, `shifter`, `thumb2branch`).
- [x] Testar a execução do módulo limpo `zbtest.mod` (construído via SDK oficial BREW) no LLE e mapear o ponto de despacho para `AEEMod_Load`.

### Fase 8: Absorção de Padrões HLE de Alta Relevância (Audio & SaveState Engine)
- [x] Incorporar padrão de Save State Dual-Core (`ZeeboSaveStateManager` em `zeebo_save_state.h`) capturando CPU Unicorn context (`uc_context_save`) + regiões mapeadas de memória física/compartilhada.
- [x] Incorporar mixer de áudio multi-stream (`UnifiedAudioSink` em `zeebo_audio_sink.h`) com controle de canais e vtable HLE/LLE limpa evitando problemas de ciclo de vida e interworking.

### Fase 9: Subsistema Gráfico (Adreno 130 / IGL) — acoplamento LLE e rasterização
- [x] Evidência de firmware (GPU_TODO §13): 3D é offload MPU→QDSP5 atrás de fachada OpenGL ES 1.1 ATI-Imageon; **não há ring buffer PM4 A2xx observável do lado ARM11** → descartar `pm4_adreno.h` como produtor (mantido como referência de estudo §14).
- [x] Esqueleto `IGpuRasterizer` (Citra-style) + 2 backends (software correto + GL host stub) + `rasterizer_factory`, verificado por framebuffer (`gpu_smoke` 3/3: clear, triângulo, pm4-walk).
- [x] Produtor correto `IglHook` interceptando a vtable IGL/IEGL (80/28 slots, ABI ARMADILHA: R0=1º arg real) → `IGpuRasterizer`, verificado por framebuffer (`igl_smoke` 3/3; 40 slots gl* fixados contra gl_hle.cpp).
- [x] **Integração no build do emulador** (`tools/cpp/Makefile`: targets `gpu`/`test-gpu`) + handoff GPU→display provado por pixels em PPM 640x480 RGB565 (`gpu_display_integration`: clear azul 0x001F e vermelho 0xF800), commit `1508115`.
- [x] **Transform fixed-function (mvp+viewport)** implementado no `IglHook` (stacks modelview/projection, slots de matriz reais, aplicação obj→clip→NDC por vértice) + rasterizer respeitando viewport; verificado por PIXEL (`igl_transform_smoke` 2/2: triângulo desenha e `glTranslatex(+0.5)` o desloca 160px — antigo centro fica vazio, novo centro preenchido), commit `fbb094d`. `make test-gpu` = 10/10 PASS.
- [x] **Acoplamento do pipeline gráfico ao `zeebo_lle_main`**: `SoftRasterizer` e `IglHook` instanciados em `ZeeboLLESystem`. Framebuffer dirty do MDDI/Adreno 130 drena `rast_->end_frame()` diretamente para `sink_->update_frame(rgb565_src)` via textura SDL2 640x480 RGB565.
- [x] **Entrada integrada de Gamepad e Teclado**: processamento de eventos `SDL_CONTROLLERBUTTONDOWN`/`UP` no `UnifiedInput` (`KEYPAD_BASE = 0xa9a00000`) para A, B, C, D, direcionais e Home, complementando o teclado.
- [ ] Conectar os ponteiros de despacho global guest (`gpIGL`/`gpIEGL`) da vtable IGL à `GuestMachine` do `IglHook` no momento em que as tarefas BREW subirem.
- [ ] Texturas/ATITC (`glTexImage2D`→upload, `glCompressedTexImage2D`→decode), multitexture+combine/dot3, backend GL host (ubershader).

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
- [x] **Core 0 Iguana User-space Pipeline & Mapeamento de Pool L4_MapControl**:
  - Falso diagnóstico do "gargalo de 4GB / size_log2=32" refutado por execução e RE: o calculador de fpages `0xb000d4dc` gera fpages legítimas de 1MB (`0xb0d00146`), e o comparador de mínimo em `0xb000d6fc` seleciona `size=1MB`.
  - O loop `mempool_init` (`0xb000d5b4`) executa com sucesso 96 iterações de `L4_MapControl`, cobrindo de `0xb0d00000` a `0xb6d00000` (pool físico `0x10000000..0x16000000`).
  - Sequência de boot do Iguana mapeada em `0xb0003424`: avança por `mempool_init`, `0xb00055dc`, `0xb000b1dc`, `0xb0001e80`, `0xb00056c4`, `0xb0004de4`, `0xb00070c8` até `bi_execute` (`0xb00001fc`) e o servidor em `0xb000345c`.
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
- [x] **Estrutura de Arquivos EFS2APPS Mapeada (76.731 Dirents)**:
  - Formato binário comprovado contra o dump `nand/1.1.2.bin`: `0x69 [inode u32][reclen u8][type u8][parent_ref u32][pad 00][name (reclen-5)]`.
  - Inode raiz `0x6064` indexa diretórios de primeiro nível (`"mod"`, `"mif"`, `".efs_private"`, `"err"`, etc.).
  - Módulos de jogos agrupados por transação COW sob seus respectivos pais (ex: `reksio.mod` @`0x32606ef`, inode `0x265e4`, sob diretório pai `0x04abef64`).
- [x] **Core 1 REX MMU Bring-Up (commit `d71d27d`)**:
  - Diagnosticada a causa do congelamento de registradores em `0xf0017718`: instrução `mcr p15, c1, c0, 0` ativando `SCTLR.M=1` (MMU do ARM9).
  - Em modo padrão `UC_TLB_CPU`, o Unicorn tentava consultar page tables não mapeadas, gerando 941 NOPs virtuais até cair nos zeros em `0xf00184cc`.
  - Forçado `uc_ctl_tlb_mode(core1_.uc, UC_TLB_VIRTUAL)` (idêntico ao Core 0), mantendo a janela plana/espelhada e permitindo que o Core 1 execute normalmente além da barreira em `0xdf602d4c`.
- [ ] **Integração de EFS2APPS no VFS Guest / IFILEMGR**:
  - Implementar parser C++ das entradas `0x69` e expor aos despachos do subsistema `IFILEMGR` do ARM11, permitindo resolução de caminhos como `fs:/mod/reksio/reksio.mod`.
- [ ] **Gatilho de Instanciação do ZeeboApp & Acoplamento IGL (Z-Wheel 3D)**:
  - Disparar `EVT_APP_START` (`0x1f96`) em `0x10532344`, fornecendo vtable gráfica conectada ao `IglGuestBridge` no offset `0x28` para capturar a emissão de comandos OpenGL ES 1.1 da Z-Wheel via `SoftRasterizer`.
- [ ] **Core 1 REX Task Scheduler Loop**:
  - Permitir avanço do agendador do modem até o repouso em `rex_wait` (`0x16ef0b02`) para recepção contínua de RPCs de áudio e rede.

---

## Próximos Passos Priorizados (Plano de Ação)

1. **Passo 1 (Harness de Execução do ZeeboApp + Renderização Z-Wheel)**:
   - Construir teste/harness em `tools/cpp/` que inicialize contexto Unicorn com os segmentos de `0:APPS`, configure a estrutura `applet` com ponteiro para a vtable IGL (`IglGuestBridge`) em `applet[0x2c]`, e invoque `0x10532344` com `r1 = 0x1f96` (`EVT_APP_START`).
   - Capturar o primeiro frame 3D desenhado via `SoftRasterizer` e exportar como PPM.

2. **Passo 2 (Parser C++ EFS2APPS para IFILEMGR)**:
   - Codificar `zeebo_efs2_fs.h` implementando o parser de registros `0x69` a partir da partição `0:EFS2APPS` (`0x3220000`).
   - Oferecer métodos de lookup de path (`open("fs:/mod/reksio/reksio.mod")`) e listagem de diretório (`readdir("fs:/mod")`).
   - Validar com teste automatizado (`make test-efs2-fs`).

3. **Passo 3 (Loop de Agendamento do REX no Core 1)**:
   - Acompanhar a execução do Core 1 a partir de `0xdf602d4c` até a entrada do despachante multitarefa REX (`0xf0013b84` / `rex_sched`), garantindo retorno limpo e repouso em `rex_wait`.

---

## Regras de Higiene e Verificação
- **Clean-room Absoluto:** `a1Sim` permanece estritamente como oráculo caixa-preta (proibido descompilar).
- **Integridade da NAND:** Leitura exclusiva na cópia de trabalho; dump original preservado com `chmod a-w`.
- **Validação por Execução Real:** Todo avanço deve ser demonstrado por código executável com commits atômicos e registros auditados em `notes/FINDINGS.md`.
- **Rastreabilidade de build:** todo commit deve deixar o HEAD compilável — nenhum `#include`/alvo de Makefile pode apontar para arquivo não versionado. (Violado em `431461b`/`f51cccd`; corrigido em `0a5abd4`.)

---

## Como acelerar os itens restantes (estratégia de paralelização)

Os 5 itens da Fase 11 têm uma **cadeia crítica** (1→2→3) e dois **trilhos independentes** (4, 5).

Cadeia crítica (destrava boot user-space real):
- **Item 1** é o gargalo raiz. O NOP-slide de Core1 (PC +0x9c40/ciclo) prova que `0x00a00000`
  (e_entry cru) não é o vetor de reset do ARM9. Acelerar por: (a) scanner determinístico do
  preâmbulo de reset (`msr cpsr_c,#0xd3` + `ldr sp`) sobre `1.1.2_AMSS.bin` — já há
  `nand/sig_scan*.py` como base; (b) slide-detector de ~30 linhas no hook de Core1 que
  `uc_emu_stop` ao detectar PC linear por N insns — troca 90s de boot cego por feedback
  imediato; (c) cruzar o entry com o scheduler REX documentado (`rex_wait @0x16ef0b02`).
- **Item 2** não depende do 1: a sonda de página vazia em `map_one` é instrumentação barata
  e dá evidência byte-level imediata. Fazer junto com o 1.
- **Item 3** provavelmente destrava sozinho quando o REX subir (item 1). Não forçar o bit —
  apenas a sonda para confirmar a hipótese antes de qualquer hack.

Trilhos independentes (podem rodar em paralelo já, contra as sondas isoladas):
- **Item 4 (loader BREW):** o ponto de despacho `AEEMod_Load` já foi mapeado na Fase 7 pelo
  `zeebo_lle_mod_probe`. Desenvolver/testar o `BrewLoader` contra essa sonda isolada — não
  depende de 1-3. Só falta resolver os VAs de `ISHELL_CreateInstance`/`AEEClsCreateInstance`.
- **Item 5 (GPU guest link):** `IglHook`/`GuestMachine` já estão prontos e verificados por
  pixel. Só falta resolver os ponteiros globais `gpIGL`/`gpIEGL` e ligar ao Core0 — também
  independente do boot completo.

Plano de execução sugerido:
- **Frente A (delegate_task, crítica):** Item 1 completo — scanner de reset + preâmbulo
  Core1 (CPSR/SP) + slide-detector. Critério: Core1 executa branch real em <1000 insns.
- **Frente B (delegate_task, paralela):** Item 2 (probe de página vazia) + Item 3 (sonda do
  bit em `[desc+0xc8]`, sem forçar).
- **Frente C (delegate_task, paralela):** Itens 4+5 — BrewLoader contra mod_probe + resolução
  de `gpIGL`/`gpIEGL` ligando GuestMachine ao Core0.

Evitar regressão: cada frente termina com `make test-gpu` (10/10), `make -C qdsp5 -f Makefile.qdsp5 test`,
`test_audio_sink` e `run_lle_cputests.sh` (12/12) verdes antes do commit.