// zeebo_brew_loader.h — Item 4: Loader / dispatch de applet BREW (.mod).
//
// Estrutura o caminho AEECShell -> ISHELL_CreateInstance -> AEEMod_Load ->
// AEEClsCreateInstance para um `.mod` de jogo/homebrew injetado no espaço de
// Core 0 (ARM11 Apps). Mantido MODULAR e desacoplado do orquestrador: recebe
// um uc_engine* e um conjunto de VAs de símbolo (resolvidos por RE do APPS.bin
// ou reaproveitados do `zeebo_lle_mod_probe`, que já mapeou o ponto de despacho
// de AEEMod_Load — ROADMAP Fase 7).
//
// REGRA DE OURO (Rafael): nada aqui força "retorno-sucesso" cego. Quando um VA
// de símbolo não é conhecido (0), o dispatch é NO-OP honesto e loga o motivo —
// nunca finge que carregou. O único efeito real garantido é a injeção de bytes
// do `.mod` na RAM guest (uc_mem_write verificado).
#pragma once
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include <fstream>
#include <limits>
#include <unicorn/unicorn.h>
#include "zeebo_l4_mmu.h"

namespace zeebo::brew {

using u8  = uint8_t;
using u32 = uint32_t;
using u64 = uint64_t;

// Como o entry de AEEMod_Load foi resolvido (proveniência honesta).
enum EntryKind : u32 {
    ENTRY_NONE       = 0, // desconhecido / rejeitado
    ENTRY_ELF        = 1, // ELF ARM: e_entry
    ENTRY_RAW_BRANCH = 2, // MOD cru: 1ª palavra é `b/bl AEEMod_Load`
    ENTRY_RAW_START  = 3, // MOD cru: AEEMod_Load começa direto no load_va
};

inline const char* entry_kind_label(u32 kind) {
    switch (kind) {
        case ENTRY_ELF:        return "ELF e_entry";
        case ENTRY_RAW_BRANCH: return "MOD cru branch inicial";
        case ENTRY_RAW_START:  return "MOD cru início direto";
        default:               return "não resolvido";
    }
}

// ClassID BREW de um applet. 0 = "qualquer" (casa com o primeiro CreateInstance).
struct AppletModule {
    std::string host_path;      // caminho do .mod no host
    u32         load_va = 0;    // onde foi injetado no guest
    u32         size = 0;       // bytes injetados
    u32         clsid = 0;      // ClassID alvo (0 = wildcard)
    u32         entry_va = 0;   // VA de AEEMod_Load do módulo (relativo ao load_va, se ELF)
    u32         entry_kind = ENTRY_NONE; // proveniência do entry_va
    bool        injected = false;
    bool        loaded = false; // AEEMod_Load já despachado com contexto
    // Ambiente de prefixo elf2mod: duas palavras reservadas ABAIXO da base de
    // carga (load_va-8, load_va-4). O runtime BREW/elf2mod as usa como scratch
    // de relocação/ponteiros de módulo. Só ficam prontas se houver espaço (base
    // >= 8) e o mapeamento guest cobrir a página anterior.
    bool        prefix_ready = false;
    u32         reserved_lo_va = 0; // load_va - 8
    u32         reserved_hi_va = 0; // load_va - 4
};

// VAs dos símbolos BREW resolvidos no APPS.bin (AEECShell). Todos [infer] até
// serem confirmados por RE. Zero = desconhecido -> dispatch honesto/no-op.
struct BrewSymbols {
    // Vetor da AEECShell onde ela decide criar a instância do applet do jogo.
    // Confirmado subindo no boot recente (commit "vetor BREW/AEECShell").
    u32 aeecshell_dispatch_va = 0x10c874f4;
    // Entradas de despacho a resolver via RE / reaproveitar do mod_probe.
    u32 ishell_create_va   = 0;  // ISHELL_CreateInstance(pShell, clsid, ppOut)
    u32 aeemod_load_va     = 0;  // AEEMod_Load(pIModule, ...)
    u32 aeeclscreate_va    = 0;  // AEEClsCreateInstance(clsid, pShell, pModule, ppObj)
};

// ── Eventos e teclas BREW (AEEEvent / AVKType) ───────────────────────────────
// Códigos de evento de tecla do BREW (AEE_events.h). O manipulador de eventos
// do applet (HandleEvent) recebe (pApp, evt, wParam=keycode, dwParam).
enum : u32 {
    EVT_KEY_PRESS   = 0x0100, // 256
    EVT_KEY_RELEASE = 0x0101, // 257
    EVT_KEY         = 0x0102, // 258
};

// Códigos de tecla AVK do Zeebo / Z-Pad (subconjunto usado no console).
enum : u32 {
    AVK_LEFT   = 0xFF51,
    AVK_UP     = 0xFF52,
    AVK_RIGHT  = 0xFF53,
    AVK_DOWN   = 0xFF54,
    AVK_SELECT = 0xFF0D, // Enter / Botão A (confirmar)
    AVK_CLR    = 0xFF08, // Backspace / Botão B (voltar/limpar)
    AVK_0      = 0x30,   // '0'..'9' = 0x30..0x39
    AVK_9      = 0x39,
    // Botões de jogo do Z-Pad. [infer] AVK_SOFT1/2 e AVK_INFO/SPACE seguem a
    // faixa AVK_* padrão do BREW SDK; usados p/ mapear C/V/Espaço/Esc do host.
    AVK_SOFT1  = 0xFF57, // Z-Pad botão 1
    AVK_SOFT2  = 0xFF58, // Z-Pad botão 2
    AVK_INFO   = 0xFF59, // Z-Pad botão 3
    AVK_SPACE  = 0x20,   // Z-Pad botão 4 (Espaço)
    AVK_FUNC   = 0xFF1B, // Home / Escape (menu)
};

// Botões lógicos do Z-Pad (independente do backend de entrada). O mapa
// SDL2→Z-Pad vive no orquestrador; a conversão Z-Pad→AVK é canônica aqui.
enum ZpadButton {
    ZP_NONE = 0, ZP_UP, ZP_DOWN, ZP_LEFT, ZP_RIGHT,
    ZP_A, ZP_B, ZP_1, ZP_2, ZP_3, ZP_4, ZP_HOME,
};

// Z-Pad lógico → código AVK BREW.
inline u32 avk_for_zpad(ZpadButton b) {
    switch (b) {
        case ZP_UP:    return AVK_UP;
        case ZP_DOWN:  return AVK_DOWN;
        case ZP_LEFT:  return AVK_LEFT;
        case ZP_RIGHT: return AVK_RIGHT;
        case ZP_A:     return AVK_SELECT;
        case ZP_B:     return AVK_CLR;
        case ZP_1:     return AVK_SOFT1;
        case ZP_2:     return AVK_SOFT2;
        case ZP_3:     return AVK_INFO;
        case ZP_4:     return AVK_SPACE;
        case ZP_HOME:  return AVK_FUNC;
        default:       return 0;
    }
}

// Dígito 0-9 → AVK ('0'..'9'). Retorna 0 se fora da faixa.
inline u32 avk_for_digit(int d) { return (d >= 0 && d <= 9) ? (u32)(0x30 + d) : 0u; }

// ABI [infer, AAPCS BREW]: ISHELL_CreateInstance(r0=pIShell, r1=ClassID, r2=ppobj).
// AEEClsCreateInstance(r0=ClassID, r1=pIShell, r2=pIModule, r3=ppobj).
class BrewLoader {
public:
    explicit BrewLoader(uc_engine* uc = nullptr) : uc_(uc) {}

    // ── Decode seguro de branch ARM (Bug 3) ──────────────────────────────────
    // Decodifica um branch ARM incondicional (B=0xEA, BL=0xEB, cond=AL) situado
    // em `insn_va` e escreve o alvo em *out. Retorna false para não-branch,
    // branch condicional (cond != 0xE) ou se o alvo estourar 32 bits. O offset
    // de 24 bits é COM SINAL, deslocado <<2, somado a insn_va+8 (pipeline ARM).
    static bool decode_arm_branch(u32 insn, u32 insn_va, u32* out) {
        const u32 cond = insn >> 28;
        const u32 op   = (insn >> 24) & 0xF;
        if (cond != 0xE) return false;      // só AL (incondicional)
        if (op != 0xA && op != 0xB) return false; // B ou BL
        int32_t imm24 = (int32_t)(insn & 0x00FFFFFF);
        if (imm24 & 0x00800000) imm24 |= (int32_t)0xFF000000; // sign-extend
        // Alvo = insn_va + 8 + (imm24 << 2). Feito em 64 bits com sinal para não
        // estourar; rejeita se cair fora do espaço de 32 bits.
        const int64_t tgt = (int64_t)insn_va + 8 + ((int64_t)imm24 << 2);
        if (tgt < 0 || tgt > (int64_t)std::numeric_limits<u32>::max()) return false;
        if (out) *out = (u32)tgt;
        return true;
    }

    // Resolve o VA de AEEMod_Load de um `.mod`. Suporta: ELF ARM (e_entry),
    // MOD ARM cru com `b/bl AEEMod_Load` inicial, e MOD cru cujo AEEMod_Load é a
    // própria 1ª palavra (prólogo). `*kind` (opcional) recebe a proveniência.
    // Rejeita alvos fora do módulo ou dentro da região reservada de prefixo
    // [load_va-8, load_va). Honesto: 0/ENTRY_NONE quando não há certeza.
    static u32 resolve_mod_entry(const std::vector<u8>& d, u32 load_va,
                                 u32* kind = nullptr) {
        if (kind) *kind = ENTRY_NONE;
        if (d.size() < 4) return 0;
        const u64 module_end = static_cast<u64>(load_va) + d.size();

        // ELF ARM (padrão super-ELF / SDK): usa e_entry.
        if (d.size() >= 0x20 && d[0]==0x7f && d[1]=='E' && d[2]=='L' && d[3]=='F') {
            const u32 e_entry = rd32(d, 24);
            if (e_entry >= load_va && static_cast<u64>(e_entry) < module_end) {
                if (kind) *kind = ENTRY_ELF;
                return e_entry;
            }
            if (static_cast<u64>(e_entry) < d.size()) {
                const u64 rel = static_cast<u64>(load_va) + e_entry;
                if (rel < module_end && rel <= std::numeric_limits<u32>::max()) {
                    if (kind) *kind = ENTRY_ELF;
                    return static_cast<u32>(rel);
                }
            }
            return 0;
        }

        // MOD ARM cru: 1ª palavra pode ser `b/bl AEEMod_Load`.
        u32 tgt = 0;
        const u32 w0 = rd32(d, 0);
        if (decode_arm_branch(w0, load_va, &tgt)) {
            // Alvo tem de cair DENTRO do módulo [load_va, module_end) e nunca na
            // região reservada de prefixo abaixo da base.
            if (tgt >= load_va && static_cast<u64>(tgt) < module_end) {
                if (kind) *kind = ENTRY_RAW_BRANCH;
                return tgt;
            }
            return 0; // branch fora de bounds / reservado -> honesto
        }

        // MOD cru sem branch inicial: AEEMod_Load é o próprio load_va (a 1ª
        // função ligada com --entry=AEEMod_Load). Exige base plausível (>0x1000,
        // convenção BREW PIC) para não confundir dado solto com código.
        if (load_va > 0x1000) {
            if (kind) *kind = ENTRY_RAW_START;
            return load_va;
        }
        return 0;
    }

    // ── Ambiente de prefixo elf2mod (Bug 3) ──────────────────────────────────
    // Reserva/zera as duas palavras abaixo da base de carga (load_va-8, -4) que
    // o runtime BREW/elf2mod usa como scratch. Mapeia a página anterior se
    // preciso. `lo`/`hi` são os valores iniciais (default 0). Retorna false —
    // honesto, sem crash — se não há espaço (base < 8), uc não vinculado, ou o
    // map/write falha. Idempotente.
    bool prepare_elf2mod_prefix(u32 load_va, u32 lo = 0, u32 hi = 0) {
        if (!uc_) { printf("[BREW] prefix: uc não vinculado\n"); return false; }
        if (load_va < 8) { // sem espaço p/ 2 palavras abaixo da base
            printf("[BREW] prefix: base 0x%08x baixa demais p/ prefixo\n", load_va);
            return false;
        }
        const u32 lo_va = load_va - 8;
        const u32 hi_va = load_va - 4;
        // Mapeia toda página que cubra [lo_va, load_va) (pode ser a mesma da base).
        const u64 pbegin = static_cast<u64>(lo_va) & ~0xfffULL;
        const u64 pend   = (static_cast<u64>(load_va) + 0xfffULL) & ~0xfffULL;
        for (u64 page = pbegin; page < pend; page += 0x1000) {
            const uc_err me = uc_mem_map(uc_, page, 0x1000, UC_PROT_ALL);
            if (me != UC_ERR_OK && me != UC_ERR_MAP) {
                printf("[BREW] prefix: uc_mem_map @0x%08llx falhou: %s\n",
                       (unsigned long long)page, uc_strerror(me));
                return false;
            }
        }
        if (uc_mem_write(uc_, lo_va, &lo, 4) != UC_ERR_OK ||
            uc_mem_write(uc_, hi_va, &hi, 4) != UC_ERR_OK) {
            printf("[BREW] prefix: uc_mem_write falhou\n");
            return false;
        }
        if (mod_.injected && mod_.load_va == load_va) {
            mod_.prefix_ready = true;
            mod_.reserved_lo_va = lo_va;
            mod_.reserved_hi_va = hi_va;
        }
        return true;
    }

    void bind_uc(uc_engine* uc) { uc_ = uc; }
    // VTLB LUT opcional (aliasing físico host-backed). Quando o alvo da injeção
    // já está coberto pela LUT, escreve direto na RAM de host (O(1), sem cópia
    // interna do Unicorn). Caso contrário cai no uc_mem_write normal.
    void bind_lut(zeebo_l4::VtlbLut* lut) { lut_ = lut; }
    void set_symbols(const BrewSymbols& s) { sym_ = s; }
    const BrewSymbols& symbols() const { return sym_; }
    bool has_module() const { return mod_.injected; }
    const AppletModule& module() const { return mod_; }

    // (a) Injeta a imagem do `.mod` no espaço guest de Core 0. Mapeia uma janela
    // de 8MB alinhada se necessário. Retorna true só com uc_mem_write OK.
    bool inject_mod(const std::string& host_path, u32 load_va, u32 clsid = 0) {
        if (!uc_) { printf("[BREW] inject_mod: uc não vinculado\n"); return false; }
        std::ifstream f(host_path, std::ios::binary | std::ios::ate);
        if (!f) { printf("[BREW] inject_mod: falha ao abrir '%s'\n", host_path.c_str()); return false; }
        const std::streamoff st = f.tellg();
        // tellg() devolve -1 se o stream está em erro após open (ex.: arquivo é
        // um diretório ou não suporta seek). Sem essa checagem, o cast `(u32)st`
        // transforma -1 em 0xFFFFFFFF e std::vector<u8> d(sz) tenta alocar ~4GB
        // -> std::bad_alloc não tratado (crash do host).
        if (st < 0) { printf("[BREW] inject_mod: tellg() falhou em '%s'\n", host_path.c_str()); return false; }
        const u32 sz = static_cast<u32>(st);
        f.seekg(0);
        std::vector<u8> d(sz);
        f.read((char*)d.data(), sz);
        // Confirma que leu o arquivo inteiro (eof no fim do payload real), não
        // apenas uma fração. `good()` após read de exatamente `sz` bytes.
        if (static_cast<size_t>(f.gcount()) != d.size()) {
            printf("[BREW] inject_mod: leitura incompleta de '%s' (%zu/%u bytes)\n",
                   host_path.c_str(), static_cast<size_t>(f.gcount()), sz);
            return false;
        }
        return inject_bytes(d, load_va, clsid, host_path);
    }

    // (a') Injeta um payload já materializado (ex.: extraído do EFS2 da NAND via
    // efs2::Efs2Filesystem) no espaço guest de Core 0. Mesma garantia honesta que
    // inject_mod: só retorna true com uc_mem_write verificado. `origin` é apenas o
    // rótulo de proveniência (ex.: "efs2:reksio.mod") — não abre arquivo no host.
    bool inject_bytes(const std::vector<u8>& d, u32 load_va, u32 clsid = 0,
                      const std::string& origin = "<bytes>") {
        if (!uc_) { printf("[BREW] inject_bytes: uc não vinculado\n"); return false; }
        if (d.empty()) { printf("[BREW] inject_bytes: payload vazio ('%s')\n", origin.c_str()); return false; }
        if (d.size() > std::numeric_limits<u32>::max() ||
            static_cast<u64>(load_va) + d.size() > 0x100000000ULL) {
            printf("[BREW] inject_bytes: payload/range excede VA de 32 bits\n");
            return false;
        }
        u32 sz = static_cast<u32>(d.size());

        const u64 map_begin = static_cast<u64>(load_va) & ~0xfffULL;
        const u64 map_end = (static_cast<u64>(load_va) + d.size() + 0xfffULL) & ~0xfffULL;
        for (u64 page = map_begin; page < map_end; page += 0x1000) {
            const uc_err map_err = uc_mem_map(uc_, page, 0x1000, UC_PROT_ALL);
            if (map_err != UC_ERR_OK && map_err != UC_ERR_MAP) {
                printf("[BREW] inject_bytes: uc_mem_map @0x%08llx falhou: %s\n",
                       (unsigned long long)page, uc_strerror(map_err));
                return false;
            }
        }

        uc_err e = uc_mem_write(uc_, load_va, d.data(), d.size());
        if (e != UC_ERR_OK) {
            printf("[BREW] inject_bytes: uc_mem_write falhou: %s\n", uc_strerror(e));
            return false;
        }
        // Espelho na LUT (aliasing host-backed) quando disponível: garante que
        // telemetria/leituras diretas enxerguem os bytes injetados sem uc_mem_read.
        // Valida a COBERTURA de [load_va, load_va+size) — não só a 1ª página —
        // porque lut_->write() copia o payload inteiro cruzando várias páginas;
        // se alguma página posterior não estiver na LUT, o espelho ficaria PARCIAL
        // e a divergência RAM-real × LUT passaria silenciosa. Só espelha se todas
        // as páginas cobertas existirem na LUT.
        if (lut_) {
            const u64 last_page = (static_cast<u64>(load_va) + d.size() - 1) >> 12;
            const u64 first_page = static_cast<u64>(load_va) >> 12;
            bool cov = true;
            for (u64 pg = first_page; pg <= last_page; ++pg) {
                if (!lut_->is_mapped(pg << 12)) { cov = false; break; }
            }
            if (cov && lut_->write(load_va, d.data(), d.size()))
                printf("[BREW]   (espelhado na VTLB LUT host-backed @0x%08x)\n", load_va);
        }
        mod_ = AppletModule{};
        mod_.host_path = origin;
        mod_.load_va = load_va;
        mod_.size = sz;
        mod_.clsid = clsid;
        mod_.injected = true;
        mod_.entry_va = resolve_mod_entry(d, load_va, &mod_.entry_kind);
        // Ambiente de prefixo elf2mod: reserva as palavras em load_va-8/-4 (best
        // effort — honesto: só marca prefix_ready se o mapeamento couber).
        prepare_elf2mod_prefix(load_va);
        printf("[BREW] payload '%s' injetado em 0x%08x (%u bytes)%s\n", origin.c_str(), load_va, sz,
               mod_.entry_va ? "" : " [entry AEEMod_Load não resolvido do header]");
        if (mod_.entry_va) {
            printf("[BREW] AEEMod_Load do módulo @ 0x%08x [%s]\n",
                   mod_.entry_va, entry_kind_label(mod_.entry_kind));
        }
        return true;
    }

    // (b) Chamado quando Core0 atinge um endereço de código. Roteia os pontos de
    // interesse do dispatch BREW. Retorna true se tratou o site (o caller deve
    // então redirecionar o PC / parar a fatia conforme sua convenção de hook).
    // NÃO altera PC aqui — devolve a decisão ao orquestrador (desacoplamento).
    bool on_code(u32 pc) {
        if (pc == sym_.aeecshell_dispatch_va) { on_aeecshell_dispatch(); return true; }
        if (sym_.ishell_create_va && pc == sym_.ishell_create_va) { on_create_instance(); return true; }
        if (sym_.aeemod_load_va && pc == sym_.aeemod_load_va)       { on_aeemod_load();     return true; }
        if (sym_.aeeclscreate_va && pc == sym_.aeeclscreate_va)     { on_cls_create();      return true; }
        return false;
    }

    // (c) Despacha um evento BREW ao manipulador Thumb do applet, executando-o
    // DE VERDADE sob Unicorn (não forja retorno). Convenção HandleEvent do BREW:
    //   r0 = pApplet, r1 = evt (EVT_KEY_PRESS/RELEASE), r2 = wParam (keycode AVK),
    //   r3 = dwParam. Retorno r0 = TRUE(1) se o applet consumiu o evento.
    // `handler_va` é o VA Thumb do HandleEvent (bit 0 ignorado; forçamos Thumb).
    // `applet_va` é o ponteiro do objeto applet (r0). `stack_top`/`ret_magic`
    // definem a pilha de scratch e o LR sentinela onde a execução para.
    // Retorna r0 do manipulador; `*ok` (opcional) indica execução limpa.
    u32 dispatch_event(u32 handler_va, u32 applet_va, u32 evt, u32 keycode,
                       u32 stack_top, u32 ret_magic, u32 dwparam = 0, bool* ok = nullptr) {
        if (ok) *ok = false;
        if (!uc_) { printf("[BREW/Input] dispatch_event: uc não vinculado\n"); return 0; }
        if (!handler_va) { printf("[BREW/Input] dispatch_event: handler_va nulo\n"); return 0; }
        u32 r0 = applet_va, r1 = evt, r2 = keycode, r3 = dwparam;
        u32 sp = stack_top, lr = ret_magic | 1u; // LR Thumb → para no sentinela
        set_reg(UC_ARM_REG_R0, r0); set_reg(UC_ARM_REG_R1, r1);
        set_reg(UC_ARM_REG_R2, r2); set_reg(UC_ARM_REG_R3, r3);
        set_reg(UC_ARM_REG_SP, sp); set_reg(UC_ARM_REG_LR, lr);
        uc_err e = uc_emu_start(uc_, handler_va | 1u, ret_magic & ~1u, 0, 0);
        u32 ret  = reg(UC_ARM_REG_R0);
        u32 endp = reg(UC_ARM_REG_PC);
        const bool clean = e == UC_ERR_OK &&
                           ((endp & ~1u) == (ret_magic & ~1u));
        const char* evname = (evt == 0x0100) ? "EVT_KEY_PRESS"
                           : (evt == 0x0101) ? "EVT_KEY_RELEASE"
                           : (evt == 0x0102) ? "EVT_KEY" : "EVT_?";
        printf("[BREW/Input] %s key=0x%04x → HandleEvent@0x%08x  ret r0=%u %s (uc=%s)\n",
               evname, keycode, handler_va, ret,
               ret == 1 ? "(consumido ✓)" : "(não tratado)",
               clean ? "OK" : uc_strerror(e));
        if (ok) *ok = clean;
        // `ret` (r0) é lido do guest MESMO quando clean==false (uc_emu_start !=
        // UC_ERR_OK ou PC não pousou no ret_magic); nesse caso o valor pode ser
        // lixo. O caller DEVE checar *ok antes de confiar no retorno — clean é o
        // único critério de execução limpa do applet (regra de ouro).
        return ret;
    }

private:
    uc_engine*   uc_ = nullptr;
    zeebo_l4::VtlbLut* lut_ = nullptr;
    BrewSymbols  sym_{};
    AppletModule mod_{};

    static u32 rd32(const std::vector<u8>& d, u32 off) {
        // Guarda contra overflow de u32: `off + 4` estoura se off for perto de
        // 0xFFFFFFFF, transformando a checagem em falso-negativo e lendo fora do
        // buffer. A forma `off > size - 4` (com size >= 4 garantido) é imune.
        if (d.size() < 4 || off > (d.size() - 4)) return 0;
        u32 v; std::memcpy(&v, d.data() + off, 4); return v;
    }

    // Se o `.mod` for um ELF ARM (padrão BREW SDK), o e_entry aponta ao stub que
    // exporta AEEMod_Load. A resolução completa (ELF + MOD ARM cru + prefixo) é
    // pública em resolve_mod_entry (acima), reutilizada aqui e nos testes.

    u32 reg(int r) const { u32 v = 0; if (uc_) uc_reg_read(uc_, r, &v); return v; }
    void set_reg(int r, u32 v) { if (uc_) uc_reg_write(uc_, r, &v); }

    // AEECShell atingiu 0x10c874f4: entrega o contexto do módulo à Shell. Se há
    // um .mod injetado, garante que os args de CreateInstance apontem para ele.
    // NOTA de honestidade (regra de ouro): `mod_.loaded = true` aqui significa APENAS
    // que o dispatcher da Shell atingiu o VA de entrega — NÃO é prova de que o
    // AEEMod_Load / applet executou de fato. `loaded` é um sinal de "shell chegou ao
    // handoff", não um milestone de execução de jogo; o orquestrador não deve
    // promovê-lo a `executed`.
    void on_aeecshell_dispatch() {
        printf("[BREW] AEECShell dispatch @0x%08x\n", sym_.aeecshell_dispatch_va);
        if (!mod_.injected) {
            printf("[BREW]   (nenhum .mod injetado — Shell segue sem applet de jogo)\n");
            return;
        }
        printf("[BREW]   contexto do applet '%s' @0x%08x disponível (clsid=0x%08x)\n",
               mod_.host_path.c_str(), mod_.load_va, mod_.clsid);
        mod_.loaded = true;
    }

    void on_create_instance() {
        u32 clsid = reg(UC_ARM_REG_R1);
        printf("[BREW] ISHELL_CreateInstance(clsid=0x%08x)\n", clsid);
        if (mod_.injected && (mod_.clsid == 0 || mod_.clsid == clsid)) {
            printf("[BREW]   -> casa com .mod injetado; AEEMod_Load @0x%08x\n", mod_.entry_va);
        }
    }

    void on_aeemod_load() {
        printf("[BREW] AEEMod_Load @0x%08x (mod@0x%08x)\n", sym_.aeemod_load_va, mod_.load_va);
        mod_.loaded = true;
    }

    void on_cls_create() {
        u32 clsid = reg(UC_ARM_REG_R0);
        printf("[BREW] AEEClsCreateInstance(clsid=0x%08x)\n", clsid);
    }
};

} // namespace zeebo::brew
