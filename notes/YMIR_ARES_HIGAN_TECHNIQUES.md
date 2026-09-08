# Técnicas de Ymir, ares e higan aplicáveis ao Zeebo LLE

Data da revisão: 2026-09-08

## Escopo e proveniência

- Ymir: clone local `~/projects/ymir-muos-h700/ymir-src`, commit `22d0d9cb7ac1e2343261144cdf217f54fff5684d`.
- ares: fonte oficial `ares-emulator/ares`, commit `af4cbb04f067682a8a3cf42695ff78bed634b38d`.
- higan: fonte oficial `higan-emu/higan`, commit `8f4df010715298455b92cfc0821ea833770a8b5a`.
- Ymir e o núcleo do higan são GPL-3.0-or-later: usar arquitetura e comportamento como referência; não copiar código para o Zeebo LLE sem antes decidir a licença do projeto.
- O núcleo do ares usa licença ISC permissiva, mas dependências possuem licenças próprias. Qualquer incorporação literal exige preservar avisos e isolar a dependência.

## Resultado executivo

Há quatro ganhos concretos. Dois devem entrar cedo no Zeebo LLE:

1. **Checkpoint de máquina completa, validado e reproduzível** (Ymir/ares).
2. **Separação entre probes sem efeitos e tracers opcionais** (Ymir), exposta ao motor de scripts/ControlServer.
3. **Testes de CPU por estado inicial/final e transações de barramento** (ares).
4. **Adaptador GDB RSP sobre a API de debug existente** (ares), útil mas posterior aos três anteriores.

O scheduler por corrotinas de higan/ares não é um quick win para a arquitetura atual baseada em Unicorn. Para Zeebo, o scheduler de deadlines absolutos do Ymir é uma referência mais direta.

## 1. Ymir

### A. Scheduler de deadlines absolutos — adotar o desenho, não o código

`libs/ymir-core/include/ymir/core/scheduler.hpp` mantém um contador global, deadlines absolutos, o deadline mais próximo pré-calculado e fatores racionais `numerator/denominator` por evento. Eventos são registrados com IDs estáveis e podem ser serializados.

Aplicação no Zeebo:

- unificar timers APPS/ARM11, ARM9/modem, MDDI/GPU e futuros eventos QDSP5;
- executar Unicorn somente até `min(quantum, próximo_deadline)`;
- representar clocks diferentes sem acumular `double`;
- carregar **spillover cycles** quando um core ultrapassa o quantum/deadline e serializar esse débito; o Ymir faz isso ao sincronizar master/slave SH-2 e SH-1;
- salvar/restaurar também deadlines e callback-ID, não ponteiros.

**Prioridade:** alta depois do Passo 13. No boot atual, um scheduler completo não resolve `bi_execute`; antes disso, basta criar a interface e um teste determinístico pequeno.

### B. Barramento paginado com fast path e acesso de debug — adotar incrementalmente

`libs/ymir-core/include/ymir/sys/bus.hpp` divide o espaço em páginas. Cada página aponta diretamente para RAM ou para handlers MMIO; mapeamento de arrays pode espelhar a mesma memória em várias faixas. O mesmo mapa mantém waitstates e separa `Read/Write` de `Peek/Poke` sem efeitos.

Aplicação no Zeebo:

- formalizar aliases VA→mesma RAM física sem duplicar armazenamento;
- manter caminho direto para RAM e hooks apenas para MMIO;
- associar custo de acesso por região ao futuro scheduler;
- impedir que depuração altere FIFOs, flags read-to-clear ou ponteiros de transferência.

Como Unicorn já controla mapeamento e hooks, aproveitar o desenho; não transplantar o template GPL.

### C. Probes versus tracers — adotar já

A documentação do Ymir separa:

- `Probe`: consulta/mutação direta de estado interno, sempre disponível e sem custo em hot path;
- `Tracer`: callbacks de eventos, conectados somente quando necessários e com modo de tracing explícito;
- `Bus::Peek/Poke`: leitura/escrita de debug que evita efeitos colaterais;
- eventos quentes podem ir para ring buffer lock-free e ser consumidos fora da thread de emulação.

Aplicação no Zeebo:

- `ProbeRegistry` para MMU, IRQ, timers, EFS/VFS, GPU, surfaces e filas IPC;
- comandos de script `probe.get`, `probe.set`, `probe.list`;
- distinguir `peek` físico/sem efeito de uma leitura MMIO real;
- manter tracing fora do C++ por padrão: o C++ publica eventos tipados somente quando um assinante existe; filtragem e automação ficam em Lua/Python/ControlServer.

Tracers semânticos também distinguem execução, branch, call, return, exceção, interrupção e mudanças de pilha. Isso pode fortalecer o `backtrace`: o cliente mantém uma pilha de chamadas baseada em eventos reais, em vez de tentar inferi-la apenas da RAM.

Isso atende à diretriz de parar de adicionar instrumentação ad hoc sem perder observabilidade.

### D. Save-state validado e pós-restauração — corrigir o save-state atual

O Ymir usa três etapas por componente: `SaveState`, `ValidateState`, `LoadState`, com `PostLoadState`/sincronização quando caches, callbacks ou render threads precisam ser reconstruídos. O scheduler e os eventos pendentes também fazem parte do estado.

O `ZeeboSaveStateManager` atual (`tools/cpp/zeebo_save_state.h`, versão 2) salva contextos Unicorn, contadores/PCs e todas as regiões mapeadas. Ele **não salva o estado dos modelos de dispositivo**, scheduler/eventos, hooks de script, GPU/GL, EFS/VFS ou metadados de IRQ. Portanto ainda não é checkpoint de máquina completa.

Quick win proposto: formato chunked versão 3, com hash da cópia de NAND/firmware e cada subsistema implementando:

- `save(writer)`;
- `validate(reader)` sem mutar a máquina;
- `load(reader)`;
- `post_load()` para invalidar cache JIT, recompor callbacks/ponteiros e caches derivados.

Gate obrigatório: salvar no ponto A, rodar N eventos, guardar hash de bytes/pixels/registros; restaurar A, repetir N eventos e exigir os mesmos hashes.

### E. Protocolo JSON-RPC tipado — aproveitar sem quebrar NDJSON

O clone local possui framing por linha e mensagens JSON-RPC 2.0 tipadas, com IDs, erros estruturados e notificações assíncronas. Nosso ControlServer já usa NDJSON, mas o parser textual e a ausência de notificações robustas são débitos conhecidos.

Aplicação: manter o transporte TCP/NDJSON e evoluir o envelope para `id/method/params/result/error`; usar notificações para breakpoint, watchpoint e evento de probe. Fazer compatibilidade temporária com os comandos atuais.

### F. Dirty tracking para o futuro dynarec — preservar no plano, não antecipar

O trabalho de dynarec presente no clone local usa buckets de código com bits sujos e contadores de versão; blocos compilados verificam a versão da região e são recompilados após escrita. Também mantém caches por instância de CPU e telemetria de hit/miss/fallback.

Aplicação futura: ARM11 e ARM9 com caches separados, `notify_write(address,size)`, regiões ROM dispensadas de tracking e gate intérprete-versus-JIT por bytes. Enquanto Unicorn continuar sendo o executor, `uc_ctl_remove_cache` permanece o mecanismo correto.

### G. Headless core e callbacks — já estamos alinhados

Ymir roda sem áudio/vídeo e oferece renderer nulo. O Zeebo LLE já é CLI/headless e valida pixels/bytes; não há mudança estrutural necessária.

## 2. ares

### A. Testes transacionais de CPU — adotar já

`tests/arm7tdmi/arm7tdmi.cpp` carrega casos com estado inicial/final e uma sequência esperada de transações de barramento (`prefetch/load/store`, tamanho, endereço, dado, ciclo e flags de acesso). O teste falha tanto por registro errado quanto por acesso de memória fora de ordem.

Aplicação no Zeebo:

- evoluir os atuais 12 testes ARM para vetores declarativos;
- registrar estado ARM11 inicial/final, exceções e transações de memória;
- separar casos ARM, Thumb, interwork, MMU/abort e ARMv6;
- aceitar corpora externos somente quando a licença/proveniência estiver clara; não assumir que casos ARM7TDMI cobrem ARM1176.

É mais forte que validar apenas o PC final ou instruction-count e combina com o critério “bytes, nunca insn-count”.

### B. Serializer simétrico — aproveitar o contrato, não puxar nall

O `nall::serializer` usa uma única função `serialize(s)` para leitura e escrita, reduzindo divergência entre dois caminhos manuais. Para o formato chunked v3 do Zeebo:

- cada dispositivo declara seus campos uma vez;
- header inclui magic, versão numérica, string de compatibilidade/build e hash da NAND;
- inteiros são little-endian;
- floats de GPU são serializados pelo padrão de bits, não pelo valor nativo, pois o próprio nall alerta que floating point não é portável entre implementações;
- rejeitar integralmente o estado na fase de validação antes de mutar qualquer componente.

Não vale importar toda a nall; uma interface local pequena preserva o benefício sem dependências transitivas.

### C. GDB Remote Serial Protocol — aproveitar como adaptador, não substituir o agente

O ares possui `nall::GDB::Server`, independente de sistema e dirigido por callbacks de memória, registradores, invalidação de cache, breakpoints e watchpoints. A integração real está no N64 e suporta GDB CLI, VSCode e CLion.

O Zeebo já possui pause/continue/step, breakpoints, `peek/poke`, backtrace e invalidação do cache Unicorn no ControlServer. Logo um servidor RSP ARM é de escopo moderado:

- mapear registradores ARM11/CPSR e XML de target;
- reutilizar memória sem efeitos e `uc_ctl_remove_cache` após escrita;
- implementar `Z0/Z2/Z3/Z4` para break/watch;
- manter NDJSON/Python como API principal do agente autônomo.

**Prioridade:** média. É excelente para depuração humana e IDE, mas não desbloqueia o boot sozinho.

### D. Trace masking e invalidação — portar o comportamento para a camada de scripts

O tracer de instruções do ares pode:

- mascarar endereços já visitados;
- suprimir repetições recentes por profundidade configurável;
- contar eventos omitidos;
- invalidar o bit “visitado” quando código executável é modificado.

Aplicação: adicionar filtros equivalentes no cliente Python/Lua do Zeebo, alimentados por hooks existentes. Isso reduz logs gigantes sem nova instrumentação fixa no core.

### E. Barramento: falha explícita em acesso não mapeado — combinar com o fast path

O barramento do N64 no ares despacha faixas de forma explícita e possui caminhos como `freezeUnmapped`/`freezeUncached`, registrando também o PC responsável. Não devemos trocar a LUT/memória direta do Zeebo por uma cascata de ranges, mas podemos adotar a política:

- RAM e aliases continuam no caminho rápido;
- MMIO conhecido vai para handlers tipados por largura;
- região desconhecida pausa com endereço, largura, direção, valor e PC em evento estruturado;
- escrita em região executável invalida o cache correspondente.

Isso é mais diagnosticável que transformar silenciosamente uma falha de mapeamento em leitura zero ou sucesso.

### F. Entropia determinística — adotar quando houver fontes não determinísticas

O ares oferece seed determinístico. O Zeebo deve fixar RTC, RNG, input e ordem de eventos nos harnesses, registrando o seed no checkpoint. Isso será importante ao chegar ao BREW/Z-Wheel completo.

## 3. higan

### O que vale preservar

- **contador relativo ARM11↔ARM9**: um `int64` representa qual core está adiantado; cada lado acumula a frequência do outro. É mais simples que um scheduler de corrotinas para apenas dois processadores;
- **sync-on-shared-access**: deixar um core avançar e sincronizar o outro quando houver acesso às janelas IPC/SMD/mailbox compartilhadas; manter fallback configurável para slices conservadores;
- serialização bidirecional uniforme e little-endian;
- save-state não pode avançar o tempo e todos os dispositivos devem participar;
- componentes com clocks próprios precisam compartilhar uma linha temporal determinística.

O melhor desenho para o Zeebo é híbrido: contador relativo/dívida para ARM11↔ARM9 e fila de deadlines do Ymir para timers, GPU, IRQ e QDSP5. O acesso compartilhado força reconciliação antecipada.

### O que não devemos importar agora

O scheduler de higan/ares usa corrotinas cooperativas (`libco`) por componente. É elegante para consoles com chips independentes, mas conflita com o Zeebo atual:

- dois engines Unicorn e hooks já formam as unidades de execução;
- corrotinas complicariam pause/step, callbacks e save-state;
- o problema imediato é semântica BootInfo/L4, não sincronização fina;
- o scheduler do Ymir por deadlines pode ser introduzido incrementalmente e testado por bytes.

A maior parte das ideias úteis do higan chegou mais madura ao ares. Usar ares como referência principal e higan apenas para contexto arquitetural.

## Priorização sugerida

### QW-A — checkpoint determinístico completo

Completar save-state com dispositivos/eventos e teste save→run→restore→rerun. Alto retorno para RE do Passo 13 e para bisecção de regressões.

### QW-B — ProbeRegistry no ControlServer

Começar por MMU/BootInfo, IRQ e GPU. API mínima: `probe.list`, `probe.get`, `probe.set`. Sem traces permanentes no C++.

### QW-C — filtros de trace no cliente Python

Máscara por endereço, janela de repetição, contagem de omitidos e invalidação após `poke`. Pequeno e imediatamente útil.

### QW-D — vetores transacionais ARM11

Converter um teste existente e provar acesso de barramento + estado final; depois ampliar a cobertura ARMv6/Thumb.

### Depois do Passo 13

- protótipo do contador relativo ARM11↔ARM9 com fallback;
- sync-on-shared-access nas janelas IPC/SMD;
- scheduler de deadlines absolutos para periféricos/eventos;
- GDB RSP ARM11;
- watchpoints de leitura/escrita;
- seed/RTC/input determinísticos integrados ao checkpoint.

## Fontes primárias verificadas

- Ymir scheduler: `libs/ymir-core/include/ymir/core/scheduler.hpp` no clone local.
- Ymir execução/save-state/debug: `libs/ymir-core/src/ymir/sys/saturn.cpp`, `libs/ymir-core/docs/mainpage.hpp` e `libs/ymir-core/include/ymir/hw/sh2/sh2.hpp`.
- ares scheduler/tracer/GDB/testes: <https://github.com/ares-emulator/ares/tree/af4cbb04f067682a8a3cf42695ff78bed634b38d/ares/ares>, <https://github.com/ares-emulator/ares/tree/af4cbb04f067682a8a3cf42695ff78bed634b38d/nall/nall/gdb> e <https://github.com/ares-emulator/ares/tree/af4cbb04f067682a8a3cf42695ff78bed634b38d/tests/arm7tdmi>.
- higan scheduler/debug/serializer: <https://github.com/higan-emu/higan/tree/8f4df010715298455b92cfc0821ea833770a8b5a/higan/higan> e <https://github.com/higan-emu/higan/blob/8f4df010715298455b92cfc0821ea833770a8b5a/nall/serializer.hpp>.
- Estado atual do Zeebo: `tools/cpp/zeebo_save_state.h`, `tools/cpp/zeebo_lle_main.cpp` e `tools/cpp/Makefile`.

## Decisão

Adotar **Ymir para estado/scheduler/debug sem efeitos** e **ares para testes transacionais, filtros de trace e GDB RSP**. Não importar o scheduler por corrotinas de higan/ares nesta fase. Não copiar código GPL do Ymir/higan; documentar e reimplementar os comportamentos necessários. O ares pode fornecer código permissivo, mas somente em módulo isolado com atribuição e revisão das dependências.
