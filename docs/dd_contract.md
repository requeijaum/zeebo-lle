# Inventário estático de dependências — `ddragonz.mod`

Gerado por `tools/py/scan_deps.py`; gate em `tools/py/test_scan_deps.py`.

## Por que este documento existe

Até o commit `0f64ffa` (fecho de DD3), **toda** dependência de vtable/static-base foi
descoberta de forma reativa: rodar → falhar com `ip=0` → disassemblar → instalar stub →
repetir. Isso custa uma janela por slot e não responde à pergunta que importa:
*quantos faltam?*

O scanner inverte a ordem. Varre o `.mod` procurando os idiomas de chamada indireta do
RVCT/ARM e emite a lista completa **antes** de executar qualquer coisa.

## Validação do instrumento

O scanner prevê retroativamente **8/8** das fronteiras que haviam sido descobertas por
tentativa e erro (`test_scan_deps.py`, exit 0). Critério definido antes de rodar: se ele
não previsse uma fronteira já conhecida, o errado seria o scanner, não o inventário.

Controle negativo executado de fato: revertendo para `capstone.disasm()` ingênuo o gate
reprova com `0 hits` e 8 MISS. A decodificação ingênua para no primeiro word
indecodificável — cobre 177 instruções de ~115 mil (0,2% do binário), porque um `.mod`
BREW intercala código, literal pools e `.rodata` livremente. Daí `disasm_resilient()`,
que reinicia a varredura a cada buraco.

## Resultado

432 call-sites indiretos, em duas famílias:

| família | offsets distintos | call-sites | cobertos por stub hoje |
|---|---|---|---|
| `vtable` (objeto COM, `this` em r0) | 45 | 194 | 5 offsets / 49 sites (25,3%) |
| `static-base` (AEEHelperFuncs, ROPI) | 13 | 238 | 5 offsets / 40 sites (16,8%) |

Ou seja: o tick completo de DD3 (1587 instruções, `FAULT_NONE`) exercita **menos de um
quarto** da superfície de chamada indireta do jogo. O número não é motivo de alarme — é a
medida honesta de quanto falta, que antes não existia.

### static-base ainda não cobertos, por frequência

| offset | slot | call-sites | nota |
|---|---|---|---|
| `+0x00c0` | 48 | **138** | maior consumidor isolado do binário inteiro |
| `+0x006c` | 27 | 32 | segundo maior; provável par de `+0x68` (malloc) → free/realloc |
| `+0x0000` | 0 | 19 | |
| `+0x0008` | 2 | 4 | |
| `+0x013c` | 79 | 2 | |
| `+0x00e8` | 58 | 1 | |
| `+0x00dc` | 55 | 1 | |
| `+0x01b4` | 109 | 1 | |

Já cobertos: `+0x04` (memset), `+0x14` (strlen), `+0x68` (malloc), `+0xb0` (GetUpTimeMS),
`+0xe4` (strtowstr).

### Leitura do dado

`+0xc0` com 138 sites e `+0x6c` com 32 são, juntos, 170 dos 238 sites de static-base (71%).
Nenhum dos dois foi alcançado ainda porque o tick atual retorna antes. São o próximo alvo
óbvio — e identificá-los custou 15 segundos de varredura, não uma janela de tentativas.

Quanto a vtable: `+0x04` (35 sites) e `+0x08` (21) dominam, mas o offset sozinho não
identifica a interface — o mesmo `+0x04` é `Release` em qualquer objeto COM. Resolver isso
exige correlacionar o call-site com o CLSID que originou o objeto, o que o scanner ainda
não faz.

## Limitações conhecidas

- **Só modo ARM.** Trechos THUMB (a vtable do applet, por exemplo) não são varridos.
- **Janela fixa de 8 instruções** entre a carga do slot e o `bx`. Idiomas mais espaçados
  escapam. O gate de 8/8 mostra que a janela cobre o que encontramos até aqui, não que
  cobre tudo.
- **Não distingue call-site alcançável de morto.** 432 é o teto estático, não a demanda
  real de execução.
- ~~**Não resolve identidade de interface.**~~ **RESOLVIDO** para `IDisplay` por
  `tools/py/vtbl_layout.py` — ver seção "Identidade dos slots de IDisplay" abaixo.
  Continua em aberto para as demais interfaces e para a static-base.

## Identidade dos slots de `IDisplay`

`scan_deps.py` dá offsets; não dá nomes. Os nomes usados até `09eaf3f` eram
**suposição**, e estavam errados. `tools/py/vtbl_layout.py` expande as macros
`INHERIT_*` do SDK e devolve a ordem real: 26 slots, com `INHERIT_IDisplay`
herdando de **`IBase`** (`AddRef`, `Release` — dois métodos, sem
`QueryInterface`), de modo que o primeiro método próprio de `IDisplay`
(`GetFontMetrics`) cai em `0x08`.

> **Correção.** O commit `6067a03` afirmou que a raiz era `IQueryInterface` com
> três métodos e que o deslocamento de um slot explicava a rotulagem errada
> anterior. Isso está **errado**: `AEEIDisplay.h:223` mostra `INHERIT_IBase`. A
> ferramenta sempre produziu o layout certo — a narrativa é que estava errada.
> O erro foi pego ao comparar com a tabela `DISPLAY` do zeebx, que lista os
> mesmos nomes nos mesmos offsets partindo explicitamente de `IBase`.

### Correção de rotulagem

| offset | real (SDK) | eu supunha | call-site | conferido por aridade? |
|---|---|---|---|---|
| `0x14` | `DrawRect` | *slot 5* | `0x12023a74` | **sim** (5 args, 5º na pilha) |
| `0x1c` | `Update` | *slot 7* | `0x12024538` | **sim** (2 args, tail-call) |
| `0x28` | `SetColor` | *slot 10* | `0x120244c8` | não — só posição no layout |
| `0x48` | `SetClipRect` | *GetDestination* | `0x12023a48` | não — só posição no layout |

Os dois primeiros têm confirmação independente pelo binário. Os dois últimos vêm
**apenas** da posição no layout do SDK: são melhores que os rótulos antigos, mas
ainda não foram cruzados com os argumentos do call-site. Tratar como provisórios.

`GetDestination` é o slot 15 (`0x3c`) e `GetDeviceBitmap` o slot 16 (`0x40`) —
nenhum dos dois é chamado no caminho exercitado hoje.

### Caso NÃO resolvido: offset `0x10`

Eu rotulava `0x10` como *GetInfo*; no `IDisplay` esse slot é `DrawText`. Mas
**não** basta trocar a etiqueta, porque o call-site `0x1201a618` obtém a vtable de
`[obj + 0xc]`, com `obj` vindo da static-base `+0xc0` — não é o `IDisplay`
devolvido por `ISHELL_CreateInstance`. O objeto pertence a outra interface, ainda
não identificada.

O comportamento que o binário exige ali (escrever largura/altura num out-param)
é de um *get dimensions*, e não de `DrawText`. No harness o stub passou a chamar-se
`DISP_OFF10_DIMS_STUB`: nome descritivo do offset e do comportamento observado,
sem afirmar um método do SDK que não foi provado. Identificar essa interface é
trabalho pendente e depende de mapear a static-base `+0xc0` (138 call-sites).

### Por que a identificação é confiável (e não outra suposição)

Três evidências independentes, todas verificáveis:

1. **CLSID por aritmética do SDK.** `AEECLSID_CORE = QVERSION + 0x1000 = 0x01001000`;
   `AEECLSID_DISPLAY = CORE+1 = 0x01001001`, exatamente o literal passado a
   `ISHELL_CreateInstance` em `@0x60c`. (`0x01001002` = `CORE+2` = `AEECLSID_HEAP`,
   o que também confere com o uso observado.)

2. **Aridade de `DrawRect`.** `void DrawRect(iname*, const AEERect*, RGBVAL, RGBVAL, uint32)`
   = 5 argumentos, o quinto empilhado. O call-site faz `mov r3,#2; str r3,[sp]`
   antes do `bx ip` — um método de 1–2 argumentos não explicaria esse store.
   Args observados: `pRect=NULL` (tela toda), `clrFrame=-1`, `dwFlags=2`.

3. **Aridade de `Update`.** `void Update(iname*, boolean)` = 2 args, retorno não
   usado. O call-site carrega `r0`, `mov r1,#1` e faz **tail-call** (`bx r2` sem
   `mov lr,pc`), consistente com `void`.

Gate: `make test-vtbl-layout`. Controle negativo incluído — deslocar a numeração
em um único slot faz a conferência de aridade reprovar.

### Consequência para DD4 — CONFIRMADA

Os slots que DD3 stubou como no-op retornando 0 incluem `DrawRect` e `Update`
(ambos confirmados por aridade) e, provavelmente, `SetColor` e `SetClipRect` —
o caminho de desenho. O tick de DD3 **já estava pedindo para desenhar**; os
pedidos caíam em stubs mudos.

## DD4 (parcial): o primeiro frame

`DrawRect` deixou de ser mudo. O harness observa o call-site `0x12023a74` um
instante antes do `bx`, lê os argumentos como o guest os montou e executa a
operação num framebuffer 640×480 do host.

### Argumentos medidos na primeira chamada

| arg | reg | valor | significado |
|---|---|---|---|
| `this` | r0 | `0x00300500` | `DISPLAY_OBJ` |
| `pRect` | r1 | `0x00000000` | `NULL` = tela inteira |
| `clrFrame` | r2 | `0xffffffff` | `RGB_NONE` |
| `clrFill` | r3 | `0xffffff00` | **`RGB_WHITE`** |
| `dwFlags` | `[sp]` | `0x00000002` | `IDF_RECT_FILL` |

**Quarta evidência da identificação.** `RGB_NONE` + `IDF_RECT_FILL` + `pRect`
é exatamente a expansão do inline `IDisplay_FillRect` do SDK
(`AEEIDisplay.h:377`). Não foi procurado: o binário produziu esses valores
sozinho, e eles casam com um macro do SDK que não participou da dedução por
aridade. Confirmação independente de que `0x14` é `DrawRect`.

### `RGBVAL` não é `0xRRGGBB`

`AEERGBVAL.h:24` define `MAKE_RGB(r,g,b) = (r<<8) | (g<<16) | (b<<24)`: o byte
**menos** significativo é alfa e os canais ficam deslocados 8 bits para cima.
Portanto `0xffffff00` é `MAKE_RGB(0xff,0xff,0xff)` = `RGB_WHITE`.

A primeira versão deste harness decodificou como `0x00RRGGBB` e gerou um frame
**amarelo**. Erro encontrado ao comparar com `Rgb::from_rgbval` do zeebx, que
faz o deslocamento correto. O jogo limpa a tela de **branco**, não de amarelo.

Assertiva de regressão: `exp_xrgb == 0x00ffffff`. Mutante que volta à
decodificação ingênua reprova com `Assertion fb.px[0] == exp_xrgb failed`.

## Por que o frame não muda entre ticks

Oito ticks sucessivos no mesmo estado de guest produzem **1 frame distinto**,
com 1587 instruções e exatamente uma chamada a `DrawRect` em cada um. Duas
hipóteses foram testadas e **refutadas**:

**1. Relógio parado — refutada como causa.** O mock de `aee_GetUpTimeMS`
(static-base `0xb0`) era `mov r0,#100`: um valor constante. Um game loop deriva
`dt` desse relógio, e com `dt=0` a decisão correta do jogo é não animar nada.
Substituído por um hook no host que avança 33 ms (~30 fps) por leitura. O jogo
**lê** o relógio (uma vez por tick, 100 → 430 ms), mas o frame continua
idêntico. O relógio parado era um bug real do harness e foi corrigido, porém
**não** é o que prende o frame.

> Cuidado metodológico: ao trocar o stub, a contagem caiu de 1587 para 1586
> apenas porque o stub passou de 2 para 1 instrução. Repor o `nop` devolveu
> 1587. A diferença era artefato da medição, não efeito do relógio.

**2. Slot de desenho não implementado — refutada.** Toda entrada vazia da
vtable de `IDisplay` foi preenchida com um stub-sonda que registra o offset
chamado (entrada vazia devolveria 0 e o jogo seguiria como se tivesse
funcionado — falha silenciosa). Resultado: **nenhum** slot sem stub é chamado.
Os slots em uso são exatamente os cinco já instalados: `0x10`, `0x14`
(`DrawRect`), `0x1c` (`Update`), `0x28` (`SetColor`), `0x48` (`SetClipRect`).

**Conclusão honesta.** O jogo não está pedindo para desenhar mais nada. Ele não
está num loop de gameplay que só carece de pixels; está num estado anterior e
estável, que a cada tick pinta o fundo e chama `Update`. A presença de
`strlen`/`strtowstr` no caminho sugere tela de texto (splash/menu), não
gameplay. O próximo passo não é implementar mais slots de desenho — é descobrir
que transição de estado o jogo espera e não recebe. Candidatos a investigar:
eventos de input (`EVT_KEY`) nunca entregues, e `data.ggz`/`sound.ggz`
inacessíveis por falta de `IFileMgr`.

### Resultado

307200/307200 pixels pintados, cor única `0xffffff` (branco), uma chamada por
tick. Frame gravado em PPM binário via `ZEEBO_DD4_FRAME=<arquivo>`.

### Lições do zeebx (leitura de contrato, sem cópia)

O zeebx é GPL-2.0-only e o zeebo-lle não pode receber código dele. O que foi
usado são decisões de arquitetura observáveis, reimplementadas:

- **Framebuffer em RGB565 nativo**, não XRGB32. A tela do Zeebo é VGA 640×480
  RGB565; guardar nesse formato torna `BitBlt` de bitmap nativo uma cópia
  direta, sem conversão por pixel. Nosso harness ainda usa XRGB32 — aceitável
  para um fill, mas a converter antes de haver blit de sprite.
- **Contador de pixels tocados** (`touched`/`is_dirty`) em vez de comparar o
  buffer inteiro para saber se houve desenho.
- **`from_rgb565` replica os bits altos nos baixos** (`(r<<3)|(r>>2)`), senão
  branco puro volta como 248 em vez de 255.
- A tabela `DISPLAY` de 26 slots do zeebx bate com o layout extraído do SDK, o
  que serviu de verificação cruzada independente.

### Dois controles negativos (executados, não descritos)

1. **Pintar constante do harness** em vez de `clrFill` → `Assertion fb.px[0] == cap.r3 failed`.
   Garante que a cor vem do jogo, não de nós.
2. **Desligar o observador** (`watch.va = 0`) → `Assertion cap.calls >= 1 failed`,
   com `fills=0`. O guest roda idêntico, mas nada é pintado. Garante que os
   pixels são consequência da execução do jogo, não de código nosso rodando ao lado.

### O que este frame NÃO é

Uma tela amarela sólida não é gameplay. É o `FillRect` de fundo do primeiro
tick — legítimo e vindo do jogo, mas apenas o começo do frame. Sprites, tiles e
texto dependem de `data.ggz` (ainda sem `IFileMgr`) e dos slots de blit, que
não chegaram a ser chamados. Não tratar como "Double Dragon rodando".

## Uso

```
python3 tools/py/scan_deps.py <mod> [--base 0x12000000] [--json saida.json]
python3 tools/py/test_scan_deps.py     # gate; exit 77 se o .mod não estiver presente
```

O `.mod` é proprietário e não vive no repositório; o gate pula com exit 77 quando ausente.
