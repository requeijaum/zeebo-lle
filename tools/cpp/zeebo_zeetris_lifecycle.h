// zeebo_zeetris_lifecycle.h — Zeetris lifecycle probe: ABI do entry + clsid.
//
// Clean-room, evidência-primeiro. Duas derivações estruturais, zero suposição:
//
//   * derive_mif_clsid(bytes): CLASS ID pela POSIÇÃO ESTRUTURAL do registro de
//     applet do MIF (delega ao zeebo::brew::MifParser). Varredura crua NUNCA
//     decide. Honesto: ok=false quando não há registro estrutural válido.
//
//   * analyze_entry_abi(mod, load_va): resolve o entry de AEEMod_Load
//     (BrewLoader::resolve_mod_entry — o zeetris.mod começa com `b entry` =>
//     ENTRY_RAW_BRANCH) e DECODIFICA o prólogo do entry (STMFD sp!/push) para
//     medir quantos registradores de argumento (r0.. contíguos) ele preserva —
//     a evidência AAPCS da aridade. Não presume ARM vs Thumb: o entry cru de um
//     `.mod` BREW é código ARM, e é isso que o campo mode_arm registra.
//
// REGRA DE OURO: nada aqui alega boot, força retorno-sucesso, nem usa handler
// fixo. É um instrumento de MEDIÇÃO que só reporta o que está nos bytes.
#pragma once
#include <cstdint>
#include <vector>
#include "zeebo_brew_loader.h"
#include "zeebo_brew_mif.h"

namespace zeebo::zeetris {

using u8  = uint8_t;
using u32 = uint32_t;

// ── CLSID estrutural do MIF ──────────────────────────────────────────────────
struct MifClsid {
    bool ok    = false;
    u32  clsid = 0;
};

inline MifClsid derive_mif_clsid(const std::vector<u8>& mif) {
    MifClsid r;
    zeebo::brew::MifAppletInfo mi =
        zeebo::brew::MifParser::parse(mif.data(), mif.size());
    if (mi.valid) { r.ok = true; r.clsid = mi.clsid; }
    return r;
}

// ── Decodificador de STMFD sp!/push ARM ──────────────────────────────────────
// Reconhece `push {reglist}` == STMFD sp!, reglist == STMDB sp!, reglist:
//   cond(31:28)=AL(0xE), 27:25=100, P=1(24), U=0(23), W=1(21), L=0(20),
//   Rn=13(sp) em 19:16. reglist em 15:0. Escreve a máscara em *out.
// Honesto: false para qualquer coisa que não seja exatamente esse encoding.
inline bool decode_arm_push(u32 insn, u32* out) {
    if ((insn >> 28) != 0xE) return false;             // AL apenas
    if (((insn >> 25) & 0x7) != 0x4) return false;      // block data transfer
    const u32 P = (insn >> 24) & 1;
    const u32 U = (insn >> 23) & 1;
    const u32 W = (insn >> 21) & 1;
    const u32 L = (insn >> 20) & 1;
    const u32 Rn = (insn >> 16) & 0xF;
    // push = STMFD sp! = STMDB sp!: P=1, U=0, W=1, L=0 (store), Rn=sp(13).
    if (!(P == 1 && U == 0 && W == 1 && L == 0 && Rn == 13)) return false;
    if (out) *out = insn & 0xFFFFu;
    return true;
}

// Conta registradores de ARGUMENTO preservados: r0,r1,r2,r3 CONTÍGUOS a partir
// de r0 (para no primeiro ausente). AAPCS passa args em r0..r3; um prólogo que
// salva `push {r0,r1,r2,...}` está preservando os args recebidos, e a contagem
// contígua é a evidência de quantos o entry efetivamente usa.
inline int count_arg_regs(u32 mask) {
    int n = 0;
    for (int i = 0; i < 4; ++i) {
        if (mask & (1u << i)) ++n;
        else break;
    }
    return n;
}

inline bool mask_has_lr(u32 mask) { return (mask & (1u << 14)) != 0; }

// ── ABI do entry derivada dos bytes ──────────────────────────────────────────
struct EntryAbi {
    bool resolved     = false;               // entry de AEEMod_Load resolvido
    u32  entry_va     = 0;
    u32  entry_kind   = zeebo::brew::ENTRY_NONE;
    u32  first_insn   = 0;                    // 1ª palavra do módulo (o `b entry`)
    u32  first_va     = 0;                    // load_va
    u32  prologue_insn = 0;                   // 1ª instrução DO ENTRY
    bool is_push      = false;                // prólogo é STMFD sp!/push
    u32  push_mask    = 0;
    int  arg_regs     = 0;                    // r0.. contíguos preservados
    bool preserves_lr = false;
    bool mode_arm     = true;                 // entry cru de .mod BREW é ARM
};

// Lê uma palavra LE de `d` em `off`, 0 se fora de bounds.
inline u32 zeetris_rd32(const std::vector<u8>& d, size_t off) {
    if (d.size() < 4 || off > d.size() - 4) return 0;
    u32 v; std::memcpy(&v, d.data() + off, 4); return v;
}

inline EntryAbi analyze_entry_abi(const std::vector<u8>& mod, u32 load_va) {
    EntryAbi a;
    a.first_va   = load_va;
    a.first_insn = zeetris_rd32(mod, 0);
    u32 kind = zeebo::brew::ENTRY_NONE;
    u32 entry = zeebo::brew::BrewLoader::resolve_mod_entry(mod, load_va, &kind);
    if (!entry) return a;                    // honesto: entry não resolvido
    a.resolved   = true;
    a.entry_va   = entry;
    a.entry_kind = kind;
    a.mode_arm   = true;                     // .mod BREW: código ARM
    // Prólogo do entry (offset relativo ao load_va).
    const size_t entry_off = static_cast<size_t>(entry - load_va);
    a.prologue_insn = zeetris_rd32(mod, entry_off);
    u32 mask = 0;
    if (decode_arm_push(a.prologue_insn, &mask)) {
        a.is_push      = true;
        a.push_mask    = mask;
        a.arg_regs     = count_arg_regs(mask);
        a.preserves_lr = mask_has_lr(mask);
    }
    return a;
}

} // namespace zeebo::zeetris
