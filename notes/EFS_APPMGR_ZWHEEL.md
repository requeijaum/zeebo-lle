# EFS, BREW appmgr e Z-Wheel na NAND 1.1.2

Mapeamento de onde residem o BREW appmgr, a Z-Wheel (menu do Zeebo) e os
applets do sistema na cópia de trabalho da NAND (`nand/1.1.2.bin`, 128 MiB) e
suas partições. Todos os offsets abaixo são **offsets no dump lógico
`1.1.2.bin`** (imagem de páginas de 2048 B sem OOB; o `1.1.2_spare.bin` é a
variante com 64 B de spare/página). Evidência coletada por varredura de strings
e parse da tabela de partições — nenhum binário proprietário foi descompilado.

---

## 1. Geografia da NAND — tabela de partições MIBIB

A tabela de partições (System Partition Table da Qualcomm) foi encontrada pelo
magic `0x55ee73aa` em `1.1.2.bin @ 0x60810`. Cada entrada é
`name[16] + start_block(u32) + num_blocks(u32) + attr(u32)`. Geometria do
Zeebo: página 2048 B, spare 64 B, **64 páginas/bloco** → bloco = 128 KiB de
dados.

| Partição      | start (bloco) | len (blocos) | Papel |
|---------------|---------------|--------------|-------|
| `0:MIBIB`     | 0x000         | 0x00a        | Tabela de partições / bootable info block |
| `0:QCSBL`     | 0x00a         | 0x002        | Qualcomm Secondary Boot Loader |
| `0:OEMSBL1`   | 0x00c         | 0x003        | OEM SBL (estágio 1) |
| `0:OEMSBL2`   | 0x00f         | 0x003        | OEM SBL (estágio 2) |
| `0:AMSS`      | 0x012         | 0x0a5        | Modem AMSS (ARM9) → `1.1.2_AMSS.bin` |
| `0:APPSBL`    | 0x0b7         | 0x003        | Applications bootloader → `1.1.2_APPSBL.bin` |
| `0:FOTA`      | 0x0ba         | 0x002        | Firmware-Over-The-Air staging |
| **`0:EFS2`**  | **0x0bc**     | **0x02a**    | **EFS2 do modem (ARM9)** |
| `0:APPS`      | 0x0e6         | 0x0a9        | Firmware do ARM11 APPS (BREW) → `1.1.2_APPS.bin` (ELF stripped) |
| `0:FTL`       | 0x18f         | 0x002        | Flash Translation Layer control |
| **`0:EFS2APPS`** | **0x191**  | resto        | **EFS2 do lado APPS — filesystem `fs:/` da BREW** |

Conversão para offset em bytes no dump lógico: `offset = start_block * 64 *
2048 = start_block * 0x20000`.

- `0:APPS` @ bloco 0xe6 = **0x1CC0000** — é onde vive o ELF do BREW/Iguana
  (corresponde a `1.1.2_APPS.bin`, extraído separadamente).
- `0:EFS2APPS` @ bloco 0x191 = **0x3220000** — a árvore de arquivos `fs:/`
  usada pela BREW (mif/mod/bar/ringers/sys). A maioria dos hits `fs:/...`
  abaixo (0x1ef_xxxx, 0x20d_xxxx, 0x22b_xxxx, 0x2c2_xxxx, 0x2fe_xxxx) cai na
  faixa lógica de APPS/EFS2APPS.

> Nota: uma segunda tabela em `@0x64810` (mesmos nomes, valores diferentes /
> attr `0xff...`) é uma tabela **template/OTP de fábrica** em unidades
> distintas — não é a geometria efetiva. A tabela válida é a de `@0x60810`.

---

## 2. Onde residem os applets BREW no EFS2

A BREW no Zeebo usa a convenção padrão de EFS `fs:/`:

- `fs:/mif/<applet>.mif` — **Module Information File**: registra o applet,
  seu(s) AEECLSID(s), tipo, ícones e flags de privilégio.
- `fs:/mod/<applet>/<applet>.mod` — o **módulo executável** (código ARM do
  applet, formato `arch=arm;ext=.mod`, ver string em `0x1eed1a5`).
- `fs:/mod/<applet>/<applet>.bar` — **BREW Application Resource** (recursos:
  strings, bitmaps, layouts).
- `fs:/sys/` — recursos de sistema compartilhados (`aeecontrols.bar`,
  `oemmsgs.bar`, `hid_devices.cfg`).
- `fs:/ringers/`, `fs:/shared/fonts/`, `fs:/card0..3/`, `fs:/mcp/` — mídia,
  fontes, cartões e mobile-content-platform.

O launcher BREW resolve applets numéricos (jogos comprados na loja) por ID de
diretório: `fs:/mod/<num>/<nome>.mod` + `fs:/mif/<num>.mif`. IDs numéricos de
módulo presentes no dump: **26108, 26109, 32765, 32789, 274755, 276809,
279126, 279233** (ex.: `fs:/mod/279126/karnovr.mod`, `fs:/mod/276809/
rolimaz.mod`, `fs:/mod/279233/darkseal.mod`) e mifs numéricos correspondentes
(`fs:/mif/26109.mif`, `fs:/mif/276809.mif`, etc.). Estes são jogos/aplicativos
provisionados via download da loja.

### 2.1 BREW appmgr (Application Manager)

Componente do sistema BREW responsável por instalar / listar / remover
applets. Localização no EFS:

- MIF:  `fs:/mif/brewappmgr.mif`   (string em **`0x2c285ee`**)
- MOD/recursos (não há `.mod` próprio; usa `.bar` de recursos):
  - `fs:/mod/brewappmgr/appmgrls.bar`  (**`0x2fe9ffc`**) — recurso "list"
  - `fs:/mod/brewappmgr/appmgrln.bar`  (**`0x2fea01c`**) — recurso "line/entry"
- Nomes de recurso soltos (referenciados pelo código compilado em APPS):
  `appmgrln.bar` @ `0x22df100`, `appmgrls.bar` @ `0x22df114`, dentro de uma
  função ARM (contexto `push {...} ... appmgrln.bar ... appmgrls.bar`) — ou
  seja, o **código do appmgr está linkado dentro do ELF de APPS** (0:APPS), e
  só os recursos `.bar`/`.mif` vivem no EFS2APPS. Isto é típico do BREW: o
  AppManager é um applet OEM embutido no firmware, não um `.mod` baixável.

### 2.2 Z-Wheel / menu do Zeebo (o "launcher" principal)

A **Z-Wheel** é a interface de menu rotativo da tela inicial do Zeebo
(Jogos / Loja / Configurar / Zeebo). Não existe arquivo `zwheel`/`z-wheel` no
EFS — o termo "Z-Wheel" aparece **apenas em strings de UI/ajuda** (help
screens EN/PT, faixa `0x388xxxx–0x3c1xxxx`), e.g.:
- `0x388902e` "Go to the Home screen and choose *Setup* on the Z-Wheel."
- `0x3975010` "What is the *Zeebo* option on Home screen Z-Wheel?"

O componente de software que **implementa** a Z-Wheel é o applet do launcher
Zeebo, chamado internamente **ZeeboApp** (também "Zeebo_Start" / "ZeeboApp"):
- `0x212dfec` `Zeebo_Start: handling ZEEBO_LAUNCH_ARG_UPGRADE_UI`
- `0x212e048` `ZeeboApp: Start success`
- `0x212e2e8` `ZeeboApp: AEEAppletNew(AEECLSID_ZEEBO_APP,...) failed.`
- `0x1d45641` `ZeeboApp,SUCCESS CLassID = %x, nErr= %d`
- Subsistemas: `ZeeboAppUpgrade.c` (`0x3dda554`), `ZeeboAppUPDTUpdateAlarmSet`,
  checagem de nova versão via loja (`ZeeboAppUpgradeCheckCB`).

O **CoreApp** (`fs:/mif/aeecoreapp.mif`, `fs:/mif/coreapp.mif`,
`fs:/mod/coreapp/coreapp{,_qvga,_vga}.bar`; código em `..\apps\core\
CoreApp.c` e `CoreMenu.c`) é o applet-shell base da UI (dialogs, menu, power).
A ZeeboApp roda sobre a infra do CoreApp e monta a Z-Wheel + integração com a
loja (Shop / Buy Z-Credits / Zeebo Mall). Lançamento de jogos parte daí via
`ISHELL_StartApplet` (strings `0x2592ce4` `ISHELL_StartApplet returned error
%d, ClsID (%08X), %s` e `0x3dc7450` `ISHELL_StartApplet failure ... Unable to
launch Game` em `Tectoy.c`).

Cadeia de launch: **APPSBL → 0:APPS (BREW/Iguana boot) → AEEShell → CoreApp
shell → ZeeboApp (Z-Wheel/Home) → ISHELL_StartApplet(clsid do jogo) → jogo em
`fs:/mod/<id>/`**.

---

## 3. Como o EFS2 é acessado (Iguana/BREW)

Há **dois** EFS2 no Zeebo (padrão Qualcomm dual-processor):

1. **`0:EFS2`** (bloco 0xbc) — filesystem privado do **modem AMSS (ARM9)**;
   guarda NV items, calibração RF, config de rede. Nomes vistos:
   `0:EFS2`/`0:EFS2APPS`/`0:FTL` em tabelas de config (`@0x608d6`, `@0x19920a`).
2. **`0:EFS2APPS`** (bloco 0x191) — filesystem do **lado APPS (ARM11)**; é o
   `fs:/` que a BREW enxerga (mif/mod/bar/sys/ringers).

Acesso pela BREW/Iguana: a AEE (`IFILEMGR`/`IFILE`, ver `AEECLSID_FILEMGR` em
`0x26cb469`, `0x3dd2eff`) resolve caminhos `fs:/...` contra o **EFS2APPS local
do próprio ARM11** — não é RPC ao modem. O EFS2APPS é montado pelo driver de
NAND/FTL do lado APPS (partição 0:APPS contém o driver; 0:FTL faz a translation
layer). Apenas dados de **modem/NV/rede** transitam por RPC ONCRPC/SMD ao ARM9
(ver `notes/data_services_net_rpc.md`); os arquivos de applet **não**.

Consequência para a emulação LLE: para servir `fs:/mif/*.mif`,
`fs:/mod/*/*.mod`, `fs:/mod/brewappmgr/*.bar`, basta modelar o **EFS2APPS local
via driver NAND/FTL** (já há `NandController`/`DMOVModel` em
`tools/cpp/zeebo_devices.h`) — não é necessário implementar o EFS RPC do modem
para chegar ao appmgr/Z-Wheel. Rotinas de bootstrap `CopyBrewSystemIntoEnand` /
`CopyBrewAppletsIntoMifEnand` / `CopyBrewAppletsIntoModEnand` (strings
`0x1f56xxx`) mostram o firmware copiando a BREW system + applets para a
partição "Enand" (embedded NAND / EFS) no primeiro boot.

---

## 4. Identificadores de classe BREW (AEECLSID)

O ELF de APPS é stripped: os AEECLSID numéricos não estão como símbolos, mas os
**nomes simbólicos** aparecem em strings de log e o valor numérico é resolvido
via metadados do Toolset (`ClassDB.xml`, `.mif`, `.clif` — clean-room OK).

CLSIDs simbólicos confirmados no dump (via strings `AEECLSID_*`, 43
ocorrências):

| Símbolo | Papel | Evidência |
|---------|-------|-----------|
| `AEECLSID_ZEEBO_APP` | **Launcher Zeebo / Z-Wheel** (applet principal) | `0x212e2e8` `AEEAppletNew(AEECLSID_ZEEBO_APP,...)` |
| `AEECLSID_TECTOY`    | Applet OEM Tectoy (integração loja/jogos) | `0x2bda958`, `Tectoy.c` |
| `AEECLSID_DOWNLOAD`  | Download manager (provisão de jogos) | `0x2bda904` |
| `AEECLSID_WARNDIALOG`| Diálogos de aviso | `0x2bda8c0` |
| `AEECLSID_FILEMGR`   | IFILEMGR (acesso a `fs:/`) | `0x26cb469` |
| `AEECLSID_GL` / `_EGL` / `_GRAPHICS` | Adreno 130 / GLES | `0x3d5e62d`.. |
| `AEECLSID_WEB`, `_MEDIAUTIL`, `_MEDIAMPEG4`, `_CAMERA`, `_STATIC`, `_CONFIG`, `_CM`, `_NETWORK`, `_PDP`, `_STK`, `_CARD`, `_SUPPSVC` | serviços de sistema | várias |

**BREW appmgr:** não há string `AEECLSID_APPMGR` no dump — o AppManager BREW
usa o CLSID padrão de sistema **`AEECLSID_APP_MGR` (0x01004003)** na base
`0x0100xxxx` da BREW (identificável no `ClassDB.xml`/`AEEAppGen`), registrado
via `fs:/mif/brewappmgr.mif`. Confirmar o valor exato lendo o header do
`brewappmgr.mif` (offset 0x2c285ee no EFS): os primeiros bytes do MIF contêm o
record de classe com o AEECLSID em little-endian.

**Parede do oracle (contexto):** ao tentar bootar applet no a1Sim, a classe
`0x0109BA52` = `AEECLSID_SignalPrioGroupDefault` (de `Toolset/.../ClassDB.xml`)
é registrada antes de `RunProgram` (ver `notes/FINDINGS.md` §5b). Base
`0x0109xxxx` = classes internas BREW 4.0.2 do Zeebo.

> Para obter os valores numéricos exatos de `AEECLSID_ZEEBO_APP` e
> `AEECLSID_APP_MGR`, parsear o header dos MIFs correspondentes
> (`brewappmgr.mif`) ou o `ClassDB.xml` do Toolset (clean-room, metadados
> publicados). Não hardcodar valores adivinhados no emulador.

---

## 5. Resumo de offsets-chave (dump `1.1.2.bin`)

```
0x00060810  Tabela de partições MIBIB (magic 0x55ee73aa)
0x001CC0000 início lógico de 0:APPS (bloco 0xe6) — ELF BREW/Iguana
0x03220000  início lógico de 0:EFS2APPS (bloco 0x191) — árvore fs:/
0x0022DF100 código APPS refs "appmgrln.bar"/"appmgrls.bar" (appmgr embutido)
0x002C285EE  "fs:/mif/brewappmgr.mif"
0x002FE9FFC  "fs:/mod/brewappmgr/appmgrls.bar"
0x002FEA01C  "fs:/mod/brewappmgr/appmgrln.bar"
0x00212DFEC  "Zeebo_Start: ... ZEEBO_LAUNCH_ARG_UPGRADE_UI"
0x00212E2E8  "AEEAppletNew(AEECLSID_ZEEBO_APP,...) failed"
0x0001EFD3BF "fs:/mod/coreapp/coreapp_qvga.bar" (CoreApp shell)
0x0388902E   strings de UI "Z-Wheel" (help EN); PT em 0x38B0B02+
```

## 6. Próximos passos sugeridos
1. Ler o header binário de `fs:/mif/brewappmgr.mif` no EFS2APPS para extrair o
   AEECLSID numérico exato do AppManager.
2. Cruzar `AEECLSID_ZEEBO_APP` com o `ClassDB.xml` do Toolset para o valor.
3. Modelar leitura do EFS2APPS (0:APPS + 0:FTL) no `NandController`/`DMOVModel`
   para servir `.mif`/`.mod`/`.bar` ao BREW durante o boot LLE — rota que
   dispensa o EFS-RPC do modem para alcançar appmgr → ZeeboApp/Z-Wheel → jogo.

---

## 7. Validação do acesso DMA a 0:EFS2APPS (harness)

Harness: `tools/cpp/zeebo_efs2apps.cpp` (alvo `make test-efs2apps`). Reusa o
mesmo `NandController`/`DMOVModel` e a lista de descritores exata que o driver
NAND/FTL do ARM11 emite (`read_page` idêntico ao de `zeebo_partition.cpp`), e
compara byte-a-byte contra `nand/1.1.2.bin`. Resultado: **10/10 OK,
DMOV execs≈522**.

Conclusões confirmadas por bytes reais (não por contagem de instruções):

- **Geometria confirmada**: dump = 1024 blocos de 128 KiB. `0:EFS2APPS` começa
  no bloco 0x191 (offset 0x3220000, página 25664) e vai até o fim (0x8000000);
  `0:APPS` começa no bloco 0xe6 (0x1cc0000).
- **Caminho DMA válido**: a primeira página de `0:EFS2APPS` e a de `0:APPS`
  lidas via descritor DMOV batem exatamente com o dump.
- **Divisão de responsabilidades comprovada por localização física**: as
  *strings* de caminho `fs:/mif/brewappmgr.mif`, `fs:/mod/brewappmgr/appmgr{ls,ln}.bar`,
  `fs:/mod/coreapp/coreapp_qvga.bar` e o log `ZeeboApp: AEEAppletNew(...)`
  residem TODAS dentro do ELF de **0:APPS** (blk 0xe6–0x18e), não no EFS2APPS —
  ou seja, o código do appmgr e do ZeeboApp/CoreApp está linkado no firmware
  APPS, e só os dados/recursos e os dirents de jogos vivem no EFS2APPS.
- **Conteúdo do EFS2APPS**: a partição contém dirents reais do filesystem
  (`reksio.mod`, `karnovr.mod`, `rolimaz.mod`, `darkseal.mod`, `tectoy.mod`,
  `ticket.mod`, `*.ini`) — 723 entradas `.mod`, mais referências a `ZeeboApp`.
  Registro de dirent observado: `... <len:1> <clsid/attr:4> <nome> <u16> ...`
  (ex.: `0f 05 64 ef ab 04 00 "reksio.mod"`). O parse EFS2 completo (super-bloco
  0xa5a5 @0x322812a) fica para o passo de FS de mais alto nível.
- **Integridade sustentada**: varredura DMA página-a-página de 1 MiB (512
  páginas) da região de dirents: 0 páginas divergentes.
