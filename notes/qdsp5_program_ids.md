# QDSP5 — Program IDs ONCRPC das tasks de áudio (transporte de PCM/bitstream)

Método: constantes u32 little-endian nos ranges `0x3000xxxx` (server prog) e `0x3100xxxx`
(callback prog), adjacentes à string de registro `unable to register (XXXPROG, XXXVERS, sm).`
ou ao nome do arquivo `.c` do stub RPC. **dist = str_offset - const_offset.** `|dist|<64` = VERIFICADO.
Binários: `nand/1.1.2_AMSS.bin` (ARM9/modem), `nand/1.1.2_APPS.bin` (ARM11/apps).

## Descoberta central (o achado mais valioso)

**AUDPLAY e AUDPP NÃO são programas ONCRPC.** São *tasks do firmware QDSP5* (imagem DSP),
alcançadas por **command queues** do DSP, não por RPC próprio. O ARM chega até elas por UM ÚNICO
serviço ONCRPC: **ADSP_RTOS**, que existe em duas direções (par server/callback):

| Direção | Serviço (stub .c) | PROG | dist prova | quem roda | papel |
|---|---|---|---|---|---|
| App→Modem (ARM manda comando/PCM p/ DSP) | `adsprtosatom_clnt.c` / `adsprtosatom_svc.c` | **0x3000000a** | -12 (clnt), +32/+16 (svc) | client=APPS, server=AMSS | **injeta comandos e PCM nas filas do QDSP5** |
| Modem→App (DSP notifica ARM) | `adsprtosmtoa_clnt.c` / `adsprtosmtoa_svc.c` | **0x3000000b** | -12/-32 | client=AMSS, server=APPS | callback de status/eventos do DSP |

O caminho real de PCM do jogo:
```
game/AUDMGR (0x30000013)  → habilita sessão/codec (NÃO carrega samples)
        │
        ▼
ADSPRTOSATOM (0x3000000a, proc adsp_rtos_app_to_modem_command)
        │   escreve na command queue do DSP:
        ▼
QDSP5 task AUDPP  →  fila QDSP_AUDPPTASK_UPAUDPPCMDxQUEUE   (mixer/pós-proc; HOST_PCM feed)
QDSP5 task AUDPLAYx → fila QDSP_AUDPLAYxTASK_UPAUDPLAYxBITSTREAMCTRLQUEUE  (bitstream/PCM decode)
        │
        ▼
ADSPRTOSMTOA (0x3000000b) → callback de volta ao ARM (buffer consumido, image swap etc.)
```

### Prova do caminho Host-PCM (`AUDPP_HOST_PCM_AUDMGR_CONFIG`)
Strings contíguas em APPS @ file 0x2373xx (~2324309–2324765), arquivo `audpphostpcm.c`:
- `audpp_host_pcm_write_req: Writing..`  ← ARM escreve PCM
- `audpp_host_pcm_intf_msg_cb: ARMTODSP` ← **mensagem ARM→DSP** (confirma que o feed vai via ADSP_RTOS ATOM)
- Máquina de estados: `AUDPP_HOST_PCM_AUDMGR_CONFIG` → `AUDPP_HOST_PCM_HPCM_ACTIVE` → `AUDPP_HOST_PCM_AUDPP_ACTIVE` → `AUDPP_HOST_PCM_RESET`
- `HostPCM: Audmgr is un-configured audmgr_config: %d` — AUDMGR só *configura codec/device*; o PCM em si passa pela fila AUDPP.

Filas AUDPP confirmadas (APPS @ file 6293690): `QDSP_AUDPPTASK_UPAUDPPCMD1/2/3QUEUE`.
Filas AUDPLAY confirmadas (APPS @ file 6295090+): `QDSP_AUDPLAY0..4TASK_UPAUDPLAYxBITSTREAMCTRLQUEUE`,
`QDSP_AUDRECTASK_UPAUDRECBITSTREAMQUEUE`, `QDSP_VOCDECTASK_RXMPUDECPKTQUEUE`.

> **Conclusão para o emulador:** para reproduzir áudio do jogo, interceptar as chamadas ONCRPC ao
> program **0x3000000a (ADSPRTOSATOM)** e decodificar o comando `adsp_rtos_app_to_modem_command`,
> cujo payload seleciona a task (AUDPP/AUDPLAY) e a queue destino. AUDMGR (0x30000013) só abre a
> sessão. O PCM host-fed passa por AUDPP via `audpp_host_pcm_write_req` → ATOM → fila do DSP.

---

## Tabela de program IDs de áudio e vizinhos (VERIFICADOS)

| program_id | nome_task/serviço | file_offset(const) | str_offset | dist | status | bin |
|---|---|---|---|---|---|---|
| 0x3000000a | ADSPRTOSATOMPROG (app→modem, **feed PCM**) | 14587744 | 14587756 (`adsprtosatom_clnt.c`) | +12→-12 | **VERIFICADO** | APPS |
| 0x3000000a | ADSPRTOSATOMPROG (server) | 14079016 | 14078984 (`adsprtosatom_svc.c`) | -32 | **VERIFICADO** | AMSS |
| 0x3000000b | ADSPRTOSMTOAPROG (modem→app cb) | 16583512 | 16583544 (`ADSPRTOSMTOAPROG` reg str) | +32 | **VERIFICADO** | APPS |
| 0x3000000b | ADSPRTOSMTOAPROG (client) | 11118704 | 11118776 (`adsprtosmtoa_clnt.c`) | ~-8 | **VERIFICADO** | AMSS |
| 0x30000013 | AUDMGRPROG (gerente de sessão/codec) | 16957256 | 16957268 | -12 | **VERIFICADO** (âncora) | AMSS |
| 0x31000013 | AUDMGRCBPROG (callback) | 4777052 | 4777064 | -12 | **VERIFICADO** | APPS |

Observação: **não existe `AUDPLAYPROG`/`AUDPPPROG`/`AUDRECPROG` como ONCRPC** — busca por essas
strings de registro retorna vazio nos dois binários. AUDPLAY/AUDPP/AUDREC são tasks do QDSP5 image,
endereçadas por command-queue via ADSP_RTOS. Isso é o ponto essencial: o "programa que carrega
samples" é **ADSPRTOSATOM 0x3000000a**, não um programa de nome "AUDPLAY".

---

## Catálogo ONCRPC completo (todos os `0x3000xxxx` com string de registro adjacente, dist=-12)

Enumeração das constantes cuja adjacência a `unable to register (XPROG...)` é VERIFICADA. Padrão
consistente: a const fica exatamente 12 bytes antes da string (a struct `{prog; vers; name_ptr;...}`).

### AMSS (server progs)
| prog | serviço | file_offset | dist |
|---|---|---|---|
| 0x30000000 | SMD_PORT_MGRPROG (base/router) | 11133412 | (também -51 em nomes) |
| 0x3000000a | ADSPRTOSATOMPROG | 14079016 | +32 |
| 0x3000000b | ADSPRTOSMTOAPROG | 11118704 | ~ |
| 0x30000010 | RDEVMAPPROG | 15968068 | -12 |
| 0x30000013 | AUDMGRPROG | 16957256 | -12 |
| 0x3000001f | SECUTILPROG | 16979796 | -12 |
| 0x30000024 | SMD_PORT_MGRPROG | 11133412 | -12 |
| 0x30000025 | BUS_PERFPROG | 14110540 | -12 |
| 0x3000003b | DIAGPROG | 15045304 | -12 |
| 0x30000043 | RFMPROG | 13072944 | -12 |
| 0x30000047 | DSUCSDMPSHIMPROG | 14062428 | -12 |
| 0x3000004e | FTM_WLANPROG | 17960976 | -12 |
| 0x30000055 | PMEM_REMOTEPROG | 14111348 | -12 |
| 0x3000005e | FTM_BTPROG | 11117560 | -12 |
| 0x30000067 | UI_CALLCTRLPROG | 11132204 | -12 |
| 0x30000068 | UIUTILSPROG | 12159876 | -12 |
| 0x3000006a | HWPROG | 14719844 | -12 |

### APPS (server progs) + callbacks
| prog | serviço | file_offset | dist |
|---|---|---|---|
| 0x3000000a | ADSPRTOSATOMPROG | 14587744 | -12 |
| 0x3000000b | ADSPRTOSMTOAPROG | 16583512 | -32 |
| 0x30000011 | FS_RAPIPROG | 6751240 | -12 |
| 0x30000015 | DOG_KEEPALIVEPROG | 2813108 | -12 |
| 0x3000004c | WLAN_ADP_FTMPROG | 10787368 | -12 |
| 0x3000004d | WLAN_CP_CMPROG | 18530872 | -12 |
| 0x30000062 | KEYPADPROG | 6752064 | -12 |
| 0x30000063 | HSU_APP_APISPROG | 6603228 | -12 |
| 0x31000010 | RDEVMAPCBPROG (callback) | 2781508 | -12 |
| 0x31000013 | AUDMGRCBPROG (callback) | 4777052 | -12 |
| 0x3100003b | DIAGCBPROG (callback) | 769204 | -12 |

### HIPÓTESE (constantes existem mas sem string de registro adjacente)
Todo o resto do range `0x3000xxxx`/`0x3100xxxx` enumerado no binário aparece com centenas de
ocorrências dispersas (n=10..1000) — são majoritariamente **falsos positivos** (bytes ASCII/dados
que coincidem com o range), não program IDs. Só as linhas acima (dist=-12/±32 a string de stub RPC)
são program IDs reais. Exemplos rotulados HIPÓTESE por não terem string próxima: 0x30000001..09,
0x3000000c..0f, 0x30000012, 0x30000014, 0x30000016..1e — provavelmente CM/DB/PBM/NV/misc, mas
**não confirmados aqui** (fora do escopo áudio). Não inventar nomes.

---

## Resumo de verificação
- **6 program IDs de áudio/vizinhos VERIFICADOS** (adjacência forte): 0x3000000a, 0x3000000b,
  0x30000013, 0x31000013 (+ os dois lados client/server de cada ADSP_RTOS).
- **Program+proc do caminho de PCM:** `ADSPRTOSATOM (0x3000000a)`, proc
  `adsp_rtos_app_to_modem_command`, alvo = command queues QDSP5 das tasks **AUDPP** (mixer/HostPCM,
  filas `UPAUDPPCMDxQUEUE`) e **AUDPLAYx** (bitstream, `UPAUDPLAYxBITSTREAMCTRLQUEUE`).
  AUDMGR (0x30000013) apenas configura codec/device; não transporta samples.
