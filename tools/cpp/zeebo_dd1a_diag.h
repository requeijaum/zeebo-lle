// zeebo_dd1a_diag.h — QW99 Parte 4, DD1a: primeiro PC ASSISTIDO do Double Dragon.
//
// Diagnóstico honesto e ROTULADO `hybrid/assisted`. NÃO fecha boot orgânico
// (marco B), NÃO é PASS de jogo. Reaproveita a resolução de entry cru e a
// injeção do BrewLoader existente (68a477d e sucessores); não reimplementa o
// falso gap "ELF-only" e NUNCA usa o handler fixo 0x10532344.
//
// O que este módulo faz, e SÓ isto:
//   1. Valida a identidade do pacote: App ID (diretório) 274754, a presença do
//      AEECLSID 0x0102F789 nos bytes do MIF, e o tamanho do ddragonz.mod.
//   2. Resolve o entry ARM cru de ddragonz.mod via BrewLoader::resolve_mod_entry
//      (ddragonz.mod começa com `str lr,[sp,#-4]!` => ENTRY_RAW_START = load_va).
//   3. Executa esse entry sob Unicorn em modo INTÉRPRETE (UC_MODE_ARM, sem
//      Dynarmic), com orçamento de instruções, pilha de scratch e SOMENTE as
//      páginas do módulo mapeadas.
//   4. Registra se o PC entrou/permaneceu na faixa do módulo e qual foi a
//      PRIMEIRA falha genuína (fetch/read/write unmapped) — a primeira
//      dependência ausente (import/GOT/IShell/OEM), que é exatamente o que
//      separa DD1a de DD1-runtime.
//
// REGRA DE OURO: nada aqui força retorno-sucesso, salto de PC, dispatch fixo,
// nem promove o resultado a boot orgânico. Um entry cru que não sobrevive ao
// primeiro import é DD1a, e o diagnóstico diz isso.
#pragma once
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include <unicorn/unicorn.h>
#include "zeebo_brew_loader.h"
#include "zeebo_brew_mif.h"

namespace zeebo::dd1a {

using u8  = uint8_t;
using u32 = uint32_t;
using u64 = uint64_t;

// Identidade autoritativa do Double Dragon (ROADMAP QW99).
constexpr u32 DD_APP_ID       = 274754;      // diretório mod/274754, mif/274754.mif
constexpr u32 DD_CLSID        = 0x0102F789;  // AEECLSID (NÃO 274754)
constexpr u32 DD_MOD_SIZE     = 462748;      // ddragonz.mod (bytes)
// Handler fixo da Z-Wheel — PROIBIDO como substituto de execução do módulo.
constexpr u32 FORBIDDEN_ZWHEEL_HANDLER = 0x10532344;

enum FaultType : int {
    FAULT_NONE            = 0, // execução parou no sentinela (sem falha)
    FAULT_BUDGET          = 1, // esgotou o orçamento de instruções (sem falha)
    FAULT_FETCH_UNMAPPED  = 2, // PC saltou para VA não mapeada (dependência ausente)
    FAULT_READ_UNMAPPED   = 3, // leu de VA não mapeada (import/GOT/dado ausente)
    FAULT_WRITE_UNMAPPED  = 4, // escreveu em VA não mapeada
    FAULT_OTHER           = 5, // outra falha do Unicorn
};

inline const char* fault_label(int f) {
    switch (f) {
        case FAULT_NONE:           return "sem falha (parou no sentinela)";
        case FAULT_BUDGET:         return "orçamento esgotado (sem falha)";
        case FAULT_FETCH_UNMAPPED: return "fetch em VA não mapeada (dependência ausente)";
        case FAULT_READ_UNMAPPED:  return "leitura em VA não mapeada (import/dado ausente)";
        case FAULT_WRITE_UNMAPPED: return "escrita em VA não mapeada";
        case FAULT_OTHER:          return "outra falha do intérprete";
        default:                   return "?";
    }
}

// Derivação ESTRITA do App ID a partir do caminho real do pacote.
struct AppIdParse {
    bool ok     = false;
    u32  app_id = 0;
};

// Converte `s` em u32 apenas se for uma sequência não-vazia de dígitos que cabe
// em 32 bits (parsing estrito: nada de sinais, espaços, prefixos ou overflow).
inline bool parse_u32_strict(const std::string& s, u32& out) {
    if (s.empty()) return false;
    uint64_t v = 0;
    for (char c : s) {
        if (c < '0' || c > '9') return false;
        v = v * 10u + static_cast<uint64_t>(c - '0');
        if (v > 0xFFFFFFFFull) return false;   // overflow u32
    }
    out = static_cast<u32>(v);
    return true;
}

// Deriva o App ID do caminho real: diretório após ".../mod/<id>/..." OU o stem
// do arquivo em ".../mif/<id>.mif". PROVENIÊNCIA: o identificador vem do layout
// EFS real do pacote, não de uma constante passada pelo chamador (que tornava a
// checagem tautológica). Retorna ok=false se o segmento não for numérico estrito.
inline AppIdParse parse_app_id_from_path(const std::string& path) {
    AppIdParse r;
    // Caminho de módulo: .../mod/<id>/<arquivo>
    size_t pm = path.find("/mod/");
    if (pm != std::string::npos) {
        size_t start = pm + 5;
        size_t end = path.find('/', start);
        std::string seg = (end == std::string::npos)
                              ? path.substr(start)
                              : path.substr(start, end - start);
        r.ok = parse_u32_strict(seg, r.app_id);
        return r;
    }
    // Caminho de MIF: .../mif/<id>.mif
    size_t pf = path.find("/mif/");
    if (pf != std::string::npos) {
        std::string fname = path.substr(pf + 5);
        size_t dot = fname.rfind(".mif");
        if (dot == std::string::npos) return r;
        r.ok = parse_u32_strict(fname.substr(0, dot), r.app_id);
        return r;
    }
    return r;
}

// Identidade do pacote (etapa 1). Autoridade estrutural, corroboração diagnóstica.
struct PackageIdentity {
    bool app_id_ok             = false; // App ID derivado do caminho == 274754
    u32  app_id                = 0;
    bool clsid_ok              = false; // CLSID estrutural do MIF == 0x0102F789
    u32  clsid                 = 0;     // CLSID do registro estrutural (MifParser)
    bool clsid_structural      = false; // o CLSID DD veio do registro estrutural
    bool clsid_scan_corroborates = false; // varredura crua achou os 4 bytes (diag.)
    bool mod_size_ok           = false; // ddragonz.mod == 462748 bytes
    u32  mod_size              = 0;
    // Identidade só é válida por AUTORIDADE ESTRUTURAL — a varredura crua nunca
    // decide, apenas corrobora.
    bool valid() const { return app_id_ok && clsid_ok && mod_size_ok; }
};

// Resultado do primeiro-PC assistido (etapas 2-4).
struct FirstPcResult {
    bool ran            = false; // o intérprete chegou a iniciar
    u32  load_va        = 0;
    u32  entry_va       = 0;     // = load_va para ENTRY_RAW_START
    u32  entry_kind     = 0;     // zeebo::brew::EntryKind
    bool entered_module = false; // algum PC executado dentro de [load_va, end)
    u32  first_pc       = 0;     // primeiro PC executado (deve == entry)
    u32  last_pc        = 0;     // PC no ponto de parada
    u64  instructions   = 0;     // instruções executadas antes de parar
    int  fault          = FAULT_NONE;
    u32  fault_va       = 0;     // VA da primeira dependência ausente
    uc_err uc_status    = UC_ERR_OK;
    // Rótulo imutável: DD1a é sempre assistido/híbrido, nunca orgânico/PASS.
    const char* label() const { return "hybrid/assisted"; }
};

// Confirma a identidade do pacote por AUTORIDADE ESTRUTURAL:
//   * App ID: DERIVADO dos caminhos reais (dir mod/<id>/ e/ou mif/<id>.mif),
//     com parsing estrito — não é mais o constante passado pelo chamador.
//   * CLSID: extraído do REGISTRO ESTRUTURAL de applet do MIF via
//     zeebo::brew::MifParser (NÃO por varredura cega). A varredura crua de
//     4 bytes é apenas CORROBORAÇÃO diagnóstica, jamais autoridade.
//   * mod_size: tamanho do ddragonz.mod.
// Cada campo é independente e honesto.
inline PackageIdentity validate_package(const std::string& mod_path,
                                        const std::string& mif_path,
                                        const std::vector<u8>& mif_bytes,
                                        u64 mod_size) {
    PackageIdentity id;

    // App ID derivado do caminho (preferência ao dir de módulo; fallback ao MIF).
    AppIdParse ap = parse_app_id_from_path(mod_path);
    if (!ap.ok) ap = parse_app_id_from_path(mif_path);
    id.app_id = ap.app_id;
    id.app_id_ok = ap.ok && (ap.app_id == DD_APP_ID);

    id.mod_size = static_cast<u32>(mod_size);
    id.mod_size_ok = (mod_size == DD_MOD_SIZE);

    // CLSID: autoridade ESTRUTURAL (registro de applet do MIF).
    zeebo::brew::MifAppletInfo mi =
        zeebo::brew::MifParser::parse(mif_bytes.data(), mif_bytes.size());
    if (mi.valid) {
        id.clsid = mi.clsid;
        if (mi.clsid == DD_CLSID) {
            id.clsid_ok = true;
            id.clsid_structural = true;
        }
    }

    // Varredura crua: CORROBORAÇÃO diagnóstica apenas (nunca decide validade).
    const u8 le[4] = { (u8)(DD_CLSID), (u8)(DD_CLSID >> 8),
                       (u8)(DD_CLSID >> 16), (u8)(DD_CLSID >> 24) };
    for (size_t i = 0; i + 4 <= mif_bytes.size(); ++i) {
        if (std::memcmp(mif_bytes.data() + i, le, 4) == 0) {
            id.clsid_scan_corroborates = true;
            break;
        }
    }
    return id;
}

namespace detail {
struct HookState {
    u32 mod_begin = 0;
    u32 mod_end   = 0;      // exclusivo
    u32 sentinel  = 0;
    bool entered  = false;
    u32  first_pc = 0;
    u32  last_pc  = 0;
    u64  count    = 0;
    u64  budget   = 0;
    int  fault    = FAULT_NONE;
    u32  fault_va = 0;
};

inline void code_hook(uc_engine* uc, uint64_t address, uint32_t /*size*/, void* user) {
    auto* st = static_cast<HookState*>(user);
    if (st->count == 0) st->first_pc = static_cast<u32>(address);
    st->last_pc = static_cast<u32>(address);
    if (address >= st->mod_begin && address < st->mod_end) st->entered = true;
    ++st->count;
    if (st->count >= st->budget) {
        st->fault = FAULT_BUDGET;
        uc_emu_stop(uc);
    }
}

inline bool mem_hook(uc_engine* uc, uc_mem_type type, uint64_t address,
                     int /*size*/, int64_t /*value*/, void* user) {
    auto* st = static_cast<HookState*>(user);
    st->fault_va = static_cast<u32>(address);
    switch (type) {
        case UC_MEM_FETCH_UNMAPPED:
        case UC_MEM_FETCH_PROT:    st->fault = FAULT_FETCH_UNMAPPED; break;
        case UC_MEM_READ_UNMAPPED:
        case UC_MEM_READ_PROT:     st->fault = FAULT_READ_UNMAPPED;  break;
        case UC_MEM_WRITE_UNMAPPED:
        case UC_MEM_WRITE_PROT:    st->fault = FAULT_WRITE_UNMAPPED; break;
        default:                   st->fault = FAULT_OTHER;          break;
    }
    uc_emu_stop(uc);
    return false; // não trata: deixa a falha abortar a fatia (honesto)
}
} // namespace detail

// Executa o entry cru do módulo já injetado sob Unicorn intérprete, com pilha de
// scratch e orçamento. `mod_begin`/`mod_size` delimitam a faixa do módulo.
// `stack_top` deve estar mapeado. `budget` limita instruções. Se `force_success`
// for true (SÓ para o controle de mutação), o resultado mente que entrou e não
// falhou — os testes exigem que essa mutação torne o controle negativo VERMELHO.
inline FirstPcResult run_first_pc(uc_engine* uc, u32 entry_va, u32 mod_begin,
                                  u32 mod_size, u32 stack_top, u64 budget,
                                  bool force_success = false) {
    FirstPcResult r;
    r.load_va  = mod_begin;
    r.entry_va = entry_va;
    if (!uc || !entry_va || mod_size == 0) return r;
    const u32 sentinel = 0xF0F0F0F0u;

    detail::HookState st;
    st.mod_begin = mod_begin;
    st.mod_end   = mod_begin + mod_size;
    st.sentinel  = sentinel;
    st.budget    = budget ? budget : 1;

    uc_hook hc = 0, hm = 0;
    uc_hook_add(uc, &hc, UC_HOOK_CODE, (void*)detail::code_hook, &st, 1, 0);
    uc_hook_add(uc, &hm, UC_HOOK_MEM_UNMAPPED | UC_HOOK_MEM_PROT,
                (void*)detail::mem_hook, &st, 1, 0);

    // Estado inicial mínimo: SP na pilha de scratch, LR no sentinela (para o
    // interpretador parar se o módulo retornar), r0..r3 zerados.
    u32 zero = 0;
    uc_reg_write(uc, UC_ARM_REG_R0, &zero);
    uc_reg_write(uc, UC_ARM_REG_R1, &zero);
    uc_reg_write(uc, UC_ARM_REG_R2, &zero);
    uc_reg_write(uc, UC_ARM_REG_R3, &zero);
    uc_reg_write(uc, UC_ARM_REG_SP, &stack_top);
    uc_reg_write(uc, UC_ARM_REG_LR, &sentinel);

    r.ran = true;
    // ARM (não Thumb): entry cru começa em modo ARM. Sem timeout, conta por
    // instrução via hook + orçamento; para no sentinela (fim >= sentinel).
    uc_err e = uc_emu_start(uc, entry_va, sentinel, 0, 0);
    r.uc_status      = e;
    r.entered_module = st.entered;
    r.first_pc       = st.first_pc;
    r.last_pc        = st.last_pc;
    r.instructions   = st.count;
    r.fault          = st.fault;
    r.fault_va       = st.fault_va;
    r.entry_kind     = 0;

    // Se parou limpo no sentinela sem falha registrada, foi retorno do módulo.
    if (r.fault == FAULT_NONE && e != UC_ERR_OK) r.fault = FAULT_OTHER;

    uc_hook_del(uc, hc);
    uc_hook_del(uc, hm);

    if (force_success) { // CONTROLE DE MUTAÇÃO — jamais no caminho real.
        r.entered_module = true;
        r.fault = FAULT_NONE;
    }
    return r;
}

} // namespace zeebo::dd1a
