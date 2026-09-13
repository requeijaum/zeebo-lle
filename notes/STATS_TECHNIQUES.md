# STATS_TECHNIQUES — Probabilidade e estatística aplicadas ao RE/emudev do Zeebo

Companheiro de `MORE_INFO.md` (ferramentas). Aqui: métodos quantitativos para
**decidir** com os dados que os projetos já produzem, em vez de decidir no olho.

Todos os números marcados **[MEDIDO]** foram calculados nesta sessão sobre os
datasets reais do repo (`zeebo-emulator/testkit/*.jsonl`, `zeebo-lle/nand/*.bin`).
Os marcados **[FÓRMULA]** são método, sem dado ainda.

Regra que atravessa o documento inteiro:
> **um controle só vale se puder falhar.** Se o teste não tem como dar negativo
> quando a hipótese é falsa, ele não é evidência — é decoração.

---

## 0. Índice por retorno imediato

| # | Técnica | Dado necessário | Status |
|---|---|---|---|
| 1 | Histograma de PC não-filtrado | hook de PC | pronto p/ colar |
| 2 | Statistical debugging no corpus 62 | `census62.jsonl` + `gl_scan.jsonl` | **rodado; ver §2.4 (auto-refutação)** |
| 3 | Regra dos três (controles negativos) | nenhum | **aplicada ao campaign_report §13.2** |
| 4 | Significância de varredura | tamanho do dump | **calculado** |
| 5 | SPRT no lockstep | `lockstep.cpp` | desenho pronto |
| 6 | Unseen species (Good-Turing/Chao1) | contagem de stubs | precisa instrumentar |
| 7 | Qui-quadrado código/dado | dump | pronto p/ colar |
| 8 | Bootstrap na comparação de frames | PPMs | pronto |
| 13 | Auditoria dos harnesses existentes | 3 repos | **rodado** |

---

## 1. Histograma de PC — e o bug do snippet original

O snippet proposto:

```c
if (pc >= 0xf0000000 && pc < 0xf0020000) faixa[(pc >> 12) & 15]++;
```

**Defeito:** o comentário diz que o objetivo é "provar se o hook é cego acima de
certa faixa", mas o `if` só conta **dentro** da faixa. Se o hook for cego em
`0xf0020000+`, a saída é idêntica ao caso "não há PC nenhum lá". O teste tem
**poder zero** contra a própria hipótese que enuncia. É o mesmo erro do gate
comercial que a skill do LLE proíbe.

**Correção — cobrir 100% do espaço com o mesmo custo:**

```c
// [CONTROLE] histograma do espaco INTEIRO por nibble alto.
// Pode falhar: se o hook for cego em faixa alta, o bucket sai 0 enquanto
// os baixos enchem. Sem filtro previo = a hipotese e' testavel.
static uint64_t hi[16] = {0};      // pc >> 28  -> cobre 0x0..0xF
static uint64_t fine[16] = {0};    // janela fina, so' apos confirmar trafego
static uint64_t total = 0;
hi[pc >> 28]++;
if (pc >= 0xf0000000 && pc < 0xf0020000) fine[(pc >> 12) & 15]++;
if (++total % 20000000ULL == 0) {
    fprintf(stderr, "[HIST] hi:");
    for (int i = 0; i < 16; i++) fprintf(stderr, " %x:%llu", i, (unsigned long long)hi[i]);
    fprintf(stderr, " | fine:");
    for (int i = 0; i < 16; i++) fprintf(stderr, " %d:%llu", i, (unsigned long long)fine[i]);
    fprintf(stderr, "\n");
}
```

**Leitura do resultado:** se `hi[0xF]==0` e `hi[0x1]` (região 0x1xxxxxxx do
firmware APPS) enche → o hook vê o guest mas não a faixa alta. Se **todos** os
buckets fora de um estreito estiverem zerados → cegueira do hook, não do guest.

**Generalização sem faixa fixa:** quando não se sabe onde olhar, o histograma por
nibble ainda impõe uma grade. Alternativas de memória fixa e sem grade:
- **HyperLogLog** — cardinalidade de PCs distintos em ~2 KB, erro ~2%. Responde
  "quantos endereços únicos o guest executou" sem armazenar o conjunto.
- **Count-Min Sketch** — frequência aproximada por PC com teto de erro, memória
  fixa. Acha os PCs quentes (loop de espera, dispatcher) sem um mapa que cresce.
- **Reservoir sampling** — amostra uniforme de k PCs de um fluxo de tamanho
  desconhecido, uma passada, memória O(k). Útil para dumpar exemplos representativos.

---

## 2. Statistical debugging no corpus 62 — RESULTADOS REAIS

Referência: Liblit et al., *Scalable Statistical Bug Isolation* (PLDI 2005).

O projeto tem 62 títulos com **rótulo binário conhecido** em ambos os emuladores.
Isso é um experimento com 62 amostras rotuladas, hoje usado como lista de tarefas.

Definições (adaptadas para "render" em vez de "bug"):

    Failure(P)  = P(renderiza | preditor P verdadeiro)
    Context(P)  = P(renderiza | caso alcançado)          -- taxa base
    Increase(P) = Failure(P) - Context(P)

`Increase` alto = P **prevê** o desfecho, não apenas coocorre. Ranquear por
`Increase`, mas usar o **limite inferior de Wilson** em vez da proporção crua,
senão um preditor visto em 2 jogos supera um visto em 30.

### 2.1 Baseline medido [MEDIDO]

    nosso render (distinct >= 100):   4/62   ['abd','ddragonz','ironsight','torkandkral']
    zeebx render:                    36/62
    gap (zeebx sim, nos nao):        34

### 2.2 Tabela de contingência: status do gl_scan × render nosso [MEDIDO]

| status gl_scan | não-render | render | n |
|---|---|---|---|
| GL-FOUND | 0 | 1 | 1 |
| no-connect | 32 | 0 | 32 |
| ok | 12 | 2 | 14 |
| step-timeout | 13 | 1 | 14 |

`no-connect` = 32 casos, **zero** renders. Preditor perfeito de falha, e é
metade do corpus. O control server nunca sobe → o guest morre antes.

### 2.3 Preditor `assets` — números crus (ler junto com §2.4) [MEDIDO]

⚠ **Leia a §2.4 antes de usar esta tabela.** Os números estão corretos, a
interpretação óbvia deles não.

Cruzando o campo `assets` do census (o título recebeu `.bar`/`.ggz` ou `none`):

**No NOSSO emulador:**

| assets | não-render | render | taxa |
|---|---|---|---|
| bar | 12 | 3 | 20% |
| ggz | 0 | 1 | 100% |
| none | 45 | **0** | **0%** |

Fisher exato bicaudal: **p = 0,0035**. Nenhum dos 45 títulos sem asset renderiza
no nosso. `Increase(assets≠none) = 0,250 − 0,065 = +0,185`;
Wilson 95% de 4/16 = [0,102 ; 0,495] — o intervalo não toca a taxa base 0,065.

**No ZEEBX, o sinal INVERTE:**

| assets | não-render | render | taxa |
|---|---|---|---|
| bar | 13 | 2 | 13% |
| ggz | 0 | 1 | 100% |
| none | 12 | **33** | **73%** |

Fisher exato: **p = 0,00023**. O zeebx renderiza **33 dos 45** títulos que no
nosso harness estão marcados `assets: none`.

### 2.4 Auditoria do próprio achado — a interpretação ingênua está ERRADA

Antes de tirar conclusão, fui checar o código que gera os dois datasets. O
resultado **refuta** a leitura óbvia, e a lição metodológica é mais valiosa que
o achado original.

**Fato 1 — o zeebx nunca recebe asset.** `zeebx_census.py:29` monta:

```python
[str(ZEEBX), "run", str(modpath), f"--seconds={SECS}", f"--dump-gl={dump}"]
```

Só o `.mod`. Nenhum `--bar`, nenhum `.ggz`. Já `census62.py:build_argv()` passa
`--bar <path>` / data+sound `.ggz` quando `assets != none`.

**Fato 2 — não há asset escondido.** Varri as 45 pastas marcadas `assets:none`:
**0 delas** contêm `.bar` ou `.ggz`. O rótulo do `corpus62.json` está correto; o
`find_asset()` não está falhando. (As pastas têm `.vfs`, `.sar`, `.pak`,
`.h2z`, `.tex`, `.cimg` etc. — formatos que nenhum dos dois emuladores consome
por linha de comando.)

**Portanto `assets` NÃO é uma variável de tratamento — é um rótulo de
subpopulação.** Meu §2.3 tratou os dois emuladores como se estivessem no mesmo
experimento; não estão. A "inversão" é um artefato de comparar harnesses com
inputs diferentes. Isso é **confundimento por desenho**, exatamente o erro que a
§10.3 alerta — e eu o cometi na primeira versão desta seção.

**A comparação válida é pareada, dentro do subgrupo `none`** (onde ambos rodam
com o mesmo input: só o `.mod`):

    nosso   0/45 renderizam
    zeebx  33/45 renderizam

McNemar pareado: 34 discordantes, todos na mesma direção, **p = 2⁻³⁴ ≈ 6e-11**.

**Esta é a afirmação defensável**, e é mais forte que a original: nos títulos que
**não precisam de asset nenhum**, o zeebx renderiza 33 e nós renderizamos zero.
A diferença é puramente de emulação — não há input diferencial para explicá-la.
Confirma (não refuta) o que a skill do HLE registra: o gargalo é **alcançar o
loop de render**, e agora com magnitude quantificada.

**O que sobra de útil no preditor `assets` para o NOSSO lado:** dos nossos 4
renders, 4 têm asset (3 `bar` + 1 `ggz`) e 0 dos 45 sem asset. Como Fisher
`p = 0,0035`. Mas com n=4 isso é frágil, e a explicação provável é trivial —
títulos com asset são os que investimos tempo em fazer funcionar. **Correlação
com o histórico de esforço, não com causa técnica.** Não usar para priorizar.

**Controle negativo ainda pendente:** verificar que `distinct>=100` não é
artefato (título que só pinta fundo). O census guarda `top` (cor dominante);
exigir que a 2ª cor tenha massa não-trivial fecharia isso.

### 2.5 Esqueleto para ranquear qualquer preditor

```python
import json, math
from collections import defaultdict

def wilson(k, n, z=1.96):
    if n == 0: return (0.0, 1.0)
    p = k/n; d = 1 + z*z/n
    c = (p + z*z/(2*n))/d
    h = z*math.sqrt(p*(1-p)/n + z*z/(4*n*n))/d
    return (max(0.0, c-h), min(1.0, c+h))

def rank_predicates(cases, predicates, outcome):
    """cases: lista de dicts. predicates: {nome: fn(case)->bool}.
       outcome: fn(case)->bool (True = renderizou)."""
    base = sum(outcome(c) for c in cases) / len(cases)
    rows = []
    for name, fn in predicates.items():
        sel = [c for c in cases if fn(c)]
        if not sel: continue
        k = sum(outcome(c) for c in sel); n = len(sel)
        lo, hi = wilson(k, n)
        rows.append({"pred": name, "n": n, "failure": k/n,
                     "context": base, "increase": k/n - base,
                     "wilson_lo": lo, "conf": lo - base})
    # ordenar pelo LIMITE INFERIOR, nao pela proporcao crua
    return sorted(rows, key=lambda r: r["conf"], reverse=True)
```

Próximo passo natural: trocar `predicates` de metadados (assets/status) para
**"stub X foi chamado ao menos uma vez"**, emitindo o conjunto de stubs por
título no gl_scan. Aí o ranking aponta a chamada que separa quem renderiza de
quem não — o cluster Data East de 10 títulos vira 10 réplicas do mesmo ensaio.

---

## 3. Regra dos três — quantos controles negativos bastam

Se `n` entradas inválidas rodaram e **nenhuma** passou no gate, o limite
superior de 95% para a taxa de falso-positivo é ≈ **3/n**. [MEDIDO]

| n controles | teto 95% de FP |
|---|---|
| 1 | 300% (nada provado) |
| 3 | 100% (nada provado) |
| 10 | 30% |
| 30 | **10%** |
| 100 | 3% |
| 300 | 1% |

**Consequência direta para a skill do LLE:** "rodei um arquivo inválido e ele
falhou" é compatível com um gate que deixa passar quase tudo. Para afirmar que
o gate discrimina, o piso prático é ~30 controles.

Bateria sugerida (script de 10 linhas): bytes aleatórios; header truncado; magic
trocado; tamanho zero; ELF de outra arquitetura; `.mod` válido de OUTRO título;
arquivo com só o header certo e corpo zerado; asset trocado entre títulos.

O último é o mais informativo: se `ironsight` renderiza com o `.bar` do
`torkandkral`, o "render" não depende do conteúdo — e o gate provou o harness.

---

## 4. Significância de varredura de assinatura

O `qdsp5_proc_ids.md` recuperou `0x30000013` por adjacência a uma string. O
raciocínio está certo; dar número a ele fortalece a nota e protege dos próximos
scans.

Hits esperados por acaso ≈ `N × 2^(-bits)`, com N = posições no dump.
Para os 22,15 MB do `1.1.2_AMSS.bin` [MEDIDO]:

| padrão | hits esperados por acaso |
|---|---|
| 4 bytes (32 bits) | **0,0052** |
| 3 bytes (24 bits) | 1,3 |
| 2 bytes (16 bits) | **338** |

Um hit de constante de 4 bytes é fortíssimo (esperado 0,005). Adjacência à
string correta multiplica por outro fator grande. **A evidência de
`0x30000013` é sólida e agora quantificada.**

O inverso é o alerta: assinatura de 2 bytes tem ~338 hits por acaso — qualquer
"achado" nessa escala é ruído até prova em contrário. Os `nand/sig_scan*.py`
devem imprimir `N·p` junto do resultado. Correção de múltiplas comparações
aplicada a RE, custo zero — é exatamente a classe de erro que produziu o
`0x30000060` fabricado.

**Bônus — teste de aleatoriedade posicional:** se um padrão de k bytes aparece
`m` vezes, verificar se as posições são compatíveis com Poisson(N·p). Clustering
forte (várias ocorrências na mesma região) indica tabela real; espalhamento
uniforme indica coincidência.

---

## 5. SPRT no lockstep dynarmic × Unicorn

`src/lockstep.cpp` hoje termina em `"no divergence, budget exhausted"` — ou seja,
orçamento fixo de instruções. Isso tem dois problemas: gasta tempo demais quando
não há bug, e não dá garantia nenhuma quando termina limpo.

**Sequential Probability Ratio Test (Wald, 1945):** acumular a log-razão de
verossimilhança e parar assim que cruzar um limiar.

    A = log((1-β)/α)      # limiar de rejeitar equivalencia
    B = log(β/(1-α))      # limiar de aceitar equivalencia
    S = 0
    por bloco comparado:
        S += log(P(obs | divergente) / P(obs | equivalente))
        se S >= A: DIVERGE (para)
        se S <= B: EQUIVALENTE com erro tipo-II <= beta (para)

Ganho: divergência real é pega em poucas amostras; e o término limpo passa a
significar "equivalente com β explícito" em vez de "rodou muito, não quebrou".

**Amostragem estratificada por classe de instrução.** Amostrar blocos
uniformemente gasta o orçamento em `MOV`/`LDR`. Estratificar e alocar quota fixa
para as classes onde dynarec historicamente erra:

- shift/rotate com carry-out (`MOVS r0, r1, LSR #32` e o caso `#0`)
- flags em `ADCS`/`SBCS`/`RSCS`
- multiplicação longa com flags (`SMULLS`, `UMLALS`)
- saturadas ARMv6 (`QADD`, `SSAT`, `USAT`)
- `MSR`/`MRS` e troca de modo
- interworking Thumb↔ARM (`BX`, `BLX`)
- load/store múltiplo com base no set (`LDM` com writeback + base na lista)
- acesso não-alinhado

Estimador estratificado: `p̂ = Σ (Nh/N)·p̂h`, variância menor que a amostra
uniforme para o mesmo n.

**Cobertura de opcodes com Good-Turing (ver §6):** ao final, `f1/N` sobre classes
de opcode diz que fração do espaço de instruções o teste **nunca tocou**.

---

## 6. Unseen species — estimar o que ainda não se viu

Pergunta hoje sem resposta no projeto: quantos stubs/procs/opcodes **faltam
descobrir**? A ecologia resolveu isso; a adaptação é direta.

Seja `f1` = itens vistos exatamente 1×, `f2` = vistos exatamente 2×,
`S_obs` = distintos observados, `N` = total de observações.

**Good-Turing** — massa de probabilidade ainda não observada:

    P_unseen ≈ f1 / N

Se metade dos stubs distintos apareceu 1× só, a cobertura está longe. Se
`f1 ≈ 0`, aquele caminho de execução já foi essencialmente todo visto.

**Chao1** — piso para o total de espécies:

    S_est = S_obs + f1² / (2·f2)      (f2 > 0)
    S_est = S_obs + f1·(f1-1)/2       (f2 = 0)

**Captura-recaptura (Lincoln-Petersen)** — tamanho da superfície de API do BREW
a partir de dois títulos, sem ler o SDK:

    N ≈ (n_A · n_B) / n_AB

Ex.: jogo A toca 40 stubs, B toca 50, 20 em comum → ecossistema ≈ 100.
Usar o estimador de Chapman `((nA+1)(nB+1)/(nAB+1)) − 1` para amostras pequenas,
que é menos enviesado.

Aplicações no projeto:
- quantos procs AUDMGR ainda não apareceram em captura (Q0)
- fração do código de um título que nunca executou num boot (espécies = PCs de
  bloco básico) — mede quão longe do gameplay o guest parou
- quantas interfaces BREW o corpus inteiro implica que existem

---

## 7. Classificar código × dado melhor que entropia

`MORE_INFO.md` §5.3 propõe entropia de Shannon. Ela separa comprimido de
não-comprimido, mas é fraca para separar **código ARM de tabela de dados** —
ambos caem na faixa média.

**Qui-quadrado no campo de condição.** Em código ARM de verdade, o nibble alto
de cada word é fortemente enviesado para `0xE` (condição AL, incondicional),
tipicamente 60–85%. Dados não têm esse viés.

```python
from collections import Counter
def arm_code_score(buf, off, n=256):
    """Retorna (frac_E, chi2). frac_E alto + chi2 alto => codigo ARM."""
    nib = Counter()
    for i in range(off, min(off+n*4, len(buf)-3), 4):
        w = int.from_bytes(buf[i:i+4], "little")
        nib[w >> 28] += 1
    tot = sum(nib.values())
    if tot == 0: return (0.0, 0.0)
    exp = tot/16                      # H0: nibbles uniformes (dado)
    chi2 = sum((nib[k]-exp)**2/exp for k in range(16))
    return (nib[0xE]/tot, chi2)
```

Com 15 graus de liberdade, χ² > 30 já rejeita uniformidade a p<0,01. Dá
**p-valor** em vez de um limiar de entropia chutado. Para Thumb, o análogo é a
distribuição dos 5 bits altos do halfword.

**Detecção de ponto de mudança em vez de janela fixa.** Janela deslizante de
256 B borra fronteiras e obriga a escolher o tamanho. **CUSUM** ou **PELT**
acham *onde* a distribuição muda — que é exatamente o que se quer para segmentar
um dump de 22 MB em regiões code/data/comprimido, sem grade arbitrária.

---

## 8. Comparação de frames quando há não-determinismo

A skill do HLE registra que acima do tick ~513 o guest ocioso gera variância, e
que por isso o oráculo só congela checkpoints abaixo do plateau. Correto — mas
isso descarta a região interessante. Alternativa: **parar de comparar hash e
comparar distribuição.**

- **Bootstrap** sobre a MAE por quadrante: reamostrar com reposição, obter IC 95%.
  Aí "MAE 2,53 vs 2,69" vira diferença significativa ou não, em vez de dois
  números soltos. (Os fixes registrados na skill — RGBA8, TexEnv — foram
  validados por números pontuais; bootstrap os tornaria defensáveis.)
- **Kolmogorov-Smirnov bicaudal** entre histogramas de cor nosso × zeebx: testa
  se as duas distribuições de pixel vêm da mesma população, robusto a shift.
- **Tolerância perceptual**: SSIM em vez de MAE quando o alvo é "parece igual",
  já que MAE penaliza shift de 1 LSB igual a artefato estrutural.
- **Teste de permutação** para runs não-determinísticos: rodar k vezes cada
  versão, embaralhar os rótulos, ver onde a diferença observada cai na
  distribuição nula. Não assume normalidade — adequado a n pequeno.

---

## 9. Diff de traces sem guardar traces

Para achar a primeira divergência entre dois emuladores sem gravar gigabytes:

- **MinHash / simhash** do conjunto de blocos básicos por janela de execução.
  Similaridade de Jaccard alta = janelas equivalentes; a primeira janela onde
  despenca localiza a divergência. Só então gravar o trace completo daquele
  intervalo. **Busca binária com esboço**, em vez de log linear.
- **Bloom filter** por janela: teste barato de "este PC apareceu no outro
  emulador?" com falso-positivo controlado e zero falso-negativo.
- **Rolling hash do banco de registradores** a cada N blocos: uma palavra por
  checkpoint em vez do estado inteiro; divergência aparece como quebra de hash e
  aí se refina.

---

## 10. Erros estatísticos a evitar (específicos deste projeto)

1. **Teste sem poder** — §1. O filtro antes da contagem impede a hipótese de
   falhar. Verificar sempre: "se minha hipótese for falsa, este teste muda?"
2. **Múltiplas comparações** — §4. Varrer 22 MB com padrão curto acha qualquer
   coisa. Reportar `N·p` junto do hit.
3. **Confundidor** — §2.4. `assets` correlaciona com render, mas pode
   correlacionar com "título completo no dump". Correlação forte não dispensa o
   experimento de intervenção.
4. **Suporte baixo** — preditor visto em 2 casos com 100% de acerto é ruído.
   Ordenar por Wilson inferior, nunca por proporção crua.
5. **Aceitar H0 por cansaço** — "budget exhausted" não é equivalência. §5.
6. **n=1 como prova** — §3. Um controle negativo não limita nada.
7. **Comparar pontos onde há distribuição** — §8.

---

## 11. Próximos passos concretos

Reordenado após a auditoria das §2.4 e §13 (o item "experimento de intervenção
sobre assets" foi **removido** — a §2.4 mostrou que não há intervenção possível:
`assets` é rótulo de subpopulação, não tratamento).

1. **Ampliar cobertura do fuzz QDSP5 antes de rodar mais tempo** (§13.2).
   Com `cov 134`, 75k ou 300k execuções mudam pouco: o fuzzer não acha bug em
   código que não executa. Completar `classify()`/engine-map (Q1.2a) é o
   multiplicador real. **Maior retorno da lista.**
2. **`N·p` impresso nos `sig_scan*.py`** (§4, §13.5) — 2 linhas, previne o
   próximo `0x30000060`.
3. **Trocar o histograma de PC pela versão não-filtrada** (§1) — 5 linhas.
4. **Reportar regra dos três nos artefatos de campanha** (§13.2) — trocar
   "210/210 pass" por "discordância ≤1,4% (95%)". Uma linha no gerador de JSON.
5. **Investigar os 2 cex de `wrapper_abi`** (§13.4) — Wilson [1,5%; 18,1%] não
   inclui zero; são bugs reais, não ruído.
6. **Controle negativo do oráculo de render** (§2.4) — exigir 2ª cor com massa
   não-trivial, senão `distinct>=100` pode ser fundo pintado.
7. **Emitir conjunto de stubs por título no gl_scan** → habilita o ranking de
   predicados de verdade (§2.5). É a técnica de maior alavancagem estrutural,
   mas depende de instrumentação nova.
8. **Good-Turing sobre features do libFuzzer** (§13.3) — critério de parada
   mensurável em vez de palpite.
9. **SPRT no lockstep** (§5, §13.4) quando o bringup passar de amostras pontuais
   para varredura.

---

## 13. Aplicação aos harnesses que JÁ existem [MEDIDO]

Auditoria das três bases (`zeebo-lle`, `zeebo-emulator`, `zeebo-dynarmic-bringup`).
A infraestrutura estatística está mais avançada do que as notas sugerem — o que
falta é **quantificar o que já roda**, não construir coisa nova.

### 13.1 O que já existe e é estatisticamente sólido

- `tools/fuzz_pathb_campaign.py` — **property-based differential fuzzing** com
  `random.seed(0xB007)` fixo, registrando `firmware_sha256` e versões de
  `unicorn`/`capstone` no artefato. Isso é **reprodutibilidade correta**: o
  experimento pode ser refeito bit a bit. É o padrão a replicar nos demais.
- `tools/cpp/qdsp5/qdsp5_fuzz.cpp` — libFuzzer + ASAN; achou 2 bugs reais
  (OOM em `do_play` na iteração ~11.474; heap overflow de 1 byte com
  `pcm_bytes` ímpar). Fuzzing guiado por cobertura, com oráculo real (ASAN).
- `zeebo-emulator/testkit/` — corpus rotulado de 62 títulos em dois emuladores.
- `nand/xdr_emu.py` — Unicorn com sentinelas + `UC_HOOK_MEM_READ` para recuperar
  serializers XDR.

### 13.2 Números que faltavam nesses artefatos [MEDIDO]

**Regra dos três aplicada ao `campaign_report.json`:**

| suíte | n | pass | cex | teto 95% p/ taxa de falha não vista |
|---|---|---|---|---|
| minpage_ctz_differential | 210 | 210 | 0 | **1,43%** |
| largest_fpage | 409 | 409 | 0 | **0,73%** |
| wrapper_abi | 36 | 34 | **2** | — (tem cex) |

Ou seja: "210/210 pass" significa **"a taxa de discordância modelo×firmware é
≤1,4% com 95% de confiança"** — não "o modelo está certo". Frase honesta e
publicável, e mostra onde vale aumentar n.

**Poder do fuzz QDSP5 (75.563 execs, 0 crashes):**

| taxa real do bug | P(sobreviver 75.563 execs) |
|---|---|
| 1e-3 | 1,5e-33 (impossível esconder) |
| 1e-4 | 0,0005 |
| **1e-5** | **0,47** |

Leitura: o fuzz **exclui** bugs com taxa ≥1e-4, mas um bug de taxa 1e-5 tem 47%
de chance de ter passado batido. Como a nota já reconhece (`cov 134` baixa por
`classify()` incompleto), a cobertura — não o número de execuções — é o limite.
`P(escapar) = e^(−p·N)`; para excluir 1e-5 a 95% seriam ~300k execs **na mesma
cobertura**, o que não resolve: caminhos não cobertos têm p efetivo = 0.

**Prioridade correta:** ampliar cobertura (Q1.2a, rotas dos 6 engines) vale mais
que rodar mais tempo. O fuzzer atual não pode achar bug em código que não executa
— é o mesmo defeito do histograma filtrado da §1, em outra roupagem.

### 13.3 Good-Turing sobre o corpus do libFuzzer

libFuzzer já mantém `cov`/`ft` (features). O que falta: emitir a **distribuição
de frequência das features** para calcular `f1/N`. Com ela:

- `f1/N` alto → o fuzz ainda está descobrindo; continuar rendendo.
- `f1/N` ≈ 0 → saturou naquela cobertura; parar e ampliar o alvo.

Isso converte "quando paro de fuzzar?" de palpite em critério mensurável. Vale
igual para o `fuzz_pathb_campaign.py` (features = pares (PC, resultado)).

### 13.4 `lockstep.cpp` e o `wrapper_abi` com 2 contraexemplos

`src/lockstep.cpp` do bringup encerra com `"no divergence, budget exhausted"` —
é exatamente o caso da §5 (aceitar H0 por cansaço). Trocar por SPRT dá β
explícito no lugar de um orçamento arbitrário.

Já `wrapper_abi` tem **2 cex em 36** — aí não é questão de confiança, é bug
localizado. 2/36 = 5,6%; Wilson 95% = [1,5% ; 18,1%]. Se o alvo é "ABI correta",
o intervalo não inclui zero: são falhas reais, não ruído.

### 13.5 Lacuna transversal: nenhum harness registra `N·p`

Os `sig_scan*.py` e as buscas de padrão em `nand/` não imprimem o número de hits
esperados por acaso (§4). É a correção mais barata do repo — 2 linhas — e ataca
diretamente a classe de erro que gerou o `0x30000060` fabricado.

---

## 14. Referências

- Liblit, Naik, Zheng, Aiken, Jordan. *Scalable Statistical Bug Isolation*, PLDI 2005.
- Wald. *Sequential Analysis*, 1947 (SPRT).
- Good. *The population frequencies of species and the estimation of population
  parameters*, Biometrika 1953.
- Chao. *Nonparametric estimation of the number of classes in a population*, 1984.
- Hanley & Lippman-Hand. *If nothing goes wrong, is everything all right?*,
  JAMA 1983 (regra dos três).
- Wilson. *Probable inference, the law of succession, and statistical inference*, 1927.
- Broder. *On the resemblance and containment of documents*, 1997 (MinHash).
- Killick, Fearnhead, Eckley. *Optimal detection of changepoints*, JASA 2012 (PELT).
- McKeeman. *Differential Testing for Software*, Digital Technical Journal 1998.
- McNemar. *Note on the sampling error of the difference between correlated
  proportions or percentages*, Psychometrika 1947.

---

## 15. Registro de auto-correções desta nota

Documentado porque o padrão de erro se repete no projeto (ver §8 de `MORE_INFO.md`).

1. **§2.3/§2.4 — interpretei uma correlação forte (p=0,0002) como achado
   técnico sem checar o código que gera os datasets.** O `zeebx_census.py` nunca
   passa asset; `assets` não é variável de tratamento. Corrigido para McNemar
   pareado dentro do subgrupo `none` (p≈6e-11), que é conclusão mais forte e
   defensável. **Mesmo padrão autorreconhecido em `MORE_INFO.md`: generalizar
   sem checar os artefatos que estão no disco.**
2. **§11 — o "próximo passo de maior retorno" original (experimento de
   intervenção sobre assets) era impossível** e foi removido.
3. Fisher exato implementado duas vezes de forma independente e conferido
   (`scipy` não está instalado no host); soma hipergeométrica = 1,0 como sanity.
