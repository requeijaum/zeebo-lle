# Handoff p/ o agente da base LLE — estado do boot (2ª análise, boot noturno)

Log: /tmp/lle_boot_night.log (250 cycles). Hook Q0.1 plugado OK (consumer=0x16e8cb96
dispatcher=0x16e8cba0), ainda 0 pacotes de áudio — porque o Core0 não alcança a emissão
de RPC e o Core1 crasha. Dois gargalos NOVOS, ambos na base (L4/Iguana/AMSS):

## PROGRESSO (bom) — vs. boot anterior
- Core0 PASSOU do IPC L4 0xb0003404 (que travava antes) e do BSS-zero 0xb0000028. ✓
- Commit "resolve boot Iguana e transição LLE até vetor BREW/AEECShell (0x10c874f4)" surtiu efeito:
  rodou 250 cycles, avançou muito mais.

## GARGALO 1 — Core0 preso em loop 0xb000d4a8..0xb000d4bc (cycle 20..fim)
Desassembly (APPS.bin, ARM):
```
0xb000d494: bl   0xb000c720        ; r0 = descriptor (mesma fn dos travamentos anteriores)
0xb000d498: ldr  r3, [r0, #0xc8]
0xb000d49c: bic  r2, r3, #0x3fc
0xb000d4a0: bic  r2, r2, #3        ; r2 = campo mascarado
0xb000d4a8: ldr  r3, [r4]          ; \
0xb000d4ac: add  r3, r3, #1        ;  > contador: incrementa [r4]
0xb000d4b0: str  r3, [r4]          ; /
0xb000d4b4: tst  r2, #1            ; conta bits setados de r2
0xb000d4b8: lsr  r2, r2, #1
0xb000d4bc: beq  0xb000d4a8        ; loop enquanto bit==0
```
DIAGNÓSTICO: popcount/scan de bits de r2, que vem de `[descriptor+0xc8]`. O pc oscila só
entre d4a8/b0/b8 do cycle 20 ao 240 => r2 nunca chega ao estado esperado, OU é um
poll/delay loop aguardando um bit de status que a base ainda não seta. r0 vem de
`bl 0xb000c720` — a MESMA função de descriptor que retornava struct meio-inicializada nos
travamentos anteriores. Vale inspecionar o que 0xb000c720 devolve e o campo +0xc8.

## GARGALO 2 — Core1/AMSS crash: pc=0x00fffffe UC_ERR_FETCH_UNMAPPED (cycle ~160)
O Core1 NUNCA executou o REX de verdade: desde o 1º cycle o pc sobe LINEAR ~0x9c40/cycle
(0x00a09c40, 0x00a13880, 0x00a1d4c0, ...) — isso é o Unicorn executando dados/zeros como
código, sem nenhum branch. Aos ~1.57M insns bateu no teto **0x01000000** (fim do seg13 de
código AMSS, VA 0x00b1a000..0x014d5000) e caiu em fetch unmapped, congelando.
DIAGNÓSTICO: o AMSS não está sendo executado a partir de um reset/entry válido com o RTOS
escalonando; está "correndo solto" linearmente desde 0x00a00000. Provável: falta o setup de
stack/vetores/MMU do modem, ou o entry 0x00a00000 deveria pular p/ init do REX e não o faz.
Enquanto o Core1 não roda o REX, o consumer ONCRPC 0x16e8cb96 nunca é atingido mesmo que o
Core0 emita o pacote.

## Impacto p/ QDSP5 (meu lado — pronto e aguardando)
Cadeia de dependência p/ capturar 1 pacote de áudio:
  Core0 sai do loop d4a8  ->  sobe som do BREW/AEECShell  ->  emite RPC AUDMGR 0x30000013
  ->  Core1 (REX rodando) recebe no consumer 0x16e8cb96  ->  hook Q0.1 loga tudo.
Hoje quebram os passos 1 (loop d4a8) e 3 (Core1 não roda REX). Os dois são da base.
Nada a fazer no qdsp5/ até isso: hook, base VA (0x16e00000), parser e state machine já prontos.
