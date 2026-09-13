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
#include <cstdio>
#include <string>
#include <vector>
#include <sys/select.h>
#include <unistd.h>

#include <unicorn/unicorn.h>

namespace zeebo_msm {

using u8  = std::uint8_t;
using u32 = std::uint32_t;
using u64 = std::uint64_t;
using i64 = std::int64_t;

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

// --- VIC do MSM7x00 (2 bancos, 64 linhas) ----------------------------------
// Offsets da fonte primaria (Cinder, kern/dev/msm_irqreg.h) — nao os do header do
// zloader. Sao os que um driver de kernel REAL programa, e foi com eles que o boot do
// Linux passou a receber interrupcao. O modelo principal do emulador usa outra tabela
// (+0x0000/+0x18/+0x28); essa divergencia esta' registrada e NAO foi resolvida ainda.
constexpr u32 VIC_OFF_ENCLEAR0 = 0x0020u;
constexpr u32 VIC_OFF_ENSET0   = 0x0030u;
constexpr u32 VIC_OFF_STATUS0  = 0x0080u;
constexpr u32 VIC_OFF_CLEAR0   = 0x00b0u;
constexpr u32 VIC_OFF_VEC_RD   = 0x00d0u;
constexpr u32 VIC_OFF_VEC_PEND = 0x00d4u;
constexpr u32 VIC_NO_PEND      = 0xffffffffu;

// Numeros de IRQ (arch/arm/mach-msm/include/mach/irqs-7x00.h). O GPT e' a IRQ 7 — o
// modelo principal do emulador usa 8 e o proprio header dele admite nao ter fonte.
constexpr u32 INT_GP_TIMER = 7u;
constexpr u32 INT_MDP      = 19u;
constexpr u32 INT_USB_HS   = 47u;   // palavra 1, bit 15
constexpr u32 UART2_IRQ    = 11u;   // INT_UART3 = 11 (a tty e' ttyMSM2, a UART e' a 3)

inline u32  g_vic_en[2]      = {0, 0};
inline u32  g_vic_pending[2] = {0, 0};
inline bool g_irq_in_service = false;
inline u32  g_irq_delivered  = 0;
inline bool g_irq_log        = false;
inline u32  g_vic_cursor     = 0;   // round-robin: sem isso a IRQ de menor
                                    // numero (timer=7) starva as outras (MDP=19)

inline void on_vic_write(uc_engine* uc, uc_mem_type type, uint64_t addr,
                         int size, int64_t value, void* ud) {
    (void)uc; (void)type; (void)size; (void)ud;
    const u32 off = static_cast<u32>(addr) - VIC_BASE;
    const u32 v = static_cast<u32>(value);
    switch (off) {
        case VIC_OFF_ENSET0:        g_vic_en[0] |= v; break;
        case VIC_OFF_ENSET0 + 4:    g_vic_en[1] |= v; break;
        case VIC_OFF_ENCLEAR0:      g_vic_en[0] &= ~v; break;
        case VIC_OFF_ENCLEAR0 + 4:  g_vic_en[1] &= ~v; break;
        case VIC_OFF_CLEAR0:                       // ack: o kernel ja tratou
            g_vic_pending[0] &= ~v;
            g_irq_in_service = false;
            break;
        case VIC_OFF_CLEAR0 + 4:
            g_vic_pending[1] &= ~v;
            g_irq_in_service = false;          // idem palavra 0: sem isto a IRQ 47
            break;                             // (USB HS) trava apos a 1a entrega
        default: break;
    }
    if (g_irq_log)
        std::printf("[vic] W off=0x%03x val=0x%08x -> en0=0x%08x pend0=0x%08x\n",
                    off, v, g_vic_en[0], g_vic_pending[0]);
}

// Seleciona a proxima IRQ ativa cobrindo as DUAS palavras (0-63). O VIC do MSM tem 64
// linhas; USB HS = 47 (palavra 1, bit 15). Antes so' a palavra 0 era varrida e qualquer
// IRQ >= 32 ficava pendente para sempre.
inline u32 vic_pick_irq() {
    const u32 act0 = g_vic_pending[0] & g_vic_en[0];
    const u32 act1 = g_vic_pending[1] & g_vic_en[1];
    if (!act0 && !act1) return VIC_NO_PEND;
    for (u32 i = 0; i < 64u; ++i) {
        const u32 b = (g_vic_cursor + i) & 63u;
        const u32 act = (b < 32u) ? act0 : act1;
        if (act & (1u << (b & 31u))) { g_vic_cursor = (b + 1u) & 63u; return b; }
    }
    return VIC_NO_PEND;
}

inline void on_vic_read(uc_engine* uc, uc_mem_type type, uint64_t addr,
                        int size, int64_t value, void* ud) {
    (void)type; (void)size; (void)value; (void)ud;
    const u32 off = static_cast<u32>(addr) - VIC_BASE;
    const u32 act = g_vic_pending[0] & g_vic_en[0];
    u32 val = 0;
    if (off == VIC_OFF_VEC_RD || off == VIC_OFF_VEC_PEND) {
        val = vic_pick_irq();
    }
    else if (off == VIC_OFF_STATUS0)     val = act;
    else if (off == VIC_OFF_STATUS0 + 4) val = g_vic_pending[1] & g_vic_en[1];
    else if (off == VIC_OFF_ENSET0)      val = g_vic_en[0];
    else if (off == VIC_OFF_ENSET0 + 4)  val = g_vic_en[1];
    if (g_irq_log)
        std::printf("[vic] R off=0x%03x -> 0x%08x\n", off, val);
    uc_mem_write(uc, static_cast<u32>(addr), &val, 4);
}

// --- Modelo da UART do MSM7x00 (base: drivers/tty/serial/msm_serial.h) ------
// 0x00 MR1(w)  0x04 MR2(w)  0x08 SR(r)/CSR(w)  0x0c TF(w)/RF(r)
// 0x10 CR(w)/MISR(r)       0x14 IMR(w)/ISR(r)
// UART_SR_TX_EMPTY=(1<<3)  UART_SR_TX_READY=(1<<2)  UART_ISR_TX_READY=(1<<7)
// UART_IMR_TXLEV=(1<<0)  UART_IMR_RXSTALE=(1<<3)  UART_IMR_RXLEV=(1<<4)
// wait_for_xmitr(): se SR nao tem TX_EMPTY, ele gira lendo ISR. handle_tx() so'
// escreve enquanto SR tiver TX_READY. Os DOIS bits precisam estar certos.
//
// A UART e' o oraculo de todo payload bare-metal: "string certa na UART" e' o
// criterio de sucesso dos degraus P0/P1. `g_console` guarda o TX do guest (com os
// caracteres imprimiveis) e `on_uart_write` tambem ecoa em stdout.
inline std::string g_console;      // tudo que o guest escreveu na UART1
inline u32  g_uart_imr[3] = {0, 0, 0};
inline bool g_uart_log = false;

inline int uart_idx(u32 base) {
    return (base == UART1_BASE) ? 0 : ((base == UART2_BASE) ? 1 : 2);
}

inline u32 uart_irq_of(int idx) { return (idx == 0) ? 10u : ((idx == 1) ? UART2_IRQ : 12u); }

// --- Entrada de teclado -> RX da UART --------------------------------------
// O guest le o caractere em 0x0c (RF) e ve RX_READY (SR bit 0). O driver so'
// consome no ISR, entao a chegada de dado tambem levanta a IRQ da UART.
inline std::string g_rx_buf;          // bytes a entregar (stdin ou script)
inline bool g_stdin_tty   = false;    // stdin e' tty: ler sob demanda com select
inline bool g_rx_staged   = false;    // caractere atual visivel em RF
inline char g_rx_char     = 0;
inline bool g_uart_tx_irq = false;
inline bool g_uart_rx_irq = false;

inline void uart_update_irq() {
    const u32 bit = 1u << UART2_IRQ;
    if (g_uart_tx_irq || g_uart_rx_irq) g_vic_pending[0] |= bit;
    else                                g_vic_pending[0] &= ~bit;
}

inline void rx_fill_from_host() {
    if (!g_stdin_tty || !g_rx_buf.empty() || g_rx_staged) return;
    fd_set rf;
    FD_ZERO(&rf);
    FD_SET(0, &rf);
    struct timeval tv;
    tv.tv_sec = 0;
    tv.tv_usec = 0;
    if (select(1, &rf, nullptr, nullptr, &tv) <= 0) return;
    char c = 0;
    if (read(0, &c, 1) == 1) g_rx_buf.push_back(c);
}

inline void rx_try_stage() {
    if (g_rx_staged || g_rx_buf.empty()) return;
    // so' levanta IRQ de RX se o driver habilitou RXLEV/RXSTALE
    if ((g_uart_imr[1] & ((1u << 4) | (1u << 3))) == 0u) return;
    g_rx_char = g_rx_buf.front();
    g_rx_buf.erase(0, 1);
    g_rx_staged = true;
    g_uart_rx_irq = true;
    uart_update_irq();
    if (g_uart_log)
        std::printf("[rx] entregando 0x%02x\n", (unsigned char)g_rx_char);
}

inline void on_uart_write(uc_engine* uc, uc_mem_type type, uint64_t addr,
                          int size, int64_t value, void* ud) {
    (void)uc; (void)type; (void)size; (void)ud;
    const u32 a = static_cast<u32>(addr);
    const u32 base = uart_base_for(a);
    if (!base) return;
    const u32 off = a - base;
    if (g_uart_log)
        std::printf("[uart%d] W off=0x%02x val=0x%08x\n", uart_idx(base), off, (u32)value);
    if (off == UART_OFF_TF) {                  // TX FIFO: e' o caractere de saida
        const char c = static_cast<char>(value & 0xff);
        if (c == '\n' || c == '\r' || (c >= 0x20 && c < 0x7f)) g_console.push_back(c);
        std::putchar(c);
        std::fflush(stdout);
    } else if (off == UART_OFF_IMR) {          // mascara de interrupcao
        const int i = uart_idx(base);
        g_uart_imr[i] = (u32)value;
        // TXLEV habilitado = o driver tem dado para enviar e espera a IRQ de TX
        // (handle_tx escreve a FIFO so' quando o ISR roda).
        if (i == 1) {
            g_uart_tx_irq = ((u32)value & UART_IMR_TXLEV) != 0u;
            uart_update_irq();
        }
    }
}

inline bool on_uart_read(uc_engine* uc, uc_mem_type type, uint64_t addr,
                         int size, int64_t value, void* ud) {
    (void)type; (void)size; (void)value; (void)ud;
    const u32 a = static_cast<u32>(addr);
    const u32 base = uart_base_for(a);
    if (!base) return true;
    const u32 off = a - base;
    const int idx = uart_idx(base);
    u32 val = 0;
    if (off == UART_OFF_SR) {
        val = UART_SR_TX_EMPTY | UART_SR_TX_READY;
        if (g_rx_staged) val |= 0x1u;          // UART_SR_RX_READY (1<<0)
    } else if (off == UART_OFF_TF) {           // leitura de 0x0c = RF (FIFO de RX)
        if (g_rx_staged) {
            val = (u32)(unsigned char)g_rx_char;
            g_rx_staged = false;
            g_uart_rx_irq = false;
            uart_update_irq();
        }
    } else if (off == UART_OFF_IMR) {          // leitura de 0x14 = ISR
        val = UART_ISR_TX_READY;
    } else if (off == UART_OFF_CR) {           // leitura de 0x10 = MISR (mascarado)
        val = g_uart_imr[idx] & (UART_IMR_TXLEV | (1u << 3) | (1u << 4));
    }
    if (g_uart_log)
        std::printf("[uart%d] R off=0x%02x -> 0x%08x\n", idx, off, val);
    uc_mem_write(uc, a, &val, 4);              // o guest le este valor
    return true;
}

// --- Entrada de excecao ABORT (o Unicorn NAO vetoriza aborts no guest) ------
// O hardware ARM1136, ao levar um abort, salva o CPSR em SPSR_abt, poe
// LR_abt = PC+4 (prefetch) / PC+8 (data), muda para modo ABT e salta para o vetor.
// O Unicorn nao faz isso: o fault morre no host e o handler do guest nunca roda.
// Completamos o modelo da CPU aqui -- quem decide o destino do acesso continua sendo o
// handler do SO.
//
// `g_exc_vector_base` e' o ponto de configuracao: o Linux aqui usa VETORES ALTOS
// (0xffff0000), e um SO que rode com V=0 (o xv6, identity-mapped) usa 0x00000000. Sem
// parametrizar, o harness de um deles mandaria o outro para o endereco errado.
inline u32 g_exc_vector_base = 0xffff0000u;

struct PendingAbort { u32 addr; u32 fsr; u32 kind; };  // kind: 0=pabt, 1=dabt
inline std::vector<PendingAbort> g_pending_aborts;
inline u32 g_abort_count = 0;

inline bool on_fault_entry(uc_engine* uc, uc_mem_type type, u64 addr, int size,
                           i64 value, void* user) {
    (void)size; (void)value; (void)user;
    const bool is_fetch = (type == UC_MEM_FETCH_UNMAPPED);
    u32 cpsr = 0, pc = 0;
    uc_reg_read(uc, UC_ARM_REG_CPSR, &cpsr);
    uc_reg_read(uc, UC_ARM_REG_PC, &pc);
    // FSR de translation fault em pagina; bit 10 (0x400) = escrita
    u32 fsr = 0x7u | (is_fetch ? 0u : (type == UC_MEM_WRITE_UNMAPPED ? 0x400u : 0u));
    g_pending_aborts.push_back({(u32)addr, fsr, is_fetch ? 0u : 1u});
    if (g_pending_aborts.size() > 8) g_pending_aborts.erase(g_pending_aborts.begin());
    if (++g_abort_count <= 25) {
        std::printf("[abort] #%u %s addr=0x%08x pc=0x%08x cpsr=0x%08x fsr=0x%x\n",
                    g_abort_count, is_fetch ? "PABT" : "DABT", (u32)addr, pc, cpsr, fsr);
        std::fflush(stdout);
    }
    if (g_abort_count > 20000u) {
        std::printf("[abort] limite de faults (20000) atingido; parando\n");
        uc_emu_stop(uc);
        return true;
    }
    // Entrada de excecao do hardware: CPSR -> SPSR_abt, modo ABT, I=F=1, PC=vetor
    u32 lr_ret = pc + (is_fetch ? 4u : 8u);
    u32 abt_cpsr = (cpsr & ~0x3fu) | 0x17u | 0x80u | 0x40u;
    uc_reg_write(uc, UC_ARM_REG_CPSR, &abt_cpsr);
    uc_reg_write(uc, UC_ARM_REG_SPSR, &cpsr);
    uc_reg_write(uc, UC_ARM_REG_LR, &lr_ret);
    u32 vec = g_exc_vector_base + (is_fetch ? 0x0cu : 0x10u);
    uc_reg_write(uc, UC_ARM_REG_PC, &vec);
    return true;   // o acesso foi "tratado": retomamos no vetor
}

// --- GPT/DGT do MSM7x00 (timer) --------------------------------------------
// mach/msm_iomap-7x00.h: MSM_CSR_BASE = VA 0xE0001000 (PA 0xC0100000).
// timer.c (cpu_is_msm7x01): event_base = MSM_CSR_BASE, source_base = +0x10;
// registradores TIMER_MATCH_VAL=0x00, TIMER_COUNT_VAL=0x04, TIMER_ENABLE=0x08,
// TIMER_CLEAR=0x0c; GPT_HZ=32768 no clockevent e o clock source e' o DGT a
// 19200000>>5 = 600kHz (bate com "sched_clock: 27 bits at 600kHz" do log real).
// INT_GP_TIMER_EXP = 7 (irqs-7x00.h) -- ja' esta' definido acima como INT_GP_TIMER.
constexpr u32 CSR_BASE        = 0xE0001000u;
constexpr u32 CSR_SIZE        = 0x00001000u;
constexpr u32 CSR_PA          = 0xC0100000u;
constexpr u32 TIMER_MATCH_VAL = 0x00u;
constexpr u32 TIMER_COUNT_VAL = 0x04u;
constexpr u32 TIMER_ENABLE    = 0x08u;
constexpr u32 TIMER_CLEAR     = 0x0cu;
constexpr u32 GPT_HZ          = 32768u;
constexpr u32 DGT_HZ          = 19200000u;

// Relogio de instrucoes do guest. E' o mesmo contador que os harnesses incrementam no hook
// de codigo: o modelo do timer precisa dele para converter instrucoes em tempo do guest.
inline u64 g_icount = 0;

// 1 segundo de tempo do guest = INSN_PER_SEC instrucoes emuladas.
constexpr u64 INSN_PER_SEC = 1000000ull;

inline u32  g_gpt_match  = 0xffffffffu;
inline u32  g_gpt_enable = 0;
inline u64  g_gpt_base   = 0;       // g_icount no ultimo TIMER_CLEAR
inline bool g_timer_log  = false;
inline bool g_timer_on   = false;   // liga o clockevent virtual
inline u32  g_timer_prints = 0;     // cap do log (o kernel acessa o CSR milhares de vezes)

// O GPT conta a partir do ultimo CLEAR (o driver faz CLEAR; MATCH=delta; ENABLE).
// Sem isso o contador livre fica sempre >= MATCH e vira tempestade de ticks.
inline u32 gpt_count_now() {
    return static_cast<u32>(((g_icount - g_gpt_base) * GPT_HZ) / INSN_PER_SEC);
}
inline u32 dgt_count_now() {          // clock source: livre, nunca zerado
    return static_cast<u32>((g_icount * DGT_HZ) / INSN_PER_SEC);
}

// Reflete o estado do GPT na linha 7 do VIC (clockevent one-shot do kernel).
inline void timer_refresh_irq() {
    if (!g_timer_on) return;
    const u32 bit = 1u << INT_GP_TIMER;
    if ((g_gpt_enable & 1u) && gpt_count_now() >= g_gpt_match) g_vic_pending[0] |= bit;
    else                                                       g_vic_pending[0] &= ~bit;
}

inline void on_csr_write(uc_engine* uc, uc_mem_type type, uint64_t addr,
                         int size, int64_t value, void* ud) {
    (void)uc; (void)type; (void)size; (void)ud;
    const u32 a = static_cast<u32>(addr);
    const u32 off = (a >= CSR_BASE && a < CSR_BASE + CSR_SIZE) ? (a - CSR_BASE)
                  : ((a >= CSR_PA && a < CSR_PA + CSR_SIZE) ? (a - CSR_PA) : 0xffffffffu);
    if (off == 0xffffffffu) return;
    const u32 v = static_cast<u32>(value);
    if (off == TIMER_MATCH_VAL)      g_gpt_match = v;
    else if (off == TIMER_ENABLE)    g_gpt_enable = v;
    else if (off == TIMER_CLEAR)   { g_gpt_enable = 0; g_gpt_base = g_icount; }
    if (g_timer_log && g_timer_prints++ < 60)
        std::printf("[gpt] W off=0x%02x val=0x%08x (match=0x%x en=%u cnt=%u)\n",
                    off, v, g_gpt_match, g_gpt_enable, gpt_count_now());
    if (off == TIMER_MATCH_VAL || off == TIMER_ENABLE || off == TIMER_CLEAR)
        timer_refresh_irq();
}

inline void on_csr_read(uc_engine* uc, uc_mem_type type, uint64_t addr,
                        int size, int64_t value, void* ud) {
    (void)type; (void)size; (void)value; (void)ud;
    const u32 a = static_cast<u32>(addr);
    const u32 off = (a >= CSR_BASE && a < CSR_BASE + CSR_SIZE) ? (a - CSR_BASE)
                  : ((a >= CSR_PA && a < CSR_PA + CSR_SIZE) ? (a - CSR_PA) : 0xffffffffu);
    if (off == 0xffffffffu) return;
    u32 val = 0;
    if (off == TIMER_MATCH_VAL)          val = g_gpt_match;
    else if (off == TIMER_COUNT_VAL)     val = gpt_count_now();
    else if (off == TIMER_ENABLE)        val = g_gpt_enable;
    else if (off == TIMER_COUNT_VAL + 0x10u) val = dgt_count_now();   // DGT
    if (g_timer_log && g_timer_prints++ < 60)
        std::printf("[gpt] R off=0x%02x -> 0x%08x\n", off, val);
    // O guest le atraves da MMU: o valor tem de estar no PA, nao no VA do hook.
    uc_mem_write(uc, CSR_PA + off, &val, 4);
}

}  // namespace zeebo_msm
