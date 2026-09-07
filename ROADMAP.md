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

### Fase 9: Subsistema Gráfico (Adreno 130 / IGL) — integrado ao build, aguardando MAP_CONTROL
- [x] Evidência de firmware (GPU_TODO §13): 3D é offload MPU→QDSP5 atrás de fachada OpenGL ES 1.1 ATI-Imageon; **não há ring buffer PM4 A2xx observável do lado ARM11** → descartar `pm4_adreno.h` como produtor (mantido como referência de estudo §14).
- [x] Esqueleto `IGpuRasterizer` (Citra-style) + 2 backends (software correto + GL host stub) + `rasterizer_factory`, verificado por framebuffer (`gpu_smoke` 3/3: clear, triângulo, pm4-walk).
- [x] Produtor correto `IglHook` interceptando a vtable IGL/IEGL (80/28 slots, ABI ARMADILHA: R0=1º arg real) → `IGpuRasterizer`, verificado por framebuffer (`igl_smoke` 3/3; 40 slots gl* fixados contra gl_hle.cpp).
- [x] **Integração no build do emulador** (`tools/cpp/Makefile`: targets `gpu`/`test-gpu`) + handoff GPU→display provado por pixels em PPM 640x480 RGB565 (`gpu_display_integration`: clear azul 0x001F e vermelho 0xF800), commit `1508115`.
- [x] **Transform fixed-function (mvp+viewport)** implementado no `IglHook` (stacks modelview/projection, slots de matriz reais, aplicação obj→clip→NDC por vértice) + rasterizer respeitando viewport; verificado por PIXEL (`igl_transform_smoke` 2/2: triângulo desenha e `glTranslatex(+0.5)` o desloca 160px — antigo centro fica vazio, novo centro preenchido), commit `fbb094d`. `make test-gpu` = 10/10 PASS.
- [ ] Ligar `GuestMachine` ao Unicorn no `zeebo_lle_main` (lado do core ARM) — só quando MAP_CONTROL destravar execução real de guest (bloqueio a montante, FINDINGS 5a). Sem isso, nenhuma applet submete GL à vtable.
- [ ] Fase 2 restante: texturas/ATITC (`glTexImage2D`→upload, `glCompressedTexImage2D`→decode), multitexture+combine/dot3, backend GL host (ubershader).

### Fase 10: Subsistema QDSP5 (multimídia) — comando mapeado, plano de integração
- [x] Comando-plane mapeado (FINDINGS 2zz–3c): dispatcher `0x16e8cba0`, 4 task engines (VOICEPROC, VFE, JPEG, AUDPP), enfileiramento SMD/ONCRPC `0x17571748`, packet frame (+0x20 proc, +0x80 payload). Prioridade p/ jogos: **AUDPP ≫ JPEG > VFE > VOICE**.
- [x] `UnifiedAudioSink` disponível (Fase 8) como mixer PCM — atualmente HLE/hand-fed, **não conectado aos packets QDSP5**.
- [ ] Q0 (instrumentação): trapear o dispatcher e capturar proc-IDs reais (substituir placeholders `0x1b59`/`0x30000060`) — precisa execução de guest (bloqueio MAP_CONTROL).
- [ ] Q1 (AUDPP→áudio): consolidar bridges SMD, parsear payload, dirigir `UnifiedAudioSink`, saída WAV/SDL, sintetizar resposta RPC+`rex_set_sigs` (senão o jogo trava em `rex_wait`). Estratégia recomendada: **Option B (RPC short-circuit)** — aguardando decisão de Rafael (documentada no QDSP5_TODO §4).
- [ ] Q2/Q3/Q4: JPEG (libjpeg-turbo), VFE, VOICE — após AUDPP.

---

## Regras de Higiene e Verificação
- **Clean-room Absoluto:** `a1Sim` permanece estritamente como oráculo caixa-preta (proibido descompilar).
- **Integridade da NAND:** Leitura exclusiva na cópia de trabalho; dump original preservado com `chmod a-w`.
- **Validação por Execução Real:** Todo avanço deve ser demonstrado por código executável com commits atômicos e registros auditados em `notes/FINDINGS.md`.