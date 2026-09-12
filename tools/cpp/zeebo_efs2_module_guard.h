// zeebo_efs2_module_guard.h — Fail-closed gate for EFS2 applet extraction/launch.
//
// REGRA DE OURO (Rafael): um payload cru do EFS2 só pode ser REPORTADO ou
// INJETADO como applet se tiver uma entrada de módulo executável PLAUSÍVEL.
// Provada por bytes, sem forjar nada. São aceitas SOMENTE duas proveniências:
//
//   (1) ELF ARM: magic 0x7f 'E' 'L' 'F' + e_entry que resolve DENTRO do payload
//       (absoluto em [load_va, load_va+size) ou relativo em [0, size)); ou
//   (2) MOD ARM cru cuja PRIMEIRA palavra é um branch incondicional (B/BL, AL)
//       cujo alvo cai DENTRO da faixa de carga [load_va, load_va+size).
//
// TUDO O MAIS É REJEITADO — inclusive o antigo fallback "raw-start" (aceitar o
// próprio load_va como entry só porque a base é > 0x1000). Esse fallback deixava
// QUALQUER blob (metadados de gnode, dados de modem/NV, zeros) passar como se
// fosse código. Os blobs reais reksio.mod (w0=0x9cd3ffff), 274755
// (w0=0xfd19f297) e tectoy.mod (w0=0x00000000) NÃO são branches nem ELF e, sob
// esta regra, falham fechado — como devem.
//
// Lógica PURA (sem I/O, sem Unicorn) para ser dirigida por testes.
#pragma once
#include <cstdint>
#include <cstring>
#include <limits>
#include <vector>

namespace zeebo {
namespace efs2_guard {

using u8  = uint8_t;
using u32 = uint32_t;
using u64 = uint64_t;

// Veredito da inspeção de um payload cru quanto a ser um módulo executável.
enum ModuleVerdict : int {
    MOD_REJECT     = 0, // sem entrada de módulo plausível — NÃO reportar/injetar.
    MOD_ELF        = 1, // ELF ARM com e_entry resolvido dentro do payload.
    MOD_ARM_BRANCH = 2, // MOD cru: 1ª palavra é branch AL cujo alvo cai na faixa.
};

inline const char* verdict_label(ModuleVerdict v) {
    switch (v) {
        case MOD_ELF:        return "ELF e_entry";
        case MOD_ARM_BRANCH: return "MOD cru branch inicial";
        default:             return "REJEITADO (sem entry de módulo plausível)";
    }
}

inline u32 rd32(const std::vector<u8>& d, size_t off) {
    u32 v = 0;
    if (off + 4 <= d.size()) std::memcpy(&v, d.data() + off, 4);
    return v;
}

// Decode seguro de branch ARM incondicional (B=0xEA, BL=0xEB, cond=AL=0xE).
// Alvo = insn_va + 8 + (imm24 com sinal << 2), em 64 bits. Retorna false para
// não-branch, branch condicional, ou alvo fora do espaço de 32 bits.
inline bool decode_arm_branch(u32 insn, u32 insn_va, u32* out) {
    const u32 cond = insn >> 28;
    const u32 op   = (insn >> 24) & 0xF;
    if (cond != 0xE) return false;              // só AL (incondicional)
    if (op != 0xA && op != 0xB) return false;   // B ou BL
    int32_t imm24 = (int32_t)(insn & 0x00FFFFFF);
    if (imm24 & 0x00800000) imm24 |= (int32_t)0xFF000000; // sign-extend
    const int64_t tgt = (int64_t)insn_va + 8 + ((int64_t)imm24 << 2);
    if (tgt < 0 || tgt > (int64_t)std::numeric_limits<u32>::max()) return false;
    if (out) *out = (u32)tgt;
    return true;
}

// Classifica um payload cru. `load_va` é a base de injeção pretendida; `size` é
// o tamanho real do payload. `entry_out` (opcional) recebe o VA do entry quando
// aceito. NÃO aceita o fallback raw-start: um blob sem ELF e sem branch inicial
// válido é MOD_REJECT.
inline ModuleVerdict classify_payload(const std::vector<u8>& d, u32 load_va,
                                      u32* entry_out = nullptr) {
    if (entry_out) *entry_out = 0;
    if (d.size() < 4) return MOD_REJECT;
    const u64 module_end = static_cast<u64>(load_va) + d.size();

    // (1) ELF ARM: e_entry absoluto na faixa, ou relativo dentro do payload.
    if (d.size() >= 0x20 && d[0] == 0x7f && d[1] == 'E' && d[2] == 'L' && d[3] == 'F') {
        const u32 e_entry = rd32(d, 24);
        if (e_entry >= load_va && static_cast<u64>(e_entry) < module_end) {
            if (entry_out) *entry_out = e_entry;
            return MOD_ELF;
        }
        if (static_cast<u64>(e_entry) < d.size()) {
            const u64 rel = static_cast<u64>(load_va) + e_entry;
            if (rel < module_end && rel <= std::numeric_limits<u32>::max()) {
                if (entry_out) *entry_out = static_cast<u32>(rel);
                return MOD_ELF;
            }
        }
        return MOD_REJECT; // ELF cujo e_entry não resolve dentro do payload.
    }

    // (2) MOD ARM cru: 1ª palavra tem de ser um branch AL cujo alvo cai DENTRO
    // da faixa de carga. Nada de fallback raw-start.
    u32 tgt = 0;
    const u32 w0 = rd32(d, 0);
    if (decode_arm_branch(w0, load_va, &tgt)) {
        if (tgt >= load_va && static_cast<u64>(tgt) < module_end) {
            if (entry_out) *entry_out = tgt;
            return MOD_ARM_BRANCH;
        }
    }
#ifdef ZEEBO_EFS2_GUARD_MUTANT_ACCEPT_ALL
    // MUTANTE (controle negativo): reintroduz o fallback raw-start que aceitava
    // QUALQUER blob com base plausível. Os negativos DEVEM ficar vermelhos aqui.
    if (load_va > 0x1000u) {
        if (entry_out) *entry_out = load_va;
        return MOD_ARM_BRANCH;
    }
#endif
    return MOD_REJECT;
}

// Predicado curto: o payload é lançável como applet? (não rejeitado)
inline bool is_launchable_module(const std::vector<u8>& d, u32 load_va,
                                 u32* entry_out = nullptr) {
    return classify_payload(d, load_va, entry_out) != MOD_REJECT;
}

} // namespace efs2_guard
} // namespace zeebo
