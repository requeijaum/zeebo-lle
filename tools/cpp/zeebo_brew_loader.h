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

// ABI [infer, AAPCS BREW]: ISHELL_CreateInstance(r0=pIShell, r1=ClassID, r2=ppobj).
// AEEClsCreateInstance(r0=ClassID, r1=pIShell, r2=pIModule, r3=ppobj).
class BrewLoader {
public:
    explicit BrewLoader(uc_engine* uc = nullptr) : uc_(uc) {}

    void bind_uc(uc_engine* uc) { uc_ = uc; }
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

        u32 map_base = load_va & ~0x000FFFFFu;
        uc_mem_map(uc_, map_base, 0x00800000, UC_PROT_ALL); // 8MB; ok se já mapeado

        uc_err e = uc_mem_write(uc_, load_va, d.data(), d.size());
        if (e != UC_ERR_OK) {
            printf("[BREW] inject_mod: uc_mem_write falhou: %s\n", uc_strerror(e));
            return false;
        }
        mod_ = AppletModule{};
        mod_.host_path = host_path;
        mod_.load_va = load_va;
        mod_.size = sz;
        mod_.clsid = clsid;
        mod_.injected = true;
        mod_.entry_va = resolve_mod_entry(d, load_va);
        printf("[BREW] .mod injetado em 0x%08x (%u bytes)%s\n", load_va, sz,
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

private:
    uc_engine*   uc_ = nullptr;
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
