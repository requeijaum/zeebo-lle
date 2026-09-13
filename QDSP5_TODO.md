# QDSP5 Subsystem — Deep-Dive & TODO

> **Atualização 2026-09-12**: o freeze de desenvolvimento em `tools/cpp/qdsp5/` foi
> levantado por decisão do Rafael em 2026-09-11 (idem `tools/cpp/gpu/`). Este documento
> deixa de valer como restrição de escopo: trabalho ativo no QDSP5 está autorizado,
> respeitando os gates descritos aqui.

> Zeebo LLE emulator. Target: Qualcomm **MSM7201A** (ARM11 apps + ARM9 modem/AMSS + **QDSP5** DSP).
> Scope of this document: everything about the QDSP5 multimedia DSP — what is
> reverse-engineered and proven, what is stubbed, and the concrete work left to make
> it produce real audio/video/image output. Grounded ONLY in the repo's primary
> sources (`notes/FINDINGS.md` sessions 2zz–3c, `ROADMAP.md`, and `tools/cpp/*`).
> Written incrementally — sections appended in dependency order.

Status legend: ✅ proven in code/exec · 🟡 mapped but data-plane empty · ⛔ not started

---

## 0. Executive summary

On the MSM7201A the QDSP5 is **not driven by the games directly**. The ARM11 (BREW/apps)
sends multimedia work to the ARM9 (AMSS) over **ONCRPC on top of the SMD shared-memory
channel**; the AMSS firmware then feeds the **QDSP5 hardware command queues**. So emulating
"the DSP" here means emulating the **RPC command plane** the firmware writes into — NOT the
QDSP5 instruction set.

What that buys us (and the trap): we have fully mapped the **command plane** (dispatcher,
queues, packet framing, all four task engines). We have **zero data plane** — nothing behind
the queues decodes JPEG, mixes PCM to a DAC, or processes VFE frames. Current "audio" is a
standalone HLE-style mixer (`zeebo_audio_sink.h`) that is **not wired to the QDSP5 packets at
all**. Closing that command→data gap is the whole job below.

---

## 1. Architecture — the path a sound/frame takes

```
ARM11 (BREW app)
  │  builds ONCRPC CALL packet  (program 0x30000060 = MSM Audio/QDSP service)
  ▼
SMD half-channel  0x1755d1dc   (state 2=OPENED → 3=FLUSHING wakes consumer)
  │
ONCRPC queue head 0x17571748   (+00 head, +04 tail, +08 count, +1c init=1)
  │  router loop 0x16ef0b2c..3a  (r0==2 → bl 0x16e8cb96 packet consumer)
  ▼
Dispatch engine   0x16e8cba0..e0
  │  proc ID @ pkt+0x20, len @ +0x24, payload @ +0x80, 1280B (0x500) max frame
  ▼
QDSP5 task command queues (per engine, below)  →  [DATA PLANE — MISSING]  →  DAC / framebuffer
```

Packet frame (verified, session 2zz / `oncrpc_packet_header` in `zeebo_smd_bridge.cpp`):
- `+0x00` xid · `+0x04` msg_type(0=CALL) · `+0x08` rpc_version(2) · `+0x0c` program
- `+0x10` version · **`+0x20` procedure ID** · `+0x24` payload length · **`+0x80` payload body**
- alignment mask `0x580`, max payload `0x500` (1280B), success code `0x1b59`.

---

## 2. The four QDSP5 task engines (command plane) ✅ mapped

All discovered from firmware assertion strings of the form
`"Assertion cmd_size <= QDSP_<TASK>_<QUEUE>_MAX_CMD_SIZE failed"` — primary-source evidence,
not inference. Every engine is reached through the single dispatcher `0x16e8cba0..e0`.

| Engine | Task name | Command queue(s) | String addr | Session |
|--------|-----------|------------------|-------------|---------|
| Voice DSP | `QDSP_VOICEPROCTASK` | `UPVOCPROCQUEUE` | `0x16ea9cb0` | 3a |
| Video Front End | `QDSP_VFETASK` | `VFECOMMANDSCALEQUEUE` | `0x16ea9ce8` | 3b |
| " | " | `VFECOMMANDTABLEQUEUE` | `0x16ea9d40` | 3b |
| " | " | `VFECOMMANDQUEUE` | `0x16ea9d88` | 3b |
| JPEG codec | `QDSP_JPEGTASK` | `UPJPEGACTIONCMDQUEUE` | `0x16ea9dd0` | 3c |
| " | " | `UPJPEGCFGCMDQUEUE` | `0x16ea9e18` | 3c |
| Audio post-proc | `QDSP_AUDPPTASK` | `UPAUDPPCMD2QUEUE` | `0x16ea9e68` | 3c |

Role notes (from FINDINGS 3a–3c):
- **VOICEPROCTASK** — upstream voice codec params (telephony path; low priority for a game console).
- **VFETASK** — camera/video front end: `SCALE` = scaling/aspect/resample, `TABLE` = gamma/color
  LUTs & transform matrices, `COMMAND` = frame trigger/buffer control.
- **JPEGTASK** — HW codec: `ACTION` triggers encode/decode slices + DCT/quant passes; `CFG`
  sets resolution, YUV420/YUV422 sampling, Huffman tables.
- **AUDPPTASK** — **the one that matters most for games**: multi-band EQ, volume ramping,
  dynamic-range control, surround/mixing → DAC output.

Priority for a game console: **AUDPP ≫ JPEG > VFE > VOICE**. Audio is what games actually use;
VFE/VOICE are phone-heritage blocks the Zeebo firmware carries but games rarely touch.

---

## 3. Current implementation state — honest audit

### 3.1 Command plane — what actually runs today
- ✅ **Dispatcher decoded** (`0x16e8cba0..e0`, session 2zz): token store, callback fetch
  `[r3+0x1c]`, proc ID `[r4+0x20]`, len `[r4+0x24]`, payload advance `+0x80`, deserializer call.
- ✅ **Queue enqueue engine decoded** (`0x16ef0b70..bae`, session 2yy) and **queue head layout**
  at `0x17571748` verified (`+0x1c=1` init).
- ✅ **Router branch logic** (`0x16ef0b2c..3a`, sessions 3g/3h): `r0==2` OPENED → packet consumer
  `0x16e8cb96`; `r0==3` FLUSHING → flush `0x16e8cb88`.
- ✅ **Packet injection validated** (session **3p**) in `tools/cpp/zeebo_smd_bridge.cpp`,
  class `VirtualSMDBridge::inject_oncrpc_packet`: builds an ONCRPC CALL, writes header +
  payload@`+0x80`, links a queue node, flips SMD channel `0x1755d1dc` to FLUSHING. Standalone
  test PASSES.
- ✅ **Orchestrator hook** in `zeebo_lle_main.cpp` (`c0_mem_hook`, ~line 769):
  formerly duplicate inline injection logic was **unified** into `zeebo_smd_bridge_unified.h`
  and gated under `qdsp5_doorbell_probe.h` (commit `02dea54`/`6a4ca20`).
  Fabricated liveness injection is **disabled by default**, and only active when
  explicitly opted in with `ZEEBO_QDSP5_RPC_PROBE=1`. A standalone test
  `test_qdsp5_doorbell_probe.cpp` validates that probes are suppressed when unset/off.

- ⚠️ **UNVERIFIED constants baked into both bridges:** `program = 0x30000060` ("MSM Audio/QDSP
  service") and proc `0x1b59` are **hardcoded comments with no primary-source evidence** — neither
  value appears in any firmware string dump in FINDINGS. `0x1b59` is in fact the RPC *success
  status code* (session 2zz, `0x16e8cbde` literal pool). Both are placeholders and MUST be
  replaced by Q0.2-captured values before any of this is trustworthy.

### 3.2 Data plane — what is MISSING
- ⛔ **No procedure-ID map.** We inject/handle the *literal* `0x1b59` (which is actually the RPC
  *success status code* from session 2zz, reused as a placeholder proc ID). The real
  per-engine AUDPP/JPEG/VFE procedure IDs and their payload struct layouts are **not decoded**.
- ⛔ **No payload parsers.** Nothing interprets the `+0x80` body for any engine (EQ bands,
  volume, PCM buffer pointers, JPEG dimensions, VFE tables).
- ⛔ **AUDPP → audio is not connected.** `zeebo_audio_sink.h` (`UnifiedAudioSink`) exists and
  is unit-tested (`test_audio_sink.cpp`), but it is an **HLE-style PCM mixer with a hand-fed
  API** (`allocate_voice`/`submit_pcm`/`mix_samples`). **Nothing routes an AUDPP command packet
  into it**, and its output goes nowhere (no OS audio device / WAV file).
- ⛔ **JPEG/VFE decoders absent.** Queues are mapped; no DCT/IDCT, no YUV→RGB, no scaler.
- ⛔ **No response path.** After a command "completes", the firmware expects a reply/status and
  a completion signal (`rex_set_sigs` on the requesting TCB). We do not synthesize replies, so
  a real game issuing a blocking audio RPC would hang waiting for completion.

### 3.3 The core gap in one sentence
> We can put a well-formed packet *into* the QDSP5 command queue and wake the consumer, but
> the consumer runs the **real firmware's** DSP task code which then tries to talk to
> **nonexistent QDSP5 hardware** — and we neither model that hardware nor short-circuit the
> RPC with a synthesized result that produces real output.

---

## 4. The strategic fork — how to bridge command→data

There are two ways to make the QDSP5 produce output, and AUDPP forces the choice:

**Option A — Full LLE (model the QDSP5 hardware).** Emulate the QDSP5 as a real DSP: let the
AMSS task code write to QDSP5 MMIO / shared RAM, and implement those registers + the DSP's
own microcode behavior. Faithful, but the QDSP5 runs signed Qualcomm firmware images we do not
have disassembled, and the DSP ISA (QDSP5/"QDSP4000") is a separate reverse-engineering project.
**Very high cost, matches the project's no-HLE purity.**

**Option B — RPC short-circuit (intercept at the dispatcher).** Hook the packet consumer
`0x16e8cb96` / dispatch `0x16e8cba0`, decode the proc ID + payload ourselves, perform the
operation in host C++ (mix PCM, decode JPEG via libjpeg, scale via our own code), write the
result to the destination buffer the firmware pointed at, and synthesize the completion
reply + `rex_set_sigs`. The firmware never notices the QDSP5 is absent. **This is HLE of the
DSP layer only** — the ARM cores still run pure LLE firmware.

**Recommendation: Option B for AUDPP/JPEG now, keep A as a research track.** Rationale: the
value (games with sound) is in AUDPP, and Option B reuses the already-built `UnifiedAudioSink`.
Rafael's stated preference is max-ambition scope, so this is not "the conservative option" —
it is the only path that yields audible output this quarter, while the LLE-purity goal is
preserved for the CPU/kernel/bus layers where it was actually proven. Flag for Rafael's call.

> ⚠️ DECISION NEEDED (Rafael): accept Option B (DSP-layer HLE via RPC short-circuit) as the
> audio path, or mandate Option A (full QDSP5 hardware model)? Everything in §5 assumes B.

---

## 5. TODO — phased, dependency-ordered

### Phase Q0 — Instrumentation & ground truth (prereq for everything) ⛔
- [ ] **Q0.1** Add a dispatcher trap in `zeebo_lle_main.cpp`: hook exec at `0x16e8cba0` (or the
      consumer `0x16e8cb96`) and log `program`, **real proc ID** `[r4+0x20]`, len `[r4+0x24]`,
      and hexdump payload `+0x80`. Captures what the *firmware itself* sends — replacing BOTH
      guessed constants: proc `0x1b59` AND program `0x30000060` (both currently unverified).
- [ ] **Q0.2** Boot far enough that the firmware issues a genuine AUDPP RPC (needs a game or the
      BREW audio init path running). Record the first real proc IDs per engine into
      `notes/qdsp5_proc_ids.md`.
- [ ] **Q0.3** Cross-reference captured proc IDs against AMSS symbol strings already dumped
      (grep the firmware for `audpp`, `snd`, `adsp_rtos`, `AUDPLAY`, `AFE`).

### Phase Q1 — AUDPP audio path (highest value) ⛔
- [ ] **Q1.0** Consolidate `VirtualSMDBridge` (test) and `UnifiedSMDBridge` (orchestrator) into
      one bridge class so injection/reply logic lives in a single place before building on it.
- [ ] **Q1.1** Define `struct audpp_cmd` layouts from captured payloads: command opcode,
      target buffer ptr, sample rate, channel count, EQ/volume params.
- [ ] **Q1.2** Build `ZeeboQDSP5` dispatcher-intercept class: match program `0x30000060` +
      AUDPP proc IDs, parse payload, drive `UnifiedAudioSink` (`allocate_voice` / `submit_pcm`).
- [ ] **Q1.3** Wire `UnifiedAudioSink::mix_samples` to a **real output**: start with a WAV
      file writer (`qdsp5_out.wav`) for deterministic tests; then optional SDL2/ALSA live sink.
- [ ] **Q1.4** Synthesize the RPC completion reply + `rex_set_sigs` on the caller TCB so the
      firmware's audio call returns instead of hanging (see §6).
- [ ] **Q1.5** Replace the dummy `0x42`/`0x1b59` injection in `c0_mem_hook` with the real
      AUDPP-driven path; keep the dummy behind a `--probe-rpc` flag for liveness testing.
- [ ] **Q1.6** Integration test: firmware audio init → captured PCM in `qdsp5_out.wav` is
      non-silent and matches expected sample rate.

### Phase Q2 — JPEG codec ⛔
- [ ] **Q2.1** Decode `UPJPEGCFGCMDQUEUE` payload (resolution, YUV420/422, Huffman tables).
- [ ] **Q2.2** Decode `UPJPEGACTIONCMDQUEUE` (encode/decode trigger, slice buffers).
- [ ] **Q2.3** Back it with libjpeg-turbo (decode to the firmware's output buffer); synthesize reply.
- [ ] **Q2.4** Test against a known BREW splash/asset JPEG if one is present in NAND.

### Phase Q3 — VFE (video front end) ⛔ (low priority; camera heritage)
- [ ] **Q3.1** Decode SCALE/TABLE/COMMAND payloads.
- [ ] **Q3.2** Implement scaler + gamma/color LUT + transform-matrix passes in host C++.
- [ ] **Q3.3** Only if a game/app actually drives VFE — otherwise stub with a logged NACK.

### Phase Q4 — VOICE ⛔ (lowest priority; telephony)
- [ ] **Q4.1** Stub `UPVOCPROCQUEUE`: accept, log, synthesize success reply. No real vocoder
      unless a title needs it.

### Phase Q5 — Full-LLE research track (parallel, optional) ⛔
- [ ] **Q5.1** Locate the QDSP5 firmware image in the NAND dump; identify load address + format.
- [ ] **Q5.2** Map QDSP5 MMIO / ADSP-RTOS shared-memory interface as seen from ARM9.
- [ ] **Q5.3** Feasibility spike: does the QDSP5 ISA have an existing disassembler? Decide
      go/no-go for true hardware emulation vs. permanent Option-B.

---

## 6. The completion/reply path (critical, easy to miss)

A real game issues audio via a **blocking** RPC: it enqueues the CALL and then `rex_wait`s on a
signal mask until the DSP task signals done. Our reverse engineering already mapped that
signaling machinery — reuse it, don't reinvent:

- **`rex_set_sigs(tcb, mask)`** at `0x1730f2aa` (vector `0x16f80f0c`): asserts a signal bitmask on
  a task's TCB; triggers an L4e yield (`SVC #0x6`) if the woken task outranks the running one.
- **`rex_wait(mask)`** at `0x1730f442` (vector `0x16f80f1c`): what the caller is blocked in.
- Event-pump loop `0x16ef0b02..1a` calls `rex_wait` with mask `0x00180000`, 100ms timeout.

So Q1.4 concretely = after our host-side mix consumes the packet: (1) write the RPC reply
struct where the caller expects it, (2) call the firmware's `rex_set_sigs` on the requesting
TCB with the completion bit, so `rex_wait` returns and the game proceeds. Without this, audio
"works" (WAV is written) but the game **still hangs**.

Open sub-question: which signal bit = AUDPP-done? Capture during Q0.2 by watching which mask
the audio-issuing thread waits on.

---

## 7. Open questions / unknowns (fill as discovered)

- [ ] Real AUDPP/JPEG/VFE **procedure IDs** (Q0.2 output). Current `0x1b59` is a placeholder.
- [ ] RPC **reply struct** layout and where the caller reads it.
- [ ] AUDPP **PCM source**: does the payload carry a buffer pointer into shared RAM, or a DME
      descriptor? Determines how `submit_pcm` gets its samples.
- [ ] Native **output sample rate / format** the Zeebo DAC expects (assume 44100/16/stereo
      until proven; `UnifiedAudioSink` defaults to that).
- [ ] Does BREW use AUDPP directly, or a higher `snd`/`AUDPLAY` HLE layer above it?
- [ ] QDSP5 firmware image location in NAND (Q5.1).

---

## 8. Source index (primary evidence used)

- `notes/FINDINGS.md`:
  - Session **2yy** (`0x16ef0b70..bae`) — RPC enqueue engine + queue head `0x17571748` layout.
  - Session **2zz** (`0x16e8cba0..e0`) — dispatch engine + packet framing (proc@+0x20, +0x80 body).
  - Session **3a** (`0x16ea9cb0`) — QDSP_VOICEPROCTASK / UPVOCPROCQUEUE.
  - Session **3b** (`0x16ea9ce8..d88`) — QDSP_VFETASK three queues.
  - Session **3c** (`0x16ea9dd0..e68`) — QDSP_JPEGTASK + QDSP_AUDPPTASK.
  - Sessions **3e/3f/3g/3h** — REX signaling (`rex_set_sigs/wait/get_sigs`) + router branches.
- `ROADMAP.md` §4 (QDSP5 subsystems) and Phase 1 (packet ingestion, `b55dae0`).
- ✅ **Packet injection validated** (session **3p**) in `zeebo_smd_bridge.cpp`
  (`VirtualSMDBridge::inject_oncrpc_packet`) — resolves ROADMAP "Gap 1". Note: the ROADMAP's
  `b55dae0` commit is the *task-table mapping*, not the injection.
- `tools/cpp/zeebo_audio_sink.h` + `test_audio_sink.cpp` — the standalone PCM mixer
  (`UnifiedAudioSink`).
- `tools/cpp/zeebo_lle_main.cpp` `c0_mem_hook` (~L769), `UnifiedSMDBridge::inject_packet` — dummy
  injection on A2M doorbell. **Distinct** from the test's `VirtualSMDBridge`.

*Last updated: session after 3c. Append new engines/proc-IDs above §8 as they are decoded.*

> ⚠️ **SUPERSEDED em parte pela §11** (decompilação estática do firmware + skeleton FASE 1):
> `program 0x30000060` foi REFUTADO → **AUDMGRPROG 0x30000013**; `proc 0x1b59` era o status
> de sucesso RPC → substituído pelos **ordinais AUDMGR 0..9**. Ler §11 antes de agir sobre
> §1/§3.2/§4/§5. Não reescrevi §1–§8 (o outro agent lê estas âncoras) — §11 é a verdade atual.

---

## 10. Q0.2 RESOLVIDO por extração estática do firmware (sem boot)

Fonte: `nand/1.1.2_AMSS.bin` + `1.1.2_APPS.bin`. Detalhes em `notes/qdsp5_proc_ids.md`.

- **AUDMGRPROG = 0x30000013** VERIFICADO (adjacência de bytes à string "unable to
  register (AUDMGRPROG...)"). O `0x30000060` era FABRICADO — eliminado do código.
- **AUDMGRCB = 0x31000013** (par de callback). Serviço real = AUDMGR (`audmgr_svc.c`).
- Nomes das procs AUDMGR capturados (enable_client, set_device_mode, get_rx/tx_sample_rate,
  register_codec_listener, ...). ORDINAIS ainda não confirmados (falta ordem do xdr).
- **Correção de arquitetura**: o array QDSP5 é MAIOR que sessions 3a-3c registraram —
  AUDPLAY0-4 (5 decoders), AUDPP CMD1/2/3, AUDREC, além de JPEG/VFE/VOICE. AUDPLAY = decode
  real; AUDPP = pós-processador downstream. Código (`kQueues`) atualizado.

Ainda aberto: ordinais exatos das procs e layout de bytes dos payloads (precisa desassemblar
os `xdr_*` ou 1 pacote capturado). O hook Q0.1 vira confirmação, não descoberta primária.

Isolated subtree (own `Makefile.qdsp5`, no Unicorn/SDL dep), mirroring the GPU skeleton's
"prove by artifact" convention — GPU proves by framebuffer, QDSP5 proves by **WAV file**.

Files:
- `qdsp5_rpc.h` — verified AMSS addresses, packet framing, the 4-engine queue table, ONCRPC
  header. Placeholder constants (`0x30000060`, `0x1b59`) clearly marked UNVERIFIED.
- `iqdsp_engine.h` — `IQdspEngine` interface + `Reply` (carries the `rex_set_sigs` completion bit).
- `qdsp5_dispatcher.{h,cpp}` — `classify()` + routing; `feed_raw()` is the future intercept point
  for a `UC_HOOK_CODE` at `PACKET_CONSUMER 0x16e8cb96`.
- `audpp_engine.cpp` — **real body**: decodes a provisional `audpp_cmd_play`, follows the PCM
  pointer via `QdspGuest`, feeds the existing `UnifiedAudioSink`.
- `stub_engines.cpp` — JPEG/VFE/VOICE honest stubs (ack + log, no fake decode).
- `qdsp5_smoke.cpp` — builds an ONCRPC packet (440Hz sine in fake guest RAM), routes it through
  the dispatcher, writes `qdsp5_out.wav`.

Verified: `make -f qdsp5/Makefile.qdsp5 test` → 3/3 PASS, `qdsp5_out.wav` = valid RIFF PCM16
stereo 44100Hz, peak 11999 (non-silent). Maps to TODO: **Q1.0** (single dispatcher — but two
bridges still to merge), **Q1.2** (intercept class skeleton), **Q1.3** (WAV sink) skeletoned;
**Q0.1/Q0.2** (real proc IDs), **Q1.4** (wire completion sig into real emulator) still open.

---

## 11. Detalhamento pós-decompilação — esqueleto + implementação planejada (ATUAL)

> Base VA do dump AMSS = **0x163a8000** (provada por 10/10 ponteiros de nome). Bate com o
> mapeamento do emulador: Core1/AMSS em **0x16e00000** (`zeebo_lle_main.cpp` L494) — logo todo
> VA abaixo é lido ao vivo por `uc_mem_read(core1_.uc, ...)` sem recomputar base.
> ARMv6/ARM1136 (sem movw/movt — endereços via literal pool / dados inline).

### 11.1 Constantes agora FIXADAS (substituem os placeholders de §1/§3/§7)

| Item | Valor antigo (morto) | Valor VERIFICADO | Prova |
|------|----------------------|------------------|-------|
| Program ID | `0x30000060` (fabricado) | **AUDMGRPROG 0x30000013** | adjacência à str "unable to register (AUDMGRPROG...)" @ file 0xeea091 + 0x102bf48 |
| Callback prog | — | **AUDMGRCB 0x31000013** | padrão 0x3100xxxx, 8B após |
| Proc ID | `0x1b59` (era success-code) | **ordinais AUDMGR 0..9** | apis table @ file 0xfab58c, stride 0x14 |
| Serviço | "MSM Audio/QDSP" | **AUDMGR + ADSP_RTOS** | `audmgr_svc.c`, `audmgr_xdr.c`, `snd_svc.c` |

Ordinais AUDMGR (índice do array = proc ID):
`0 null · 1 enable_client · 2 disable_client · 3 suspend_event_rsp ·
4 register_operation_listner · 5 unregister_operation_listner · 6 register_codec_listener ·
7 get_rx_sample_rate · 8 get_tx_sample_rate · 9 set_device_mode`.
Ressalva de auditoria: a apis table contém "audmgr_svc.c" (filename) → é ordem-de-fonte,
"provavelmente = ordem de proc" mas NÃO 100% amarrada à struct de registro ONCRPC. Q0.1
confirma no primeiro pacote real.

### 11.2 Wire-format do AUDMGR — structs recuperadas do serializador (§6c de proc_ids)

Serializador Thumb localizado (via ERR `audmgr_xdr.c:213` referenciada em VA 0x16e42654):
- **union `xdr_audmgr_server_data_s` @0x16e42546**: `switch(disc∈{0,1,5,6})`, cada braço 1×u32
  via vtable `[xdr+8]->[+0x58|+0x60]` ⇒ wire = `{ u32 disc; u32 value; }` (8B).
- **leafA @0x16e425aa**: `{ u32 @+0; u32 @+4 }` (helper veneer 0x16e9c710).
- **leafB @0x16e425d0**: `{ u8 @+0; opaque[4] @+4 }` (helper 0x16e9b114).
- **leafC @0x16e425fa**: `{ u8 present; <leafA> }` · **leafD @0x16e42626**: `{ u8 present; <leafB> }`.
- Tabela dispatch proc→xdr em VA **0x17422ae4** (array de veneers `ldr pc,[pc,#-4]` = 0xe51ff004).

LIMITE HONESTO: casar `set_device_mode=9` ao SEU leaf exige atravessar os veneers de import
dessa tabela — cadeia não fechada. Os leaf-layouts são certos; a atribuição leaf↔proc, não.
⇒ `audpp_cmd_play` **permanece HIPÓTESE** no código até Q0.1 capturar 1 pacote real.

### 11.3 Estado REAL das fases (corrige os ⛔ obsoletos de §5)

| Fase | Status antigo | Status ATUAL | Onde |
|------|---------------|--------------|------|
| Q0.1 hook | ⛔ | ✅ **construído**, drop-in header-only, teste offline PASS | `qdsp5_capture_hook.h` + `qdsp5_capture_test.cpp` |
| Q0.2 proc IDs | ⛔ | ✅ **resolvido estaticamente** (sem boot) | §10, §11.1 |
| Q0.3 cross-ref | ⛔ | ✅ feito (grep audmgr/snd/adsp_rtos) | `notes/qdsp5_proc_ids.md` |
| Q1.1 struct layout | ⛔ | 🟡 wire-format do AUDMGR mapeado; leaf↔proc aberto | §11.2 |
| Q1.2 intercept class | ⛔ | 🟡 skeleton (`Qdsp5Dispatcher::feed_raw`) | `qdsp5_dispatcher.cpp` |
| Q1.3 WAV sink | ⛔ | ✅ skeleton verde (WAV não-silencioso) | `audpp_engine.cpp` |
| Engines | 4 (§2) | **6**: +AUDPLAY0-4, +AUDREC | `stub_engines.cpp` |

### 11.4 Trabalho que a decompilação DESTRAVOU e ainda falta (novo, detalhado)

Estas sub-tarefas só puderam ser escritas porque agora conhecemos o wire-format:

- [ ] **Q0.1b — instalar o hook de verdade.** `install_capture_hook(core1_.uc)` (1 linha, após
      criar core1_.uc no orchestrator). BLOQUEADO por cortesia: outro agent edita a base. Quando
      liberado, ele emite `prog/proc/len/payload@+0x80` no primeiro RPC AUDMGR real.
- [ ] **Q1.1a — fechar leaf↔proc.** Desassemblar a tabela de dispatch VA 0x17422ae4 atravessando
      os veneers `0xe51ff004` para amarrar cada ordinal ao seu serializador de arg. Alternativa
      mais barata: 1 pacote de Q0.1b confirma o layout de `set_device_mode` empiricamente.
- [ ] **Q1.1b — mapear set_device_mode args.** Hipótese atual (marcada [infer] no capture test):
      `{ u32 device; u32 sample_rate }` (leafA). Ex. observado no teste sintético:
      device=2, rate=0xac44=44100. Confirmar contra pacote real.
- [ ] **Q1.2a — classify() para os 6 engines.** Hoje só AUDMGR→Audpp. Falta rotear AUDPLAY0-4
      (decode de bitstream — quem o jogo alimenta) e AUDREC. Precisa dos program IDs das outras
      tasks (AUDMGR é o gerente; AUDPLAY/AUDPP têm caminho próprio `AUDPP_HOST_PCM_AUDMGR_CONFIG`).
- [ ] **Q1.2b — extrair program IDs de AUDPLAY/ADSP_RTOS** do firmware (mesmo método de §11.1:
      constante 0x3000xxxx adjacente à string de registro). AUDMGR sozinho não carrega PCM.
- [ ] **Q1.4a — bit de sinal do AUDPP-done.** `rex_set_sigs` @0x1730f2aa; a máscara que o thread
      de áudio espera (§6 cita 0x00180000 no event-pump) precisa ser confirmada por Q0.1b
      (observar o `rex_wait` mask do thread que emite o RPC). Sem isso, WAV sai mas o jogo trava.
- [ ] **Q1.0 — fundir as duas bridges** (`VirtualSMDBridge`::inject_oncrpc_packet vs
      `UnifiedSMDBridge`::inject_packet) — pré-requisito ANTES de plugar o intercept real, senão
      correções divergem. Adiado por conflito com o outro agent na base principal.

### 11.5 Arquivos novos desta rodada (isolados — NÃO tocam a base do outro agent)

- `tools/cpp/qdsp5/qdsp5_capture_hook.h` — hook Q0.1 drop-in (UC_HOOK_CODE @0x16e8cba0);
  parser ONCRPC + decode-palpite [infer]; no-op sem Unicorn.
- `tools/cpp/qdsp5/qdsp5_capture_test.cpp` — prova o parser offline (sem boot). PASS.
- `stub_engines.cpp` — +`AudplayEngine` (AUDPLAY0-4), +`AudrecEngine` (array QDSP5 completo).
- `nand/xdr_trace.py`, `nand/find_movwt.py` — ferramentas de disassembly reutilizáveis (capstone).

Verificado agora: `make -f qdsp5/Makefile.qdsp5 test` → smoke (WAV PCM16 stereo 44100 peak 11999)
+ capture parser, **ambos PASS**.
