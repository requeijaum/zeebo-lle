// zeebo_module_gate.h — DD0: gate HONESTO de identidade e execução de módulo
// (QW99 Parte 1). Elimina o falso PASS "loaded-only".
//
// REGRA DE OURO (Rafael): carregar um .mod NÃO é executá-lo. Um jogo/applet só
// pode reivindicar PASS de execução quando:
//   (1) o módulo é VÁLIDO — injetado, com bytes reais (size > 0) e um entry
//       AEEMod_Load REALMENTE resolvido do header (arquivo inválido, vazio ou
//       cru sem entry reconhecido é REJEITADO ANTES de qualquer execução); e
//   (2) um PC efetivamente EXECUTADO pelo guest cai dentro da faixa do módulo
//       [load_va, load_va + size). Progresso genérico do Core 0, bytes
//       injetados, ou o handler fixo da Z-Wheel (0x10532344) NÃO contam.
//
// Este header é lógica PURA (sem I/O, sem Unicorn) para ser dirigida por testes
// e por um hook de PC no orquestrador. `pc_in_module_range` é o predicado que o
// hook UC_HOOK_CODE deve alimentar com cada PC executado; ao ver o primeiro PC
// dentro da faixa, o orquestrador marca `pc_entered_module = true` e só então
// `classify()` pode promover o estado a GAME_EXECUTED.
#pragma once
#include <cstdint>

namespace zeebo {
namespace module_gate {

using u32 = uint32_t;
using u64 = uint64_t;

// Estado honesto do ciclo de vida de um jogo/applet.
enum GameState : int {
    GAME_INVALID     = 0, // arquivo inválido/externo: falha ANTES de executar.
    GAME_LOADED_ONLY = 1, // válido e injetado, mas nenhum PC do módulo executou.
    GAME_EXECUTED    = 2, // PASS honesto: PC dentro da faixa executou de fato.
};

// Identidade mínima de um módulo injetado (espelha zeebo::brew::AppletModule).
// Preserva os identificadores de proveniência (base, size, entry, kind) sem
// acoplar ao BrewLoader — o orquestrador preenche a partir de module().
struct ModuleIdentity {
    bool injected  = false; // uc_mem_write verificado (bytes na RAM guest).
    u32  load_va   = 0;     // base de injeção no espaço guest.
    u32  size      = 0;     // bytes injetados (> 0 exigido).
    u32  entry_va  = 0;     // VA de AEEMod_Load resolvido do header (0 = não resolvido).
    u32  entry_kind = 0;    // proveniência do entry (ENTRY_NONE=0 ⇒ inválido).
};

// Um módulo é VÁLIDO somente com bytes reais injetados E um entry AEEMod_Load
// resolvido do header (ELF e_entry, MOD cru com branch, ou início direto). Suporta
// RAW MOD: entry_kind != 0 aceita as três proveniências honestas de resolve_mod_entry.
inline bool module_is_valid(const ModuleIdentity& m) {
    return m.injected
        && m.size > 0u
        && m.entry_va != 0u
        && m.entry_kind != 0u; // ENTRY_NONE (0) ⇒ arquivo inválido/externo.
}

// Predicado do hook de PC: o endereço executado `pc` pertence à faixa do módulo
// [load_va, load_va + size)? Ignora o bit Thumb (LSB) e trata os limites de
// forma meio-aberta. size==0 ⇒ nunca pertence (evita faixa degenerada).
inline bool pc_in_module_range(u32 load_va, u32 size, u32 pc) {
    if (size == 0u) return false;
    const u32 addr = pc & ~1u; // descarta o bit de estado Thumb.
    const u64 lo = static_cast<u64>(load_va);
    const u64 hi = lo + static_cast<u64>(size); // exclusivo.
    return static_cast<u64>(addr) >= lo && static_cast<u64>(addr) < hi;
}

// Classifica o estado do jogo a partir da identidade do módulo, do sucesso da
// execução do dispatch (clean == HandleEvent/AEEMod_Load rodou limpo sob
// Unicorn) e de `pc_entered_module` (o hook observou um PC dentro da faixa).
//   • módulo inválido               → GAME_INVALID   (nunca chega a executar).
//   • válido, mas !clean OU !pc_in  → GAME_LOADED_ONLY (sem PASS).
//   • válido, clean E pc_in         → GAME_EXECUTED   (PASS honesto).
inline GameState classify(const ModuleIdentity& m,
                          bool clean_dispatch,
                          bool pc_entered_module) {
#ifdef ZEEBO_DD0_MUTANT_PASS_ON_LOAD
    // MUTANTE (controle negativo): reintroduz o falso PASS "loaded-only" —
    // declara EXECUTED por mera carga, ignorando validade do entry e o PC no
    // módulo. Os testes negativos DEVEM ficar vermelhos com esta variante.
    (void)clean_dispatch; (void)pc_entered_module;
    return m.injected ? GAME_EXECUTED : GAME_INVALID;
#else
    if (!module_is_valid(m)) return GAME_INVALID;
    if (clean_dispatch && pc_entered_module) return GAME_EXECUTED;
    return GAME_LOADED_ONLY;
#endif
}

// Rótulo humano do estado (para logs do orquestrador e do teste externo).
inline const char* game_state_label(GameState s) {
    switch (s) {
        case GAME_EXECUTED:    return "EXECUTED";
        case GAME_LOADED_ONLY: return "loaded_only";
        default:               return "INVALID";
    }
}

} // namespace module_gate
} // namespace zeebo
