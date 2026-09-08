# QDSP5 — Real IDs recovered from firmware (no boot needed)

Source: static extraction from `nand/1.1.2_AMSS.bin` (21MB) + `nand/1.1.2_APPS.bin` (22MB).
Method: `strings` for the ONCRPC service/proc names, then a 4-byte scan for `0x3000xxxx`
program constants correlated by **byte-adjacency to the service-name string**. This unblocks
QDSP5_TODO Q0.2 WITHOUT booting the firmware.

## 1. The audio path is AUDMGR + ADSP_RTOS + AUDPLAY — NOT a single "QDSP service"

The earlier skeleton's `program = 0x30000060` was fabricated. The real audio ONCRPC service is
**AUDMGR** (`audmgr_svc.c`, `audmgr_xdr.c`, `audmgr.c`).

- **AUDMGRPROG  = `0x30000013`**  — VERIFIED: the literal string
  `"unable to register (AUDMGRPROG, AUDMGRVERS, sm)."` sits immediately after the `0x30000013`
  word at file offset 0x0102bf44; the `0x30000013` word recurs before `"audmgrcb_null:"`.
- **AUDMGRCB (callback) = `0x31000013`**  — VERIFIED: `0x31000013` appears 8 bytes later next to
  the `audmgrcb` string. Standard Qualcomm pairing (server `0x3000xxxx` / callback `0x3100xxxx`).
- `0x30000060` — NO string correlation anywhere. Discard.

## 2. AUDMGR procedure set (names verified; ordinals = order in the .x, to confirm)

From `*_0` client/server symbols in AMSS+APPS. ONCRPC assigns proc numbers by declaration order;
names are certain, the numeric ordinal still needs the xdr order confirmed:

    audmgr_null                         (proc 0, conventional)
    audmgr_enable_client
    audmgr_disable_client
    audmgr_suspend_event_rsp
    audmgr_register_operation_listner
    audmgr_unregister_operation_listner
    audmgr_register_codec_listener
    audmgr_set_device_mode
    audmgr_get_rx_sample_rate
    audmgr_get_tx_sample_rate
    (+ callback types: audmgr_cb_func_ptr_type, audmgr_codec_lstr_func_ptr_type,
       audmgr_opr_lstnr_cb_func_ptr_type)

## 3. QDSP5 task array is LARGER than session 3a-3c recorded

Session 3c claimed "the entire multimedia DSP task array" = VOICE/VFE/JPEG/AUDPP. The firmware
assertion strings prove that was INCOMPLETE. Full set (all from `cmd_size <= QDSP_*` assertions):

    QDSP_AUDPPTASK      UPAUDPPCMD1QUEUE / CMD2QUEUE / CMD3QUEUE   (3 queues, not 1)
    QDSP_AUDPLAY0TASK   UPAUDPLAY0BITSTREAMCTRLQUEUE
    QDSP_AUDPLAY1TASK   UPAUDPLAY1BITSTREAMCTRLQUEUE
    QDSP_AUDPLAY2TASK   UPAUDPLAY2BITSTREAMCTRLQUEUE
    QDSP_AUDPLAY3TASK   UPAUDPLAY3BITSTREAMCTRLQUEUE
    QDSP_AUDPLAY4TASK   UPAUDPLAY4BITSTREAMCTRLQUEUE   (5 decoder slots!)
    QDSP_AUDRECTASK     UPAUDRECBITSTREAMQUEUE / UPAUDRECCMDQUEUE
    QDSP_JPEGTASK       UPJPEGACTIONCMDQUEUE / UPJPEGCFGCMDQUEUE
    QDSP_VFETASK        VFECOMMANDQUEUE / SCALEQUEUE / TABLEQUEUE
    QDSP_VOICEPROCTASK  UPVOCPROCQUEUE

Key architecture correction: **AUDPLAY0-4 are the real PCM/bitstream decoder tasks** a game
feeds (5 concurrent voices/decoders); AUDPP is the post-processor (EQ/mix) DOWNSTREAM of them.
`audplaycmd.c`, `gsbitstream.c` hold the command builders. `AUDPP_HOST_PCM_AUDMGR_CONFIG` is the
host-PCM injection path (ARM streams PCM → AUDPP directly). Decoder state machine:
`AUDPLAY_STATE_RESET → ADSPRTOS_ACTIVE → AUDPP_ACTIVE → ACTIVE` per decoder (from APPS strings).

## 4. ADSP_RTOS = the QDSP5 image loader/enabler (ARM11 side)

`adsp_rtos_enable/disable(module)`, `adsp_rtos_modem_to_app` callback, `ADSP_RTOS_MOD_READY`,
`ADSP_RTOS_CMD_SUCCESS/FAIL`. This is how the ARM boots a QDSP5 task image before feeding its
queue. Any real audio needs the enable→MOD_READY handshake modeled (or short-circuited).

## 5. What is now solid vs still open

SOLID (primary-source): AUDMGRPROG 0x30000013, AUDMGRCB 0x31000013, the full proc name list,
the full QDSP5 task/queue array, the decoder state machine names, the host-PCM config path.

STILL OPEN: exact proc ORDINALS (need xdr decl order — decode `audmgr_xdr.c` region or the
apis table), and the byte layout of `audmgr_set_device_mode` / AUDPLAY bitstream cmd payloads
(need the xdr_* serializers disassembled, or one captured live packet to confirm).

## 6. Ordinais AUDMGR — FIXADOS por desassembly da apis table (VERIFICADO)

Método: a ONCRPC apis table fica no arquivo em `0xfab58c` (`1.1.2_AMSS.bin`). Base VA
resolvida = **0x163a8000** (10/10 ponteiros de nome batem). Stride = 0x14 (20 B/entry),
`entry[0] = name_ptr`; **o índice do array É o número da proc**. Leitura direta:

    proc 0  audmgr_null
    proc 1  audmgr_enable_client
    proc 2  audmgr_disable_client
    proc 3  audmgr_suspend_event_rsp
    proc 4  audmgr_register_operation_listner
    proc 5  audmgr_unregister_operation_listner
    proc 6  audmgr_register_codec_listener
    proc 7  audmgr_get_rx_sample_rate
    proc 8  audmgr_get_tx_sample_rate
    proc 9  audmgr_set_device_mode

CORREÇÃO: meus ordinais anteriores estavam ERRADOS (chutei set_device_mode=7,
get_rx=8, get_tx=9). O real é get_rx=7, get_tx=8, set_device_mode=9. Código corrigido.

## 6b. Layout de bytes do set_device_mode — AINDA NÃO fixado (honesto)

Tentei extrair o layout. Resultado negativo verificável: o campo `w3` da apis entry
(0xed,0x10a,...,0x1f2) indexa a TABELA DE STRINGS DE ERRO, não um descritor xdr — logo o
layout dos args NÃO está numa tabela de dados; está COMPILADO no serializador genérico
`xdr_audmgr_send/recv_audmgr_server_data_s` (código Thumb). Os símbolos de struct
(`audmgr_set_device_mode_args`, `audmgr_device_info_type`) estão STRIPPED do dump.
Conclusão honesta: fixar o layout exige (a) desassemblar o serializador Thumb inteiro
seguindo os `xdr_u32`/`xdr_enum`, ou (b) 1 pacote capturado ao vivo. Não inventei o struct.
O `audpp_cmd_play` no skeleton continua explicitamente marcado como HIPÓTESE.

## 6c. Serializador xdr DESASSEMBLADO — layouts do wire recuperados (VERIFICADO)

Fiz o trabalho chato. O serializador NÃO usa literal pool p/ o msg-const (ARMv6/Thumb com
`ldr [pc]`); localizei-o pela linha de ERR: o msg-const de "send switch" (`audmgr_xdr.c`
linha 0xd5=213) é referenciado do código em VA **0x16e42654**. A função de união fica logo
acima. Desassemblado com capstone (scripts em `nand/xdr_trace.py`, `nand/find_movwt.py`).

Achados diretos do código (offsets = campos reais das structs; sizes = xfer do xdr):

- **union `xdr_audmgr_server_data_s` @0x16e42546**: `switch(disc)` com disc ∈ {0,1,5,6};
  cada braço serializa UM u32 via vtable `[xdr+8]->[+0x58]` ou `[+0x60]` (x_getlong/putlong).
  => wire = `{ u32 disc; u32 value; }` (8 bytes). Default → ERR "can't switch on %d".
- **leafA @0x16e425aa**: `{ u32 @+0; u32 @+4; }` — 2× xdr_u_long (helper veneer 0x16e9c710).
- **leafB @0x16e425d0**: `{ u8 @+0; opaque[4] @+4; }` — xdr byte + bloco 4B (helper 0x16e9b114).
- **leafC @0x16e425fa**: `{ u8 present @+0; <leafA> }` — leafA prefixado por bool/presença.
- **leafD @0x16e42626**: `{ u8 present @+0; <leafB> }` — leafB prefixado por bool/presença.
- Tabela de dispatch proc→xdr localizada em VA **0x17422ae4** (array de veneers `ldr pc,[pc,#-4]`).

LIMITE HONESTO que permanece: casar CADA proc (ex.: set_device_mode=9) ao SEU serializador de
arg exige atravessar os veneers de import (`0xe51ff004`) dessa tabela — cadeia longa que ainda
não fechei. Os leaf-layouts acima são certos; a atribuição leaf↔proc específica NÃO está 100%
amarrada. Portanto: NÃO promovi `audpp_cmd_play` a fato — continua HIPÓTESE no código. O que
subiu de hipótese p/ fato: os ordinais (§6) e as structs-de-wire do AUDMGR (§6c).

## 7. Q0.1 — hook de captura ao vivo (DROP-IN, pronto p/ o outro agent plugar)

Arquivo: `tools/cpp/qdsp5/qdsp5_capture_hook.h` (header-only, ISOLADO — NÃO altera a base).
Teste offline: `tools/cpp/qdsp5/qdsp5_capture_test.cpp` (passa sem Unicorn e sem boot).

Fato-chave que torna o hook trivial: o orchestrator mapeia **Core1 (AMSS) em VA 0x16e00000**
(`zeebo_lle_main.cpp` linha 494) — o MESMO espaço do meu disassembly. Logo dispatcher
(0x16e8cba0) e consumer (0x16e8cb96) são lidos direto por `uc_mem_read(core1_.uc, ...)`.
O header ONCRPC do emulador (`oncrpc_packet_header`) bate 1:1 com meus offsets
(program@+0x0C, procedure@+0x14, payload@+0x80).

INTEGRAÇÃO (1 linha no orchestrator, após criar core1_.uc — quando o outro agent quiser):
    #include "qdsp5/qdsp5_capture_hook.h"
    zeebo::qdsp5::install_capture_hook(core1_.uc);
Compila como no-op se Unicorn não estiver incluído (não força dependência no smoke).

O que o hook emite por pacote: `[Q0.1] RPC prog=... proc=N(nome) len=... payload@+0x80: ...`,
com decode-palpite dos args de set_device_mode sob a struct-de-wire do §6c (marcado [infer]).
=> Quando o boot emitir 1 RPC AUDMGR real, isto CONFIRMA (ou refuta) o layout e fecha o
último elo do §6c/§6b, promovendo `audpp_cmd_play` de HIPÓTESE a fato — sem chute.

## 8. Skeleton completo — cobertura do array QDSP5 inteiro

`stub_engines.cpp` agora cobre TODAS as tasks do §3: JPEG, VFE, VOICE, **AUDPLAY0-4**
(1 engine `AudplayEngine`), **AUDREC**, além do AUDPP com corpo real. Dispatcher roteia as 6.
Build: `make -f tools/cpp/qdsp5/Makefile.qdsp5 test` → smoke (WAV) + capture parser, ambos PASS.
