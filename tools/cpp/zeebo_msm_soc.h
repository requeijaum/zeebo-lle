// zeebo_msm_soc.h — nucleo do MSM7201A compartilhado entre harnesses.
//
// Por que existe: o harness de boot de Linux (test_linux_boot.cpp) acumulou o modelo
// de hardware do SoC — mapa de memoria, UART, VIC, timer, entrada de excecao no host e
// o walker VA->PA pelas tabelas de pagina do proprio guest. Os harnesses dos outros
// SOs (xv6, OKL4/Iguana, Cinder, lk) precisam do MESMO modelo; duplicar isso garantiria
// que os dois divergissem. O que e' Linux-especifico (ATAGs, protocolo de boot do
// zImage, cmdline, decodificacao do framebuffer do console) NAO entra aqui.
//
// Estado: aqui so' vive o que e' PURO (constantes e funcoes sem estado mutavel). O
// estado dos perifericos ainda mora no harness que os usa, e migra nas proximas fatias.
#pragma once

#include <cstdint>
#include <string>

#include <unicorn/unicorn.h>

namespace zeebo_msm {

using u32 = std::uint32_t;
using u64 = std::uint64_t;

// --- mapa do MSM7201A (mesmas constantes do emulador principal) ---
constexpr u32 APPS_RAM_PHYS = 0x10000000u;
constexpr u32 APPS_RAM_SIZE = 0x06000000u;   // 96MB
constexpr u32 UART1_BASE    = 0xa9a00000u;
// ATENCAO ao nome: 0xA9C00000 e' a **UART3** (MSM_UART3_PHYS, msm_iomap-7x00.h:72),
// nao a UART2 (essa e' 0xA9B00000). Ela aparece como **ttyMSM2** porque o
// platform_device msm_device_uart3 tem `.id = 2` (devices-msm7x00.c:90) e o
// msm_serial nomeia a tty pelo id, nao pelo numero da UART. board-halibut.c so'
// registra msm_device_uart3. Os identificadores aqui mantem o sufixo 2 por
// compatibilidade com quem ja' os usava, mas leia-os como "a UART do ttyMSM2".
constexpr u32 UART3_PHYS    = 0xa9c00000u;   // MSM_UART3_PHYS
constexpr u32 UART2_BASE    = UART3_PHYS;    // = ttyMSM2 (id=2), IRQ INT_UART3=11
constexpr u32 UART3_BASE    = 0xa9e00000u;   // UART3 do MSM7201A (mesmo bloco, irq 12)
constexpr u32 UART_BASES[3] = {UART1_BASE, UART2_BASE, UART3_BASE};
constexpr u32 UART_SIZE     = 0x00010000u;

constexpr u32 UART_OFF_TF   = 0x000cu;       // TX FIFO (leitura = RX FIFO)
constexpr u32 UART_OFF_SR   = 0x0008u;       // status; UART_SR_TX_EMPTY=(1<<3), TX_READY=(1<<2)
constexpr u32 UART_OFF_IMR  = 0x0014u;       // escrita = IMR, leitura = ISR (TX_READY = 1<<7)
constexpr u32 UART_OFF_CR   = 0x0010u;       // command register
constexpr u32 UART_SR_TX_EMPTY  = (1u << 3);
constexpr u32 UART_SR_TX_READY  = (1u << 2);
constexpr u32 UART_ISR_TX_READY = (1u << 7);
constexpr u32 UART_IMR_TXLEV    = (1u << 0);

constexpr u32 VIC_BASE      = 0xc0000000u;   // PA do VIC (VA 0xE0000000 no iotable)
constexpr u32 VIC_SIZE      = 0x00002000u;   // registradores do VIC (o CSR em
                                             // 0xC0100000 vem logo depois e tem
                                             // hook proprio)
constexpr u32 PERIPH_BASE   = 0xc0000000u;   // VIC + GPT/DGT (0xC0100000)
constexpr u32 PERIPH_SIZE   = 0x00020000u;

// MDP/TVENC do MSM7x00: o driver de framebuffer (mdp.c) escreve nestes registradores.
constexpr u32 MDP_BASE      = 0xaa200000u;   // MSM_MDP_PHYS
constexpr u32 MDP_SIZE      = 0x000f0000u;
constexpr u32 TVENC_BASE    = 0xaa400000u;
constexpr u32 TVENC_SIZE    = 0x00001000u;

// Base da UART dona deste endereco (0 se nao for UART conhecida).
inline u32 uart_base_for(u32 a) {
    for (u32 b : UART_BASES)
        if (a >= b && a < b + UART_SIZE) return b;
    return 0;
}

// --- Leitura de memoria do GUEST -------------------------------------------
// `uc_mem_read` NAO traduz a MMU: ele le o espaco FISICO. Para ler o que o guest
// enxerga num VA e' preciso andar a tabela de pagina dele. Isto era a lacuna
// declarada para os alvos que ligam MMU (OKL4, xv6) — aqui ela esta' resolvida e
// exercitada por um Linux de verdade (page fault preguicoso + SWI).
inline bool guest_read_u32(uc_engine* uc, u32 va, u32* out) {
    uc_arm_cp_reg r0 = {15, 0, 0, 2, 0, 0, 0, 0};
    uc_arm_cp_reg r1 = {15, 0, 0, 2, 0, 0, 1, 0};
    uc_arm_cp_reg rc = {15, 0, 0, 2, 0, 0, 2, 0};
    if (uc_reg_read(uc, UC_ARM_REG_CP_REG, &r0) != UC_ERR_OK) return false;
    uc_reg_read(uc, UC_ARM_REG_CP_REG, &r1);
    uc_reg_read(uc, UC_ARM_REG_CP_REG, &rc);
    const u32 ttbr0 = (u32)r0.val, ttbr1 = (u32)r1.val, ttbcr = (u32)rc.val;
    const u32 n = ttbcr & 7u;
    // ARM: split = 2^(32-N). N=0 => TTBR0 vale para TODO o espaco (TTBR1 nao e'
    // usado). Com N>0, TTBR0 cobre [0, split) e TTBR1 o resto. O kernel aqui usa
    // TTBCR=0 (mesmo pgd para user e kernel), entao o stack de user em
    // 0xbe9c1xxx tambem esta no TTBR0.
    const u32 split = (n == 0u) ? 0u : (0x80000000u >> (n - 1u));
    const u32 pgdb = ((n == 0u || va < split) ? ttbr0 : ttbr1) & 0xffffc000u;
    u32 l1 = 0;
    if (uc_mem_read(uc, pgdb + ((va >> 20) & 0xfffu) * 4u, &l1, 4) != UC_ERR_OK) return false;
    if ((l1 & 3u) == 0u) return false;
    if ((l1 & 3u) == 2u)
        return uc_mem_read(uc, (l1 & 0xfff00000u) | (va & 0xfffffu), out, 4) == UC_ERR_OK;
    const u32 l2 = l1 & 0xfffffc00u;
    u32 pte = 0;
    if (uc_mem_read(uc, l2 + ((va >> 12) & 0xffu) * 4u, &pte, 4) != UC_ERR_OK) return false;
    if ((pte & 3u) == 2u)
        return uc_mem_read(uc, (pte & 0xfffff000u) | (va & 0xfffu), out, 4) == UC_ERR_OK;
    if ((pte & 3u) == 3u)   // ARMv6: pagina pequena ESTENDIDA (subpaginas) — base em [31:12]
        return uc_mem_read(uc, (pte & 0xfffff000u) | (va & 0xfffu), out, 4) == UC_ERR_OK;
    if ((pte & 3u) == 1u)   // pagina grande de 64KB
        return uc_mem_read(uc, (pte & 0xffff0000u) | (va & 0xffffu), out, 4) == UC_ERR_OK;
    return false;
}

// --- Leitura de bytes do GUEST (via walk manual; atravessa paginas) --------
inline bool guest_read_bytes(uc_engine* uc, u32 va, u32 len, std::string& out) {
    out.clear();
    for (u32 i = 0; i < len; i += 4) {
        u32 w = 0;
        if (!guest_read_u32(uc, va + i, &w)) return false;
        for (int b = 0; b < 4 && i + (u32)b < len; ++b)
            out.push_back((char)((w >> (8 * b)) & 0xffu));
    }
    return true;
}

// Endereco efetivo do acesso de dado que abortou: o Unicorn nao modela o FAR
// (leitura devolve 0), entao decodificamos a instrucao que falhou.
inline u32 arm_ls_fault_addr(uc_engine* uc, u32 pc, bool* decoded, bool* is_write) {
    *decoded = false;
    if (is_write) *is_write = false;
    u32 insn = 0;
    if (!guest_read_u32(uc, pc, &insn)) return 0;
    // bit 20 (L) = 0 -> store: o page fault e' de ESCRITA (FSR bit 10)
    if (is_write) *is_write = (((insn >> 20) & 1u) == 0u);
    // O enum de registradores do Unicorn NAO e' contiguo: R13/SP e R14/LR sao
    // valores proprios. "UC_ARM_REG_R0 + 13" devolvia lixo, entao qualquer
    // fault com base em sp/lr (ex.: "push {...}") calculava endereco errado.
    static const uc_arm_reg kRegs[15] = {
        UC_ARM_REG_R0, UC_ARM_REG_R1, UC_ARM_REG_R2,  UC_ARM_REG_R3,  UC_ARM_REG_R4,
        UC_ARM_REG_R5, UC_ARM_REG_R6, UC_ARM_REG_R7,  UC_ARM_REG_R8,  UC_ARM_REG_R9,
        UC_ARM_REG_R10, UC_ARM_REG_R11, UC_ARM_REG_R12, UC_ARM_REG_SP, UC_ARM_REG_LR
    };
    u32 regs[15] = {0};
    for (int i = 0; i < 15; ++i) uc_reg_read(uc, kRegs[i], &regs[i]);
    const int rn = (int)((insn >> 16) & 0xfu);
    const bool U = ((insn >> 23) & 1u) != 0;
    const bool P = ((insn >> 24) & 1u) != 0;
    // rn == 15 e' LDR/STR PC-relativo (pool de literais): o valor do PC na
    // instrucao ARM e' endereco+8. Sem isto o endereco calculado sai errado
    // (ex.: 0x368 em vez de 0x137288) e o kernel mata o init com SIGSEGV.
    const u32 base = (rn == 15) ? (pc + 8u) : regs[rn];
    const u32 stype = (insn >> 5) & 3u;
    auto apply_shift = [](u32 v, u32 type, u32 sh) -> u32 {
        sh &= 31u;
        if (sh == 0) return v;
        switch (type) {
            case 0: return v << sh;
            case 1: return v >> sh;
            case 2: return (u32)((int)v >> (int)sh);
            default: return (v >> sh) | (v << (32 - sh));
        }
    };
    const u32 cls = insn & 0x0e000000u;
    if (cls == 0x04000000u) {                        // single data transfer, offset imediato
        const u32 off = insn & 0xfffu;
        *decoded = true;
        return P ? (U ? base + off : base - off) : base;
    }
    if (cls == 0x06000000u) {                        // single data transfer, offset registrador
        // bit25=1 => offset por registrador. Bit 4 seleciona o deslocamento:
        // 0 = imediato em bits[11:7]; 1 = por registrador (Rs = bits[11:8]).
        u32 off;
        if (insn & 0x10u) {
            const u32 rs = (insn >> 8) & 0xfu;
            off = apply_shift(regs[insn & 0xfu], stype, regs[rs] & 0xffu);
        } else {
            off = apply_shift(regs[insn & 0xfu], stype, (insn >> 7) & 0x1fu);
        }
        *decoded = true;
        return P ? (U ? base + off : base - off) : base;
    }
    if (cls == 0x00000000u && (insn & 0x90u) == 0x90u) {   // halfword / signed transfer
        const bool I = ((insn >> 22) & 1u) != 0;
        const u32 off = I ? (((insn >> 4) & 0xf0u) | (insn & 0xfu)) : regs[insn & 0xfu];
        *decoded = true;
        return P ? (U ? base + off : base - off) : base;
    }
    if (cls == 0x08000000u) {                        // LDM/STM (IA/IB/DA/DB)
        const u32 nregs = (u32)__builtin_popcount(insn & 0xffffu);
        *decoded = true;
        if (U) return P ? (base + 4u) : base;                    // IB / IA
        return base - 4u * (P ? nregs : (nregs ? nregs - 1u : 0u));  // DB / DA
    }
    if ((insn & 0x0fb00ff0u) == 0x01000090u) {       // SWP
        *decoded = true;
        return base;
    }
    if ((insn & 0x0e0000f0u) == 0x000000d0u) {       // LDRD/STRD
        const bool I = ((insn >> 22) & 1u) != 0;
        const u32 off = I ? (((insn >> 4) & 0xf0u) | (insn & 0xfu)) : regs[insn & 0xfu];
        *decoded = true;
        return P ? (U ? base + off : base - off) : base;
    }
    return base;                                     // melhor esforco
}

}  // namespace zeebo_msm
