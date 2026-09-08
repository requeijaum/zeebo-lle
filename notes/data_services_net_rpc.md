# Data Services (rede) — canais SMD + programas ONCRPC no AMSS/APPS

Método idêntico ao de `qdsp5_program_ids.md`: constante u32 little-endian no range
`0x3000xxxx` (server prog) / `0x3100xxxx` (callback prog), verificada por adjacência
(`dist = const_off - str_off`) a (a) string de registro `unable to register (XPROG,
XVERS, sm).` ou (b) nome do arquivo `.c` do stub RPC. `|dist| ~ 8..36` = VERIFICADO.
Binários: `nand/1.1.2_AMSS.bin` (ARM9/modem, 21626880 B), `nand/1.1.2_APPS.bin`
(ARM11/apps, 22151168 B). Só leitura.

Objetivo: mapear o transporte de dados celulares (Data Services / PS / sockets)
entre APPS (BREW) e o modem (AMSS), para a ponte de rede estilo Flycast
(`tools/cpp/zeebo_net_bridge.h`).

---

## 1. Descoberta central — como os dados de rede realmente trafegam

O Zeebo/MSM7201A **não** expõe um `prog::DS` monolítico com "abrir socket" via RPC.
O stack é o clássico QUALCOMM **DS/PS (Data Services / Protocol Stack)** rodando
INTEIRO no modem (AMSS), com dois planos distintos:

1. **Plano de controle (ONCRPC)** — APPS pede/consulta ifaces, DNS, chamada PDP.
   Programas verificados abaixo (SMD_BRIDGE, DATA_ON_APPS, DSUCSDMPSHIM…).
2. **Plano de dados (SMD channels)** — pacotes IP/PPP brutos passam por **canais
   SMD dedicados** (`DATA5..DATA20`, `BRG_x`), NÃO por RPC. É exatamente o ponto
   onde a ponte estilo Flycast intercepta (buffers SMD ↔ sockets host).

O caminho de um pacote de dados do jogo BREW:
```
BREW app (dss_*, socket()) ── APPS ─┐
  dssocket.c / dss_iface_ioctl.c    │  (control-plane p/ abrir iface/PDP via RPC)
        │ ONCRPC                     │
        ▼                            ▼
  DATA_ON_APPS_ATOM_APIS (0x3000003b?) / SMD_BRIDGE (0x30000038/39)
        │
        ▼   pacotes IP/PPP brutos entram na FILA de um canal SMD de dados
  SMD channel "DATA5"/"DATA6"/... (smd_channel_alloc_tbl[cid]) ── SMEM 0x01F00000
        │   (half-channel struct: state, read_ptr, write_ptr, fifo)
        ▼
  AMSS: ps_iface / ps_ppp / ps_ip / dssocket → RLP/RmNet → RF (real HW)
```
Na LLE, o modem real não existe: a ponte **substitui o lado AMSS do canal SMD**,
lê os pacotes que o APPS escreveu na FIFO SMD, desencapsula PPP/IP, e faz bridge
para sockets POSIX/lwIP do host (idêntico ao Flycast modem/picoTCP).

---

## 2. Programas ONCRPC de Data Services (VERIFICADOS por byte-adjacency)

### Bridges SMD (o "cano" genérico sobre o qual os dados viajam)
| program_id | serviço (stub .c) | bin | prova (dist p/ string) | papel |
|---|---|---|---|---|
| **0x30000038** | SMD_BRIDGE_ATOM (`smd_bridge_atom_svc.c` AMSS / `_clnt.c` APPS) | ambos | -16/-32 p/ `smd_bridge` | App↔Modem stream bridge (server=AMSS) |
| **0x31000038** | SMD_BRIDGE_ATOMCB (callback) | APPS | -16/-20 | callback atom |
| **0x30000039** | SMD_BRIDGE_MTOA (`smd_bridge_mtoa_svc.c` APPS / `_clnt.c` AMSS) | ambos | -16/-20 | Modem→App stream bridge (server=APPS) |
| **0x31000039** | SMD_BRIDGE_MTOACB (callback) | AMSS | -16 | callback mtoa |
| **0x30000024** | SMD_PORT_MGRPROG (`smd_port_mgr_svc.c`) | AMSS | -12/-32 p/ regstr | gerente de portas SMD |
| 0x30000000 | SMD_PORT_MGRPROG (base/router alias) | AMSS | — | base/router |

> ATENÇÃO clean-room: `SMD_BRIDGE_ATOM=0x30000038` / `MTOA=0x30000039` e callbacks
> `0x31000038/0x31000039` são a par server/callback do bridge de STREAM SMD. É o
> mecanismo que carrega tráfego de dados (e outros streams) sobre ONCRPC quando não
> se usa o canal SMD cru. VERIFICADOS por adjacência ao stub `smd_bridge*.c`.

### Data Services / PS control-plane
| program_id | serviço | bin | prova | papel |
|---|---|---|---|---|
| **0x30000047** | DSUCSDMPSHIMPROG (`dsucsdmpshim_svc.c`) | AMSS | -32 p/ regstr, -12 histórico | CS-data (UCSD) multiproc shim |
| **0x30000046** | DSMP_UMTS_APPS_APIS (`dsmp_umts_apps_apis_clnt.c`) | AMSS | -8 | UMTS PS multiproc APIs |
| **0x3000003b?** | DATA_ON_APPS_ATOM_APISPROG (`data_on_apps_atom_apis_svc.c`) | APPS | regstr presente (const não-adjacente ao regstr; ver §4) | "Data on Apps" — traz o PS stack p/ rodar no APPS |
| 0x3000001d? | pdsm_atl (`pdsm_atl_clnt.c`, GPS/PDSM) | AMSS | -8 (HIPÓTESE) | position determination (não é dado celular puro) |

### Não são programas ONCRPC próprios (confirmado — sem regstr)
- **WMS** (`wms_svc.c`/`wms_clnt.c`, SMS): usa callback `0x31000003` (AMSS) /
  chamadas `0x30000003` (APPS). SMS, não dados de pacote.
- **ps_*** (`ps_iface.c`, `ps_ppp*.c`, `ps_ip*.c`, `ps_tcp*/ps_udp*`, `dssocket.c`,
  `dss_iface_ioctl.c`, `dssdns.c`): são o **PS stack interno do AMSS**, alcançados
  localmente (não têm prog ONCRPC próprio). Centenas de stubs presentes nos dois
  binários (lista completa varrida — ver §3).
- **RmNet** (`ds_rmnet_sm.c`, `ds_rmnet_sio.c`, só no APPS): a camada RmNet/DUN que
  faz LAN-LLC/DHCP sobre o link de dados — relevante p/ a ponte (é onde IP encontra
  o "ethernet virtual").

---

## 3. Convenção de canais SMD (VERIFICADA)

### Tabela de nomes de canais SMD (AMSS @ file 16616662; APPS @ 20204587)
Sequência contígua null-terminated extraída do binário (região `smd_internal.c`):
```
DATA10 DATA20 BRG_1 DATA12 BRG_2 DATA13 BRG_3 DATA14 BRG_4 DATA15 DATA5 BRG_5
DATA16 DATA6 CS_A2Q6 CS_M2Q6 DATA17 DATA7 DATA18 DATA8 DATA19 DATA9
GPSNMEA MCPY_RVD RPCCALL CS_A2M DUMMY RPCRPY
```
Interpretação (convenção Qualcomm SMD):
- **`DATA5..DATA20`** = canais de STREAM de dados de pacote (cada PDP/iface pega um
  `DATAn`). São os canais que a ponte de rede intercepta.
- **`BRG_1..BRG_5`** = canais do SMD_BRIDGE (par com prog 0x30000038/39).
- **`CS_A2Q6`/`CS_M2Q6`/`CS_A2M`** = circuit-switched / control (Apps↔Q6/Modem).
- **`RPCCALL`/`RPCRPY`** = canais ONCRPC (call/reply) — o transporte do plano de
  controle. **`RPCCALL`/`RPCRPY` são o par que carrega TODO ONCRPC** (áudio, data
  control-plane, etc.) sobre SMD.
- **`GPSNMEA`** = stream NMEA do GPS. **`DUMMY`/`MCPY_RVD`** = reservados/loopback.

### Tipo de link e estrutura de canal (VERIFICADO por assertion strings)
- `type == SMD_APPS_MODEM failed` (AMSS @18089170, APPS @12276886) — a edge de canal
  é do tipo **`SMD_APPS_MODEM`** (o par Apps↔Modem; convenção `SMD_APPS_MODEM_DATA`
  do kernel MSM). Fonte: `smd_task.c`.
- `smd_channel_alloc_tbl[cid].ref_count != 0` (AMSS @16616873, APPS @12878102) —
  a tabela de alocação de canais é `smd_channel_alloc_tbl[cid]`, indexada por
  channel-id (`cid`). Fonte: `smd_internal.c`.
- Half-channel struct (já verificada em FINDINGS 3p / usada no emulador):
  `0x1755d1dc` no AMSS = `struct smd_half_channel { u8 state; u8 busy; u8 error;
  u8 link_status; u32 read_ptr; u32 write_ptr; ... FIFO }`. Estados:
  `SMD_SS_CLOSED=0 OPENING=1 OPENED=2 FLUSHING=3` (router branch @0x16ef0b2c).

### Onde vivem no SoC
- SMEM base **`0x01F00000`** (2MB) — half-channels + FIFOs compartilhados entre
  ARM11 e ARM9 (já mapeado nos dois cores pelo harness).
- Doorbell A2M `0xC0100400 + n*4` → ARM9 VIC `0xC0000000` (notifica o produtor).

---

## 4. Notas de rigor / pendências (falhar rápido, documentar)

- **DATA_ON_APPS_ATOM_APISPROG**: a string de registro existe no APPS (@4360535) e o
  stub `data_on_apps_atom_apis_svc.c` (@4360596) / `_clnt.c` no AMSS (@12132860),
  mas a constante `0x3000xxxx` **não** aparece na janela ±64B adjacente ao stub
  (padrão de layout diferente do de áudio). O prog id concreto fica **HIPÓTESE**
  (provável faixa 0x3000003b..0x3000004x) até um pass dedicado casar a struct de
  registro (`{prog;vers;name_ptr}`) por perseguir o ponteiro de nome. NÃO inventar.
- **DSUCSDMPSHIM = 0x30000047**: VERIFICADO (bate com `qdsp5_program_ids.md` linha 88).
- **SMD_BRIDGE 0x30000038/0x30000039** + cb `0x31000038/0x31000039`: VERIFICADO por
  adjacência ao stub `smd_bridge*.c` nos dois binários (dist -16..-36).
- O plano de DADOS não usa prog id — é canal SMD cru (`DATAn`). Portanto a ponte de
  rede NÃO precisa decodificar RPC para o payload IP; ela lê a FIFO do half-channel
  `DATAn` diretamente. RPC só p/ o control-plane (abrir iface/PDP, DNS).
- Falso-positivos: o range `0x3000xxxx` colide com bytes ASCII; só as adjacências
  fortes acima contam. Todo o resto (centenas de hits dispersos) é ruído.

## 5. Resumo p/ o emulador (o que a ponte precisa)
- Interceptar escrita/leitura nas FIFOs dos half-channels `DATA5..DATA20` em SMEM.
- Detectar transição `SMD_SS_OPENING→OPENED` (o APPS abriu o canal de dados).
- Desencapsular o framing do link (PPP/HDLC — ver `ps_hdlc_lib.c`, `ps_ppp_fsm.c`,
  ou RmNet/ethernet-LLC via `ps_lan_llc.c` no caminho RmNet) → pacote IP.
- Fazer bridge do IP p/ socket POSIX host (ou pilha lwIP/picoTCP user-space), e
  reinjetar as respostas de volta na FIFO do canal, tocando o doorbell M2A.
- Control-plane ONCRPC (`SMD_BRIDGE`, `DATA_ON_APPS`, `DSUCSDMPSHIM`) pode ser
  respondido com sucesso mínimo p/ o APPS acreditar que a iface subiu.
