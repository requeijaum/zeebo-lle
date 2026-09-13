# MORE_INFO — Ferramental e técnicas aplicáveis ao ecossistema Zeebo

Levantamento de ferramentas (binary analysis, console hacking, emudev, ROM hacking,
eletrônica embarcada/ARM) e de técnicas com fontes, mapeadas para os projetos
`~/projects/zeebo-lle` (LLE MSM7201A), `~/projects/zeebo-emulator` (HLE BREW 4.0.2)
e `~/projects/zeebo-dynarmic-bringup` (JIT A32).

Status de ambiente verificado nesta sessão (host Debian trixie, `apt-cache policy`):
- Instalado: `binwalk`, `gdb`, `strings`, `xxd`
- Disponível no repo Debian, NÃO instalado: `gdb-multiarch` (16.3-1), `qemu-user-binfmt`
  (1:10.0.11), `openocd` (0.12.0-3), `sigrok-cli` (0.7.2), `pulseview` (0.4.2),
  `flashrom` (1.4.0), `binutils-arm-none-eabi` (2.44), `apitrace` (11.1)
- NÃO disponível no repo Debian trixie (instalar fora do apt): `radare2`, `ghidra`,
  `kaitai-struct-compiler`, `renode`, `imhex`, `angr`

Sucesso já obtido: `binwalk` (carving de firmware/NAND — `nand/1.1.2*.bin`).

---

## 0. Antes de adotar qualquer ferramenta desta lista

Este documento é um catálogo **complementar**. O projeto já tem 23 notas de RE em
`notes/` e ferramental próprio; ler antes de introduzir tooling novo:

- `QDSP5_INDEX.md`, `qdsp5_proc_ids.md`, `qdsp5_program_ids.md`,
  `qdsp5_serializer_layout.md`, `qdsp5_anchors.md`, `qdsp5_boot_handoff.md` — áudio/DSP
- `okl4_source_para_boot.md` + `refs/okl4-2.1.1-fix7/` — boot (o firmware é OKL4/Iguana)
- `tripleoxygen_efs_nand_mmu.md` — tabela de partições NAND / EFS2 / MMU
- `emulador_tecnicas_zeebx_sdk.md`, `YMIR_ARES_HIGAN_TECHNIQUES.md`,
  `ZEEBX_GL_CLEANROOM_DIFF.md` — técnicas de emulação já levantadas
- `tools/` (~30 scripts Unicorn) e `nand/*.py` (scanners, `xdr_emu.py`)

**Regra:** nada aqui substitui essas fontes. Onde este documento conflitar com uma nota
do projeto ou com o `QDSP5_TODO.md`, a nota do projeto vence.

---

## 1. Análise binária e RE de formatos

| Ferramenta | Uso no Zeebo | Nota |
|---|---|---|
| **Ghidra** | Disassembly/decompilação ARMv5/v6 (ARM1136EJ-S), scripts PyGhidra p/ mapear vtables e tabelas de ponteiros; mapas de memória ROM/SRAM/MMIO | gratuito, melhor decompiler ARM aberto |
| **radare2 / rabin2 / rasm2 / Cutter** | disassembly em lote via CLI, headers, xrefs | não empacotado no Debian trixie |
| **Binary Ninja / IDA Pro** | decompiler comercial, tipagem | se disponível |
| **binutils ARM** (`arm-none-eabi-objdump/as`) | `-D -b binary -m armv6` p/ código raw; montar snippets de teste | leve, sem toolchain de SO |
| **readelf / nm / file** | ELF de homebrew e SDK Qualcomm | |
| **ImHex** (Pattern Language) | formalizar headers proprietários: `.mod` (magic "BREW" e variante flat), `.mif`, `.bar`, `.ggz`, partições MBN | pattern C-like |
| **010 Editor** (Binary Templates) | idem, padrão histórico da cena | comercial |
| **Kaitai Struct** (`.ksy` → C++/Python/Rust) | **alto ROI**: resolve as divergências pendentes de parser registradas na skill HLE — `.bar` 16B vs 8B, `.mif` 8B vs 20B — com um parser único gerado e integrável no emulador | Web IDE p/ inspeção visual |
| **QuickBMS** | carving de contêineres de assets de engines mobile da era (EA Mobile, Gameloft, Fishlabs) | centenas de scripts prontos |
| **BinDiff / Diaphora** | diff Zeebo OS v1 vs v2, ou builds do mesmo jogo | ver §5 (diff estrutural por CFG) |

### Extração de firmware / filesystem
- **binwalk** — entrypoint (já validado).
- ⚠ **Correção: o Zeebo NÃO usa YAFFS2.** A tabela de partições dumpada
  (`notes/tripleoxygen_efs_nand_mmu.md`) mostra **EFS2** (`0:EFS2` @ bloco 0xBC e
  `0:EFS2APPS` @ bloco 0x191) — o filesystem proprietário da Qualcomm sobre NAND, não o
  YAFFS2 do Android. `unyaffs`/`yaffs2utils`/`ubireader` **não servem** aqui.
  Ferramenta correta: parsers de EFS2 da cena Qualcomm/BREW
  (`qcdm`/`efs-tools`, ou parser próprio — layout já parcialmente documentado no projeto).
- **unsquashfs, cramfs-tools** — só se aparecer partição Linux embarcada (não observado).
- **qc_image_unpacker / edl / firehose-sahara tools** — partições MBN, PBL/SBL/QCSBL, AMSS/modem da série MSM7200/7201.
- **firmware-mod-kit (FMK)** — desempacotar/reempacotar imagens embarcadas.
- **7z / unar** — contêineres internos de pacotes de atualização.

**Layout de NAND CONFIRMADO nesta auditoria** (não estimado): `nand/1.1.2.bin` = 134.217.728 B
e `nand/1.1.2_spare.bin` = 138.412.032 B → diferença exata de **64 B de spare por página de
2048 B** (65.536 páginas). Inspeção dos 3 primeiros spares confirma dados de ECC/metadata na
página 0 e `0xFF` nas seguintes. Ou seja: NAND de 128 MB, página 2048+64.

---

## 2. Emudev: infraestrutura

| Ferramenta | Uso |
|---|---|
| **Unicorn Engine** | CPU standalone p/ isolar rotinas (decode, decrypt, boot) e servir de **oráculo de lockstep** contra o Dynarmic. **Já em uso pesado no `zeebo-lle`**: `nand/xdr_emu.py` recuperou empiricamente o layout dos serializadores XDR do AUDMGR (sentinelas 0xAAAA000n + hooks `UC_HOOK_MEM_READ`), e `tools/boot_chain_unuicorn.py`, `trace_mmu_on.py`, `fuzz_pathb_stepcmp.py` etc. Padrão a replicar, não a introduzir |
| **Capstone** | disassembly programável dentro do debugger do emulador |
| **Keystone** | montador runtime p/ gerar snippets de teste de opcode |
| **Dynarmic** | JIT A32 já em uso (`ZEEB_CPU=jit`, backend `DynarmicArmCore`) — mesma família do Infuse |
| **gdb-multiarch** | anexar ao GDB RSP stub do emulador; step/breakpoint/watchpoint em ARMv6 (`apt install gdb-multiarch`) |
| **GDB RSP stub interno** | implementar servidor TCP no emulador → abre Ghidra/gdb sobre a memória guest viva |
| **Dear ImGui + imgui_memory_editor** | hex viewer/editor em tempo real de RAM/VRAM/MMIO; padrão de facto (PCSX2, Dolphin, Ryujinx) |
| **Tracy Profiler** | instrumentação em µs de threads, pipeline do dynarec, sync de timers/IRQ |
| **RenderDoc** (+ `renderdoc_app.h` in-app API) | captura de frame disparada por atalho ou por gatilho de código quando uma draw call falha |
| **apitrace** | trace/replay de chamadas GL — regressão do `SoftGlBackend` frame a frame |
| **AFL++ / libFuzzer** | fuzzing de MMIO e de chamadas HLE: detectar OOB/deadlock/leak no host |
| **arm-wrestler / dynarmic test suite** | conformidade de opcodes ARMv6: flags NZCV, carry em shift/rotate, saturadas, CLZ, Thumb |

### Áudio / mídia (RE de assets)
- **vgmstream** — codecs/contêineres de console; inclui ADPCM/IMA e **QCP (Qualcomm PureVoice)**.
- **VGMTrans / PSound** — varredura de dumps por headers de áudio, MIDI, soundfonts.
- **FFmpeg/ffprobe** — cutscenes MPEG-4 simples / H.263 da era Qualcomm.
- Relevante ao gap registrado: IMA-ADPCM (WAVE_FORMAT 17) e MP3 no HLE.

---

## 3. ROM hacking (transferível ao corpus de jogos)

Muito do stack de assets de jogos BREW/Zeebo reusa formatos de portáteis contemporâneos
(LZ77/LZSS/Huffman/RLE, tiles indexados 4/8bpp, RGB565/RGBA4444).

- **Tile Molester / CrystalTile2 / Tile Layer Pro** — achar layout gráfico "no olho" ajustando stride e bit-depth quando o formato é desconhecido.
- **Tinke / Karameru / DSLazy** — carving de contêineres de assets.
- **Kruptar / Cartographer / Atlas** — dump/insert de script com tabelas `.tbl` (DTE), útil p/ strings sem ASCII padrão.
- **Lunar IPS / Flips / xdelta / Rom Patcher JS** — patches delta (IPS/BPS/XDelta) sem redistribuir código sob copyright.

---

## 4. Eletrônica embarcada e ARM (hardware real)

### JTAG/SWD e depuração de silício
- **OpenOCD** — halt do núcleo no bootrom, leitura de registradores, dump de SRAM/TCM antes do bootloader fechar o acesso. Atenção: MSM dual-core tem **múltiplos TAPs na mesma cadeia JTAG**.
- **PyOCD**, **Black Magic Probe** — automação em Python / probe autônoma.
- **J-Link + GDB Server** — scripts de init de barramento e detecção de cadeias complexas.

### Captura de barramento
- **PulseView / sigrok-cli** — decoders UART (bootlog PBL/SBL/AMSS, tipicamente 115200), SPI, I²C, SD/MMC.
- **Bus Pirate (v3/v4/5)** — interação interativa com periféricos sem escrever firmware.
- **minicom / picocom / tio** — console serial (`tio` reconecta sozinho no reboot da placa).
- ⚠ **Níveis lógicos**: UART de debug do MSM7201A opera em 1.8V/2.8V — usar level shifter, 5V danifica as linhas.

### Flash
- **flashrom** + programador (CH341A, RPi, Bus Pirate) — leitura/gravação SPI/NOR.
- **Parsing de OOB/spare NAND** — **confirmado 2048 B + 64 B de spare** (ver §1): identificar
  ECC (BCH ou Reed-Solomon) e Bad Block Table para reconstruir imagem lógica limpa antes de
  parsear EFS2. Os dois dumps (`1.1.2.bin` sem spare, `1.1.2_spare.bin` com) já permitem
  derivar o esquema de ECC por comparação, sem hardware.

### Modelagem de periféricos e boot (LLE)
⚠ **O boot do Zeebo não é bare-metal REX puro:** o `1.1.2_APPS.bin` embute paths de fonte do
**OKL4 2.1.1** (`iguana/server/src/iguana_server.c`, `naming_server.c`, string `ig_naming`,
`k1Process: Created iguana PD=%x`) e o corpus OKL4 completo já está em
`refs/okl4-2.1.1-fix7/` (ver `notes/okl4_source_para_boot.md`). Para o lado APPS, a
**fonte do próprio microkernel** é referência muito mais forte que qualquer ferramenta
genérica desta lista — nenhum modelador de periférico substitui ler o L4/Iguana.

- **Renode (Antmicro)** — simulação declarativa (`.resc`) de periféricos, VIC, timers, MMIO; boa referência arquitetural para LLE escrito do zero.
- **`dtc` / DTS-DTB de SoCs irmãos** (HTC Dream, i7500, Motorola Cliq) — bases de MMIO, mapeamento de IRQ, clocks, GPIO, MDDI.
  ⚠ Ressalva: kernels MSM da era usavam tabelas estáticas de plataforma em C
  (`board-*.c`), não Device Tree — DT só chegou ao mach-msm bem depois. Esperar
  `board-dream.c` / `devices-msm7x00.c`, não `.dts`.
- **qemu-system-arm** — estudar modelos de máquina ARM legadas (IRQ, DMA, controlador NAND virtual).

---

## 5. Técnicas (com fontes)

### 5.1 Fontes primárias abertas para o MSM7201A
O Zeebo é arquiteturalmente o mesmo SoC dos primeiros Androids Qualcomm (HTC Dream/G1,
Motorola Cliq, Samsung i7500): 65nm, ARM11 528 MHz, **Adreno 130**, DSP QDSP5.
O **kernel MSM aberto** é a melhor especificação disponível:

⚠ **Ressalva importante — o kernel MSM é ANDROID/AMSS-side, não BREW-side.**
`qdsp5/audpp.c` documenta o driver Linux que fala com o AUDPPTASK; o Zeebo roda BREW sobre
REX/AMSS e chega ao DSP por **ONCRPC AUDMGR sobre SMD**, não pela API `msm_adsp_write` do
Linux. Usar o kernel como *dicionário de structs e nomes de comando* (`audpp_cmd_cfg`,
`AUDPP_CMD_CFG` 0x0001, filas Cmd1/2/3), **nunca** como prova do fluxo desta firmware.

- **SMD / shared memory IPC** — artigo canônico *"Google Android — IPC at the lowest levels"*
  (EETimes/EDN, Semiconductor Insights, 2009): descreve a tabela de **64 canais**
  `struct half_channel`, flags `fHEAD`/`fTAIL`/`fSTATE`, mecanismo de *doorbell* por
  interrupção, `smd_ch_list` percorrida pelo ISR do ARM11 e o registro de callback via
  `smd_open()`. Bate diretamente com o canal SMD/ONCRPC do nosso LLE.
- **QDSP5 / AUDPPTASK** — `arch/arm/mach-msm/qdsp5/audpp.c` e
  `include/mach/qdsp5/qdsp5audppcmdi.h` / `qdsp5audppmsg.h`
  (espelho vivo: `github.com/freedreno/kernel-msm`, branch `hp-tenderloin-3.0`).
  Documenta as **três filas de comando ARM→AUDPPTASK** (uPAudPPCmd1Queue curta/frequente
  6 words em MEMA; Cmd2Queue 23 words; Cmd3Queue), structs (`audpp_cmd_cfg`,
  `audpp_cmd_cfg_dec_type`, `audpp_cmd_avsync`, volume/pan, EQ, MBADRC), IDs
  (`AUDPP_CMD_CFG` 0x0001) e mensagens (`AUDPP_MSG_AVSYNC_MSG`).
  → **Contribui para a Fase Q0**, mas atenção ao estado real registrado no `QDSP5_TODO.md`:
  Q0.2 **não** está bloqueado por falta de nomes/IDs. Já existem, com evidência estática do
  próprio firmware (`notes/qdsp5_proc_ids.md`): AUDMGRPROG `0x30000013`, AUDMGRCB
  `0x31000013`, ADSPRTOSATOM `0x3000000a`, ADSPRTOSMTOA `0x3000000b`, e o conjunto de
  procedures `audmgr_*`. O que continua aberto é (a) **ordinal numérico** de cada proc,
  (b) **captura de hit real desde cold boot** (Q0.1b/c) e (c) associação header↔wire por
  bytes e call sites. O kernel MSM **não** resolve nenhum dos três — ele descreve outro
  transporte. Serve como dicionário de structs de comando AUDPP a jusante.
  Também: `audpreproc.c` (encode/record) e `qdsp5v2/` p/ variantes.
  ⚠ `0x30000060` do plano antigo foi **fabricado** — não reintroduzir.

### 5.2 JIT / dynarec
- **Differential Execution Tracing (DET)** — log determinístico de tuplas
  `(PC, instruction_hash, reg_checksum)` por bloco básico nos dois backends;
  diff linear localiza a **primeira instrução divergente**. Superior a comparar só o frame
  final. Base: *differential testing* (McKeeman) + prática Dolphin/yuzu.
- **Lockstep Unicorn × Dynarmic** — comparar R0–R15 + CPSR (NZCV) a cada fim de bloco.
  É exatamente a Etapa 1 do `zeebo-dynarmic-bringup`.
- **Fast memory / software TLB** — mapear a RAM guest com `mmap` em base fixa e endereçar
  por `guest_base + offset`, eliminando branch de bounds e vtable de I/O em cada load/store.
  Base: QEMU/TCG, *"Efficient Virtual Memory Emulation for Fast System Simulation"*.
- **SMC detection por page fault** — marcar páginas de código já compilado como read-only
  (`mprotect`) e invalidar o bloco do JIT no SIGSEGV, em vez de checar cada `STR/STM`.
  Custo zero no caminho feliz; necessário para loaders de módulo e overlays dinâmicos.
- **Event-wheel scheduler** — periféricos agendam `target_cycle = current_cycle + latency`
  num min-heap; em `WFI` a CPU salta direto para o próximo evento, matando busy-wait.

### 5.3 RE de binário / firmware
- **Entropy analysis (Shannon, janela deslizante 256B)** para segregar code/data em dumps sem símbolos:
  - baixa entropia com padrão regular → tabelas de ponteiros / vtables
  - ~5.5–6.8 bits/byte com opcodes `0xE…` válidos → código ARM
  - ≈8.0 → assets comprimidos (LZSS/zlib/deflate) ou cifrados
- **Execução simbólica restrita (angr / Triton)** — isolar rotina de checksum, decode de
  formato proprietário ou decrypt de bootloader e resolver as restrições automaticamente,
  em vez de descompilar centenas de linhas de ARM na mão.
- **Binary diffing estrutural (isomorfismo de grafo)** — comparar CFGs (nº de blocos
  básicos, arestas, chamadas externas) em vez de bytes: casa funções mesmo com otimização
  e endereços diferentes.

### 5.4 BREW / HLE clean-room
- **VTable slot fingerprinting** — o padrão `LDR R3, [R0, #0]` seguido de
  `LDR R12, [R3, #offset]` dá o slot exato (`offset / 4`) sem tocar em headers da Qualcomm.
  Método usado por WINE/Citra; preserva a distribuibilidade.
- **ABI reconstitution por AAPCS/ATPCS** — args em R0–R3 (excedentes na stack), retorno em
  R0 (ou R0:R1), R4–R11 callee-saved. Inspecionar quais registradores são populados antes de
  `BLX R12` e o `ADD SP, SP, #N` após o retorno dá o **número exato de parâmetros** do método
  → resolve divergências de vtable sem chute.
- **Object lifecycle / refcount tracking** — interfaces BREW são COM-like:
  slot 0 `AddRef`, slot 1 `Release`, slot 2 `QueryInterface(this, clsid, out)`.
  Interceptar esses 3 slots reconstrói a árvore de objetos e revela qual subsistema
  soltou/vazou referência (bitmap, áudio).
- **Stub logging por frequência + call-site** — hash `(stub_id, caller_PC)` em vez de printar
  toda chamada. Priorizar: (a) stubs chamados **uma única vez** no boot (alta chance de gate
  de inicialização); (b) stubs que retornam 0/NULL onde o chamador faz `CMP R0,#0; BEQ`.
  → Ataca diretamente os walls de boot registrados na compat-list (cluster +0x63c,
  sobreviventes 2-cores GL-boot-gated).
- **Trace-driven replay de GL** — capturar só o stream `Clear/VertexPointer/DrawElements/
  SwapBuffers` e reproduzir isolado no `SoftGlBackend`, sem esperar milhares de ticks de boot.

---

## 6. Priorização sugerida (maior alavancagem primeiro)

1. **Q0.1b/c do QDSP5** — captura de hit real desde cold boot com comprimento efetivo,
   bases de header e endianness. É o bloqueio verdadeiro; nenhuma ferramenta desta lista
   o substitui. O kernel MSM entra depois, como dicionário de structs AUDPP.
2. **Stub logging por frequência/call-site** no HLE → ataca o wall de *alcançar o loop de
   render*, que a skill já provou não ser o rasterizador.
3. **DET + lockstep Unicorn×Dynarmic** no bringup do JIT (Etapa 1 já em curso).
4. **Kaitai Struct** para `.bar`/`.mif`/`.mod` → encerra as divergências de parser vs Zeebx.
5. `apt install gdb-multiarch` + GDB RSP stub no emulador → debug interativo real.
6. Entropia + angr apenas quando bater em rotina opaca específica.

**Escopo do hardware (§4)**: as ferramentas de JTAG/UART/flash só se aplicam se houver
console físico e disposição para abri-lo. Os dumps de NAND 1.1.2 já existem em
`nand/` — para o trabalho atual, §4 é opcional/futuro, não caminho crítico.

## 7. Notas de proveniência

- Kernel MSM (CodeAurora/freedreno) é **GPL-2.0 público** — fato de RE citável e rastreável;
  usar como *especificação*, não copiar código para dentro de repositório de licença
  incompatível sem checar.
- Manter a regra da skill: Zeebx só como referência de fatos de RE rastreáveis a fonte
  pública; Infuse permanece **oráculo de comportamento black-box**, nunca decompilado.
- Regra do LLE que vale para todo item acima: **controle negativo obrigatório**. Qualquer
  ferramenta nova que "prove" progresso deve ser rodada também com entrada inválida; se o
  resultado for igual, a ferramenta provou o harness, não o guest.

## 8. Correções aplicadas nas auditorias

### Auditoria 1 (2026-09-10)
1. Afirmava que o kernel MSM "destrava a Fase Q0 sem RE cego". **Falso.** O kernel é o
   caminho Android/`msm_adsp_write`; o Zeebo usa ONCRPC AUDMGR sobre SMD. E os proc/program
   IDs já estavam recuperados estaticamente do firmware (`notes/qdsp5_proc_ids.md`).
2. Listava Unicorn como ferramenta a adotar, sem registrar que **já é o motor de RE
   empírico do projeto** (`nand/xdr_emu.py`, `tools/*unuicorn*`).
3. Status de apt incompleto — só 3 pacotes tinham sido consultados; agora 13.
4. Não separava o escopo de hardware físico (§4) do caminho crítico atual.

### Auditoria 2 (2026-09-10) — erros de FATO sobre o hardware
5. **YAFFS2 estava errado.** Eu extrapolei do "MSM7k da era = Android = YAFFS2". A tabela de
   partições real do Zeebo é **EFS2/EFS2APPS** (Qualcomm), não YAFFS2. As três ferramentas
   que eu recomendei (`unyaffs`, `yaffs2utils`, `ubireader`) não servem para este dump.
6. **Página NAND 2048+64 era chute, agora é medição.** Confirmado aritmeticamente pela
   diferença entre `1.1.2.bin` e `1.1.2_spare.bin` + inspeção dos spares.
7. **Omissão grave: OKL4.** O relatório tratava o lado APPS como se fosse REX/AMSS puro e não
   mencionava que o firmware é **OKL4 2.1.1 / Iguana**, com o source completo já em
   `refs/`. Para boot, isso supera qualquer ferramenta genérica listada.
8. **Device Tree era anacrônico** para MSM7201A — a era usava `board-*.c` estático.
9. Adicionado o corpus de notas já existente como ponto de partida obrigatório (§0).

---

## 9. Estatística e probabilidade

Ver **`notes/STATS_TECHNIQUES.md`** — nota irmã com métodos quantitativos
(statistical debugging sobre o corpus 62, regra dos três para controles
negativos, significância de varredura de assinatura, SPRT no lockstep,
Good-Turing/Chao1, qui-quadrado código×dado, bootstrap em comparação de frames)
e a auditoria estatística dos harnesses que já existem nos três repos.

Dois resultados de lá que afetam prioridades deste documento:
- A busca de constante de 4 bytes em 22 MB tem **0,005 hits esperados por acaso**
  → a evidência de `0x30000013` é sólida e agora quantificada. Padrão de 2 bytes
  tem **~338** → qualquer achado nessa escala é ruído até prova em contrário.
- O fuzz QDSP5 (75.563 execs, 0 crashes) **exclui bugs com taxa ≥1e-4**, mas um
  de taxa 1e-5 tem 47% de chance de ter escapado. O limite é cobertura
  (`cov 134`), não tempo de execução.

---
