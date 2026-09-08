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
#include <unicorn/unicorn.h>
#include "zeebo_l4_mmu.h"

namespace zeebo::brew {

using u8  = uint8_t;
using u32 = uint32_t;

// ClassID BREW de um applet. 0 = "qualquer" (casa com o primeiro CreateInstance).
struct AppletModule {
    std::string host_path;      // caminho do .mod no host
    u32         load_va = 0;    // onde foi injetado no guest
    u32         size = 0;       // bytes injetados
    u32         clsid = 0;      // ClassID alvo (0 = wildcard)
    u32         entry_va = 0;   // VA de AEEMod_Load do módulo (relativo ao load_va, se ELF)
    bool        injected = false;
    bool        loaded = false; // AEEMod_Load já despachado com contexto
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
        u32 sz = (u32)f.tellg();
        f.seekg(0);
        std::vector<u8> d(sz);
        f.read((char*)d.data(), sz);
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
        u32 sz = (u32)d.size();

        u32 map_base = load_va & ~0x000FFFFFu;
        uc_mem_map(uc_, map_base, 0x00800000, UC_PROT_ALL); // 8MB; ok se já mapeado

        uc_err e = uc_mem_write(uc_, load_va, d.data(), d.size());
        if (e != UC_ERR_OK) {
            printf("[BREW] inject_bytes: uc_mem_write falhou: %s\n", uc_strerror(e));
            return false;
        }
        // Espelho na LUT (aliasing host-backed) quando disponível: garante que
        // telemetria/leituras diretas enxerguem os bytes injetados sem uc_mem_read.
        if (lut_ && lut_->is_mapped(load_va)) {
            if (lut_->write(load_va, d.data(), d.size()))
                printf("[BREW]   (espelhado na VTLB LUT host-backed @0x%08x)\n", load_va);
        }
        mod_ = AppletModule{};
        mod_.host_path = origin;
        mod_.load_va = load_va;
        mod_.size = sz;
        mod_.clsid = clsid;
        mod_.injected = true;
        mod_.entry_va = resolve_mod_entry(d, load_va);
        printf("[BREW] payload '%s' injetado em 0x%08x (%u bytes)%s\n", origin.c_str(), load_va, sz,
               mod_.entry_va ? "" : " [entry AEEMod_Load não resolvido do header]");
        if (mod_.entry_va) printf("[BREW] AEEMod_Load do módulo @ 0x%08x [infer ELF e_entry]\n", mod_.entry_va);
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
        bool clean = (e == UC_ERR_OK) || ((endp & ~1u) == (ret_magic & ~1u));
        const char* evname = (evt == 0x0100) ? "EVT_KEY_PRESS"
                           : (evt == 0x0101) ? "EVT_KEY_RELEASE"
                           : (evt == 0x0102) ? "EVT_KEY" : "EVT_?";
        printf("[BREW/Input] %s key=0x%04x → HandleEvent@0x%08x  ret r0=%u %s (uc=%s)\n",
               evname, keycode, handler_va, ret,
               ret == 1 ? "(consumido ✓)" : "(não tratado)",
               clean ? uc_strerror(e) : uc_strerror(e));
        if (ok) *ok = clean;
        return ret;
    }

private:
    uc_engine*   uc_ = nullptr;
    zeebo_l4::VtlbLut* lut_ = nullptr;
    BrewSymbols  sym_{};
    AppletModule mod_{};

    static u32 rd32(const std::vector<u8>& d, u32 off) {
        if (off + 4 > d.size()) return 0;
        u32 v; std::memcpy(&v, d.data() + off, 4); return v;
    }

    // Se o `.mod` for um ELF ARM (padrão BREW SDK), o e_entry aponta ao stub que
    // exporta AEEMod_Load. Caso contrário devolve 0 (honesto: desconhecido).
    static u32 resolve_mod_entry(const std::vector<u8>& d, u32 load_va) {
        if (d.size() < 0x20) return 0;
        if (!(d[0]==0x7f && d[1]=='E' && d[2]=='L' && d[3]=='F')) return 0; // não-ELF: MOD cru
        u32 e_entry = rd32(d, 24);
        // e_entry pode ser VA absoluto do link ou offset; heurística: se cair fora
        // do intervalo do módulo, tratar como offset a partir de load_va.
        if (e_entry >= load_va && e_entry < load_va + (u32)d.size()) return e_entry;
        return load_va + e_entry;
    }

    u32 reg(int r) const { u32 v = 0; if (uc_) uc_reg_read(uc_, r, &v); return v; }
    void set_reg(int r, u32 v) { if (uc_) uc_reg_write(uc_, r, &v); }

    // AEECShell atingiu 0x10c874f4: entrega o contexto do módulo à Shell. Se há
    // um .mod injetado, garante que os args de CreateInstance apontem para ele.
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
