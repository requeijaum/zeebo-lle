# Zeebo Z-Wheel (App ID 274755) — Arquitetura de Dados, Schemas SQLite e VFS

**Data**: Setembro de 2026  
**Contexto**: Emulação LLE (Zeebo-LLE) e HLE (Zeebx) do console Zeebo (Qualcomm MSM7201A, BREW 4.0.2).  
**Fonte Primária**: Extração binária reversa da partição `0:EFS2APPS` (`nand/1.1.2.bin`), módulo de execução do Z-Wheel (`AEECLSID = 0x01070798`) e corpus documental OpenZeebo/TripleOxygen.

---

## 1. Identificação do Aplicativo e VFS
- **App ID**: `274755`
- **AEECLSID**: `0x01070798` (registrado na TecToy como `"TECTOY"`)
- **Ponto de montagem no VFS**:
  - Metadados: `fs:/mif/274755.mif`
  - Diretório base: `fs:/mod/274755/`
  - Diretório privado do applet: `fs:/~0x01070798/`
  - Licença e chaves: `fs:/~0x01070798/tectoyli.brf`
  - Configurações do shell: `fs:/mod/274755/tectoy.cfg` e `fs:/mod/274755/uiconfig.xml`

---

## 2. Schemas Exatos dos Bancos SQLite 3

Todos os bancos são arquivos nativos de formato `SQLite format 3` (páginas de 1024 bytes) criados pelo runtime C da Z-Wheel via SQLite 3 embutido.

### 2.1. `tt_game_info` — Catálogo de Jogos do Carrossel 3D
Arquivo: `fs:/mod/274755/tt_game_info`

```sql
CREATE TABLE GAMEINFO (
    game_id       INTEGER PRIMARY KEY,
    class_id      INTEGER,
    playcount     INTEGER,
    dt_download   INTEGER,
    dt_lastplayed INTEGER,
    boxart_path   TEXT,
    flags         INTEGER,
    size          INTEGER,
    UNIQUE(game_id, class_id)
);

CREATE TABLE TITLETEXT (
    game_id       INTEGER,
    lang_id       INTEGER,
    titletext     TEXT,
    UNIQUE(game_id, lang_id)
);

CREATE TABLE DBINFO (
    version       INTEGER DEFAULT DB_VERSION,
    subversion    INTEGER DEFAULT DB_SUBVERSION
);
```

#### IDs de Idioma (`lang_id` em `TITLETEXT`)
Os identificadores de idioma utilizam a convenção FourCC/ASCII em 32 bits:
- `538997872` (`0x20205450` / `'PT  '`): Português (Brasil)
- `538996325` (`0x20204545` / `'EN  '`): Inglês
- `538997605` (`0x20205345` / `'ES  '`): Espanhol

#### Consultas SQL Executadas pela Z-Wheel
- **Listagem para o Carrossel**:
  ```sql
  SELECT * FROM GAMEINFO, TITLETEXT 
  WHERE GAMEINFO.game_id = TITLETEXT.game_id 
    AND TITLETEXT.lang_id = %d 
  ORDER BY GAMEINFO.playcount DESC, TITLETEXT.titletext COLLATE NOCASE ASC;
  ```
- **Atualização de Estatísticas pós-jogo**:
  ```sql
  UPDATE GAMEINFO 
  SET playcount = %d, dt_lastplayed = %d 
  WHERE game_id = %d;
  ```
- **Exclusão de Registro**:
  ```sql
  DELETE FROM GAMEINFO WHERE game_id = %d;
  DELETE FROM TITLETEXT WHERE game_id = %d;
  ```

---

### 2.2. `tt_prefs.db` — Preferências Globais e Estado do Sistema
Arquivo: `fs:/mod/274755/tt_prefs.db`

```sql
CREATE TABLE PREFSINFO (
    name      TEXT PRIMARY KEY,
    strValue  TEXT,
    dwValue   INTEGER,
    flags     INTEGER
);

CREATE TABLE DBINFO (
    version     INTEGER DEFAULT DB_VERSION,
    subversion  INTEGER DEFAULT DB_SUBVERSION
);
```

#### Chaves Canônicas Encontradas no Dump da NAND 1.1.2:
| `name` | `strValue` | `dwValue` | `flags` | Significado |
|---|---|---|---|---|
| `Initialized` | `""` | `1` | `2` | Flag de primeiro boot concluído |
| `TermsAccepted` | `""` | `1` | `2` | Termos de serviço aceitos pelo usuário |
| `UpgradeWeekday` | `""` | `3` | `2` | Dia da semana para checagem de OTA FOTA |
| `LastAssetUpdateTime` | `""` | `983495226` | `2` | Timestamp Epoch do último sync de assets |
| `AsstVersion` | `""` | `0` | `2` | Versão do cache de assets |

---

### 2.3. `tt_dlqueue.db` — Fila de Download e Loja (ZeeboNet)
Arquivo: `fs:/mod/274755/tt_dlqueue.db`

```sql
CREATE TABLE DLITEMINFO (
    item_id      INTEGER PRIMARY KEY,
    price        INTEGER,
    size         INTEGER,
    titletext    TEXT,
    boxart_path  TEXT,
    flags        INTEGER,
    upgrade_id   INTEGER
);

CREATE TABLE DBINFO (
    version     INTEGER DEFAULT DB_VERSION,
    subversion  INTEGER DEFAULT DB_SUBVERSION
);
```

- **Comando de Inserção na Fila**:
  ```sql
  INSERT OR REPLACE INTO DLITEMINFO VALUES (%d, %d, %d, '%s', '%s', %d, %d);
  ```

---

### 2.4. Cache de Assets da ZeeboNet (DSL / Zeebo Shop)
Utilizado internamente pelo subsistema de download e atualização de slides da Z-Wheel:

```sql
CREATE TABLE ASSETS (
    owner       INTEGER,
    dslid       INTEGER PRIMARY KEY,
    type        INTEGER,
    version     INTEGER,
    path        TEXT,
    language    INTEGER,
    title       TEXT,
    startdate   INTEGER,
    enddate     INTEGER
);

CREATE TABLE DBINFO (
    version     INTEGER DEFAULT CACHE_DB_VERSION,
    subversion  INTEGER DEFAULT CACHE_DB_SUBVERSION
);
```

- **Query de Resolução de Slides/Banners Ativos**:
  ```sql
  SELECT path FROM ASSETS 
  WHERE owner = %d AND type = %d AND (language = %d OR language = 0);
  ```

---

## 3. Arquitetura de Inicialização e Dependências de Execução

Ao receber `EVT_APP_START` (`0x1f96`), a Z-Wheel executa a seguinte sequência:
1. **Verificação de Integridade de Cache**: Chama rotinas internas `Cache_InitAppData` executando `PRAGMA integrity_check` nos bancos SQLite.
2. **Carregamento de Preferências**: Abre `tt_prefs.db`, lê a chave `Initialized` e `TermsAccepted`. Se ausentes ou zero, redireciona o fluxo para a tela de primeiro setup e termos de uso.
3. **Consulta de Idioma**: Obtém o idioma configurado do sistema via `ISHELL_GetPrefs` e calcula a FourCC (`lang_id`).
4. **Carga do Carrossel**: Abre `tt_game_info`, executa a query com `lang_id` e monta a lista encadeada de itens da Z-Wheel.
5. **Assets Gráficos**:
   - As capas 3D e slides de fundo ficam em `./assets/games/<game_id>/` e `./assets/stage_slides/`.
   - Formato dos gráficos: **JPEG** (decodificado via subsistema BREW IImage / IJPEG).
   - O frame 3D é renderizado via vtable gráfica `[applet + 0x2c]->vtbl[0x28](1)`.

---

## 4. Recomendações para o Projeto LLE / HLE
1. **Mock e Inicialização Inicial**: Ao carregar a Z-Wheel pela primeira vez num ambiente emulado, basta fornecer um banco `tt_prefs.db` com `Initialized=1` e `TermsAccepted=1` para pular o fluxo de primeiro boot.
2. **Registro de Jogos**: Para fazer um jogo (ex: Quake, ID `274802`) aparecer na interface da Z-Wheel, basta um registro correspondente em `GAMEINFO` e `TITLETEXT` apontando para o diretório de assets com os JPEGs.
3. **Resolução de Caminhos**: O sistema de arquivos deve ser estritamente *case-insensitive* ao resolver caminhos em `assets/` e `mod/274755/`.
