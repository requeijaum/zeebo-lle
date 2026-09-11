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
- **Não resolve identidade de interface.** Emite `(base, offset)`, não `IDisplay::Blt`.
  Ligar offset → nome exige cruzar com os headers do BREW SDK.
- **Janela fixa de 8 instruções** entre a carga do slot e o `bx`. Idiomas mais espaçados
  escapam. O gate de 8/8 mostra que a janela cobre o que encontramos até aqui, não que
  cobre tudo.
- **Não distingue call-site alcançável de morto.** 432 é o teto estático, não a demanda
  real de execução.

## Uso

```
python3 tools/py/scan_deps.py <mod> [--base 0x12000000] [--json saida.json]
python3 tools/py/test_scan_deps.py     # gate; exit 77 se o .mod não estiver presente
```

O `.mod` é proprietário e não vive no repositório; o gate pula com exit 77 quando ausente.
