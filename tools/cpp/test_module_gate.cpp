// test_module_gate.cpp — DD0: gate HONESTO de módulo (QW99 Parte 1).
//
// Elimina o falso PASS "loaded-only". Um jogo/applet externo só recebe PASS de
// EXECUÇÃO quando:
//   (1) o arquivo é um módulo VÁLIDO (injetado, tamanho > 0, entry AEEMod_Load
//       realmente resolvido) — arquivo inválido/vazio é REJEITADO ANTES de
//       qualquer execução; e
//   (2) um PC REALMENTE EXECUTADO pertence à faixa do módulo [load_va,
//       load_va+size) — carregar (loaded_only) nunca basta.
//
// Provas:
//   NEGATIVO 1: módulo não injetado                    → GAME_INVALID (falha antes de executar).
//   NEGATIVO 2: injetado, size=0                        → GAME_INVALID.
//   NEGATIVO 3: injetado com entry NÃO resolvido        → GAME_INVALID (arquivo inválido/externo).
//   NEGATIVO 4: módulo válido, mas SEM PC na faixa       → GAME_LOADED_ONLY (sem PASS).
//   NEGATIVO 5: módulo válido, PC na faixa mas dispatch  → GAME_LOADED_ONLY (execução suja não conta).
//               NÃO limpo
//   NEGATIVO 6: PC fora da faixa do módulo               → não conta como executado.
//   POSITIVO 1: módulo válido + dispatch limpo + PC dentro da faixa → GAME_EXECUTED (PASS honesto).
//   POSITIVO 2: pc_in_module_range ignora o bit Thumb e respeita os limites.
//
// Controle mutacional: um mutante que declara PASS por carga (ignora
// pc_entered_module, ou trata entry não resolvido como válido) torna os
// NEGATIVOS vermelhos.
#include <cstdio>
#include "zeebo_module_gate.h"

using namespace zeebo::module_gate;

int main() {
    int failures = 0;
    auto expect = [&](bool cond, const char* name) {
        if (cond) { printf("  [PASS] %s\n", name); }
        else      { printf("  [FAIL] %s\n", name); ++failures; }
    };

    printf("== DD0: gate honesto de módulo (sem falso PASS loaded-only) ==\n");

    // Identidade de um módulo real, válido e injetado (ex.: reksio.mod / DD).
    const u32 LOAD = 0x12000000u, SIZE = 0x8000u;
    ModuleIdentity valid{ /*injected=*/true, LOAD, SIZE,
                          /*entry_va=*/LOAD + 0x400u, /*entry_kind=*/2u };

    // --- NEGATIVO 1: não injetado → inválido, falha antes de executar. ---
    {
        ModuleIdentity m = valid; m.injected = false;
        expect(!module_is_valid(m), "módulo não injetado é inválido");
        expect(classify(m, /*clean=*/true, /*pc_in=*/true) == GAME_INVALID,
               "não injetado → GAME_INVALID (falha antes da execução)");
    }

    // --- NEGATIVO 2: size 0 → inválido. ---
    {
        ModuleIdentity m = valid; m.size = 0;
        expect(!module_is_valid(m), "módulo com size=0 é inválido");
        expect(classify(m, true, true) == GAME_INVALID, "size=0 → GAME_INVALID");
    }

    // --- NEGATIVO 3: entry AEEMod_Load não resolvido → inválido/externo. ---
    {
        ModuleIdentity m = valid; m.entry_va = 0; m.entry_kind = 0;
        expect(!module_is_valid(m),
               "módulo sem entry resolvido é inválido (arquivo externo/corrompido)");
        expect(classify(m, true, true) == GAME_INVALID,
               "entry não resolvido → GAME_INVALID antes de executar");
    }

    // --- NEGATIVO 4: válido mas nenhum PC entrou na faixa → loaded_only. ---
    {
        expect(module_is_valid(valid), "módulo real é válido");
        expect(classify(valid, /*clean=*/true, /*pc_in=*/false) == GAME_LOADED_ONLY,
               "válido sem PC na faixa permanece GAME_LOADED_ONLY (sem PASS)");
    }

    // --- NEGATIVO 5: PC na faixa mas dispatch NÃO limpo → loaded_only. ---
    {
        expect(classify(valid, /*clean=*/false, /*pc_in=*/true) == GAME_LOADED_ONLY,
               "execução suja (não limpa) não vira PASS mesmo com PC na faixa");
    }

    // --- NEGATIVO 6: PC fora da faixa não conta como execução do módulo. ---
    {
        expect(!pc_in_module_range(LOAD, SIZE, LOAD - 4),
               "PC abaixo da base não pertence ao módulo");
        expect(!pc_in_module_range(LOAD, SIZE, LOAD + SIZE),
               "PC no fim exclusivo não pertence ao módulo");
        expect(!pc_in_module_range(LOAD, SIZE, 0x10532344u),
               "PC do handler fixo da Z-Wheel não pertence a este módulo");
    }

    // --- POSITIVO 1: válido + limpo + PC na faixa → EXECUTED (PASS honesto). ---
    {
        expect(classify(valid, /*clean=*/true, /*pc_in=*/true) == GAME_EXECUTED,
               "módulo válido + dispatch limpo + PC na faixa → GAME_EXECUTED (PASS)");
    }

    // --- POSITIVO 2: pc_in_module_range respeita limites e ignora Thumb bit. ---
    {
        expect(pc_in_module_range(LOAD, SIZE, LOAD),
               "PC == base pertence ao módulo");
        expect(pc_in_module_range(LOAD, SIZE, (LOAD + 0x400u) | 1u),
               "PC com bit Thumb dentro da faixa pertence ao módulo");
        expect(pc_in_module_range(LOAD, SIZE, LOAD + SIZE - 1),
               "último byte da faixa pertence ao módulo");
    }

    if (failures == 0) {
        printf("PASS: gate honesto de módulo — nenhum PASS por carga isolada (DD0).\n");
        return 0;
    }
    printf("FAIL: %d asserção(ões) do gate honesto falharam.\n", failures);
    return 1;
}
