# QDSP5 — INDEX de dados + ferramental (estado consolidado 2026-09-07)

Ponto único de entrada. Todos os fatos abaixo são VERIFICADOS contra binário/emulação real,
salvo marca HIPÓTESE/[infer]/CANDIDATO. Binários: `nand/1.1.2_AMSS.bin` (ARM9), `nand/1.1.2_APPS.bin` (ARM11).

## O caminho de áudio (fato central, reescreve premissas antigas)
    game/AUDMGR(0x30000013)  → abre sessão/codec  (NÃO carrega PCM)
          ▼
    ADSPRTOSATOM(0x3000000a, proc adsp_rtos_app_to_modem_command)  → escreve command queue do DSP
          ▼
    QDSP5 task AUDPP  → fila UPAUDPPCMDxQUEUE          (mixer / HOST_PCM feed)
    QDSP5 task AUDPLAYx → fila UPAUDPLAYxBITSTREAMCTRLQUEUE  (bitstream/PCM decode)
          ▼
    ADSPRTOSMTOA(0x3000000b)  → callback ARM (buffer consumido)  ⇒ dispara rex_set_sigs (Q1.4)
State machine host-PCM (audpphostpcm.c): AUDMGR_CONFIG → HPCM_ACTIVE → AUDPP_ACTIVE → RESET.

## Program IDs ONCRPC de áudio (VERIFICADOS, dist≤32 a string de stub .c)
    0x3000000a ADSPRTOSATOM  app→modem  ← INJETA PCM/comandos (o programa que importa)
    0x3000000b ADSPRTOSMTOA  modem→app  ← callback de conclusão
    0x30000013 AUDMGR        gerente sessão/codec (âncora)
    0x31000013 AUDMGRCB      callback do AUDMGR
    NÃO existe AUDPLAYPROG/AUDPPPROG/AUDRECPROG — são tasks do DSP image, não RPC.
Ordinais AUDMGR (ordem de fonte): 0 null … 7 get_rx_rate, 8 get_tx_rate, 9 set_device_mode.

## Wire-format (desasm @0x16e42546 + EMPÍRICO via Unicorn — batem)
    union xdr_..._server_data_s @0x16e42546: {u32 disc; u32 value} = 8B, disc∈{0,1,5,6}
    leafA @0x16e425aa {u32; u32}   leafB @0x16e425d0 {u8; opaque[4]}   leafC/D prefixo byte-presença
    leafD @0x16e42627 → ptr tabela dispatch proc→xdr @VA 0x17422ae4
    ABERTO: disc→campo (union emite src+0 p/ toda disc: discriminante vem da struct, não de r2).
    HIPÓTESE proc 9 args: {device:u32, sample_rate:u32}  (leafA, [infer]).

## Base / endereços (VERIFICADOS)
    dump base VA 0x163a8000 (10/10 ptrs); ARMv6 ARM1136 (sem movw/movt).
    Core1/AMSS mapeado no emulador @0x16e00000 (zeebo_lle_main.cpp:494) ⇒ VAs batem p/ uc_mem_read.
    Dispatcher ONCRPC 0x16e8cba0; consumer 0x16e8cb96. Header: prog@+0x0C, proc@+0x14, payload@+0x80.

## Sinal de conclusão (PARCIAL — CANDIDATOS, não amarrado)
    máscara 0x00180000 = bits 19/20; handler bit19=fatal, bit20=normal.
    CAVEAT: o pump analisado é watchdog, não cliente de áudio. Bit real fica no rex_wait do
    módulo de áudio (~0x1642xxxx), disparado pelo callback ADSPRTOSMTOA. → completion_signal.md.

## Arquivos de dados (notes/)
    qdsp5_program_ids.md   — program IDs + caminho de PCM (task-1, fechado)
    qdsp5_serializer_layout.md — layout empírico via Unicorn (leafs confirmados)
    qdsp5_proc_ids.md      — §6 ordinais, §6c wire-format, §7 hook, §8 cobertura
    qdsp5_completion_signal.md — bits 19/20 CANDIDATOS + caveat watchdog
    qdsp5_fuzz_report.md   — 2 bugs reais corrigidos (OOM + overflow 1B)
    qdsp5_anchors.md       — fatos-âncora p/ subagentes não redescobrirem

## Ferramental (nand/ e tools/cpp/qdsp5/)
    nand/xdr_emu.py    — emula serializador xdr sob Unicorn (patch veneers→stub bx lr); REUTILIZÁVEL
    nand/xdr_trace.py, find_movwt.py, sig_scan*.py — scanners de constantes/strings
    tools/cpp/qdsp5/   — skeleton 6 famílias; make -f Makefile.qdsp5 test (smoke WAV + capture) → ALL PASS
    qdsp5_capture_hook.h — hook Q0.1 drop-in 1-linha: install_capture_hook(core1_.uc)
    make -f Makefile.qdsp5 fuzz — libFuzzer+ASAN (requer clang++)

## Estado de bloqueio
    audpp_cmd_play / args proc 9 / bit AUDPP-done = tudo fecha com 1 pacote real do hook Q0.1
    (aguardando o outro agent levar o boot até a init de áudio). Disassembly restante é caro;
    o hook é definitivo. Isolamento: só qdsp5/, notes/, nand/ — base do outro agent intacta.

## Progresso Frente B (IMPLEMENTADO 2026-09-07, sem bloqueio)
    B1 ✓ qdsp5_rpc.h: prog::ADSPRTOSATOM=0x3000000a, prog::ADSPRTOSMTOA=0x3000000b (com prova).
    B2 ✓ qdsp5_dispatcher.cpp classify(): 0x3000000a → Engine::Audpp (host-PCM); split
         Audpp/Audplay pelo campo de task fica p/ Frente A (sub-payload [infer]).
    B3 ✓ audpp_engine.cpp: state machine HostPcm (UNCONFIGURED→AUDMGR_CONFIG→HPCM_ACTIVE→
         AUDPP_ACTIVE→RESET), transições em PLAY/STOP, logadas.
    Verificação: make -f Makefile.qdsp5 test → ALL PASS; make fuzz → 194k runs, 0 crash,
    cov 134→146 (rotas novas cobertas). Isolamento confirmado (só qdsp5/notes/nand).
    PENDENTE Frente A: 1 pacote real fecha disc→campo da union + args proc 9 + split de task.
    PENDENTE Frente C: bit real de conclusão no rex_wait ~0x1642xxxx (não o watchdog 0x00180000).
