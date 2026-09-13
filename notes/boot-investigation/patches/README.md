# Patches de investigação — laço de page-table walk do Core1

Patches **NÃO aplicados** (a árvore está limpa). São diffs contra `c8f8cf2`,
guardados como evidência de medição, não como correção pronta.

Aplicar com:

    git apply notes/boot-investigation/patches/<arquivo>

## Contexto

O Core1 (ARM926, OKL4) parava após `creating root server (000a8001)`, girando
num laço infinito de page-table walk em `0xf0003df4..f0004080`, com o VA
`sl=0xb0000007` congelado.

**Causa raiz medida**: a janela do heap REX (`0xf0000000 + 2MB`) cobre não só o
`.text` do kernel, mas também o `.rodata`. O shadow do Split I/D nasce como
cópia do pristino, mas o laço de zeragem do kernel apaga essa faixa **no
shadow**. Leituras de `.rodata` passam a devolver zero.

O alvo concreto é a **tabela de tamanhos de página do OKL4** em `0xf000efc8`,
lida por `f0016a2c  ldr r1,[fp,r5,lsl#2]` (`fp=f000efc8`, `r5=3`):

    pristino/RAM: [0]=0c [1]=10 [2]=14 [3]=1a [4]=20
    shadow:       [0]=00 [1]=00 [2]=00 [3]=00 [4]=00

Com `r1=0`, o campo *size* do fpage sai 0 (`and r1,r1,#0x3f`;
`orr r2,r2,r1,lsl#4`), a máscara `mvn r2,#0 lsl r8` vira `0xffffffff`, e o walk
nunca converge.

Nota: o `7` em `0xb0000007` é o campo **rights** do fpage L4 — valor legítimo,
não corrupção. Isso foi perseguido por engano por várias rodadas.

## 01-rodata-splitid-probe.patch

Serve leituras de `0xf000e000..0xf0010000` a partir do pristino em vez do
shadow. **Prova de causalidade**, não correção:

    antes:  laço infinito, 7.010.000 insns, sl=b0000007
    depois: 58.910.000 insns; o laço some e o kernel avança até
            `Assertion !"Failed to create root server TCB" failed
             in file pistachio/src/thread.cc, line 1273`

Por que não foi commitado: a faixa é **hardcoded**. A correção correta é
derivar o fim real da imagem do kernel e não entregar essa região ao heap.

## 02-stack-splitid-experimento.patch

A pilha do kernel (`sp ≈ 0xf00196xx`) também cai na janela do heap, e o
mecanismo de Split I/D "restaurava" words da pilha como se fossem código,
apagando o frame salvo. Medido em `f0019750`: `SHADOW=b0000007` mas `RAM=0`.

O patch exclui a faixa da pilha da restauração. A assimetria some
(`RAM=SHADOW`), mas **não destrava o boot** — é uma anomalia real e distinta,
com limites igualmente hardcoded (`0xf0019000..0xf001a000`).

## Conclusão estrutural

Os três fixes já commitados (`e7de3aa` literal pool, `7e84667` jump table,
mais o da pilha aqui) são remendos pontuais do mesmo defeito: a distinção certa
não é *código vs dado*, é **imagem do kernel vs heap real**.

### Tentativa de correção estrutural — REFUTADA por medição

A hipótese natural ("encolher o heap para começar após o fim da imagem") **não
funciona**. Segmentos reais do ELF, medidos:

    va=f0000000  filesz=0001a324  memsz=0001e2c0
    va=f0020000  filesz=00006000  memsz=00006000

A pilha do kernel (`sp ~ 0xf00196xx`) está DENTRO do primeiro segmento, abaixo
até do `filesz` (`0xf001a324`) — a mesma região carregada que contém o
`.rodata` de `0xf000efc8`. Um limiar único não separa os dois casos:

- `0xf000efc8` (.rodata) precisa vir do **pristino**
- `0xf0019750` (pilha)   precisa vir do **shadow**

Testado com `memsz` e com `filesz`: em ambos o boot REGRIDE de 5.210.000 para
**16.717 insns** com `pc=0` (a pilha congela).

### Direção correta

A separação tem de ser por **natureza da região** (somente-leitura vs
gravável), não por endereço-limite — ou seja, limites de **seção**, não de
segmento. Se o super-ELF do AMSS não expõe seções, o caminho é inferir a faixa
`.rodata` por comportamento: região escrita apenas pelo laço de zeragem e lida
como constante.
