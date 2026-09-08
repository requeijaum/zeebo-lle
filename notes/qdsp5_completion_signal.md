# QDSP5 — Sinal de conclusão AUDPP-done (rex_set_sigs / rex_wait)

Status: PARCIAL. A máscara e os handlers foram decodificados; o bit exato = "áudio done"
fica como CANDIDATO ranqueado (não amarrado a call site de áudio inequívoco). Análise
estática (capstone) interrompida por timeout de rede; achados abaixo são do disassembly real.

## Veneers resolvidos (VERIFICADO)
- rex_set_sigs → veneer de entrada **0x16f80f04**
- rex_wait     → veneers **0x16f80f14** e **0x16611edc**
- Muitos callers usam `ldr rX,[pc]; blx rX` (ponteiro via pool), não BLX direto — por isso
  a varredura por BLX direto acha poucos call sites. Uma varredura completa precisa seguir
  também os pool-loads (trabalho restante).

## Event-pump (VERIFICADO) — 0x16ef0afe
- `movs r7,#3; lsls r7,#0x13` → máscara = **3<<19 = 0x00180000** (bits **19** e **20**).
- Após rex_wait retornar os sinais em r5, o pump testa:
  - **bit 19** (`lsls r5,#0xc; bpl`) → chama **0x16ef2888**
  - **bit 20** (`lsls r5,#0xb; bpl`) → chama **blx 0x16f80ef4**

## Handlers (VERIFICADO parcialmente)
- bit 19 → 0x16ef2888: termina em **loop infinito** após chamar 0x17138c1a ⇒ caminho
  FATAL/erro (provável assert/reset), NÃO o caminho normal de áudio.
- bit 20 → veneer 0x16f80ef4: roteia p/ um handler **normal**.

## Conclusão (CANDIDATOS ranqueados — NÃO verificado como áudio)
1. **bit 20 (0x00100000)** — candidato mais forte a "processamento normal / done": é o
   ramo não-fatal do pump. Mas o pump é um WATCHDOG PERIÓDICO, não o cliente de áudio.
2. **bit 19 (0x00080000)** — caminho fatal; improvável ser "done" (mais p/ erro do DSP).

CAVEAT HONESTO (a virada que o próprio subagent registrou): o pump é um watchdog; o
cliente que faz `rex_wait` esperando a conclusão do RPC de áudio é OUTRO thread, provavelmente
no módulo de áudio (região ~0x1642xxxx dos rex_wait callers). O bit de "AUDPP-done" real
deve ser o que ESSE cliente aguarda — ainda não isolado.

## Trabalho restante p/ fechar
- Seguir os `ldr rX,[pc]; blx rX` para achar TODOS os call sites de rex_set_sigs (não só BLX direto).
- Desassemblar os rex_wait callers em ~0x1642xxxx (módulo de áudio) e ler a máscara que esperam.
- Cruzar com strings audmgr_*/audpp/snd_svc.c próximas.
- OU: capturar ao vivo com o hook Q0.1 (observar o mask do rex_wait do thread que emite o RPC de áudio) — caminho mais barato e definitivo.

## ADENDO 2026-09-07 — 2ª tentativa (mapeamento ELF + execução/dump)
CORREÇÃO DE BASE (importante — contaminava scans anteriores): o código de áudio do AMSS
NÃO está em base 0x163a8000. Program headers do ELF (nand/1.1.2_AMSS.bin, 18 PT_LOAD):
  - seg15: file 0xa58000 -> VA 0x16e00000, size 0x75d000  ← TODO o código de áudio/RPC
  - strings audmgr.c/audmgr_client mapeiam em VA 0xb5a33c/0xb5a356 (seg de dados baixo)
  - snd.c VA 0x173674cc, audmgr_svc.c VA 0x172920c2
  Validação: serializador conhecido 0x16e42546 -> file 0xa9a546 EXATO; pump 0x16ef0afe -> file 0xb48afe.

VALIDADO na base certa (capstone): pump @0x16ef0af8..0b0a:
  movs r6,#0x7d; lsls r6,#3; muls; movs r7,#3; lsls r7,#0x13 (=0x00180000);
  movs r0,r7; movs r1,r7; blx 0x16f80f14 (rex_wait). Disassembly do subagent CONFIRMADO byte a byte.

DIAGNÓSTICO DEFINITIVO (por que estático não fecha): os veneers rex_set_sigs/rex_wait
(0x16f80f04/0x16f80f14) estão ZERADOS no file — preenchidos em RUNTIME pelo loader RPC.
Os call-sites usam `ldr rX,[pc]; blx rX` com pool-const também runtime-filled; só existe
1 blx imediato p/ rex_wait no segmento inteiro. => seguir todos os rex_wait exige estado RODANDO.

EXECUÇÃO/DUMP tentada: `./zeebo_lle_main --headless` roda, mas Core0 trava em spin de
bootinfo (pc=0xb0000028-30) e Core1/AMSS só marcha linear; o boot NÃO alcança init de áudio.
Logo, nem veneers preenchem nem RPC de áudio real ocorre — MESMA causa de o hook Q0.1 nunca
capturar. O smd_bridge tem inject_oncrpc_packet() mas (a) usa program FABRICADO 0x30000060 e
proc 0x1b59 (status code, não proc), (b) não carrega ELF nem executa (sem uc_emu_start) — não serve.

STATUS: bit AUDPP-done continua NÃO-AMARRADO. A causa raiz agora é conhecida e é uma
dependência de RUNTIME que o boot atual (outro agent) não produz — não é limite de técnica estática.
CAMINHOS REAIS p/ fechar: (1) boot avançar até init de áudio + hook Q0.1 observar o mask do
rex_wait do thread que emite o RPC; (2) harness dedicado que carregue o AMSS ELF, injete um
pacote AUDMGR 0x30000013 REAL na fila e execute Core1 até processar — caro, mas isolado da base.
