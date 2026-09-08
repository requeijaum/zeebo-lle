# QDSP5 — Relatório de fuzzing do parser Q0.1

Alvo: `Qdsp5Dispatcher::feed_raw` + parser de `qdsp5_capture_hook.h` (o caminho que
receberá pacotes ONCRPC crus do firmware quando o hook Q0.1 for plugado ao vivo).
Harness: `tools/cpp/qdsp5/qdsp5_fuzz.cpp` (libFuzzer + AddressSanitizer).
Build/run: `make -f Makefile.qdsp5 fuzz` (requer clang++; fora do 'test' default).

## Crashes REAIS encontrados e corrigidos

### Crash #1 — OOM / allocation-DoS em AudppEngine::do_play  (CORRIGIDO)
- Gatilho: `pcm_bytes` do payload não validado → `std::vector<int16_t>` alocava até
  ~4 GB (`malloc(4193845248)`). Um pacote hostil/malformado derrubaria o processo.
- Iteração de descoberta: ~11.474.
- Fix: teto `kMaxPcmBytes`; `pcm_bytes` acima do teto é rejeitado com log, sem alocar.

### Crash #2 (latente) — heap overflow de 1 byte em do_play  (CORRIGIDO junto)
- Gatilho: `pcm_bytes` ÍMPAR → buffer dst tem `(pcm_bytes/2)*2` bytes mas a leitura
  usava `pcm_bytes` → lia 1 byte além do vector.
- Fix: `n_samples = pcm_bytes / sizeof(int16_t)` e leitura de exatamente `n_samples*2`
  bytes (trunca o byte ímpar em vez de estourar).

### Hardening adicional em feed_raw  (defensivo)
- `len` que vier de campo do firmware é clampado a `kMaxPayload` (0x500) — um ONCRPC
  válido nunca excede o budget verificado. Evita OOB read do payload.

## Estado após fixes (verificado agora)
- Rebuild libFuzzer+ASAN: **75.563 execuções em 9 s, 0 crashes** (cov 134, ft 153).
  Antes do fix, OOM em ~11k. Pacotes UNROUTED (prog/proc aleatórios) são tratados sem
  crash (rota "engine map TODO" + retorno limpo).
- Smoke test default continua verde: `make -f Makefile.qdsp5 test` → ALL PASS.

## Escopo / honestidade
- O fuzzer usa um `QdspGuest` de teste com memória limitada e bounds-check, então os
  crashes achados são de LÓGICA do parser (não artefatos do harness).
- Cobertura ainda baixa (cov 134) porque o classify()/engine-map está incompleto
  (Q1.2a) — quando as rotas dos 6 engines existirem, re-rodar o fuzz para cobrir os
  novos caminhos de decode.
