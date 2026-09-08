# QDSP5 — Fatos-âncora para subagents paralelos (VERIFICADOS)

Binários (dump de flash real, NÃO bootar):
- ~/projects/zeebo-lle/nand/1.1.2_AMSS.bin  (21 MB, ARM9/AMSS firmware)
- ~/projects/zeebo-lle/nand/1.1.2_APPS.bin  (22 MB, ARM11/apps)

Base VA do dump AMSS = 0x163a8000 (provada por 10/10 ponteiros de nome).
  file_offset = VA - 0x163a8000.  (0x163a8000 == emulador mapeia Core1 em 0x16e00000, mesmo espaço)
Arch: ARMv6 / ARM1136 — SEM movw/movt. Endereços vêm de literal pools (ldr rX,[pc,#imm]) ou dados inline.
Thumb: endereços de função com bit0=1 são Thumb; veneers de import = 0xe51ff004 (ldr pc,[pc,#-4]).

Constantes JÁ verificadas (não redescobrir):
- AUDMGRPROG = 0x30000013  (program ID do serviço de áudio; adjacente a "unable to register (AUDMGRPROG...)")
- AUDMGRCB   = 0x31000013  (callback prog)
- Ordinais AUDMGR (índice do array = proc): 0 null,1 enable_client,2 disable_client,3 suspend_event_rsp,
  4 register_operation_listner,5 unregister_operation_listner,6 register_codec_listener,
  7 get_rx_sample_rate,8 get_tx_sample_rate,9 set_device_mode.  (apis table @ file 0xfab58c, stride 0x14)

Serializador xdr AUDMGR (Thumb) — já localizado:
- union xdr_audmgr_server_data_s @ VA 0x16e42546 : switch(disc∈{0,1,5,6}); cada braço 1×u32
  via vtable [xdr+8]->[+0x58|+0x60].  wire = {u32 disc; u32 value} (8B).
- leafA @0x16e425aa : {u32 @+0; u32 @+4}          (helper veneer 0x16e9c710)
- leafB @0x16e425d0 : {u8 @+0; opaque[4] @+4}     (helper veneer 0x16e9b114)
- leafC @0x16e425fa : {u8 present; <leafA>}
- leafD @0x16e42626 : {u8 present; <leafB>}
- tabela dispatch proc->xdr @ VA 0x17422ae4 (array de veneers 0xe51ff004)
- ERR de "send switch" referenciada em VA 0x16e42654 = audmgr_xdr.c linha 213 (0xd5)
- helpers blx: 0x16e9c710, 0x16e9b114, 0x16e9af88

ONCRPC packet framing (verificado):
  +0x00 xid · +0x04 msg_type(0=CALL) · +0x08 rpc_ver(2) · +0x0c program · +0x10 version
  program +0x0C · version +0x10 · procedure +0x14 · payload body +0x80. max 0x500 (1280B). success code 0x1b59 (STATUS, não proc).

REX signaling (para Q1.4a):
- rex_set_sigs(tcb,mask) @ 0x1730f2aa (vector 0x16f80f0c)
- rex_wait(mask)         @ 0x1730f442 (vector 0x16f80f1c)
- event-pump loop 0x16ef0b02..1a chama rex_wait com mask 0x00180000, 100ms.

Ferramentas prontas: capstone 5.0.7, unicorn 2.1.2, arm-none-eabi-objdump. (todas no python3 do sistema)
Scripts existentes: nand/xdr_trace.py (desassembla VA Thumb), nand/find_movwt.py.

REGRA ABSOLUTA: NÃO editar a base principal do emulador (tools/cpp/zeebo_lle_main.cpp,
tools/cpp/Makefile, tools/cpp/zeebo_smd_bridge.cpp) — outro agent está editando. Todo output novo
vai em tools/cpp/qdsp5/ (isolado) ou notes/ ou nand/*.py.
