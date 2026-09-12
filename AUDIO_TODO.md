# AUDIO_TODO — áudio verificável do AppMgr, Z-Wheel e Double Dragon

Revisão: 2026-09-10. Código examinado: `f7dea3ac1da87ab2037c81af21fd708563dc3368`.
Estado: saída SDL e mixer existem; áudio originado pelo firmware/jogo NÃO demonstrado.
Esta revisão é documental: não corrige o runtime nem libera alterações no QDSP5.

**Atualização 2026-09-10 (HEAD `5471a8b`)** — dois defeitos de MEMÓRIA no caminho de áudio
já foram corrigidos, mas nenhum deles produz som do jogo:

- `f3d493f` — corrida de dados no `UnifiedAudioSink` (callback SDL vs. thread de emulação)
  e uso-após-liberação na destruição do sink. Teste sob TSan: `test_audio_lifetime.cpp`.
- `f3d493f` — leitura fora dos limites em `zeebo_elf.cpp` (cabeçalhos de seção não validados
  antes do acesso). Teste sob ASan: `test_elf_bounds.cpp`.

Isso torna o caminho de áudio SEGURO, não FUNCIONAL. O gate do item 1 (nenhuma captura
associa requisição real do DD a PCM reproduzido) permanece ABERTO e inalterado.

**Bloqueio a montante**: o boot do Core0 não chega ao consumidor AMSS do QDSP5 em cold boot.
Enquanto o boot não fechar, `feed_raw` recebe `dummy_payload(16, 0x42)` do HOST, não pacote
do guest — ver a fila de trabalho do backend recompilado na Fase 14 do ROADMAP.

## 0. Escopo e divisão de responsabilidades

- Este arquivo: integração ponta a ponta, segurança do callback, transporte, saída host,
  sincronização A/V, regressões e aceitação de som jogável.
- [QDSP5_TODO.md](QDSP5_TODO.md): protocolos, filas, engines, layouts, replies e pesquisa DSP.
- [GPU_TODO.md](GPU_TODO.md): pixels de origem guest, composição e apresentação.
- [ROADMAP.md](ROADMAP.md): loader/ABI BREW, scheduler, arquivos e objetivo comercial.

`tools/cpp/qdsp5/` e `tools/cpp/gpu/` estão LIBERADOS para desenvolvimento por Rafael (2026-09-11).
A injeção fabricada de pacotes RPC de liveness foi desacoplada em `zeebo_smd_bridge_unified.h` e
permanece desligada por padrão, só habilitada via `ZEEBO_QDSP5_RPC_PROBE=1` exato.
Qualquer edição em QDSP5 ou GPU deve seguir TDD estrito com controle negativo e citar o gate
correspondente de `QDSP5_TODO.md` ou `GPU_TODO.md`. Não promover contadores ou ACKs vazios a
áudio/vídeo funcional.

Legenda: `[x]` comprovação limitada descrita no item; `[ ]` pendente.
Tipos de evidência: execução reproduzida; inspeção estática; hipótese a confirmar.
Um smoke com PCM sintetizado pelo host não satisfaz um gate de áudio comercial.

## 1. Baseline observado

- [x] `UnifiedAudioSink` mistura/re-amostra PCM em teste isolado:
  `tools/cpp/zeebo_audio_sink.h` e `test_audio_sink.cpp`.
- [x] `AudppEngine` alimenta esse mixer a partir de uma struct PROVISÓRIA;
  `qdsp5_smoke` produz WAV sintético não silencioso, peak=11999.
- [x] `UnifiedHostAudio` abre SDL e registra callback estéreo S16.
  `zeebo_lle_main.cpp:426–487`; headless não abre dispositivo.
- [x] Há conexão de callback para `Qdsp5Dispatcher::mix_audio` em `main:1155–1160`.
  Portanto é falso dizer que não existe saída host ou ligação AUDPP→mixer.
- [x] Existem capture hook e feed nos PCs AMSS `0x16e8cb96`/`0x16e8cba0`
  (`main:2515`, `3451–3466`). Registro do hook não prova que o cold boot o alcança.
- [ ] Nenhuma captura nesta revisão associa uma requisição real do DD a PCM reproduzido.
- [ ] `audpp_cmd_play`, máscara de conclusão e associação proc→layout continuam sem
  validação ponta a ponta contra o firmware. Ver QDSP5_TODO.

### Testes executados nesta revisão

Em snapshot isolado do HEAD acima:

    cd tools/cpp
    make test_audio_sink && ./test_audio_sink
    make -C qdsp5 -f Makefile.qdsp5 test

Ambos retornaram 0. O teste de mixer reportou mistura/resampling; o de QDSP5
reportou WAV sintético e parser offline. Não houve escuta de áudio real do jogo.
Logs locais: `/home/rafaelfrequiao/projects/zeebo-lle-audit-f7dea3a/todo-audio.log`
e `todo-qdsp.log` no mesmo diretório. Assets proprietários não são necessários
para esses testes sintéticos e não devem ser versionados.

## 2. A0 — segurança da saída host antes de áudio contínuo

- [ ] **A00 / P1 — encerrar callback antes de destruir a fonte.**
  Evidência estática: `host_audio_` precede `qdsp_disp_` na declaração dos membros
  (`main:3592–3593`); a destruição reversa libera o dispatcher primeiro. A lambda
  captura seu ponteiro cru, e o destrutor do sistema só chama `flush_uarts`.
  Risco: callback acessa memória já liberada no encerramento.
  Aceitação: iniciar/parar repetidamente em GUI, fonte ocupada, ASan sem UAF;
  nenhuma chamada à fonte depois do fechamento do device. Não alegar reprodução
  do crash somente com base na ordem dos membros.

- [ ] **A01 / P1 — sincronizar produtor e consumidor do MESMO mixer.**
  `UnifiedHostAudio::mutex_` protege a função do callback, não `feed_raw` no core.
  `AudppEngine::handle` modifica/reseta `sink_`; `mix_audio` o percorre sem lock
  comum (`audpp_engine.cpp:60–88`). Evidência estática de concorrência insegura.
  Aceitação: stress PLAY/STOP/mix e TSan, sem race; verificar não bloquear o callback
  indefinidamente. Uma solução no orquestrador precisa cobrir todas as entradas e
  a destruição; solução dentro da engine fica bloqueada pelo freeze.

- [ ] **A02 / P1 — honrar sample rate negociado pelo SDL.**
  `SDL_OpenAudioDevice` aceita `SDL_AUDIO_ALLOW_FREQUENCY_CHANGE` (`main:446`),
  mas `have.freq` só é impresso; AUDPP mistura a 44100 Hz (`audpp_engine.cpp:55,133`).
  Aceitação: device simulado a 48000 Hz recebe duração/frequência corretas de uma
  fonte a 44100 Hz, inclusive em callbacks curtos. Negociar formato fixo ou converter
  explicitamente; não simplesmente reinterpretar amostras numa nova frequência.

- [ ] **A03 / P2 — propagar falha de init e estados de áudio.**
  `main:1152` ignora o bool retornado por `host_audio_->init`. Device ausente,
  headless, mute e dispositivo ativo devem ter estados distintos e logs honestos.
  Aceitação: driver SDL inválido produz erro identificável sem anúncio de som ativo.

- [ ] **A04 / P2 — ciclo pause/resume/reset e troca de dispositivo.**
  Definir pausa do clock de áudio, drenagem ou descarte controlado, latência e
  retomada sem samples antigos. Dependência: bug PAUSE do main (`1427–1434`):
  ramo pausado consome a tecla de retomada sem tratá-la; reproduzido via SDL_PushEvent.
  Aceitação: testes de sequência determinísticos e nenhuma duplicação após resume.

## 3. A1 — produzir dados REAIS antes de cobrar música

Cadeia a verificar, não uma implementação já concluída:

    applet executado → API de áudio BREW → sessão/command plane apropriado
      → transporte guest → tarefa/codec/PCM corretos → mixer → SDL/WAV
      → completion/callback correto → jogo continua

- [ ] **A10 / P1 — SMEM coerente entre APPS e AMSS.**
  Em `main:2061–2062` há dois mapeamentos independentes. Repro com STR guest:
  Core0 escreve `0x12345678` em SMEM+0x1000; Core1 lê zero.
  Gate: alias físico compartilhado, visibilidade bidirecional e ordenação de filas.
  Mudança de infraestrutura, não trabalho na pasta QDSP5.

- [ ] **A11 / P1 — handshake ProcComm sem resposta sobrescrita.**
  `main:3205` escreve CMD_DONE dentro de UC_HOOK_MEM_WRITE, antes do STR original.
  Repro: comando `0x42` permanece `0x42`, apesar de STATUS=3; deveria observar DONE.
  Gate: o guest lê conclusão real depois da escrita, sem patch de polling.

- [ ] **A12 / RE — alcançar o caminho de áudio desde cold boot.**
  Loader deve executar o módulo correto, inicializar BREW e permitir eventos/timers.
  O handler fixo `0x10532344` não executa DD. Mapear PC/retorno de abertura da sessão
  e primeiro buffer real; identificar precisamente o bloqueio anterior se não chegar.
  Não forçar `rex_wait`, sinais, PC ou mensagens para contar como boot funcional.

- [ ] **A13 / RE — proveniência e enquadramento de cada pacote.**
  Registrar PC, core, ponteiro, comprimento real, program/proc/task/queue e hexdump
  limitado. Separar frame interno da fila, header ONCRPC e payload; provar endianness.
  O main atualmente lê 512 bytes fixos e usa r0/r1 como pacote/TCB. Confirmar esses
  contratos em cada PC antes de interpretar dados. Ver Q0.1b/Q1.1 no QDSP5_TODO.
  `dummy_payload(16,0x42)` gerado pelo host NÃO é evidência de áudio do guest.

- [ ] **A14 / RE — escolher espaço de endereçamento correto para PCM.**
  O feed roda no Core1, mas `QdspGuest.ctx` aponta para Core0 (`main:3458`).
  A validade depende do contrato real do ponteiro e do compartilhamento físico.
  Gate: bytes lidos pelo mixer correspondem ao buffer que o produtor escreveu,
  incluindo aliases, limites, ownership e duração de vida.

- [ ] **A15 / RE — completion real sem fabricar sucesso.**
  O callback atual injeta dois novos CALLs com `{sig,tcb}` (`main:1162–1172`).
  Isso não prova reply associado ao XID, callback válido ou despertar do TCB correto.
  Gate: request→completion correlacionado, uma conclusão por comando, solicitante
  desbloqueado e próxima operação do jogo observada. Máscara `0x00080000` é hipótese.

## 4. A2 — streaming, formatos e correção audível

Estes itens não autorizam implementação durante a auditoria/freeze.

- [ ] **A20** Derivar do DD os formatos efetivamente usados para música e efeitos.
  Não assumir que WAV nos assets implica PCM já decodificado no RPC; distinguir
  gerenciador AUDMGR, decoders AUDPLAY e pós-processador AUDPP.
- [ ] **A21** Buffering limitado, backpressure, underrun/overrun e ownership.
  Continuidade entre buffers de uma voz, fim de stream, looping e canais separados.
  O caminho provisório cria uma voz por PLAY; validar semântica antes de reutilizá-lo.
- [ ] **A22** Volume zero/mute, rampas, clipping e mix de música+efeitos.
  A struct provisória trata volume zero como 1.0 e SET_VOL só reconhece a operação.
  Não promover esse comportamento ao contrato do firmware.
- [ ] **A23** Regressões de resampling, mono/estéreo, taxas diferentes, buffers ímpares,
  limites inválidos e múltiplos callbacks; comparar duração e samples esperados.
- [ ] **A24** Captura WAV da MESMA saída enviada ao device, com timestamps e métricas.
  Não capturar uma segunda síntese independente e apresentá-la como áudio ouvido.
- [ ] **A25** Sincronização A/V: clocks, pausa, jitter, latência e drift medidos.
  Definir tolerâncias explícitas antes de fechar o gate, sem números inventados.

## 5. A3 — gates para dizer que funciona

- [ ] **G-A0 / host seguro:** A00–A03 passam nas regressões e nos testes negativos.
- [ ] **G-A1 / origem:** requisição de áudio da aplicação identificada; bytes do buffer
  e transformação em samples auditáveis, sem dummy host, sem retorno forçado.
- [ ] **G-A2 / saída:** PCM real não silencioso chega ao SDL e à captura; taxa, canais,
  duração e efeitos respondem às ações. Não-silêncio isolado pode ser lixo: exige escuta.
- [ ] **G-A3 / continuidade:** música e efeitos durante partida controlável de DD por
  pelo menos cinco minutos, sem travar em RPC, sem UAF/race nem drift crescente.
- [ ] **G-A4 / repetibilidade:** repetir desde cold boot, registrar HEAD, backend CPU,
  flags, cenário e captura A/V; comprovar separadamente AppMgr e Z-Wheel.
- [ ] **G-negativo:** arquivo inválido e fonte ausente NÃO recebem o mesmo status
  de áudio comercial; SDL dummy aberto e WAV senoidal ficam rotulados sintéticos.

Oráculos locais: `/home/rafaelfrequiao/projects/zeebo-lab/assets/` contém capturas
históricas DD. Usar Infuse apenas como caixa-preta para timbre, eventos e cenário;
não copiar código e não exigir igualdade binária entre mixers/backend diferentes.
DD: App ID `274754`, CLSID `0x0102F789`. ROMs permanecem fora do Git e read-only.

## 6. Ordem recomendada e provas

1. A00–A03: corrigir segurança/contrato host com regressões pequenas e isoladas.
2. A10–A12: infraestrutura e execução real de BREW/DD; não são todos quick wins.
3. A13–A15 + Q0/Q1: protocolo, buffers e completion provados; RE com escopo limitado.
4. A20–A25: streaming, codecs necessários, qualidade e sincronização.
5. Gates G-A0…G-A4 e negativos. Nenhum deles está encerrado nesta revisão.

Reprodução local dos bugs de integração: diretório
`/home/rafaelfrequiao/projects/zeebo-lle-audit-f7dea3a/`, arquivos
`run_integration.py`, `integration_body.cpp`, `integration-repro.log`.
O harness inclui o código real do main e usa ARM sintético; isso prova os mecanismos
SMEM/ProcComm/PAUSE, não que um jogo específico já chegou a esses pontos.
