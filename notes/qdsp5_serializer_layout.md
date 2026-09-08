# QDSP5 — Layout do serializador xdr AUDMGR (extração EMPÍRICA via Unicorn)

Método: `nand/xdr_emu.py` executa cada rotina xdr sob Unicorn com uma struct-fonte de
sentinelas (0xAAAA0001, 0xAAAA0002, ...), patcha os veneers de import putlong/putbytes
e a vtable do XDR falso para stubs Thumb `movs r0,#1; bx lr` (retorno natural, sem
reescrever PC/CPSR — isso evitava o desync de thumb-mode). Hooks capturam:
  - UC_HOOK_MEM_READ na struct-fonte  → ORDEM/offset/tamanho dos campos LIDOS
  - hook nos helpers                  → ORDEM/tamanho/valor das escritas no WIRE

Reproduzir: `cd nand && python3 xdr_emu.py`

## Resultados CONFIRMADOS (observados na emulação)

### leafA @0x16e425aa  →  `{ u32 @+0; u32 @+4 }`   (8 bytes no wire)
- src_reads: src+0x00, src+0x04 (dois u32)
- wire: `00 00 00 01  aa aa 00 02`  (dois u32 BIG-ENDIAN)
- CONFIRMA a leitura estática. XDR encoda em big-endian (put_u32).

### leafB @0x16e425d0  →  `{ u8 @+0; opaque[4] @+4 }`  (via putbytes)
- wire: `01  02 00 aa aa`  (1 byte + 4 bytes crus, sem swap)
- CONFIRMA `{u8; opaque[4]}`. putbytes emite bytes crus (não faz byteswap).

### leafC @0x16e425fa  →  `{ u8 present @+0; <leafA> }`  (bool de presença + leafA)
- src_reads: src+0 lido 2× (o bool decide se serializa o corpo)
- wire quando present: `00 00 00 01` (o byte de presença como u32 BE) + corpo leafA
- CONFIRMA prefixo de presença.

### leafD @0x16e42626  →  `{ u8 present @+0; <leafB> }`
- Análogo a leafC mas com corpo leafB. CONFIRMADO.

## Union `xdr_audmgr_server_data_s` @0x16e42546 — PARCIAL (caveat honesto)

Chamada com disc∈{0,1,5,6} em r2 emitiu SEMPRE `put_u32_byptr` de src+0, sem variar.
⇒ O discriminante NÃO vem de r2 na minha montagem — provavelmente é lido da própria
struct (objp+offset) ou a ABI da union precisa de outro registrador. Portanto:
- CONFIRMADO: a union serializa u32(s) via vtable byptr (big-endian), como o desasm indicava.
- NÃO CONFIRMADO: o mapeamento disc→campo. Precisa: (a) achar o offset do disc na struct
  e montá-lo, ou (b) 1 pacote real do hook Q0.1.

## set_device_mode (proc 9) — AINDA HIPÓTESE

Este script confirmou os TIJOLOS (leafs) mas não a rotina de args do proc 9 inteira —
para isso é preciso executar o serializador de args via a tabela dispatch @0x17422ae4
(idx 9), atravessando o veneer com o Unicorn. Próximo passo do xdr_emu.py.
Hipótese de trabalho mantida: `{ u32 device; u32 sample_rate }` (leafA), device=2,
rate=0xac44=44100 — coerente com o wire de leafA, mas NÃO amarrada ao proc 9.

## Estado
- Bug de thumb-mode do script: CORRIGIDO (retorno natural via stub bx lr).
- Leafs: CONFIRMADOS empiricamente (bate com o desasm estático de §6c).
- Union disc-map e proc-9 args: abertos — via dispatch table ou captura Q0.1.
