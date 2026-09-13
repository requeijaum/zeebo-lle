// test_linux_boot.cpp — Teste RED do pivo "bootar um kernel Linux no MSM7201A".
//
// POR QUE ESTE PIVO
// -----------------
// O caminho BREW/Zeetris travou num ponto que depende de reconstruir o ISHELL:
// o jogo esta VIVO mas parado num estado de espera (a transicao 0x12001a38, que
// leva o seletor ctx+0x904f de 0->1, nunca e chamada). Enquanto isso, nao ha
// como exercitar CPU e GPU com carga de trabalho conhecida e verificavel.
//
// Um kernel Linux resolve isso: e software ABERTO, com fonte, cujo comportamento
// esperado e conhecido byte a byte. Se o kernel imprime no console, a CPU (MMU,
// exceptions, timers, IRQ) esta certa -- e o veredito nao depende de engenharia
// reversa. E o oposto do problema do Zeetris, onde eu nunca sei se o silencio e
// bug meu ou estado legitimo do jogo.
//
// CRITERIOS (falsificaveis, todos RED no momento da escrita)
//   L1  a imagem do kernel existe e e reconhecivel (zImage ARM ou vmlinux ELF)
//   L2  a CPU executa >= 100k instrucoes sem derail (prova de decode ARM valido)
//   L3  o console UART1 emite a assinatura "Uncompressing Linux" ou "Booting Linux"
//
// L3 e o criterio que importa: e o kernel FALANDO, nao um contador meu subindo.
// Foi exatamente a licao das rodadas anteriores -- atividade nao e progresso.
//
// INFRA JA EXISTENTE (verificada antes de escrever este teste):
//   UART1 0xa9a00000, TF@0x0c, SR@0x08 (TX_READY bit2)  -- zeebo_lle_main.cpp
//   APPS RAM fisica 0x10000000, 96MB                     -- APPS_RAM_PHYS_BASE
//   VIC 0xc0000000, GPT                                  -- zeebo_peripheral_bus.h
//
// GATE: exit 77 = SKIP quando nao ha imagem de kernel (nenhuma no repo ainda).

#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <string>
#include <deque>
#include <vector>
#include <map>
#include <algorithm>
#include <unistd.h>          // read/isatty: entrada de teclado -> RX da UART
#include <sys/select.h>
#include <unicorn/unicorn.h>

using u8 = uint8_t; using u16 = uint16_t; using u32 = uint32_t; using u64 = uint64_t; using i64 = int64_t;

namespace {

// --- nucleo do MSM7201A vem do header COMPARTILHADO -------------------------
// O que e' do SoC (mapa, UART, VIC, timer, entrada de excecao no host e o walker
// VA->PA pelas tabelas de pagina do guest) vive em zeebo_msm_soc.h, para o harness
// de boot de Linux e os harnesses dos outros SOs compartilharem o MESMO modelo em vez
// de dois que divergem. Aqui fica so' o que e' Linux-especifico (ATAGs, protocolo do
// zImage, cmdline, decodificacao do framebuffer do console).
#include "zeebo_msm_soc.h"
using zeebo_msm::APPS_RAM_PHYS;
using zeebo_msm::APPS_RAM_SIZE;
using zeebo_msm::UART1_BASE;
using zeebo_msm::UART2_BASE;
using zeebo_msm::UART3_BASE;
using zeebo_msm::UART3_PHYS;
using zeebo_msm::UART_BASES;
using zeebo_msm::UART_SIZE;
using zeebo_msm::UART_OFF_TF;
using zeebo_msm::UART_OFF_SR;
using zeebo_msm::UART_OFF_IMR;
using zeebo_msm::UART_OFF_CR;
using zeebo_msm::UART_SR_TX_EMPTY;
using zeebo_msm::UART_SR_TX_READY;
using zeebo_msm::UART_ISR_TX_READY;
using zeebo_msm::UART_IMR_TXLEV;
using zeebo_msm::uart_base_for;
using zeebo_msm::VIC_BASE;
using zeebo_msm::VIC_SIZE;
using zeebo_msm::PERIPH_BASE;
using zeebo_msm::PERIPH_SIZE;
using zeebo_msm::MDP_BASE;
using zeebo_msm::MDP_SIZE;
using zeebo_msm::TVENC_BASE;
using zeebo_msm::TVENC_SIZE;
using zeebo_msm::guest_read_u32;
using zeebo_msm::guest_read_bytes;
using zeebo_msm::arm_ls_fault_addr;
using zeebo_msm::CSR_BASE;
using zeebo_msm::CSR_SIZE;
using zeebo_msm::CSR_PA;
using zeebo_msm::TIMER_MATCH_VAL;
using zeebo_msm::TIMER_COUNT_VAL;
using zeebo_msm::TIMER_ENABLE;
using zeebo_msm::TIMER_CLEAR;
using zeebo_msm::GPT_HZ;
using zeebo_msm::DGT_HZ;
using zeebo_msm::INSN_PER_SEC;
using zeebo_msm::g_icount;
using zeebo_msm::g_gpt_match;
using zeebo_msm::g_gpt_enable;
using zeebo_msm::g_gpt_base;
using zeebo_msm::g_timer_log;
using zeebo_msm::g_timer_on;
using zeebo_msm::g_timer_prints;
using zeebo_msm::gpt_count_now;
using zeebo_msm::dgt_count_now;
using zeebo_msm::timer_refresh_irq;
using zeebo_msm::on_csr_write;
using zeebo_msm::on_csr_read;
using zeebo_msm::g_exc_vector_base;
using zeebo_msm::deliver_irq;
using zeebo_msm::PendingAbort;
using zeebo_msm::g_pending_aborts;
using zeebo_msm::g_abort_count;
using zeebo_msm::on_fault_entry;
using zeebo_msm::g_console;
using zeebo_msm::g_uart_imr;
using zeebo_msm::g_uart_log;
using zeebo_msm::uart_idx;
using zeebo_msm::uart_irq_of;
using zeebo_msm::g_rx_buf;
using zeebo_msm::g_stdin_tty;
using zeebo_msm::g_rx_staged;
using zeebo_msm::g_rx_char;
using zeebo_msm::g_uart_tx_irq;
using zeebo_msm::g_uart_rx_irq;
using zeebo_msm::uart_update_irq;
using zeebo_msm::rx_fill_from_host;
using zeebo_msm::rx_try_stage;
using zeebo_msm::on_uart_write;
using zeebo_msm::on_uart_read;
using zeebo_msm::VIC_OFF_ENCLEAR0;
using zeebo_msm::VIC_OFF_ENSET0;
using zeebo_msm::VIC_OFF_STATUS0;
using zeebo_msm::VIC_OFF_CLEAR0;
using zeebo_msm::VIC_OFF_VEC_RD;
using zeebo_msm::VIC_OFF_VEC_PEND;
using zeebo_msm::VIC_NO_PEND;
using zeebo_msm::INT_GP_TIMER;
using zeebo_msm::INT_MDP;
using zeebo_msm::INT_USB_HS;
using zeebo_msm::UART2_IRQ;
using zeebo_msm::g_vic_en;
using zeebo_msm::g_vic_pending;
using zeebo_msm::g_irq_in_service;
using zeebo_msm::g_irq_delivered;
using zeebo_msm::g_irq_log;
using zeebo_msm::g_vic_cursor;
using zeebo_msm::on_vic_write;
using zeebo_msm::on_vic_read;
using zeebo_msm::vic_pick_irq;

// Onde o zImage ARM e tipicamente carregado: base da RAM + 0x8000.
constexpr u32 KERNEL_LOAD   = APPS_RAM_PHYS + 0x8000u;

u64 g_insn = 0;

static void on_smem_write(uc_engine* uc, uc_mem_type /*type*/, uint64_t addr,
                          int size, int64_t value, void* /*user_data*/) {
    u32 off = static_cast<u32>(addr & 0xfff);
    // APP_COMMAND (offset 0x00)
    if (off == 0x00 && size == 4 && value != 0) {
        u32 done = 1;      // PCOM_CMD_DONE
        u32 success = 0;   // APP_STATUS = PCOM_CMD_SUCCESS (0)
        uc_mem_write(uc, 0x01f00000u + 0x00, &done, 4);
        uc_mem_write(uc, 0x01f00000u + 0x04, &success, 4);
        uc_mem_write(uc, 0xe0100000u + 0x00, &done, 4);
        uc_mem_write(uc, 0xe0100000u + 0x04, &success, 4);
    }
}

// --- Modelo da UART do MSM7x00 (base: drivers/tty/serial/msm_serial.h) -------
// 0x00 MR1(w)  0x04 MR2(w)  0x08 SR(r)/CSR(w)  0x0c TF(w)/RF(r)
// 0x10 CR(w)/MISR(r)       0x14 IMR(w)/ISR(r)
// UART_SR_TX_EMPTY=(1<<3)  UART_SR_TX_READY=(1<<2)  UART_ISR_TX_READY=(1<<7)
// UART_IMR_TXLEV=(1<<0)  UART_IMR_RXSTALE=(1<<3)  UART_IMR_RXLEV=(1<<4)
// wait_for_xmitr(): se SR nao tem TX_EMPTY, ele gira lendo ISR. handle_tx()
// so escreve enquanto SR tiver TX_READY. Os dois bits precisam estar certos.
//
// O MODELO (estado + on_uart_write/on_uart_read + rx_fill_from_host/rx_try_stage) vive
// em zeebo_msm_soc.h, compartilhado com os harnesses dos outros SOs. O que fica aqui e'
// so' o dispatcher de MMIO, que decide quando chamar.
void on_code(uc_engine* uc, uint64_t addr, uint32_t size, void* ud) {
    (void)uc; (void)addr; (void)size; (void)ud;
    ++g_insn;
}

std::vector<u8> read_file(const std::string& p) {
    std::ifstream f(p, std::ios::binary | std::ios::ate);
    if (!f) return {};
    std::streamoff n = f.tellg();
    if (n <= 0) return {};
    std::vector<u8> b(static_cast<size_t>(n));
    f.seekg(0);
    f.read(reinterpret_cast<char*>(b.data()), n);
    return b;
}

u32 rd32(const std::vector<u8>& d, size_t o) {
    if (o + 4 > d.size()) return 0;
    return (u32)d[o] | ((u32)d[o+1] << 8) | ((u32)d[o+2] << 16) | ((u32)d[o+3] << 24);
}

struct KernelImage {
    enum Kind { None, ZImage, Elf } kind = None;
    const char* label() const {
        return kind == ZImage ? "zImage ARM" : (kind == Elf ? "vmlinux ELF" : "desconhecida");
    }
};

// zImage ARM: magic 0x016f2818 em offset 0x24 (arch/arm/boot/compressed/head.S).
// vmlinux: ELF \x7fELF, e_machine = 40 (EM_ARM).
KernelImage identify(const std::vector<u8>& d) {
    KernelImage k;
    if (d.size() >= 0x30 && rd32(d, 0x24) == 0x016f2818u) { k.kind = KernelImage::ZImage; return k; }
    if (d.size() >= 0x14 && d[0] == 0x7f && d[1] == 'E' && d[2] == 'L' && d[3] == 'F') {
        const u32 machine = (u32)d[0x12] | ((u32)d[0x13] << 8);
        if (machine == 40) { k.kind = KernelImage::Elf; return k; }
    }
    return k;
}

struct Verdict {
    bool l1 = false, l2 = false, l3 = false;
    u64 insn = 0;
    std::string kind = "(nenhuma)";
    bool all() const { return l1 && l2 && l3; }
};

static const u64 kInsnBudget = [](){ const char* e = std::getenv("ZEEBO_BUDGET"); return e ? std::strtoull(e, nullptr, 0) : 100000ull; }();

void report(const Verdict& v) {
    std::printf("\n--- veredito: boot de kernel Linux ---\n");
    std::printf("  L1 imagem de kernel valida   : %-5s (%s)\n",
                v.l1 ? "PASS" : "FAIL", v.kind.c_str());
    std::printf("  L2 CPU executa >=100000k insn: %-5s (%llu instrucoes)\n",
                v.l2 ? "PASS" : "FAIL", (unsigned long long)v.insn);
    std::printf("  L3 console emite assinatura  : %-5s\n", v.l3 ? "PASS" : "FAIL");
    if (!g_console.empty()) {
        std::printf("\n  --- console UART1 (%zu bytes) ---\n", g_console.size());
        std::printf("%s\n", g_console.substr(0, 2000).c_str());
        if (g_console.size() > 2000) {
            const size_t tail_n = g_console.size() > 3000 ? 3000 : g_console.size() - 2000;
            std::printf("\n  --- console UART1: ULTIMOS %zu bytes ---\n", tail_n);
            std::printf("%s\n", g_console.substr(g_console.size() - tail_n).c_str());
        }
    } else {
        std::printf("  (console UART1 silencioso)\n");
    }
}

// Controle positivo do instrumento: sem ele, "console vazio" e ambiguo entre
// "o kernel nao falou" e "meu hook de UART nao funciona".
int run_selftest() {
    std::printf("=== Controle positivo do instrumento (selftest) ===\n");
    uc_engine* uc = nullptr;
    if (uc_open(UC_ARCH_ARM, UC_MODE_ARM, &uc) != UC_ERR_OK) {
        std::printf("FAIL: uc_open\n"); return 1;
    }
    // MODELO DE CPU: sem isto o Unicorn usa o default (Cortex-A15, ARMv7),
    // e o kernel aborta com "unrecognized/unsupported processor variant
    // (0x412fc0f1)". O Zeebo e ARM1136 -- ver notes/ARM_CPU_WAS_WRONG.md.
    uc_ctl_set_cpu_model(uc, UC_CPU_ARM_1136);
    uc_mem_map(uc, 0x10000000u, 0x1000u, UC_PROT_ALL);
    for (u32 ub : UART_BASES) uc_mem_map(uc, ub, UART_SIZE, UC_PROT_ALL);
    uc_hook hw = 0;
    uc_hook_add(uc, &hw, UC_HOOK_MEM_WRITE, (void*)on_uart_write, nullptr,
                UART1_BASE, UART1_BASE + UART_SIZE);

    // Programa ARM minimo que escreve "OK\n" na UART1 e para.
    // Montado com arm-none-eabi-as (NAO a mao: a primeira versao deste teste
    // tinha encoding de ORR errado -- o proprio selftest pegou o bug).
    //     ldr r0,=0xa9a00000 ; mov r1,#'O' ; str r1,[r0,#0xc] ; ... ; b .
    const u32 prog[] = {
        0xe59f0018,  // ldr r0, [pc, #0x18]   -> literal 0xa9a00000
        0xe3a0104f,  // mov r1, #0x4f  'O'
        0xe580100c,  // str r1, [r0, #0xc]
        0xe3a0104b,  // mov r1, #0x4b  'K'
        0xe580100c,  // str r1, [r0, #0xc]
        0xe3a0100a,  // mov r1, #0x0a  '\n'
        0xe580100c,  // str r1, [r0, #0xc]
        0xeafffffe,  // b .    (self loop)
        0xa9a00000,  // literal pool: UART1_BASE
    };
    uc_mem_write(uc, 0x10000000u, prog, sizeof(prog));
    uc_emu_start(uc, 0x10000000u, 0x10000000u + 7*4, 0, 8);

    const bool ok = (g_console.find("OK") != std::string::npos);
    std::printf("  console capturado: \"%s\"\n",
                g_console.empty() ? "(vazio)" : g_console.c_str());
    std::printf("  detector de console: %s\n", ok ? "PASS" : "FAIL");
    uc_close(uc);
    if (!ok) {
        std::printf("\nINSTRUMENTO QUEBRADO: o hook de UART nao captura nem um TX\n"
                    "sintetico. Um veredito L3=FAIL nao significaria nada.\n");
        return 1;
    }
    // Contraprova: a assinatura do Linux NAO pode aparecer num console que so tem "OK".
    const bool false_pos = (g_console.find("Booting Linux") != std::string::npos);
    if (false_pos) {
        std::printf("INSTRUMENTO QUEBRADO: falso positivo na assinatura.\n");
        return 1;
    }
    std::printf("\nInstrumento VALIDADO: captura TX real e nao inventa assinatura.\n");
    return 0;
}

// --- Teste sintetico: o Unicorn entrega excecoes do guest ao VETOR? --------
// Isto decide se o kernel Linux pode tratar page faults (mapeamento preguicoso)
// dentro deste harness. Se o Unicorn nao vetorizar, a excecao morre no host e
// nenhum handler do kernel roda.
static void vec_test() {
    uc_engine* uc = nullptr;
    if (uc_open(UC_ARCH_ARM, UC_MODE_ARM, &uc) != UC_ERR_OK) {
        std::printf("[vec-test] uc_open FALHOU\n"); return;
    }
    uc_ctl_set_cpu_model(uc, UC_CPU_ARM_1136);
    uc_mem_map(uc, 0x00000000u, 0x1000u, UC_PROT_ALL);   // vetores (V=0)
    uc_mem_map(uc, 0x00010000u, 0x1000u, UC_PROT_ALL);   // codigo
    u32 udf = 0xe7f000f0u;                    // udf #0  → EXCP_UDEF
    uc_mem_write(uc, 0x00010000u, &udf, 4);
    u32 spin = 0xeafffffeu;                   // b .  (marcador de "vetorizou")
    uc_mem_write(uc, 0x00010008u, &spin, 4);
    u32 vec = 0xea003fffu;                    // b 0x10008
    uc_mem_write(uc, 0x00000004u, &vec, 4);

    uc_err e = uc_emu_start(uc, 0x00010000u, 0, 0, 50);
    u32 pc = 0;
    uc_reg_read(uc, UC_ARM_REG_PC, &pc);
    const bool vectored = (e == UC_ERR_OK && pc == 0x00010008u);
    std::printf("[vec-test] UDF: emu=%s(%d) pc_final=0x%08x -> vetorizacao: %s\n",
                uc_strerror(e), (int)e, pc, vectored ? "SIM" : "NAO");

    // Mesmo teste com SWI (svc #0) -> vetor de SWI em 0x00000008, em MOTOR NOVO
    // (o motor anterior ficou com estado invalido depois do UDF). Isto decide se
    // syscalls de user space podem funcionar neste harness.
    uc_engine* uc2 = nullptr;
    if (uc_open(UC_ARCH_ARM, UC_MODE_ARM, &uc2) == UC_ERR_OK) {
        uc_ctl_set_cpu_model(uc2, UC_CPU_ARM_1136);
        uc_mem_map(uc2, 0x00000000u, 0x1000u, UC_PROT_ALL);
        uc_mem_map(uc2, 0x00010000u, 0x1000u, UC_PROT_ALL);
        uc_mem_write(uc2, 0x00010008u, &spin, 4);
        uc_mem_write(uc2, 0x00000008u, &vec, 4);
        u32 svc = 0xef000000u;                // svc #0
        uc_mem_write(uc2, 0x00010000u, &svc, 4);
        uc_err e2 = uc_emu_start(uc2, 0x00010000u, 0, 0, 50);
        u32 pc2 = 0;
        uc_reg_read(uc2, UC_ARM_REG_PC, &pc2);
        const bool vec2 = (e2 == UC_ERR_OK && pc2 == 0x00010008u);
        std::printf("[vec-test] SWI: emu=%s(%d) pc_final=0x%08x -> vetorizacao: %s\n",
                    uc_strerror(e2), (int)e2, pc2, vec2 ? "SIM" : "NAO");
        uc_close(uc2);
    }
    // O kernel programa o registrador TLS do hardware (CP15 c13,c0,2) no
    // syscall set_tls; a libc le por "mrc p15,0,rX,c13,c0,2". Se o Unicorn nao
    // modelar isso, todo acesso TLS cai em endereco baixo (ex.: 0x368).
    uc_arm_cp_reg r_tls = {15, 0, 0, 13, 0, 0, 2, 0};
    u32 tp_in = 0x0badf00du;
    r_tls.val = tp_in;
    uc_err ew = uc_reg_write(uc, UC_ARM_REG_CP_REG, &r_tls);
    uc_arm_cp_reg r_tls2 = {15, 0, 0, 13, 0, 0, 2, 0};
    uc_err er = uc_reg_read(uc, UC_ARM_REG_CP_REG, &r_tls2);
    std::printf("[vec-test] TLS (cp15 c13,c0,2): write=%s(%d) read=%s(%d) valor=0x%08x -> modelado: %s\n",
                uc_strerror(ew), (int)ew, uc_strerror(er), (int)er, (u32)r_tls2.val,
                (ew == UC_ERR_OK && er == UC_ERR_OK && (u32)r_tls2.val == tp_in) ? "SIM" : "NAO");

    // O kernel escreve TPIDRURW (c13,c0,2) e a libc le TPIDRURO (c13,c0,3) pelo
    // helper __kuser_get_tls. Na ARMv6 o segundo espelha o primeiro; se o
    // Unicorn nao espelhar, a libc recebe TP=0 e todo acesso TLS vira lixo.
    {
        uc_arm_cp_reg ro = {15, 0, 0, 13, 0, 0, 3, 0};
        uc_err ero = uc_reg_read(uc, UC_ARM_REG_CP_REG, &ro);
        std::printf("[vec-test] TLS mirror: c13,c0,3 le 0x%08x (err=%d) -> espelha c13,c0,2: %s\n",
                    (u32)ro.val, (int)ero, ((u32)ro.val == tp_in) ? "SIM" : "NAO");
    }

    // O kernel salva/restaura o SP de user space com LDM/STM modo-usuario ("^")
    // (ldmdb r8, {sp, lr}^ / ldmia sp, {r0 - lr}^). Se o Unicorn nao honrar o
    // "^", o SP do usuario volta errado depois de CADA syscall. Teste direto:
    // em modo SVC grava {sp,lr}^ e confere se o valor gravado e' o do banco USR.
    {
        uc_engine* u3 = nullptr;
        if (uc_open(UC_ARCH_ARM, UC_MODE_ARM, &u3) == UC_ERR_OK) {
            uc_mem_map(u3, 0x00000000u, 0x1000u, UC_PROT_ALL);
            uc_mem_map(u3, 0x00010000u, 0x1000u, UC_PROT_ALL);
            uc_mem_map(u3, 0x00030000u, 0x1000u, UC_PROT_ALL);
            u32 prog[2] = {0xe8c06000u, 0xeafffffeu};   // stmia r0, {sp, lr}^ ; b .
            uc_mem_write(u3, 0x00010000u, prog, sizeof(prog));
            u32 r0 = 0x00030000u;
            uc_reg_write(u3, UC_ARM_REG_R0, &r0);
            // banco USR: sp=0x1111, lr=0x2222
            u32 usr_cpsr = 0x10u;
            uc_reg_write(u3, UC_ARM_REG_CPSR, &usr_cpsr);
            u32 sp_usr = 0x00001111u, lr_usr = 0x00002222u;
            uc_reg_write(u3, UC_ARM_REG_SP, &sp_usr);
            uc_reg_write(u3, UC_ARM_REG_LR, &lr_usr);
            // banco SVC: sp=0x3333
            u32 svc_cpsr = 0x13u;
            uc_reg_write(u3, UC_ARM_REG_CPSR, &svc_cpsr);
            u32 sp_svc = 0x00003333u;
            uc_reg_write(u3, UC_ARM_REG_SP, &sp_svc);
            uc_emu_start(u3, 0x00010000u, 0, 0, 4);
            u32 w0 = 0, w1 = 0;
            uc_mem_read(u3, 0x00030000u, &w0, 4);
            uc_mem_read(u3, 0x00030004u, &w1, 4);
            std::printf("[vec-test] banco USR via '^': gravou sp=0x%08x lr=0x%08x "
                        "(esperado 0x1111/0x2222) -> LDM/STM modo-usuario: %s\n",
                        w0, w1, (w0 == 0x1111u && w1 == 0x2222u) ? "OK" : "QUEBRADO");
            uc_close(u3);
        }
    }

    std::fflush(stdout);
    uc_close(uc);
}

}  // namespace


// Diagnostico: reporta a PRIMEIRA falha de acesso a memoria (endereco e PC).
static bool g_fault_seen = false;
static u64  g_fault_addr = 0;
static u32  g_fault_pc   = 0;
static int  g_fault_type = 0;
static bool on_mem_invalid(uc_engine* uc, uc_mem_type type, u64 addr,
                           int size, i64 value, void* user) {
    (void)size; (void)value; (void)user;
    if (!g_fault_seen) {
        g_fault_seen = true; g_fault_addr = addr; g_fault_type = (int)type;
        uc_reg_read(uc, UC_ARM_REG_PC, &g_fault_pc);
    }
    return false; // nao continuar: queremos o erro honesto
}


// Instrumento (env ZEEBO_UART_PROBE=1): registra escritas em regioes altas,
// para descobrir por qual VA o kernel fala com a UART depois de ligar a MMU.
static bool g_probe = false;
static std::map<u32,int> g_hi_writes;
static void on_any_write(uc_engine* uc, uc_mem_type t, u64 addr, int size,
                         i64 value, void* user) {
    (void)uc;(void)t;(void)size;(void)value;(void)user;
    if (!g_probe) return;
    u32 a = static_cast<u32>(addr);
    if (a >= 0xffff0000u) {
        std::printf("[WRITE HIGH VEC] addr=0x%08x val=0x%llx size=%d\n", a, (unsigned long long)value, size);
    }
    // fora da RAM de aplicacao: candidato a MMIO
    if (a < APPS_RAM_PHYS || a >= APPS_RAM_PHYS + APPS_RAM_SIZE)
        g_hi_writes[a & 0xFFFFF000u]++;
}


// Instrumento (ZEEBO_UART_PROBE): amostra o PC ao longo da execucao para
// distinguir "progredindo" de "preso em laco".
static std::map<u32,long> g_pc_hist;
static u32 g_last_pc = 0;
// --- Diagnostico do primeiro retorno a user space -------------------------
// Descobre ONDE o ELF do /init foi carregado na RAM fisica e QUAL tabela de
// nivel 1 o mapeia em 0x8000 (texto). Responde a pergunta decisiva: o kernel
// chegou a mapear o texto do init na mm do processo?
static void dump_boot_mmu_diag(uc_engine* uc) {
    const u32 lo = APPS_RAM_PHYS, hi = APPS_RAM_PHYS + APPS_RAM_SIZE;
    u8 hdr[64];
    std::vector<u32> elf_pa;
    for (u32 pa = lo; pa + 0x1000 <= hi; pa += 0x1000) {
        if (uc_mem_read(uc, pa, hdr, sizeof(hdr)) != UC_ERR_OK) continue;
        if (hdr[0] == 0x7f && hdr[1] == 'E' && hdr[2] == 'L' && hdr[3] == 'F')
            elf_pa.push_back(pa);
    }
    std::printf("[mmu] cabecalhos ELF na RAM fisica: %zu\n", elf_pa.size());
    for (size_t i = 0; i < elf_pa.size() && i < 6; ++i) {
        u32 e_entry = 0;
        uc_mem_read(uc, elf_pa[i] + 24, &e_entry, 4);
        std::printf("[mmu]   PA 0x%08x entry=0x%08x\n", elf_pa[i], e_entry);
    }

    // Resolve VA -> PA dado o descritor de nivel 1 (secao ou tabela grossa).
    auto va_to_pa = [&](u32 va, u32 l1e) -> u32 {
        if ((l1e & 3u) == 0u) return 0;
        if ((l1e & 3u) == 2u) {                       // secao de 1MB
            if ((va & 0xfff00000u) != (l1e & 0xfff00000u)) return 0;
            return (l1e & 0xfff00000u) | (va & 0xfffffu);
        }
        u32 l2 = l1e & 0xfffffc00u, pte = 0;
        if (uc_mem_read(uc, l2 + ((va >> 12) & 0xffu) * 4, &pte, 4) != UC_ERR_OK) return 0;
        if ((pte & 3u) != 2u) return 0;               // small page
        return (pte & 0xfffff000u) | (va & 0xfffu);
    };

    int shown = 0;
    for (u32 l1 = lo; l1 + 0x4000 <= hi && shown < 6; l1 += 0x4000) {
        u32 e0 = 0, e2 = 0;
        if (uc_mem_read(uc, l1, &e0, 4) != UC_ERR_OK) continue;
        uc_mem_read(uc, l1 + 8, &e2, 4);
        if ((e0 & 3u) == 0u) continue;
        u32 pa_txt = va_to_pa(0x8000u, e0);
        if (!pa_txt) continue;                        // tambem cobre secao
        u32 magic = 0;
        if (uc_mem_read(uc, pa_txt, &magic, 4) != UC_ERR_OK) continue;
        if (magic != 0x464c457fu) continue;           // so mapeamentos do proprio ELF
        ++shown;
        std::printf("[mmu] tabela 0x%08x: e[0]=0x%08x e[2]=0x%08x -> VA0x8000=PA0x%08x <- CABECALHO ELF\n",
                    l1, e0, e2, pa_txt);
        u32 ep = 0;
        uc_mem_read(uc, pa_txt + 24, &ep, 4);
        std::printf("[mmu]   entry no mapeamento = 0x%08x\n", ep);
    }
    if (!shown) std::printf("[mmu] NENHUMA tabela de nivel 1 mapeia o cabecalho do ELF (VA 0x8000)\n");
    std::fflush(stdout);
}

// A ENTRADA DE EXCECAO (SPSR_abt, modo ABT, PC=vetor) agora vive em
// zeebo_msm_soc.h, junto com o vetor configuravel (`g_exc_vector_base`): o Linux usa
// vetores altos, um SO com V=0 usa 0x00000000. Este arquivo mantem o dispatcher de
// fault/interrupcao e as sondas dele (que sao Linux-especificas).

// Unicorn NAO entrega excecoes do guest ao vetor do guest (provado no vec-test):
// UDF -> UC_ERR_INSN_INVALID, SVC -> UC_ERR_EXCEPTION, abort -> INTR sem saltar
// para 0xffff000c. O host tem que fazer a ENTRADA DE EXCECAO que o hardware faz.
// Isto e' o mesmo trabalho que o QEMU faz internamente; sem ele o Linux nao
// consegue tratar page fault (mapeamento preguicoso) nem executar syscall (SWI).
static u32 g_pabt = 0, g_dabt = 0, g_swi = 0, g_pf_c = 0;


// O console do kernel nao esta saindo na UART do harness, mas o texto que o
// kernel escreveu continua em __log_buf. Este dump e' a fonte primaria do
// motivo real (inclusive a mensagem de panic).
static void dump_kernel_log(uc_engine* uc, const char* label) {
    const u32 buf_va = 0xc03cdc68u;   // __log_buf
    const u32 len    = 0x20000u;      // 128KB
    std::string raw;
    std::printf("\n=== __log_buf (%s) ===\n", label);
    if (!guest_read_bytes(uc, buf_va, len, raw)) {
        std::printf("(nao consegui ler o log buffer)\n");
        std::fflush(stdout);
        return;
    }
    std::string txt;
    txt.reserve(raw.size());
    for (char c : raw) txt.push_back((c >= 32 && c < 127) || c == '\n' ? c : '.');
    int printed = 0;
    size_t i = 0;
    while (i < txt.size()) {
        if (txt[i] != '.') {
            size_t j = i;
            while (j < txt.size() && txt[j] != '.') ++j;
            if (j - i >= 8) {
                std::printf("%s\n", txt.substr(i, j - i).c_str());
                if (++printed > 300) { std::printf("(log truncado)\n"); break; }
            }
            i = j;
        } else ++i;
    }
    if (!printed) std::printf("(log buffer vazio / sem texto)\n");
    std::fflush(stdout);
}

// Anel com os ultimos PCs de user space (para reconstruir o caminho ate a falha).
static u32 g_upc_ring[48];
static u32 g_upc_pos = 0;
static u32 g_swi_ring[24];      // numeros de syscall (r7)
static u32 g_swi_pc[24];
static u32 g_swi_pos = 0;

static void dump_user_trace(const char* label) {
    std::printf("\n=== trilha de user space (%s) ===\n", label);
    std::printf("ultimos PCs (mais antigo -> mais novo):\n ");
    for (u32 i = 0; i < 48; ++i) {
        const u32 v = g_upc_ring[(g_upc_pos + i) % 48];
        if (v) std::printf(" 0x%08x", v);
    }
    std::printf("\nultimos syscalls (nr@pc):");
    for (u32 i = 0; i < 24; ++i) {
        const u32 k = (g_swi_pos + i) % 24;
        if (g_swi_pc[k]) std::printf(" %u@0x%08x", g_swi_ring[k], g_swi_pc[k]);
    }
    std::printf("\n");
    std::fflush(stdout);
}

// Relatorio do caminho de traducao de um VA (para saber se o kernel mapeou).
static void walk_report(uc_engine* uc, u32 va) {
    uc_arm_cp_reg r0 = {15, 0, 0, 2, 0, 0, 0, 0};
    uc_arm_cp_reg r1 = {15, 0, 0, 2, 0, 0, 1, 0};
    uc_arm_cp_reg rc = {15, 0, 0, 2, 0, 0, 2, 0};
    uc_reg_read(uc, UC_ARM_REG_CP_REG, &r0);
    uc_reg_read(uc, UC_ARM_REG_CP_REG, &r1);
    uc_reg_read(uc, UC_ARM_REG_CP_REG, &rc);
    const u32 ttbr0 = (u32)r0.val, ttbr1 = (u32)r1.val, ttbrcr = (u32)rc.val;
    const u32 n = ttbrcr & 7u;
    const u32 split = (n == 0u) ? 0u : (0x80000000u >> (n - 1u));
    const u32 pgdb = ((n == 0u || va < split) ? ttbr0 : ttbr1) & 0xffffc000u;
    u32 l1 = 0, pte = 0;
    uc_mem_read(uc, pgdb + ((va >> 20) & 0xfffu) * 4u, &l1, 4);
    if ((l1 & 3u) == 1u)
        uc_mem_read(uc, (l1 & 0xfffffc00u) + ((va >> 12) & 0xffu) * 4u, &pte, 4);
    std::printf("[walk] va=0x%08x ttbr0=0x%08x ttbcr=0x%x pgdb=0x%08x l1=0x%08x pte=0x%08x\n",
                va, ttbr0, ttbrcr, pgdb, l1, pte);
}

// Detector de fault repetido no MESMO (pc, endereco): significa que o kernel
// "tratou" o fault mas a traducao continua faltando.
static void check_repeat_fault(uc_engine* uc, u32 pc, u32 addr, bool is_pabt) {
    static u32 lpc = 0, laddr = 0;
    static u32 rep = 0;
    if (is_pabt) return;
    if (pc == lpc && addr == laddr) ++rep; else { lpc = pc; laddr = addr; rep = 0; }
    if (rep != 3u) return;
    u32 r[13] = {0};
    for (int i = 0; i < 13; ++i) uc_reg_read(uc, (uc_arm_reg)(UC_ARM_REG_R0 + i), &r[i]);
    std::printf("[loop] fault repetido pc=0x%08x addr=0x%08x (3a vez)\n", pc, addr);
    std::printf("[loop]   r0=0x%08x r1=0x%08x r2=0x%08x r3=0x%08x r4=0x%08x r5=0x%08x r6=0x%08x "
                "r7=0x%08x r8=0x%08x r9=0x%08x r10=0x%08x r11=0x%08x r12=0x%08x\n",
                r[0], r[1], r[2], r[3], r[4], r[5], r[6], r[7], r[8], r[9], r[10], r[11], r[12]);
    walk_report(uc, addr);
    u32 w = 0;
    const bool ok = guest_read_u32(uc, addr, &w);
    std::printf("[loop]   leitura de 0x%08x pelo walk: %s valor=0x%08x\n",
                addr, ok ? "OK" : "FALHOU", w);
    std::fflush(stdout);
}

// --- Janela SDL2 (Wayland) com o console do guest ---------------------------
// Comentario desatualizado ate' 2026-09-13: o guest JA' TEM framebuffer. O msm_fb
// sobe e o fbcon desenha (janela 1x2, painel direito). O que nao existe no 3.4.113
// e' o **TVENC** (encoder de video composto), que so' aparece no android-msm-2.6.35
// (drivers/staging/msm/tvenc.c); hoje dirigimos um fb generico em 0x15000000 e a
// regiao 0xAA400000 fica mapeada mas inerte. Isso e' fidelidade, nao bloqueio.
// O console (ttyMSM2 -> UART) continua integro e vira o painel esquerdo; o teclado
// da janela e' injetado de volta no RX da UART.
#if defined(ZEEBO_SDL)
#include <SDL2/SDL.h>
#include <SDL2/SDL_ttf.h>

static SDL_Window*   g_sdl_win  = nullptr;
static SDL_Renderer* g_sdl_ren  = nullptr;
static TTF_Font*     g_sdl_font = nullptr;
static bool          g_sdl_on   = false;
static bool          g_sdl_quit = false;
static const char*   g_sdl_shot = "/tmp/zeebo_console.bmp";
// Janela 1x2: esquerda = texto do console UART, direita = framebuffer do guest.
static SDL_Rect      g_sdl_pane_left  = {0, 0, 720, 480};
static SDL_Rect      g_sdl_pane_right = {720, 0, 720, 480};
// Definida junto do contador de escritas do framebuffer (usada pelo dirty check do FB).
u64 fb_writes_count();

static void sdl_open() {
    // O usuario pediu Wayland: se ninguem escolheu driver, forca wayland.
    if (!std::getenv("SDL_VIDEODRIVER")) setenv("SDL_VIDEODRIVER", "wayland", 0);
    if (SDL_Init(SDL_INIT_VIDEO) != 0) {
        std::printf("[sdl] SDL_Init falhou: %s\n", SDL_GetError());
        return;
    }
    const char* drv = SDL_GetCurrentVideoDriver();
    // Janela 1x2: painel esquerdo = console UART (ttyMSM2), direito = framebuffer do
    // guest. Os dois lado a lado para depurar (o FB e' 720x480, entao 1440x480).
    g_sdl_pane_left  = {0, 0, 720, 480};
    g_sdl_pane_right = {720, 0, 720, 480};
    g_sdl_win = SDL_CreateWindow("Zeebo LLE - UART (esq) | framebuffer do guest (dir)",
                                 SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                                 1440, 480, SDL_WINDOW_SHOWN);
    if (!g_sdl_win) {
        std::printf("[sdl] janela falhou: %s\n", SDL_GetError());
        return;
    }
    // Renderer software (o acelerado nao permite ler os pixels de volta no Wayland).
    // Com ZEEBO_VSYNC=1 liga o vblank do compositor tambem -- util para inspecao
    // visual, mas bloqueia a thread (que e' a mesma da emulacao).
    Uint32 rflags = SDL_RENDERER_SOFTWARE;
    if (std::getenv("ZEEBO_VSYNC")) rflags |= SDL_RENDERER_PRESENTVSYNC;
    g_sdl_ren = SDL_CreateRenderer(g_sdl_win, -1, rflags);
    if (!g_sdl_ren) g_sdl_ren = SDL_CreateRenderer(g_sdl_win, -1, SDL_RENDERER_SOFTWARE);
    if (TTF_Init() == 0) {
        g_sdl_font = TTF_OpenFont("/usr/share/fonts/truetype/dejavu/DejaVuSansMono.ttf", 13);
    }
    std::printf("[sdl] driver=video:%s janela:ok renderer:%s fonte:%s\n",
                drv ? drv : "(nenhum)", g_sdl_ren ? "ok" : "sem", g_sdl_font ? "ok" : "sem");
    std::fflush(stdout);
    g_sdl_on = true;
}

// Teclado da janela -> RX da UART (mesmo caminho que usa o teclado do host).
// Traduz para bytes como um terminal espera: espaco, shift (maiusculas E simbolos),
// caps lock, ctrl (C-a..C-z), tab, esc, DEL, setas e as teclas de edicao (ANSI).
// A versao anterior usava SDL_GetKeyName(), que so' cobria teclas de 1 caractere --
// por isso espaço, shift e os simbolos com shift nao entravam.
static void key_to_rx(const SDL_KeyboardEvent& k) {
    const SDL_Keysym ks = k.keysym;
    const bool shift = (ks.mod & KMOD_SHIFT) != 0;
    const bool caps  = (ks.mod & KMOD_CAPS)  != 0;
    const bool ctrl  = (ks.mod & KMOD_CTRL)  != 0;
    const bool upper = (shift != caps);
    auto put = [](char c) { g_rx_buf.push_back(c); };
    if (ctrl && ks.sym >= SDLK_a && ks.sym <= SDLK_z) {       // C-a .. C-z (C-c, C-d...)
        put(static_cast<char>(ks.sym - SDLK_a + 1));
        return;
    }
    const char* seq = nullptr;
    switch (ks.sym) {
        case SDLK_RETURN: case SDLK_KP_ENTER: put('\r'); return;
        case SDLK_BACKSPACE: put(0x7f); return;                // DEL = erase do tty
        case SDLK_TAB: put('\t'); return;
        case SDLK_ESCAPE: put(27); return;
        case SDLK_SPACE: put(' '); return;
        case SDLK_UP:        seq = "\x1b[A"; break;
        case SDLK_DOWN:      seq = "\x1b[B"; break;
        case SDLK_RIGHT:     seq = "\x1b[C"; break;
        case SDLK_LEFT:      seq = "\x1b[D"; break;
        case SDLK_HOME:      seq = "\x1b[H"; break;
        case SDLK_END:       seq = "\x1b[F"; break;
        case SDLK_INSERT:    seq = "\x1b[2~"; break;
        case SDLK_DELETE:    seq = "\x1b[3~"; break;
        case SDLK_PAGEUP:    seq = "\x1b[5~"; break;
        case SDLK_PAGEDOWN:  seq = "\x1b[6~"; break;
        default: break;
    }
    if (seq) { g_rx_buf += seq; return; }
    if (ks.sym >= SDLK_a && ks.sym <= SDLK_z) {                // letras (shift/caps)
        put(static_cast<char>(upper ? (ks.sym - SDLK_a + 'A') : (ks.sym - SDLK_a + 'a')));
        return;
    }
    if (ks.sym >= SDLK_0 && ks.sym <= SDLK_9) {                // digitos e !@#$%^&*()
        static const char shifted[] = ")!@#$%^&*(";
        put(shift ? shifted[ks.sym - SDLK_0] : static_cast<char>('0' + (ks.sym - SDLK_0)));
        return;
    }
    if (ks.sym >= SDLK_KP_0 && ks.sym <= SDLK_KP_9) {          // teclado numerico
        put(static_cast<char>('0' + (ks.sym - SDLK_KP_0)));
        return;
    }
    switch (ks.sym) {                                          // pontuacao (par normal/shift)
        case SDLK_MINUS:        put(shift ? '_' : '-');  return;
        case SDLK_EQUALS:       put(shift ? '+' : '=');  return;
        case SDLK_LEFTBRACKET:  put(shift ? '{' : '[');  return;
        case SDLK_RIGHTBRACKET: put(shift ? '}' : ']');  return;
        case SDLK_BACKSLASH:    put(shift ? '|' : '\\'); return;
        case SDLK_SEMICOLON:    put(shift ? ':' : ';');  return;
        case SDLK_QUOTE:        put(shift ? '"' : '\''); return;
        case SDLK_COMMA:        put(shift ? '<' : ',');  return;
        case SDLK_PERIOD:       put(shift ? '>' : '.');  return;
        case SDLK_SLASH:        put(shift ? '?' : '/');  return;
        case SDLK_BACKQUOTE:    put(shift ? '~' : '`');  return;
        default: break;
    }
    if (ks.sym >= SDLK_F1 && ks.sym <= SDLK_F12) return;       // F* ainda nao mapeadas
}

// Auto-teste do seletor do VIC (env ZEEBO_VIC_TEST=1). Controle NEGATIVO do bug de
// IRQ >= 32: a versao antiga varria so' g_vic_pending[0], entao a IRQ 47 (USB HS,
// palavra 1 bit 15) nunca era escolhida e ficava pendente para sempre. Este teste
// REPROVA aquele codigo e aprova o vic_pick_irq() de 64 linhas.
static int vic_test() {
    std::printf("=== Auto-teste do seletor de IRQ do VIC (0-63) ===\n");
    const u32 save_en0 = g_vic_en[0],      save_en1 = g_vic_en[1];
    const u32 save_pd0 = g_vic_pending[0], save_pd1 = g_vic_pending[1];
    const u32 save_cur = g_vic_cursor;
    int fails = 0;
    auto check = [&](const char* nome, u32 got, u32 want) {
        if (got == want) std::printf("  OK   %-34s -> %u\n", nome, got);
        else { std::printf("  FAIL %-34s -> %u (esperado %u)\n", nome, got, want); ++fails; }
    };
    auto setup = [&](u32 en0, u32 pd0, u32 en1, u32 pd1) {
        g_vic_en[0] = en0; g_vic_pending[0] = pd0;
        g_vic_en[1] = en1; g_vic_pending[1] = pd1;
        g_vic_cursor = 0;
    };
    // 1) nada pendente
    setup(0, 0, 0, 0);
    check("nenhuma IRQ ativa", vic_pick_irq(), VIC_NO_PEND);
    // 2) palavra 0 (regressao: timer=7)
    setup(1u << INT_GP_TIMER, 1u << INT_GP_TIMER, 0, 0);
    check("timer (IRQ 7, palavra 0)", vic_pick_irq(), INT_GP_TIMER);
    // 3) o caso que o codigo antigo errava: IRQ 47 sozinha
    setup(0, 0, 1u << (INT_USB_HS - 32u), 1u << (INT_USB_HS - 32u));
    check("USB HS (IRQ 47, palavra 1)", vic_pick_irq(), INT_USB_HS);
    // 4) pendente mas NAO habilitada => ninguem
    setup(0, 0, 0, 1u << (INT_USB_HS - 32u));
    check("IRQ 47 pendente sem enable", vic_pick_irq(), VIC_NO_PEND);
    // 5) round-robin entre as palavras: 7 e 47 juntas nao podem starvar a 47
    setup(1u << INT_GP_TIMER, 1u << INT_GP_TIMER,
          1u << (INT_USB_HS - 32u), 1u << (INT_USB_HS - 32u));
    const u32 a = vic_pick_irq(), b = vic_pick_irq();
    check("round-robin 7/47: primeira", a, INT_GP_TIMER);
    check("round-robin 7/47: segunda",  b, INT_USB_HS);
    g_vic_en[0] = save_en0; g_vic_en[1] = save_en1;
    g_vic_pending[0] = save_pd0; g_vic_pending[1] = save_pd1;
    g_vic_cursor = save_cur;
    std::printf("=== VIC: %d falha(s) ===\n", fails);
    return fails;
}

// Auto-teste deterministico da tabela de teclas (env ZEEBO_KEY_TEST=1): alimenta
// eventos sinteticos e confere os bytes que iriam para a RX da UART. Roda antes da
// emulacao, entao a verificacao e' rapida e nao depende de janela.
static int key_test() {
    struct Case { SDL_Keycode sym; Uint16 mod; const char* want; const char* name; };
    static const Case k[] = {
        {SDLK_a, 0, "a", "a"}, {SDLK_a, KMOD_SHIFT, "A", "shift+a"},
        {SDLK_SPACE, 0, " ", "espaco"}, {SDLK_1, KMOD_SHIFT, "!", "shift+1"},
        {SDLK_SLASH, KMOD_SHIFT, "?", "shift+/"}, {SDLK_SLASH, 0, "/", "barra"},
        {SDLK_PERIOD, 0, ".", "ponto"}, {SDLK_MINUS, KMOD_SHIFT, "_", "shift+-"},
        {SDLK_SEMICOLON, KMOD_SHIFT, ":", "shift+;"}, {SDLK_RETURN, 0, "\r", "enter"},
        {SDLK_BACKSPACE, 0, "\x7f", "backspace"}, {SDLK_TAB, 0, "\t", "tab"},
        {SDLK_c, KMOD_CTRL, "\x03", "ctrl+c"}, {SDLK_d, KMOD_CTRL, "\x04", "ctrl+d"},
        {SDLK_UP, 0, "\x1b[A", "seta cima"}, {SDLK_DELETE, 0, "\x1b[3~", "delete"},
        {SDLK_a, KMOD_CAPS, "A", "caps+a"}, {SDLK_z, KMOD_CAPS | KMOD_SHIFT, "z", "caps+shift+z"},
    };
    int ok = 0, fail = 0;
    for (const Case& c : k) {
        g_rx_buf.clear();
        SDL_KeyboardEvent e{};
        e.type = SDL_KEYDOWN;
        e.keysym.sym = c.sym;
        e.keysym.mod = c.mod;
        key_to_rx(e);
        const bool good = (g_rx_buf == c.want);
        if (good) ++ok; else ++fail;
        std::printf("[key-test] %-14s -> %-8s %s\n", c.name, g_rx_buf.c_str(), good ? "OK" : "FALHOU");
        if (!good) std::printf("[key-test]   esperado: %s\n", c.want);
    }
    g_rx_buf.clear();
    std::printf("[key-test] %d OK, %d falhas\n", ok, fail);
    std::fflush(stdout);
    // Retorna o verdict: o alvo do Makefile reprovava um teste 18/18 porque a
    // chamada ficava depois do SKIP(77) e o programa seguia para o boot com o
    // orcamento default, saindo RED por um motivo que nada tinha a ver com teclas.
    return fail ? 1 : 0;
}


// --- Fila de relatorios HID (teclado USB emulado) ---------------------------
// O endpoint de interrupcao (EP1 IN) so' tem o que entregar quando ha tecla. Sem
// isso o teclado enumera, o usbhid faz bind e nasce o event0 -- mas nenhuma tecla
// chega nunca, porque todo qTD do EP1 responde NAK para sempre.
//
// Relatorio boot-protocol (8 bytes): [modificadores, reservado, keycode x6].
// Usage IDs do HID Usage Table cap.10: a-z = 0x04..0x1d, 1-9 = 0x1e..0x26,
// 0 = 0x27, Enter = 0x28, Esc = 0x29, Backspace = 0x2a, Tab = 0x2b, Espaco = 0x2c.
// Modificador bit0 = LeftCtrl, bit1 = LeftShift.
static bool g_usb_on = false;         // espelha ZEEBO_USB (EHCI ligado)
static u64  g_hid_type_at = 0;        // instrucao em que ZEEBO_HID_TYPE dispara
// Marcador no console que libera a digitacao. Amarrar a digitacao a um NUMERO de
// instrucoes (ZEEBO_HID_AT) apodrece a cada rebuild do kernel: o ponto em que a shell
// fica pronta anda junto. O marcador e' texto que o NOSSO init imprime (estavel entre
// builds), entao o gate nao depende de constante afinada a mao.
static const char* g_hid_wait = nullptr;
static bool g_hid_wait_done = false;
static u64 g_hid_wait_seen_at = 0;
// Margem depois de o marcador aparecer. NAO e' a mesma armadilha do ZEEBO_HID_AT: aqui
// o erro so' tem uma direcao perigosa (cedo demais perde tecla), e tarde demais e'
// inofensivo -- a shell continua lendo. Medido: com o prompt na tela em 264M a shell
// recebeu "ame" (perdeu os 2 primeiros caracteres); digitando em 340M chegaram todos.
// 120M de margem cobre essa janela com folga.
static u64 g_hid_wait_margin = 120000000ull;
// Ultimo texto decodificado do framebuffer (preenchido por fb_decode_text). O prompt
// da shell aparece AQUI, nunca no console da UART: esperar pelo prompt e' o sinal certo
// de "shell pronta para ler", enquanto o marcador do init sai cedo demais (o primeiro
// run com ele entregou os 8 ultimos relatorios e perdeu os 2 primeiros caracteres:
// a shell recebeu "ame" em vez de "uname").
static std::vector<std::string> g_fb_lines;
struct HidReport { u8 b[8]; };
static std::deque<HidReport> g_hid_reports;

// Injeta uma string como teclas HID (para teste headless, sem janela SDL).
// Usado por ZEEBO_HID_TYPE="ls\n" etc. So' cobre o que o mapa abaixo conhece.
static void hid_queue_key(u8 usage, u8 mods);
static void hid_type_ascii(const char* s) {
    for (const char* p = s; *p; ++p) {
        const char c = *p;
        u8 usage = 0, mods = 0;
        if (c >= 'a' && c <= 'z')      usage = static_cast<u8>(0x04 + (c - 'a'));
        else if (c >= 'A' && c <= 'Z') { usage = static_cast<u8>(0x04 + (c - 'A')); mods = 0x02; }
        else if (c >= '1' && c <= '9') usage = static_cast<u8>(0x1e + (c - '1'));
        else if (c == '0')             usage = 0x27;
        else if (c == '\n')            usage = 0x28;
        else if (c == ' ')             usage = 0x2c;
        else if (c == '.')             usage = 0x37;
        else if (c == '/')             usage = 0x38;
        else if (c == '-')             usage = 0x2d;
        hid_queue_key(usage, mods);
    }
}

static void hid_queue_key(u8 usage, u8 mods) {
    if (!usage) return;
    HidReport down{}; down.b[0] = mods; down.b[2] = usage;
    HidReport up{};                       // key-up: relatorio todo zero
    g_hid_reports.push_back(down);
    g_hid_reports.push_back(up);
}

// Traduz o evento do SDL para usage HID. Devolve 0 quando nao ha mapeamento.
static u8 hid_usage_from_sdl(const SDL_Keysym& ks, u8* mods_out) {
    u8 mods = 0;
    if (ks.mod & KMOD_SHIFT) mods |= 0x02;
    if (ks.mod & KMOD_CTRL)  mods |= 0x01;
    *mods_out = mods;
    const SDL_Keycode k = ks.sym;
    if (k >= SDLK_a && k <= SDLK_z) return static_cast<u8>(0x04 + (k - SDLK_a));
    if (k >= SDLK_1 && k <= SDLK_9) return static_cast<u8>(0x1e + (k - SDLK_1));
    switch (k) {
        case SDLK_0:         return 0x27;
        case SDLK_RETURN:    return 0x28;
        case SDLK_ESCAPE:    return 0x29;
        case SDLK_BACKSPACE: return 0x2a;
        case SDLK_TAB:       return 0x2b;
        case SDLK_SPACE:     return 0x2c;
        case SDLK_MINUS:     return 0x2d;
        case SDLK_EQUALS:    return 0x2e;
        case SDLK_PERIOD:    return 0x37;
        case SDLK_SLASH:     return 0x38;
        default:             return 0;
    }
}

static void sdl_pump() {
    SDL_Event e;
    while (SDL_PollEvent(&e)) {
        if (e.type == SDL_QUIT) { g_sdl_quit = true; continue; }
        if (e.type != SDL_KEYDOWN) continue;
        // UM caminho por tecla, nao os dois. Os dois chegam no MESMO VT (o vtbridge
        // liga serial -> tty0), entao alimentar os dois fazia cada tecla chegar duas
        // vezes e a shell ecoar "uu" para "u". Com o teclado USB enumerado o HID e' o
        // caminho de verdade (e' o que a Fase 16 queria provar); a UART fica como
        // fallback para quando o USB esta' desligado, que era o comportamento antigo.
        if (g_usb_on) {
            u8 mods = 0;
            const u8 usage = hid_usage_from_sdl(e.key.keysym, &mods);
            hid_queue_key(usage, mods);
        } else {
            key_to_rx(e.key);
        }
    }
}

// Framebuffer do guest: PA 0x15000000, 720x480, RGB565 (o board patch define
// ZEEBO_FB_BASE). Se a memoria tiver conteudo, ele e' o que aparece na janela;
// senao cai no texto do console (kernel sem driver de fb).
static constexpr u32 FB_PA   = 0x15000000u;
static constexpr int FB_XRES = 720;
static constexpr int FB_YRES = 480;
static SDL_Texture*  g_sdl_fbtex = nullptr;
static std::vector<u32> g_sdl_fbpix;

// Copia o framebuffer do guest (se houver) para o texture da janela.
// O msm_fb usa buffer duplo (yres_virtual = 2*yres), entao o quadro visivel pode
// estar na primeira ou na segunda metade: escolhemos a que tem mais tinta.
// Devolve true se desenhou o FB; false para usar o texto do console.
static bool sdl_draw_fb(uc_engine* uc) {
    if (!g_sdl_ren) return false;
    // Vblank "de verdade" nao existe aqui: quem manda no redesenho e' o limitador por
    // tempo do loop principal. E se o guest nao escreveu nada no framebuffer desde o
    // ultimo quadro, nem relê os 1,4MB: reusa o texture.
    static u64  last_drawn = 0;
    static bool fb_valid   = false;
    if (fb_valid && fb_writes_count() == last_drawn) return true;
    const u64 writes_now = fb_writes_count();
    if (!g_sdl_fbtex)
        g_sdl_fbtex = SDL_CreateTexture(g_sdl_ren, SDL_PIXELFORMAT_ARGB8888,
                                        SDL_TEXTUREACCESS_STREAMING, FB_XRES, FB_YRES);
    if (!g_sdl_fbtex) return false;
    const u32 frame_bytes = (u32)FB_XRES * FB_YRES * 2u;     // 691200
    std::vector<u8> buf(frame_bytes * 2u);
    const uc_err rerr = uc_mem_read(uc, FB_PA, buf.data(), buf.size());
    static int dprints = 0;
    if (dprints < 5) {
        ++dprints;
        std::printf("[fb] leitura de 0x%08x (%zu bytes) -> err=%d\n", FB_PA, buf.size(), (int)rerr);
        if (rerr == UC_ERR_OK) {
            size_t nz = 0;
            for (u8 b : buf) if (b) ++nz;
            std::printf("[fb]   bytes nao-zero: %zu (primeiros 8 bytes: %02x %02x %02x %02x %02x %02x %02x %02x)\n",
                        nz, buf[0], buf[1], buf[2], buf[3], buf[4], buf[5], buf[6], buf[7]);
        }
        std::fflush(stdout);
    }
    if (rerr != UC_ERR_OK) return false;
    size_t ink[2] = {0, 0};
    for (int h = 0; h < 2; ++h) {
        const u8* p = buf.data() + h * frame_bytes;
        for (u32 i = 0; i < frame_bytes; i += 2) {
            const u16 px = (u16)(p[i] | (p[i + 1] << 8));
            if (px) ++ink[h];
        }
    }
    static bool printed = false;
    if (!printed && (ink[0] || ink[1])) {
        printed = true;
        std::printf("[fb] tinta: buffer0=%zu buffer1=%zu pixels (de %d)\n",
                    ink[0], ink[1], FB_XRES * FB_YRES);
        std::fflush(stdout);
    }
    // Amostra ao longo do tempo: o texto aparece no FB e e' apagado depois?
    static int samples = 0, calls = 0;
    if (++calls % 16 == 0 && samples < 30) {
        ++samples;
        std::printf("[fbink] t=%lluM ink0=%zu ink1=%zu\n",
                    (unsigned long long)(g_icount / 1000000ull), ink[0], ink[1]);
        std::fflush(stdout);
    }
    const int half = (ink[1] > ink[0]) ? 1 : 0;
    if (ink[half] == 0) { last_drawn = writes_now; return false; }   // ainda sem imagem
    const u8* p = buf.data() + half * frame_bytes;
    g_sdl_fbpix.assign(FB_XRES * FB_YRES, 0);
    for (int i = 0; i < FB_XRES * FB_YRES; ++i) {
        const u16 px = (u16)(p[i * 2] | (p[i * 2 + 1] << 8));
        const u32 r = (px >> 11) & 0x1fu, g = (px >> 5) & 0x3fu, b = px & 0x1fu;
        g_sdl_fbpix[i] = 0xff000000u | ((r << 3 | r >> 2) << 16)
                       | ((g << 2 | g >> 4) << 8) | (b << 3 | b >> 2);
    }
    SDL_UpdateTexture(g_sdl_fbtex, nullptr, g_sdl_fbpix.data(), FB_XRES * 4);
    // Quem desenha e' o sdl_frame (painel direito); aqui so' atualizamos o texture.
    last_drawn = writes_now;
    fb_valid   = true;
    return true;
}

// Painel esquerdo: texto do console UART (fluxo de bytes do ttyMSM2).
static void sdl_text_pane(const std::string& console) {
    if (!g_sdl_on || !g_sdl_font) return;
    SDL_RenderSetViewport(g_sdl_ren, &g_sdl_pane_left);
    SDL_SetRenderDrawColor(g_sdl_ren, 16, 16, 24, 255);
    SDL_RenderFillRect(g_sdl_ren, nullptr);
    SDL_Color title = {120, 200, 255, 255};
    SDL_Surface* ts = TTF_RenderText_Blended(g_sdl_font, "UART ttyMSM2 (console do kernel)", title);
    if (ts) {
        SDL_Texture* tt = SDL_CreateTextureFromSurface(g_sdl_ren, ts);
        if (tt) { SDL_Rect d = {8, 2, ts->w, ts->h}; SDL_RenderCopy(g_sdl_ren, tt, nullptr, &d); SDL_DestroyTexture(tt); }
        SDL_FreeSurface(ts);
    }
    // ultimas 26 linhas de 88 colunas (o console e' um fluxo de bytes)
    std::vector<std::string> lines;
    std::string cur;
    for (char c : console) {
        if (c == '\r') continue;
        if (c == '\n') { lines.push_back(cur); cur.clear(); }
        else cur.push_back((c >= 32 && c < 127) ? c : ' ');
    }
    if (!cur.empty()) lines.push_back(cur);
    const size_t nlines = 26, lh = 17;
    const size_t start = lines.size() > nlines ? lines.size() - nlines : 0;
    SDL_Color fg = {220, 220, 220, 255};
    for (size_t i = start, row = 0; i < lines.size(); ++i, ++row) {
        std::string l = lines[i];
        if (l.size() > 88) l.resize(88);
        SDL_Surface* s = TTF_RenderText_Blended(g_sdl_font, l.c_str(), fg);
        if (!s) continue;
        SDL_Texture* t = SDL_CreateTextureFromSurface(g_sdl_ren, s);
        if (t) {
            SDL_Rect dst = {8, static_cast<int>(20 + row * lh), s->w, s->h};
            SDL_RenderCopy(g_sdl_ren, t, nullptr, &dst);
            SDL_DestroyTexture(t);
        }
        SDL_FreeSurface(s);
    }
}

// Painel direito: framebuffer do guest (720x480 RGB565 lido de FB_PA).
static void sdl_fb_pane() {
    if (!g_sdl_on) return;
    SDL_RenderSetViewport(g_sdl_ren, &g_sdl_pane_right);
    SDL_SetRenderDrawColor(g_sdl_ren, 0, 0, 0, 255);
    SDL_RenderFillRect(g_sdl_ren, nullptr);
    SDL_Color title = {140, 255, 160, 255};
    if (g_sdl_font) {
        SDL_Surface* ts = TTF_RenderText_Blended(g_sdl_font, "FRAMEBUFFER fb0 720x480 (tvout)", title);
        if (ts) {
            SDL_Texture* tt = SDL_CreateTextureFromSurface(g_sdl_ren, ts);
            if (tt) { SDL_Rect d = {8, 2, ts->w, ts->h}; SDL_RenderCopy(g_sdl_ren, tt, nullptr, &d); SDL_DestroyTexture(tt); }
            SDL_FreeSurface(ts);
        }
    }
    if (g_sdl_fbtex) SDL_RenderCopy(g_sdl_ren, g_sdl_fbtex, nullptr, nullptr);
}

// Um quadro da janela 1x2: UART a' esquerda, framebuffer a' direita. Os dois paineis
// sao sempre desenhados (o FB vazio fica preto, mas o painel existe) para dar para
// comparar o mesmo momento nos dois lados.
static void sdl_frame(uc_engine* uc, const std::string& console) {
    if (!g_sdl_on) return;
    sdl_draw_fb(uc);                       // atualiza o texture do FB (se houver imagem)
    SDL_RenderSetViewport(g_sdl_ren, nullptr);
    SDL_SetRenderDrawColor(g_sdl_ren, 40, 40, 48, 255);
    SDL_RenderClear(g_sdl_ren);
    sdl_text_pane(console);
    sdl_fb_pane();
    SDL_RenderSetViewport(g_sdl_ren, nullptr);
    SDL_SetRenderDrawColor(g_sdl_ren, 90, 90, 100, 255);
    SDL_RenderDrawLine(g_sdl_ren, 720, 0, 720, 480);
    SDL_RenderPresent(g_sdl_ren);
}

// Prova honesta: quantos pixels nao-fundo a janela tem (0 = nada desenhado).
// Tambem salva a memoria de FB do guest num BMP separado, para comparar com o
// que a janela mostrou (se forem o mesmo conteudo, a janela esta no modo FB).
static void sdl_fb_dump(uc_engine* uc) {
    const u32 frame_bytes = (u32)FB_XRES * FB_YRES * 2u;
    std::vector<u8> buf(frame_bytes * 2u, 0);
    if (uc_mem_read(uc, FB_PA, buf.data(), buf.size()) != UC_ERR_OK) return;
    size_t ink[2] = {0, 0};
    for (int h = 0; h < 2; ++h) {
        const u8* p = buf.data() + h * frame_bytes;
        for (u32 i = 0; i < frame_bytes; i += 2) {
            if (p[i] || p[i + 1]) ++ink[h];
        }
    }
    const int half = (ink[1] > ink[0]) ? 1 : 0;
    std::printf("[fb] memoria do guest: buffer0=%zu buffer1=%zu pixels com cor (visivel=%d)\n",
                ink[0], ink[1], half);
    if (ink[half] == 0) { std::printf("[fb] framebuffer vazio no fim do run\n"); std::fflush(stdout); return; }
    SDL_Surface* s = SDL_CreateRGBSurfaceWithFormat(0, FB_XRES, FB_YRES, 32, SDL_PIXELFORMAT_ARGB8888);
    if (!s) return;
    u32* px = static_cast<u32*>(s->pixels);
    const u8* p = buf.data() + half * frame_bytes;
    for (int i = 0; i < FB_XRES * FB_YRES; ++i) {
        const u16 v = (u16)(p[i * 2] | (p[i * 2 + 1] << 8));
        const u32 r = (v >> 11) & 0x1fu, g = (v >> 5) & 0x3fu, b = v & 0x1fu;
        px[i] = 0xff000000u | ((r << 3 | r >> 2) << 16) | ((g << 2 | g >> 4) << 8) | (b << 3 | b >> 2);
    }
    SDL_SaveBMP(s, "/tmp/zeebo_fb.bmp");
    std::printf("[fb] salvo /tmp/zeebo_fb.bmp (%d pixels com cor)\n", (int)ink[half]);
    SDL_FreeSurface(s);
    std::fflush(stdout);
}

static void sdl_shot_save() {
    if (!g_sdl_on || !g_sdl_ren) return;
    int w = 0, h = 0;
    SDL_GetRendererOutputSize(g_sdl_ren, &w, &h);
    SDL_Surface* surf = SDL_CreateRGBSurfaceWithFormat(0, w, h, 32, SDL_PIXELFORMAT_ARGB8888);
    if (!surf) return;
    if (SDL_RenderReadPixels(g_sdl_ren, nullptr, SDL_PIXELFORMAT_ARGB8888,
                             surf->pixels, surf->pitch) == 0) {
        const u32* px = static_cast<const u32*>(surf->pixels);
        size_t nonzero = 0;
        for (size_t i = 0; i < static_cast<size_t>(w) * h; ++i)
            if ((px[i] & 0x00ffffffu) > 0x00202020u) ++nonzero;
        SDL_SaveBMP(surf, g_sdl_shot);
        std::printf("[sdl] screenshot: %s (%dx%d, %zu pixels desenhados)\n",
                    g_sdl_shot, w, h, nonzero);
        std::fflush(stdout);
    }
    SDL_FreeSurface(surf);
}
#endif  // ZEEBO_SDL

// --- MDP do MSM7x00 (so o suficiente para o fb0 andar) ----------------------
// mdp_hw.h: MDP_INTR_ENABLE=0x20, MDP_INTR_STATUS=0x24, MDP_INTR_CLEAR=0x28.
// mdp.c: enable_mdp_irq() pede os bits; mdp_isr() le STATUS, escreve de volta em
// CLEAR, e se DL0_DMA2_TERM_DONE estiver setado chama o callback e acorda a
// waitqueue -- e' isso que faz msmfb_pan_display retornar em vez de esperar o
// frame-start ate o timeout (sem isso sao 878 "mdp_dma_to_mddi: busy" no boot).
// INT_MDP = 19 (irqs-7x00.h). Sem engine de DMA real, o DMA "termina" na hora:
// o conteudo do framebuffer ja foi escrito pela CPU (fbcon/cfb_*).
constexpr u32 MDP_INTR_ENABLE = 0x020u;
constexpr u32 MDP_INTR_STATUS = 0x024u;
constexpr u32 MDP_INTR_CLEAR  = 0x028u;

static u32  g_mdp_status = 0;
static bool g_mdp_log    = false;
static u32  g_draw_cnt[6] = {0, 0, 0, 0, 0, 0};   // cfb_imageblit, fbcon_putcs, bit_putcs, fbcon_init, fbcon_switch, cfb_fillrect

void on_mdp_write(uc_engine* uc, uc_mem_type type, uint64_t addr,
                  int size, int64_t value, void* ud) {
    (void)uc; (void)type; (void)size; (void)ud;
    const u32 off = static_cast<u32>(addr) - MDP_BASE;
    const u32 v = static_cast<u32>(value);
    if (off == MDP_INTR_ENABLE) {          // o driver habilitou: considera concluido
        g_mdp_status |= v;
        g_vic_pending[0] |= (1u << INT_MDP);
        if (g_mdp_log)
            std::printf("[mdp] ENABLE=0x%x -> status=0x%x (irq 19)\n", v, g_mdp_status);
    } else if (off == MDP_INTR_CLEAR) {    // ISR limpou
        g_mdp_status &= ~v;
        if (g_mdp_status == 0) g_vic_pending[0] &= ~(1u << INT_MDP);
        if (g_mdp_log)
            std::printf("[mdp] CLEAR=0x%x -> status=0x%x\n", v, g_mdp_status);
    } else if (off >= 0x10000u && off < 0x10200u) {
        // Programacao de DMA (mdp_dma_to_mddi escreve aqui): sem engine real o DMA
        // "termina" na hora -- senao o driver fica em "mdp irq already on / busy"
        // para sempre e o pan_display so sai por timeout (878 vezes por boot).
        g_mdp_status |= 0x4u;                  // DL0_DMA2_TERM_DONE
        g_vic_pending[0] |= (1u << INT_MDP);
        if (g_mdp_log) {
            static int cfg = 0;
            if (cfg++ < 40)
                std::printf("[mdp] W cfg off=0x%05x val=0x%08x (dma->irq 19)\n", off, v);
        }
    }
}

void on_mdp_read(uc_engine* uc, uc_mem_type type, uint64_t addr,
                 int size, int64_t value, void* ud) {
    (void)type; (void)size; (void)value; (void)ud;
    const u32 off = static_cast<u32>(addr) - MDP_BASE;
    if (off != MDP_INTR_STATUS) return;
    u32 val = g_mdp_status;
    if (g_mdp_log)
        std::printf("[mdp] R STATUS -> 0x%x\n", val);
    uc_mem_write(uc, static_cast<u32>(addr), &val, 4);
}

static u64 g_fb_writes = 0;        // total de escritas na faixa do framebuffer
static u64 g_fb_writes_nz = 0;     // quantas com valor != 0
u64 fb_writes_count() { return g_fb_writes; }   // usado pelo redesenho (dirty check)
// Ultimo qTD alocado pelo HCD, capturado nos PCs de retorno de ehci_qtd_alloc dentro de
// qh_urb_transaction (ver ZEEBO_USB_ASYNC no observador da lista assincrona).
static u32 g_usb_last_qtd = 0;
static u32 g_usb_last_qtd_pc = 0;

static void on_fbprobe_write(uc_engine* uc, uc_mem_type type, uint64_t addr,
                             int size, int64_t value, void* ud) {
    (void)uc; (void)type; (void)size; (void)ud;
    ++g_fb_writes;
    if (value != 0) ++g_fb_writes_nz;
    if ((g_fb_writes + g_fb_writes_nz) < 4000u && value != 0 && g_fb_writes_nz <= 8) {
        u32 pc = 0;
        uc_reg_read(uc, UC_ARM_REG_PC, &pc);
        std::printf("[fbprobe] W 0x%08llx val=0x%llx pc=0x%08x\n",
                    (unsigned long long)addr, (unsigned long long)value, pc);
        std::fflush(stdout);
    }
}

static void on_intr(uc_engine* uc, uint32_t intno, void* ud) {
    (void)ud;
    u32 cpsr = 0, pc = 0;
    uc_reg_read(uc, UC_ARM_REG_CPSR, &cpsr);
    uc_reg_read(uc, UC_ARM_REG_PC, &pc);
    if (intno != 2u && intno != 3u && intno != 4u) {
        std::printf("[INTR %u] pc=0x%08x cpsr=0x%08x (nao tratado)\n", intno, pc, cpsr);
        std::fflush(stdout);
        return;
    }
    // Ler DFAR (o endereco que faltou). O DFSR cru do Unicorn nao e' usado: injetamos
    // um FSR sintetico no formato que o Linux/ARM1136 espera (fsr_use, abaixo).
    u32 far = 0;
    uc_arm_cp_reg r_far = {15, 0, 0, 6, 0, 0, 0, 0};   // DFAR
    const uc_err ef = uc_reg_read(uc, UC_ARM_REG_CP_REG, &r_far);
    if (ef == UC_ERR_OK) far = (u32)r_far.val;

    const bool is_swi  = (intno == 2u);
    const bool is_pabt = (intno == 3u);
    bool dec = false, is_wr = false;
    const u32 fault_addr = is_pabt ? pc : (far ? far : arm_ls_fault_addr(uc, pc, &dec, &is_wr));
    // FSR do ARM1136 (VMSA curta): status 0b0111 = translation fault em pagina;
    // WnR e' o bit 11 (FSR_WRITE no Linux: 1<<11). O bit 10 (0x400) e' FSR4 e
    // manda o kernel para o indice "unknown 23" (do_bad -> SIGBUS -> mata o
    // init). Com 0x807 o kernel cai em do_page_fault("page translation fault").
    const u32 fsr_use = is_pabt ? 0x7u : (0x7u | (is_wr ? 0x800u : 0u));
    if (is_pabt && ++g_pabt <= 12u) {
        std::printf("[exc] PABT pc=0x%08x (alvo=0x%08x) cpsr=0x%08x\n", pc, pc, cpsr);
        std::fflush(stdout);
    }
    if (!is_pabt && !is_swi) {
        if (++g_dabt <= 40u) {
            u32 insn = 0;
            guest_read_u32(uc, pc, &insn);
            std::printf("[exc] DABT pc=0x%08x insn=0x%08x far=0x%08x -> addr=0x%08x fsr=0x%x%s%s\n",
                        pc, insn, far, fault_addr, fsr_use,
                        dec ? " (decodificado)" : "",
                        is_wr ? " ESCRITA" : "");
            if (fault_addr < 0x10000u) {          // endereco "wild": dump dos registradores
                u32 r[13] = {0};
                for (int i = 0; i < 13; ++i)
                    uc_reg_read(uc, (uc_arm_reg)(UC_ARM_REG_R0 + i), &r[i]);
                std::printf("[exc]   r0=0x%08x r1=0x%08x r2=0x%08x r3=0x%08x r4=0x%08x r5=0x%08x "
                            "r6=0x%08x r7=0x%08x r8=0x%08x r9=0x%08x r10=0x%08x r11=0x%08x r12=0x%08x\n",
                            r[0], r[1], r[2], r[3], r[4], r[5], r[6], r[7], r[8], r[9], r[10], r[11], r[12]);
            }
            std::fflush(stdout);
        }
    }
    if (is_swi && ++g_swi <= 5u) {
        std::printf("[exc] SWI pc=0x%08x cpsr=0x%08x\n", pc, cpsr);
        std::fflush(stdout);
    }
    if (is_swi) {
        u32 nr = 0;
        uc_reg_read(uc, UC_ARM_REG_R7, &nr);
        const u32 k = g_swi_pos++ % 24u;
        g_swi_ring[k] = nr;
        g_swi_pc[k] = pc;
    }
    if (g_pabt + g_dabt > 20000u) { std::printf("[exc] limite de aborts atingido\n"); uc_emu_stop(uc); return; }
    if (g_dabt <= 60u) check_repeat_fault(uc, pc, fault_addr, is_pabt);

    // --- entrada de excecao do hardware ---
    u32 mode, vec, lr_ret;
    // SWI: o Unicorn ja avancou o PC para a instrucao SEGUINTE, que e' exatamente
    // o LR_svc que o hardware deixaria. Somar 4 aqui pulava uma instrucao (o
    // "pop {r7}" do stub de syscall em libc), desalinhando a pilha em 4 bytes.
    if (is_swi)        { mode = 0x13u; vec = 0xffff0000u + 0x08u; lr_ret = pc; }
    else if (is_pabt)  { mode = 0x17u; vec = 0xffff0000u + 0x0cu; lr_ret = pc + 4u; }
    else               { mode = 0x17u; vec = 0xffff0000u + 0x10u; lr_ret = pc + 8u; }
    u32 new_cpsr = (cpsr & ~0x3fu) | mode | 0x80u | 0x40u;
    uc_reg_write(uc, UC_ARM_REG_CPSR, &new_cpsr);   // troca de banco de registradores
    uc_reg_write(uc, UC_ARM_REG_SPSR, &cpsr);       // SPSR_<modo> = CPSR anterior
    uc_reg_write(uc, UC_ARM_REG_LR, &lr_ret);       // LR_<modo> = retorno
    uc_reg_write(uc, UC_ARM_REG_PC, &vec);          // salta para o vetor
    if (is_pabt || !is_swi) {
        g_pending_aborts.push_back({fault_addr, fsr_use, is_pabt ? 0u : 1u});
        if (g_pending_aborts.size() > 8) g_pending_aborts.erase(g_pending_aborts.begin());
    }
}
static bool g_pc_check_on = false;     // ZEEBO_PC_CHECK
static bool g_pc_check_done = false;
static void pc_check_run(uc_engine* uc);
static void pc_check_poll(uc_engine* uc);
static void hid_type_ascii(const char* s);

static void on_code_probe(uc_engine* uc, u64 addr, u32 size, void* user) {
    (void)uc;(void)size;(void)user;
    ++g_icount;
    g_last_pc = static_cast<u32>(addr);
    // PC<->simbolo: so' faz sentido DEPOIS que o decompressor entregou o controle
    // ao kernel descomprimido. NAO da' para latchar na entry (0x10008000): esse e'
    // tambem o endereco onde o *zImage comprimido* comeca a rodar, entao o hook
    // dispararia na primeira instrucao, com o texto ainda compactado. Latchamos em
    // start_kernel (VA 0xc02994a0), que so' existe depois de descomprimir.
    // Em vez de adivinhar um PC de latch (a entry 0x10008000 e' usada DUAS vezes:
    // pelo zImage comprimido e depois pelo kernel), sondamos periodicamente ate' o
    // texto descomprimido aparecer na memoria. Barato: 1x a cada 64k instrucoes.
    if (g_pc_check_on && !g_pc_check_done && (g_icount & 0xFFFFu) == 0u) pc_check_poll(uc);
    // Timer: o clockevent one-shot do kernel (GPT, irq 7). Reflete o estado na
    // linha do VIC; a entrega de IRQ logo abaixo encontra o bit pendente.
    if ((g_icount & 0x3Fu) == 0u) timer_refresh_irq();
    // Entrega de IRQ ao guest: o Unicorn nao faz a entrada de excecao de IRQ,
    // entao o handler do kernel (handle_IRQ -> ISR da UART) nunca roda.
    // A entrada de excecao de IRQ vive no header compartilhado (peao usada pelos dois
    // harnesses). Aqui e' so' o ponto de chamada.
    if (deliver_irq(uc))
        return;
    // Entrada do host (teclado/pipe) -> RX da UART do guest.
    if ((g_icount & 0x3FFFu) == 0u) rx_fill_from_host();
    rx_try_stage();
#if defined(ZEEBO_SDL)
    // Janela: bombeia teclado e redesenha a cada ~256k instrucoes. Se o guest tem
    // framebuffer com conteudo (driver de fb carregado), ele e' o que aparece;
    // senao cai no texto do console.
    // Redesenho limitado por tempo de parede (padrao 60 fps, ZEEBO_FPS=n): antes a
    // janela desenhava a cada 256k instrucoes (centenas de fps) e relia os 1,4MB do
    // framebuffer em todo quadro. Com ZEEBO_VSYNC=1 o compositor tambem sincroniza.
    if (g_sdl_on && (g_icount & 0xFFFFu) == 0u) {
        static const u32 fps = [] {
            const char* e = std::getenv("ZEEBO_FPS");
            const int v = e ? std::atoi(e) : 60;
            return (v >= 1 && v <= 240) ? (u32)v : 60u;
        }();
        static u64 last_frame = 0;
        static u64 frames = 0, t0 = 0;
        const u64 now = SDL_GetTicks();
        const u32 step = 1000u / fps;
        if (t0 == 0) t0 = now;
        if (now - last_frame >= step) {
            last_frame = now;
            ++frames;
            sdl_pump();
            sdl_frame(uc, g_console);      // janela 1x2: UART | framebuffer
            if (g_sdl_quit) uc_emu_stop(uc);
        }
        // Medicao honesta da taxa de redesenho (a cada ~4s de tempo de parede).
        static u64 reported = 0;
        if (now - reported >= 4000u) {
            reported = now;
            const double dt = (now - t0) / 1000.0;
            std::printf("[sdl] %.1f fps efetivos (%llu quadros em %.1fs, teto %u)\n",
                        dt > 0 ? frames / dt : 0.0, (unsigned long long)frames, dt, fps);
            std::fflush(stdout);
        }
    }
    // A definicao de usb_async_poll esta' mais abaixo, junto do modelo do USB.
    void usb_async_poll(uc_engine*);
    void usb_engine_run(uc_engine*);
    void usb_periodic_run(uc_engine*);

    static const bool usb_async_on = (std::getenv("ZEEBO_USB_ASYNC") != nullptr);
    if (usb_async_on && (g_icount & 0x3FFFFu) == 0u) usb_async_poll(uc);
    // MOTOR de qTD: executa a lista assincrona (a cada 4k instrucoes -- precisa ser
    // bem mais frequente que o observador, senao o HCD estoura o timeout do URB
    // antes de a transferencia acontecer).
    // Periodico ANTES do assincrono: o qTD do EP1 e' alcancavel pelas duas varreduras
    // (o HCD reaproveita o bloco), e rodando o assincrono primeiro ele completava o
    // qTD como transferencia de controle, deixando o relatorio HID sem entregar.
    if ((g_icount & 0x3FFu) == 0u) { usb_periodic_run(uc); usb_engine_run(uc); }
    // ZEEBO_HID_TYPE: digita uma string pelo teclado USB emulado depois que o guest
    // ja' chegou na shell. Serve para provar ponta-a-ponta (sem janela) que a tecla
    // sai do host, atravessa o EP1 de interrupcao, o usbhid e o VT ate' o fbcon.
    if (g_hid_type_at && g_icount >= g_hid_type_at) {
        g_hid_type_at = 0;
        if (const char* s = std::getenv("ZEEBO_HID_TYPE")) {
            hid_type_ascii(s);
            std::printf("[usb-hid] ZEEBO_HID_TYPE disparou em %llu insn: %zu relatorios na fila\n",
                        (unsigned long long)g_icount, g_hid_reports.size());
            std::fflush(stdout);
        }
    }
    void fb_decode_text(uc_engine*);
    static const bool fb_text_on = (std::getenv("ZEEBO_FB_TEXT") != nullptr);
    if (fb_text_on && (g_icount & 0x3FFFFFu) == 0u) fb_decode_text(uc);
    // ZEEBO_HID_WAIT=<texto>: digita so' quando o texto aparecer no console OU no texto
    // ja' decodificado do framebuffer. A checagem roda no mesmo passo do decoder
    // (0x3FFFFF), nunca por instrucao.
    // O sinal BOM para "shell pronta" e' o PROMPT, que so' existe no FB -- o marcador
    // que o init imprime no console sai cedo demais: com ele o primeiro run entregou os
    // 8 ultimos relatorios e perdeu os 2 primeiros caracteres, e a shell recebeu "ame"
    // em vez de "uname".
    if (g_hid_wait && !g_hid_wait_done && (g_icount & 0x3FFFFFu) == 0u) {
        if (!g_hid_wait_seen_at) {
            bool seen = g_console.find(g_hid_wait) != std::string::npos;
            for (size_t i = 0; !seen && i < g_fb_lines.size(); ++i)
                if (g_fb_lines[i].find(g_hid_wait) != std::string::npos) seen = true;
            if (seen) {
                g_hid_wait_seen_at = g_icount;
                std::printf("[usb-hid] \"%s\" visto em %llu insn; digito em +%llu\n",
                            g_hid_wait, (unsigned long long)g_icount,
                            (unsigned long long)g_hid_wait_margin);
                std::fflush(stdout);
            }
        } else if (g_icount >= g_hid_wait_seen_at + g_hid_wait_margin) {
            g_hid_wait_done = true;
            if (const char* s = std::getenv("ZEEBO_HID_TYPE")) {
                hid_type_ascii(s);
                std::printf("[usb-hid] digitando em %llu insn: %zu relatorios na fila\n",
                            (unsigned long long)g_icount, g_hid_reports.size());
                std::fflush(stdout);
            }
        }
    }
#endif
    if (addr < 0x01000000u) {
        g_upc_ring[g_upc_pos++ % 48u] = static_cast<u32>(addr);
        static int ucount = 0;
        if (ucount++ < 20) {
            u32 cur_cpsr = 0, cur_sp = 0;
            uc_reg_read(uc, UC_ARM_REG_CPSR, &cur_cpsr);
            uc_reg_read(uc, UC_ARM_REG_SP, &cur_sp);
            std::printf("[USER-PC #%d] pc=0x%08x sp=0x%08x cpsr=0x%08x\n", ucount, static_cast<u32>(addr), cur_sp, cur_cpsr);
            std::fflush(stdout);
        }
    }
    if (addr < 0x40u || (addr >= 0xffff0000u && addr < 0xffff0100u)) {
        static int vcount = 0;
        if (vcount++ < 40) {
            u32 cpsr = 0, lr = 0;
            uc_reg_read(uc, UC_ARM_REG_CPSR, &cpsr);
            uc_reg_read(uc, UC_ARM_REG_LR, &lr);
            std::printf("[VEC] pc=0x%08x lr=0x%08x cpsr=0x%08x\n",
                        static_cast<u32>(addr), lr, cpsr);
            std::fflush(stdout);
        }
    }
    if (addr == 0xc004b3a8u) {
        // cmpxchg_futex_value_locked: ldrt r2, [r5]
        u32 cur_r5 = 0;
        uc_reg_read(uc, UC_ARM_REG_R5, &cur_r5);
        u32 val = 0;
        if (cur_r5 < 0xc0000000u) uc_mem_read(uc, cur_r5, &val, 4);
        uc_reg_write(uc, UC_ARM_REG_R2, &val);
        u32 zero = 0;
        uc_reg_write(uc, UC_ARM_REG_R0, &zero);
        u32 skip_pc = 0xc004b3b8u;
        uc_reg_write(uc, UC_ARM_REG_PC, &skip_pc);
        return;
    }
    if (addr == 0xc000936cu) {
        // movs pc, lr (retorno para user-space)
        u32 spsr = 0;
        uc_reg_read(uc, UC_ARM_REG_SPSR, &spsr);
        u32 user_pc = 0;
        uc_reg_read(uc, UC_ARM_REG_LR, &user_pc);
        std::printf("[trace 0xc000936c] LR=0x%08x SPSR=0x%08x\n", user_pc, spsr);
        if ((spsr & 0x1f) == 0x10) {
            static bool user_seen = false;
            if (!user_seen) {
                user_seen = true;
                std::printf("\n[ret_to_user] Saltando para /init no user space! LR=0x%08x SPSR=0x%08x\n", user_pc, spsr);

                // Diagnostico completo de MMU/mapeamento do init
                dump_boot_mmu_diag(uc);

                u32 ttbr0 = 0, ttbr1 = 0, ttbcr = 0;
                uc_arm_cp_reg r_ttbr0 = {15, 0, 0, 2, 0, 0, 0, 0};
                uc_arm_cp_reg r_ttbr1 = {15, 0, 0, 2, 0, 0, 1, 0};
                uc_arm_cp_reg r_ttbcr = {15, 0, 0, 2, 0, 0, 2, 0};
                uc_reg_read(uc, UC_ARM_REG_CP_REG, &r_ttbr0);
                uc_reg_read(uc, UC_ARM_REG_CP_REG, &r_ttbr1);
                uc_reg_read(uc, UC_ARM_REG_CP_REG, &r_ttbcr);
                ttbr0 = (u32)r_ttbr0.val;
                ttbr1 = (u32)r_ttbr1.val;
                ttbcr = (u32)r_ttbcr.val;
                std::printf("[MMU regs] TTBR0=0x%08x TTBR1=0x%08x TTBCR=0x%08x\n", ttbr0, ttbr1, ttbcr);
                {
                    uc_arm_cp_reg r_sctlr = {15, 0, 0, 1, 0, 0, 0, 0};
                    uc_reg_read(uc, UC_ARM_REG_CP_REG, &r_sctlr);
                    u32 sctlr = (u32)r_sctlr.val;
                    std::printf("[MMU] SCTLR=0x%08x  V(bit13)=%u  M(bit0)=%u\n",
                                sctlr, (sctlr >> 13) & 1u, sctlr & 1u);
                }
                {
                    u32 pgdb = ttbr0 & 0xffffc000u;
                    u32 v_low = 0, v_high = 0, e2048 = 0;
                    uc_mem_read(uc, pgdb, &v_low, 4);            // VA 0x00000000
                    uc_mem_read(uc, pgdb + 2048 * 4, &e2048, 4); // VA 0x80000000
                    uc_mem_read(uc, pgdb + 4095 * 4, &v_high, 4); // VA 0xffff0000
                    std::printf("[MMU] pgd: e[0]=0x%08x e[2048]=0x%08x e[4095]=0x%08x\n",
                                v_low, e2048, v_high);
                    // conteudo dos vetores, se mapeado
                    u32 vec_lo = 0, vec_hi = 0;
                    uc_mem_read(uc, 0x0000000cu, &vec_lo, 4);
                    uc_mem_read(uc, 0xffff000cu, &vec_hi, 4);
                    std::printf("[MMU] vetor pabt: low(0x0c)=0x%08x high(0xffff000c)=0x%08x\n",
                                vec_lo, vec_hi);
                }

                std::fflush(stdout);
            }
            return;
        }
    }
    if (addr == 0xc00f5edc) {
        // strbt r2, [r0], #1  (__clear_user_std)
        // HACK REMOVIDO: agora que a entrada de excecao abort funciona no host,
        // o kernel executa isso de verdade (page fault do BSS -> do_page_fault).
        static int cu = 0;
        if (cu++ < 5) {
            u32 cur_r0 = 0, cur_r1 = 0;
            uc_reg_read(uc, UC_ARM_REG_R0, &cur_r0);
            uc_reg_read(uc, UC_ARM_REG_R1, &cur_r1);
            std::printf("[clear_user] r0=0x%08x r1=%u (executando de verdade)\n", cur_r0, cur_r1);
            std::fflush(stdout);
        }
        return;
    }
    if (addr == 0xc0013cac) {
        // msm_proc_comm: loop esperando PCOM_CMD_DONE
        // r6 aponta para MSM_SHARED_RAM_BASE + APP_COMMAND (0xe0100000)
        u32 done = 1;
        uc_mem_write(uc, 0xe0100000u, &done, 4);
        uc_mem_write(uc, 0x01f00000u, &done, 4);
        u32 succ = 0;
        uc_mem_write(uc, 0xe0100004u, &succ, 4);
        uc_mem_write(uc, 0x01f00004u, &succ, 4);
        u32 ret_pc = 0xc0013cc8u;
        uc_reg_write(uc, UC_ARM_REG_PC, &ret_pc);
        return;
    }
    if (addr == 0xc0012a4c) {
        // fp aponta para base + APP_COMMAND (0xe0100000)
        u32 done = 1;
        uc_reg_write(uc, UC_ARM_REG_R3, &done);
    }
    // --- instrumentacao do caminho exec() (binfmt_elf) ---------------------
    if (addr == 0xc00d2948u) {          // load_elf_binary
        std::printf("[exec] load_elf_binary ENTRADA\n"); std::fflush(stdout);
    }
    if (addr == 0xc009322cu) {          // search_binary_handler
        u32 b = 0; uc_reg_read(uc, UC_ARM_REG_R0, &b);
        std::printf("[exec] search_binary_handler bprm=0x%08x\n", b); std::fflush(stdout);
    }
    if (addr == 0xc00922ecu) {          // setup_arg_pages
        std::printf("[exec] setup_arg_pages\n"); std::fflush(stdout);
    }
    if (addr == 0xc00d283cu) {          // elf_map(filep, addr, eppnt, prot, type, total_size)
        u32 f = 0, a = 0, ph = 0, pr = 0, ty = 0, ts = 0, sp = 0;
        uc_reg_read(uc, UC_ARM_REG_R0, &f); uc_reg_read(uc, UC_ARM_REG_R1, &a);
        uc_reg_read(uc, UC_ARM_REG_R2, &ph); uc_reg_read(uc, UC_ARM_REG_R3, &pr);
        uc_reg_read(uc, UC_ARM_REG_SP, &sp);
        if (sp) { uc_mem_read(uc, sp, &ty, 4); uc_mem_read(uc, sp + 4, &ts, 4); }
        u32 p_vaddr = 0, p_off = 0, p_filesz = 0, p_memsz = 0;
        if (ph) {
            uc_mem_read(uc, ph + 4, &p_off, 4);
            uc_mem_read(uc, ph + 8, &p_vaddr, 4);
            uc_mem_read(uc, ph + 16, &p_filesz, 4);
            uc_mem_read(uc, ph + 20, &p_memsz, 4);
        }
        std::printf("[exec] elf_map(file=0x%08x addr=0x%08x prot=0x%x type=0x%x "
                    "phdr{vaddr=0x%08x off=0x%x filesz=0x%x memsz=0x%x} total=0x%x\n",
                    f, a, pr, ty, p_vaddr, p_off, p_filesz, p_memsz, ts);
        std::fflush(stdout);
    }
    if (addr == 0xc00d28ecu || addr == 0xc00d2928u) {   // retorno de do_mmap dentro de elf_map
        u32 r = 0; uc_reg_read(uc, UC_ARM_REG_R0, &r);
        std::printf("[exec] elf_map: do_mmap -> r0=0x%08x %s\n", r,
                    (r > 0xffff0fffu) ? "(ERRO)" : "(mapeado)");
        std::fflush(stdout);
    }
    if (addr == 0xc00d2614u) {          // padzero
        u32 b = 0, n = 0;
        uc_reg_read(uc, UC_ARM_REG_R0, &b); uc_reg_read(uc, UC_ARM_REG_R1, &n);
        std::printf("[exec] padzero(bss=0x%08x len=0x%x)\n", b, n); std::fflush(stdout);
    }
    // --- traco do caminho de page fault do kernel -------------------------
    if (addr == 0xc000e950u && g_pf_c < 15u) {          // do_page_fault(addr, fsr, regs)
        u32 a = 0, f = 0;
        uc_reg_read(uc, UC_ARM_REG_R0, &a);
        uc_reg_read(uc, UC_ARM_REG_R1, &f);
        std::printf("[pf] do_page_fault addr=0x%08x fsr=0x%x\n", a, f);
        std::fflush(stdout);
    }
    if (addr == 0xc000e7c0u) {                          // __do_user_fault(tsk, addr, fsr, sig, code, regs)
        static int uf = 0;
        u32 t = 0, a = 0, f = 0, sig = 0, code = 0, sp = 0;
        uc_reg_read(uc, UC_ARM_REG_R0, &t);
        uc_reg_read(uc, UC_ARM_REG_R1, &a);
        uc_reg_read(uc, UC_ARM_REG_R2, &f);
        uc_reg_read(uc, UC_ARM_REG_R3, &sig);
        uc_reg_read(uc, UC_ARM_REG_SP, &sp);
        if (sp) uc_mem_read(uc, sp, &code, 4);
        if (uf++ < 12) {
            std::printf("[pf] __do_user_fault addr=0x%08x fsr=0x%x sig=%u code=%u (1=MAPERR 2=ACCERR)\n",
                        a, f, sig, code);
            std::fflush(stdout);
        }
    }
    if (addr == 0xc00795e8u && g_pf_c < 40u) {          // find_vma(mm, addr) -> r1=addr
        u32 a = 0;
        uc_reg_read(uc, UC_ARM_REG_R1, &a);
        std::printf("[pf]   find_vma(addr=0x%08x)\n", a);
        std::fflush(stdout);
    }
    if (addr == 0xc0076460u && g_pf_c < 15u) {          // handle_mm_fault(mm, vma, addr, flags)
        u32 a = 0, fl = 0;
        uc_reg_read(uc, UC_ARM_REG_R2, &a);
        uc_reg_read(uc, UC_ARM_REG_R3, &fl);
        ++g_pf_c;
        std::printf("[pf] handle_mm_fault addr=0x%08x flags=0x%x\n", a, fl);
        std::fflush(stdout);
    }
    if (addr == 0xc0073828u && g_pf_c < 15u) {          // vm_normal_page(vma, addr, pte)
        u32 a = 0, pte = 0;
        uc_reg_read(uc, UC_ARM_REG_R1, &a);
        uc_reg_read(uc, UC_ARM_REG_R2, &pte);
        std::printf("[pf]   vm_normal_page addr=0x%08x pte=0x%08x\n", a, pte);
        std::fflush(stdout);
    }
    // Diagnostico do salto para 0: em 0xa6c8 ha "pop {r4-r9,sl,pc}". Se o slot
    // do PC na pilha estiver zerado, o LR salvo pelo chamador estava 0 (ou a
    // pilha foi sobrescrita). Dumpamos a pilha antes do pop.
    if (addr == 0xa40cu) {          // entrada da funcao
        static int e1 = 0;
        u32 sp = 0, lr = 0;
        uc_reg_read(uc, UC_ARM_REG_SP, &sp);
        uc_reg_read(uc, UC_ARM_REG_LR, &lr);
        if (e1++ < 6) { std::printf("[stk] 0xa40c ENTRADA sp=0x%08x lr=0x%08x\n", sp, lr); std::fflush(stdout); }
    }
    if (addr == 0xa6c8u) {          // pop {r4-r9, sl, pc}
        static int e2 = 0;
        if (e2++ < 6) {
            u32 sp = 0;
            uc_reg_read(uc, UC_ARM_REG_SP, &sp);
            std::printf("[stk] 0xa6c8 POP sp=0x%08x slots:", sp);
            for (int i = 0; i < 8; ++i) {
                u32 w = 0;
                if (!guest_read_u32(uc, sp + (u32)i * 4u, &w)) { std::printf(" (leitura falhou em +%d)", i * 4); break; }
                std::printf(" [%d]=0x%08x", i * 4, w);
            }
            std::printf("\n");
            std::fflush(stdout);
        }
    }
    // Rastreio de SP nos pontos-chave (para achar o desalinhamento de 4 bytes).
    {
        static const u32 kSpProbes[] = {0xa414u, 0xa418u, 0x14d398u, 0x14d3a4u, 0x14d3a8u, 0xa6c4u, 0xa6c8u};
        for (u32 k = 0; k < sizeof(kSpProbes) / sizeof(kSpProbes[0]); ++k) {
            if (static_cast<u32>(addr) != kSpProbes[k]) continue;
            static int spc = 0;
            if (spc++ < 40) {
                u32 sp = 0, lr = 0;
                uc_reg_read(uc, UC_ARM_REG_SP, &sp);
                uc_reg_read(uc, UC_ARM_REG_LR, &lr);
                std::printf("[sp] pc=0x%08x sp=0x%08x lr=0x%08x\n", kSpProbes[k], sp, lr);
                std::fflush(stdout);
            }
            break;
        }
    }
    // Estrutura do bring-up USB: quantas vezes cada peca roda. O boot trava logo
    // apos "new USB bus registered" e o unico acesso a registrador e' o handshake do
    // HCRESET repetido -- estes contadores dizem quem esta' repetindo.
    static const bool usb_cnt_on = (std::getenv("ZEEBO_USB_LOG") != nullptr);
    // qTD recem-alocado: o HCD chama ehci_qtd_alloc e usa r0; capturamos o ponteiro nos
    // PCs de retorno (0xc018ed98/0xc018eeb0/0xc018f064/0xc018f0ec em qh_urb_transaction)
    // para depois ler o token/PID/buffer da transferencia pendente.
    static const bool usb_qtd_on = (std::getenv("ZEEBO_USB_ASYNC") != nullptr);
    if (usb_qtd_on) {
        static const u32 kRet[] = {0xc018ed98u, 0xc018eeb0u, 0xc018f064u, 0xc018f0ecu};
        for (u32 k = 0; k < 4; ++k) {
            if (static_cast<u32>(addr) != kRet[k]) continue;
            u32 r0 = 0;
            uc_reg_read(uc, UC_ARM_REG_R0, &r0);
            g_usb_last_qtd = r0;
            g_usb_last_qtd_pc = kRet[k];
            break;
        }
    }
    if (usb_cnt_on) {
        static const u32 kUsb[] = {
            0xc01893ccu, 0xc0189720u, 0xc017a354u, 0xc0174ff8u, 0xc0173564u,
            0xc0176598u, 0xc018f740u, 0xc018d0dcu, 0xc0023edcu,
            0xc019013cu, 0xc018ed78u, 0xc018e90cu, 0xc018e964u,
            0xc017ad94u, 0xc017baacu, 0xc01786a0u, 0xc018bb74u,
            0xc0189d00u, 0xc018f8dcu, 0xc01783a8u, 0xc0173b50u};
        static const char* kUsbName[] = {
            "handshake", "ehci_reset", "usb_add_hcd", "hub_port_init", "hub_port_reset",
            "hub_thread", "ehci_run", "ehci_hub_ctrl", "msleep",
            "urb_enqueue", "qh_urb_tx", "qtd_alloc", "qh_alloc",
            "usb_submit_urb", "wait_urb", "hcd_submit", "ehci_work",
            "qh_completions", "ehci_irq", "giveback_urb", "hub_activate"};
        enum { kUsbN = 21 };
        static u32 usb_cnt[kUsbN] = {0};
        for (u32 k = 0; k < kUsbN; ++k) {
            if (static_cast<u32>(addr) == kUsb[k]) { ++usb_cnt[k]; break; }
        }
        static u64 last_report = 0;
        if (g_icount - last_report > 33000000ull) {           // relatorio rolante
            last_report = g_icount;
            std::printf("[usb-cnt] insn=%llu", (unsigned long long)g_icount);
            for (u32 k = 0; k < kUsbN; ++k) std::printf(" %s=%u", kUsbName[k], usb_cnt[k]);
            std::printf("\n");
            std::fflush(stdout);
        }
    }
    // Cadeia do fbcon: quem realmente desenha no framebuffer?
    {
        // VAs do System.map do kernel ATUAL (#21). Mesma armadilha do PP: mudam a cada
        // rebuild e, desatualizados, os contadores ficam em zero e parecem "o codigo
        // nao roda" (era o caso de cfb_imageblit=0 com fbcon_putcs=60). Reconferir com:
        //   grep -E ' (cfb_imageblit|fbcon_putcs|bit_putcs|fbcon_init|fbcon_switch|cfb_fillrect)$' System.map
        // Ordem: cfb_imageblit, fbcon_putcs, bit_putcs, fbcon_init, fbcon_switch, cfb_fillrect.
        static const u32 kDraw[] = {0xc012b8fcu, 0xc0121f70u, 0xc0128cc4u, 0xc0126bb8u, 0xc012434cu, 0xc012a584u};
        static const char* kName[] = {"cfb_imageblit", "fbcon_putcs", "bit_putcs",
                                      "fbcon_init", "fbcon_switch", "cfb_fillrect"};
        for (u32 k = 0; k < 6u; ++k) {
            if (static_cast<u32>(addr) != kDraw[k]) continue;
            if (g_draw_cnt[k]++ == 0)
                std::printf("[draw] 1a chamada: %s\n", kName[k]);
            break;
        }
    }
    if (addr == 0xc01fce80u) {          // panic()
        static bool panicked = false;
        if (!panicked) {
            panicked = true;
            std::printf("\n*** [PANIC] o kernel entrou em panic() — despejando o log ***\n");
            std::fflush(stdout);
            dump_user_trace("no panic");
            dump_kernel_log(uc, "no panic");
        }
    }
    if (addr == 0xc00f6744u) {          // __delay(loops) — quem chama e com qual valor?
        static u32 dcount = 0;
        ++dcount;
        if (dcount <= 20) {
            u32 loops = 0, lr = 0;
            uc_reg_read(uc, UC_ARM_REG_R0, &loops);
            uc_reg_read(uc, UC_ARM_REG_LR, &lr);
            std::printf("[delay] #%u loops=%u (0x%x) lr=0x%08x\n", dcount, loops, loops, lr);
            std::fflush(stdout);
        } else if (dcount == 21) {
            std::printf("[delay] ... (mais chamadas; suprimido)\n");
            std::fflush(stdout);
        }
    }
    // __kuser_get_tls em 0xffff0fe0 ("mrc p15,0,r0,c13,c0,3" + "mov pc, lr").
    // Se o espelho TPIDRURO nao existir no Unicorn, a libc recebe TP=0 e todo
    // acesso TLS (TP+offset) cai em endereco baixo. Completamos o hardware.
    if (addr == 0xffff0fe4u) {
        uc_arm_cp_reg rw = {15, 0, 0, 13, 0, 0, 2, 0};
        u32 tp = 0, got = 0;
        if (uc_reg_read(uc, UC_ARM_REG_CP_REG, &rw) == UC_ERR_OK) tp = (u32)rw.val;
        uc_reg_read(uc, UC_ARM_REG_R0, &got);
        static int tls_c = 0;
        if (tls_c++ < 8) {
            std::printf("[tls] __kuser_get_tls -> 0x%08x (TPIDRURW=0x%08x)%s\n",
                        got, tp, (got == 0 && tp != 0) ? "  [injetando TP]" : "");
            std::fflush(stdout);
        }
        if (got == 0 && tp != 0) uc_reg_write(uc, UC_ARM_REG_R0, &tp);
    }
    if (addr == 0xc00083b0u || addr == 0xc000844cu) {   // do_DataAbort / do_PrefetchAbort
        const bool is_fetch = (addr == 0xc000844cu);
        const u32 want = is_fetch ? 0u : 1u;
        for (size_t i = 0; i < g_pending_aborts.size(); ++i) {
            if (g_pending_aborts[i].kind != want) continue;
            u32 a = g_pending_aborts[i].addr, f = g_pending_aborts[i].fsr;
            uc_reg_write(uc, UC_ARM_REG_R0, &a);
            uc_reg_write(uc, UC_ARM_REG_R1, &f);
            if (g_abort_count <= 25) {
                std::printf("[abort] -> %s(addr=0x%08x, fsr=0x%x)\n",
                            is_fetch ? "do_PrefetchAbort" : "do_DataAbort", a, f);
                std::fflush(stdout);
            }
            g_pending_aborts.erase(g_pending_aborts.begin() + i);
            break;
        }
    }
    if (!g_probe) return;
    if ((g_icount & 0xFFFF) == 0) g_pc_hist[static_cast<u32>(addr)]++;  // PC exato
}

// ---------------------------------------------------------------------------
// USB HS (EHCI) do MSM: o kernel ioremappa 0xa0800000 e le' os registradores de
// capacidade para subir o HCD. Sem modelo, a leitura devolve lixo e o ehci-hcd
// aborta ("can't find host controller"). Modelamos o minimo: CAPLENGTH/
// HCIVERSION/HCSPARAMS/HCCPARAMS + os operacionais, com HCRESET se limpando no
// write (ehci_reset escreve e fica em polling ate' o bit cair).
// Layout: o driver faz `ehci->caps = MSM_USB_BASE + 0x100` (USB_CAPLENGTH) e o core
// deriva os operacionais de `caps + CAPLENGTH`. Para USBCMD cair em 0x140 (como o
// msm_hsusb_hw.h define: USBCMD 0x140, PORTSC 0x184, USBMODE 0x1A8) o CAPLENGTH tem
// que ser 0x40. Antes o modelo respondia 0/lixo ali e o `ehci_reset` ficava 130 mil
// leituras em polling esperando o HCRESET cair.
static u32 g_usb_regs[0x200 / 4];
static bool g_usb_log = false;
// PORTSC da porta 1: dispositivo conectado (CCS) + habilitado (PED) + power (PP) e
// PORT_SPEED = high-speed (2), que e' o que uma porta EHCI aceita (root port de EHCI
// nao lida com full/low speed -- isso seria o controlador companheiro). O hub pede
// "Cannot enable port 1" se PED nao estiver ligado depois do reset, entao a porta ja'
// se apresenta habilitada; o reset (PR, bit 7) e' completado na leitura seguinte.
static u32  g_usb_portsc = 0x0800100Fu;
static bool g_usb_port_reset_pending = false;

// FRINDEX (0x14c). O scan_periodic() do guest monta a janela de frames que vai
// varrer a partir daqui: `clock = ehci_read_frame_index()` e depois anda de
// next_uframe ate' clock_frame. Com o registrador congelado em zero (nosso caso:
// nunca foi escrito nem sintetizado) o driver so' reexaminava o frame 0, e o qH do
// EP1 -- que fica pendurado em alguns frames do frame list -- nunca era revisitado.
// O primeiro relatorio HID era entregue porque a varredura inicial do enqueue
// percorre o anel inteiro; do segundo em diante a fila travava. Medido: apos a
// entrega #1 o guest escrevia USBSTS (ack do USBINT) e nao armava mais nenhum qTD.
static u32 g_usb_frame = 0;
static u32 usb_frame_index() { return (g_usb_frame << 3) & 0x3fffu; }

static u32 usb_reg_read(u32 off) {
    switch (off & ~0x3u) {
    // O driver faz `ehci->caps = MSM_USB_BASE + 0x100`, mas o ehci_setup do core
    // refaz `ehci->caps = hcd->regs` (base 0xa0800000) e le' o capbase ali. Como o
    // wrapper do MSM espelha o bloco, respondemos as capacidades nos dois.
    case 0x000: return 0x01000040u;          // HC_CAPBASE (espelho): caplen 0x40, versao 1.0
    case 0x004: return 0x00000011u;
    case 0x008: return 0x00000006u;
    case 0x100: return 0x01000040u;          // CAPLENGTH=0x40, HCIVERSION=0x0100 (EHCI 1.0)
    case 0x104: return 0x00000011u;          // HCSPARAMS: N_PORTS=1, PPC=1
    case 0x108: return 0x00000006u;          // HCCPARAMS: lista de frames programavel
    case 0x10c: return 0x00000000u;          // HCSP-PORTROUTE
    case 0x14c: return usb_frame_index();    // FRINDEX (ver nota acima)
    case 0x184: {                            // PORTSC1
        if (g_usb_port_reset_pending) {
            g_usb_port_reset_pending = false;
            g_usb_portsc = (g_usb_portsc & ~0x00000080u)   // PR=0 (reset terminou)
                         | 0x0000000cu                     // PED=1, PEDC=1
                         | (2u << 26);                     // PORT_SPEED = high-speed
        }
        return g_usb_portsc;
    }
    default:    return g_usb_regs[(off & 0x1ffu) >> 2];
    }
}

static void on_usb_write(uc_engine* uc, uc_mem_type type, u64 addr, int size, i64 value, void* ud) {
    (void)uc; (void)type; (void)ud; (void)size;
    u32 off = (u32)(addr & 0xfffu);
    u32 v   = (u32)value;
    // HCRESET (USBCMD bit 1) some sozinho quando o hardware aceita o reset. Neste
    // kernel o driver faz `ehci->caps = MSM_USB_BASE + 0x100` e o core deriva os
    // operacionais de `caps + CAPLENGTH`, entao o USBCMD que ele escreve/lê e' 0x100
    // (e nao 0x140, como no mapa do MSM). Sem limpar aqui o guest fica no handshake
    // do reset lendo 2 para sempre (~180k leituras ate' estourar o timeout).
    if ((off & ~0x3u) == 0x100u || (off & ~0x3u) == 0x140u) {
        v &= ~0x2u;
    }
    // PORTSC: o kernel escreve aqui para mexer nas features da porta. PR (bit 7) liga o
    // reset e os bits de mudanca (CSC 0x2, PEDC 0x8) sao write-1-to-clear.
    if ((off & ~0x3u) == 0x184u) {
        if (v & 0x80u) g_usb_port_reset_pending = true;
        u32 st = g_usb_portsc;
        if (v & 0x02u) st &= ~0x02u;
        if (v & 0x08u) st &= ~0x08u;
        g_usb_portsc = st;
        if (g_usb_log) printf("[usb] w PORTSC <- 0x%08x (PR=%u)\n", v, (v >> 7) & 1u);
        return;
    }
    if (g_usb_log) printf("[usb] w 0x%03x <- 0x%08x\n", off, v);
    g_usb_regs[(off & 0x1ffu) >> 2] = v;
}

static void on_usb_read(uc_engine* uc, uc_mem_type type, u64 addr, int size, i64 value, void* ud) {
    (void)uc; (void)type; (void)value; (void)ud; (void)size;
    u32 off = (u32)(addr & 0xfffu);
    u32 v   = usb_reg_read(off);
    static u32 n_reads = 0;
    if (g_usb_log && (n_reads < 40 || (n_reads & 0x3FFFu) == 0)) {
        u32 pc = 0, lr = 0, r4 = 0, r6 = 0, r8 = 0;
        uc_reg_read(uc, UC_ARM_REG_PC, &pc);
        uc_reg_read(uc, UC_ARM_REG_LR, &lr);
        uc_reg_read(uc, UC_ARM_REG_R4, &r4);
        uc_reg_read(uc, UC_ARM_REG_R6, &r6);
        uc_reg_read(uc, UC_ARM_REG_R8, &r8);
        printf("[usb] read 0x%03x -> 0x%08x (pc=0x%08x lr=0x%08x mask=0x%08x done=0x%08x)\n",
               off, v, pc, lr, r6, r8);
    }
    ++n_reads;
    // Serve a leitura de verdade: sem escrever na memoria do guest o kernel lia zero
    // no CAPLENGTH (HC_LENGTH = 0) e derivava o USBCMD errado (0x100 em vez de 0x140).
    g_usb_regs[(off & 0x1ffu) >> 2] = v;
    uc_mem_write(uc, (u32)addr, &v, 4);
}

// ---------------------------------------------------------------------------
// TECLADO HID EMULADO (boot protocol) na porta 1 do root hub.
// Descritores conforme USB 2.0 cap. 9 e HID 1.11. Sao os bytes que o dispositivo
// devolve nos control transfers da enumeracao; o motor de qTD abaixo os serve.
namespace hidkbd {

// Device descriptor (USB 2.0 tabela 9-8).
static const unsigned char kDevice[18] = {
    18, 0x01,               // bLength, bDescriptorType=DEVICE
    0x00, 0x02,             // bcdUSB = 2.00
    0x00, 0x00, 0x00,       // class/subclass/protocol: definidos na interface
    64,                     // bMaxPacketSize0
    0x27, 0x18,             // idVendor  = 0x1827 (livre; nao clonamos fabricante)
    0x01, 0x2b,             // idProduct = 0x2b01
    0x00, 0x01,             // bcdDevice = 1.00
    0x01, 0x02, 0x00,       // iManufacturer, iProduct, iSerialNumber
    0x01                    // bNumConfigurations
};

// HID report descriptor: teclado boot protocol (HID 1.11, apendice B.1).
static const unsigned char kReport[63] = {
    0x05, 0x01,             // Usage Page (Generic Desktop)
    0x09, 0x06,             // Usage (Keyboard)
    0xA1, 0x01,             // Collection (Application)
    0x05, 0x07,             //   Usage Page (Keyboard/Keypad)
    0x19, 0xE0,             //   Usage Minimum (LeftControl)
    0x29, 0xE7,             //   Usage Maximum (Right GUI)
    0x15, 0x00, 0x25, 0x01, //   Logical Min 0, Max 1
    0x75, 0x01, 0x95, 0x08, //   Report Size 1, Count 8
    0x81, 0x02,             //   Input (Data,Var,Abs)  -> byte de modificadores
    0x95, 0x01, 0x75, 0x08, //   Report Count 1, Size 8
    0x81, 0x03,             //   Input (Cnst,Var,Abs)  -> byte reservado
    0x95, 0x05, 0x75, 0x01, //   Report Count 5, Size 1
    0x05, 0x08,             //   Usage Page (LEDs)
    0x19, 0x01, 0x29, 0x05, //   Usage Min 1, Max 5
    0x91, 0x02,             //   Output (Data,Var,Abs) -> LEDs
    0x95, 0x01, 0x75, 0x03, //   Report Count 1, Size 3
    0x91, 0x03,             //   Output (Cnst)         -> padding
    0x95, 0x06, 0x75, 0x08, //   Report Count 6, Size 8
    0x15, 0x00, 0x25, 0x65, //   Logical Min 0, Max 101
    0x05, 0x07,             //   Usage Page (Keyboard)
    0x19, 0x00, 0x29, 0x65, //   Usage Min 0, Max 101
    0x81, 0x00,             //   Input (Data,Ary)      -> 6 teclas
    0xC0                    // End Collection
};

// Config + Interface + HID + Endpoint (34 bytes no total).
static const unsigned char kConfig[34] = {
    9, 0x02, 34, 0x00, 0x01, 0x01, 0x00, 0xA0, 50,   // CONFIG: 1 iface, bus-powered, 100mA
    9, 0x04, 0x00, 0x00, 0x01, 0x03, 0x01, 0x01, 0x00, // IFACE: HID, boot, keyboard
    9, 0x21, 0x11, 0x01, 0x00, 0x01, 0x22, 63, 0x00,   // HID: report descriptor, 63 bytes
    7, 0x05, 0x81, 0x03, 0x08, 0x00, 10                // EP 1 IN, interrupt, 8 bytes, 10ms
};

// String descriptors (UTF-16LE).
static const unsigned char kLang[4]  = {4, 0x03, 0x09, 0x04};   // 0x0409 en-US
static const unsigned char kManu[16] = {16, 0x03, 'Z',0,'e',0,'e',0,'b',0,'o',0,'-',0,'L',0};
static const unsigned char kProd[22] = {22, 0x03, 'L',0,'L',0,'E',0,' ',0,'K',0,'e',0,'y',0,'b',0,'o',0,'a',0};

static u8  g_address   = 0;      // endereco atribuido por SET_ADDRESS
static bool g_configured = false;

// Resolve GET_DESCRIPTOR. Devolve o ponteiro e o tamanho, ou nullptr.
static const unsigned char* descriptor(u16 wValue, u16 /*wIndex*/, u32* len) {
    const u8 type = (u8)(wValue >> 8), idx = (u8)(wValue & 0xff);
    switch (type) {
    case 0x01: *len = sizeof(kDevice); return kDevice;        // DEVICE
    case 0x02: *len = sizeof(kConfig); return kConfig;        // CONFIGURATION
    case 0x22: *len = sizeof(kReport); return kReport;        // HID REPORT
    case 0x21: *len = 9; return kConfig + 18;                 // HID descriptor
    case 0x03:                                                // STRING
        if (idx == 0) { *len = sizeof(kLang); return kLang; }
        if (idx == 1) { *len = sizeof(kManu); return kManu; }
        if (idx == 2) { *len = sizeof(kProd); return kProd; }
        return nullptr;
    default: return nullptr;
    }
}

} // namespace hidkbd

// ---------------------------------------------------------------------------
// MOTOR DE qTD (o "hardware" executando a lista assincrona).
// O HCD monta qH/qTD na RAM do guest e espera o controlador executa-los. Enquanto
// so' observavamos, todo URB estourava em -110 (ETIMEDOUT). Aqui executamos de fato:
// caminha a lista, para cada qTD com Active=1 faz a transferencia contra o teclado
// HID emulado, escreve os dados no buffer do guest, limpa o Active, atualiza os
// bytes restantes e levanta USBSTS.USBINT -> IRQ 47.
//
// Layout (EHCI 1.0 cap. 3.5 / 3.6):
//   qH:  0x00 link | 0x04 info1 | 0x08 info2 | 0x0c current qTD | 0x10.. overlay
//   qTD: 0x00 next | 0x04 alt   | 0x08 token | 0x0c..0x1c buffer[5]
//   token: bit7 Active, bit6 Halted, bits 8-9 PID (0=OUT 1=IN 2=SETUP),
//          bits 16-30 Total Bytes, bit 15 IOC, bits 10-11 CERR
static bool g_usb_engine_log = false;
static u32  g_usb_xfers = 0;         // qTDs executados
static u32  g_hid_delivered = 0;     // relatorios HID entregues ao guest
static u32  g_usb_irq_raised = 0;    // vezes que levantamos a IRQ 47

// Ultimo setup packet visto (o control transfer chega em 3 estagios: SETUP, DATA,
// STATUS -- cada um e' um qTD separado, entao o SETUP precisa ser lembrado).
static unsigned char g_usb_setup[8] = {0};
static bool          g_usb_setup_valid = false;

// Escreve 'len' bytes em ate' 5 paginas do qTD (buffer[0] tem offset; as demais sao
// alinhadas a 4K). Devolve quantos bytes couberam.
static u32 usb_qtd_write(uc_engine* uc, const u32 bufs[5], const unsigned char* src, u32 len) {
    u32 done = 0;
    for (int i = 0; i < 5 && done < len; ++i) {
        if (!bufs[i]) break;
        const u32 base = (i == 0) ? bufs[0] : (bufs[i] & ~0xfffu);
        const u32 room = (i == 0) ? (0x1000u - (bufs[0] & 0xfffu)) : 0x1000u;
        const u32 n    = std::min(room, len - done);
        if (uc_mem_write(uc, base, src + done, n) != UC_ERR_OK) break;
        done += n;
    }
    return done;
}

// Executa um control transfer contra o teclado HID. Devolve os bytes transferidos.
static u32 usb_control(uc_engine* uc, u32 pid, const u32 bufs[5], u32 want) {
    if (pid == 2u) {                                  // SETUP: guarda o pacote
        unsigned char s[8] = {0};
        const uc_err e = bufs[0] ? uc_mem_read(uc, bufs[0], s, 8) : UC_ERR_MAP;
        if (bufs[0] && e == UC_ERR_OK) {
            std::memcpy(g_usb_setup, s, 8);
            g_usb_setup_valid = true;
            if (g_usb_engine_log)
                std::printf("[usb-hid] SETUP %02x %02x %02x%02x %02x%02x len=%u\n",
                            s[0], s[1], s[3], s[2], s[5], s[4], (unsigned)(s[6] | (s[7] << 8)));
        }
        return want;                                  // SETUP sempre "cabe" (8 bytes)
    }
    if (!g_usb_setup_valid) return 0;
    const u8  bmReq = g_usb_setup[0], bReq = g_usb_setup[1];
    const u16 wValue = (u16)(g_usb_setup[2] | (g_usb_setup[3] << 8));
    const u16 wIndex = (u16)(g_usb_setup[4] | (g_usb_setup[5] << 8));

    if (pid == 1u) {                                  // IN: dispositivo -> host
        if (bReq == 0x06) {                           // GET_DESCRIPTOR
            u32 len = 0;
            const unsigned char* d = hidkbd::descriptor(wValue, wIndex, &len);
            if (!d) return 0;
            const u32 n = std::min(want, len);
            const u32 w = usb_qtd_write(uc, bufs, d, n);
            if (g_usb_engine_log)
                std::printf("[usb-hid] GET_DESCRIPTOR tipo=0x%02x -> %u bytes\n",
                            (unsigned)(wValue >> 8), w);
            return w;
        }
        if (bReq == 0x08) {                           // GET_CONFIGURATION
            const unsigned char c = hidkbd::g_configured ? 1 : 0;
            return usb_qtd_write(uc, bufs, &c, std::min(want, 1u));
        }
        if (bReq == 0x00) {                           // GET_STATUS
            const unsigned char st[2] = {0, 0};
            return usb_qtd_write(uc, bufs, st, std::min(want, 2u));
        }
        return 0;
    }
    // OUT / status stage
    if (bReq == 0x05) {                               // SET_ADDRESS
        hidkbd::g_address = (u8)(wValue & 0x7f);
        if (g_usb_engine_log) std::printf("[usb-hid] SET_ADDRESS %u\n", hidkbd::g_address);
    } else if (bReq == 0x09) {                        // SET_CONFIGURATION
        hidkbd::g_configured = (wValue != 0);
        if (g_usb_engine_log) std::printf("[usb-hid] SET_CONFIGURATION %u\n", wValue);
    } else if (bmReq == 0x21 && g_usb_engine_log) {   // classe HID (SET_IDLE/PROTOCOL/REPORT)
        std::printf("[usb-hid] classe req=0x%02x val=0x%04x (aceito)\n", bReq, wValue);
    }
    return want;                                      // status stage: zero-length OK
}

// Executa um qTD. Devolve true se completou (Active limpo).
// 'qtd_addr' aponta para o inicio da struct de qTD (token em +0x08, buffers em +0x0c).
// ATENCAO: o overlay do qH NAO comeca em qh+0x10 com esse layout -- ver usb_run_overlay.
// Levanta USBSTS.USBINT (bit 0) e, se habilitado em USBINTR, a IRQ 47 para o guest.
// Compartilhado pelos dois schedules (assincrono e periodico).
static void usb_raise_irq(uc_engine* uc) {
    (void)uc;
    g_usb_regs[(0x144u & 0x1ffu) >> 2] |= 0x1u;
    const u32 intr = g_usb_regs[(0x148u & 0x1ffu) >> 2];
    if (g_usb_engine_log) {
        static u32 last_intr = 0xffffffffu;
        if (intr != last_intr) {
            last_intr = intr;
            std::printf("[usb-irq] USBINTR=0x%08x (bit0=%u) irqs=%u\n",
                        intr, (unsigned)(intr & 1u), g_usb_irq_raised);
        }
    }
    if (intr & 0x1u) {
        g_vic_pending[1] |= (1u << (INT_USB_HS - 32u));
        ++g_usb_irq_raised;
        if (g_usb_engine_log && g_usb_irq_raised <= 3u)
            std::printf("[usb-irq] raise 47: vic_en[1]=0x%08x pend[1]=0x%08x\n",
                        g_vic_en[1], g_vic_pending[1]);
    }
}

static bool usb_run_qtd(uc_engine* uc, u32 qtd_addr, bool is_control, bool is_intr) {
    u32 q[8];
    if (uc_mem_read(uc, qtd_addr, q, sizeof(q)) != UC_ERR_OK) return false;
    u32 tok = q[2];
    if (!((tok >> 7) & 1u)) return false;             // nao esta' Active
    const u32 pid   = (tok >> 8) & 3u;
    const u32 total = (tok >> 16) & 0x7fffu;
    const u32 bufs[5] = {q[3], q[4], q[5], q[6], q[7]};

    u32 moved = 0;
    if (is_control) {
        moved = usb_control(uc, pid, bufs, total);
    } else if (is_intr && pid == 1u) {
        // Endpoint de interrupcao (EP1 IN). Sem tecla pendente o teclado responde
        // NAK: o qTD fica Active e o hardware tenta de novo no proximo frame --
        // nao completar e' o comportamento correto. Com tecla na fila, entregamos
        // um relatorio boot-protocol de 8 bytes e completamos o qTD, o que faz o
        // usbhid gerar o evento de tecla.
        if (g_hid_reports.empty() || total < 8u || !bufs[0]) return false;
        const HidReport rep = g_hid_reports.front();
        g_hid_reports.pop_front();
        if (uc_mem_write(uc, bufs[0], rep.b, sizeof(rep.b)) != UC_ERR_OK) return false;
        moved = 8u;
        ++g_hid_delivered;
        if (g_usb_engine_log && g_hid_delivered <= 12u)
            std::printf("[usb-hid] relatorio #%u entregue: mods=%02x key=%02x (restam %zu)\n",
                        g_hid_delivered, rep.b[0], rep.b[2], g_hid_reports.size());
    } else {
        moved = 0;
    }

    // Completa: Active=0, Total Bytes = quanto sobrou (o HCD usa isso p/ actual_length).
    const u32 left = (moved >= total) ? 0u : (total - moved);
    tok &= ~0x80u;                                    // Active = 0
    tok &= ~0x40u;                                    // Halted = 0 (sem erro)
    tok  = (tok & ~(0x7fffu << 16)) | ((left & 0x7fffu) << 16);
    uc_mem_write(uc, qtd_addr + 0x08u, &tok, 4);
    ++g_usb_xfers;
    if (g_usb_engine_log)
        std::printf("[usb-eng] qTD@0x%08x pid=%u pedidos=%u movidos=%u -> tok=0x%08x\n",
                    qtd_addr, pid, total, moved, tok);
    return true;
}

// Varre a lista assincrona e executa o que estiver Active. Chamada periodicamente
// pelo hook de instrucoes.
// Schedule PERIODICO (interrupt transfers). O teclado HID vive aqui, nao na lista
// assincrona: EP1 IN e' um endpoint de interrupcao, e o HCD o pendura no frame list
// apontado por PERIODICLISTBASE (0x154), nao em ASYNCLISTADDR (0x158). Varrer so' a
// lista assincrona fazia o motor ver apenas os qH de EP0 (controle) -- por isso o
// teclado enumerava, nascia o event0 e nenhuma tecla chegava nunca.
//
// Frame list: 1024 entradas de 32 bits. Cada uma e' um ponteiro com tipo nos bits
// 1-2 (0 = iTD, 1 = qH, 2 = siTD, 3 = FSTN) e Terminate no bit 0. Seguimos so' os
// qH (typ=1), que e' o que o usbhid usa.
void usb_periodic_run(uc_engine* uc) {
    // O clock do controlador anda um frame por tick do motor: e' o que faz o
    // scan_periodic() do guest deslizar a janela e reexaminar o qH do EP1.
    g_usb_frame = (g_usb_frame + 1u) & 0x7ffu;
    const u32 usbcmd = g_usb_regs[(0x140u & 0x1ffu) >> 2];
    const u32 flbase = g_usb_regs[(0x154u & 0x1ffu) >> 2] & ~0xfffu;
    if (!(usbcmd & 0x10u)) return;                    // Periodic Schedule Enable (PSE)
    if (!flbase) return;

    // O qH de interrupcao esta' pendurado em ALGUNS frames do frame list (o usbhid
    // pede intervalo de 10ms). Avancar um frame por chamada faz o motor cair na
    // entrada certa raramente, e a fila de teclas nunca drenava. Enquanto houver
    // relatorio pendente, procuramos o proximo frame nao-vazio em vez de esperar.
    static u32 frame = 0;
    u32 entry = 0;
    const int varredura = g_hid_reports.empty() ? 1 : 1024;
    for (int tent = 0; tent < varredura; ++tent) {
        frame = (frame + 1u) & 1023u;
        if (uc_mem_read(uc, flbase + frame * 4u, &entry, 4) != UC_ERR_OK) return;
        if (entry && !(entry & 1u)) break;             // achou entrada valida
        entry = 0;
    }
    if (!entry) return;
    for (int n = 0; n < 16 && entry && !(entry & 1u); ++n) {
        const u32 typ = (entry >> 1) & 3u;
        const u32 ptr = entry & ~0x1fu;
        if (typ != 1u) break;                         // so' qH interessa aqui
        u32 w[12];
        if (uc_mem_read(uc, ptr, w, sizeof(w)) != UC_ERR_OK) break;
        const u32 ep = (w[1] >> 8) & 0xfu;
        if (g_usb_engine_log) {
            static u32 shown = 0, shown_q = 0;
            // com fila pendente o log e' o que importa: mostra o token do overlay
            // para saber se o qTD do EP1 esta' Active (bit7) ou se o HCD o retirou.
            if (!g_hid_reports.empty() ? (shown_q++ < 40u) : (shown++ < 4u))
                std::printf("[usb-per] f=%u EP%u fila=%zu entregues=%u cur=%08x ovnext=%08x ovtok=%08x\n",
                            frame, ep, g_hid_reports.size(), g_hid_delivered, w[3], w[4], w[6]);
        }
        // overlay em +0x10 (mesmo layout do caminho assincrono)
        const u32 cur_qtd = w[3] & ~0x1fu;
        if (usb_run_qtd(uc, ptr + 0x10u, false, true)) {
            if (cur_qtd) {
                u32 ovtok = 0;
                if (uc_mem_read(uc, ptr + 0x18u, &ovtok, 4) == UC_ERR_OK)
                    uc_mem_write(uc, cur_qtd + 0x08u, &ovtok, 4);
            }
            usb_raise_irq(uc);
        }
        // Percorre AS DUAS cadeias. Num qH de interrupcao recem-primado o HCD deixa
        // hw_current = 0 e pendura o qTD em overlay.next (+0x10): seguir so' o
        // hw_current fazia o motor concluir "nao ha trabalho" com a fila de teclas
        // cheia (medido: ovtok=0 cur=0 mas ovnext=0x138a4180 com qTD Active).
        const u32 cadeias[2] = {cur_qtd, w[4] & ~0x1fu};
        for (u32 inicio : cadeias) {
            u32 cur = inicio;
            for (int k = 0; k < 8 && cur && !(cur & 1u); ++k) {
                if (usb_run_qtd(uc, cur, false, true)) {
                    // O HCD le' a conclusao no OVERLAY do qH, nao no qTD solto:
                    // sem espelhar, o ehci_urb_dequeue nunca via' a transferencia
                    // terminar e so' o primeiro relatorio passava.
                    u32 q2[8];
                    if (uc_mem_read(uc, cur, q2, sizeof(q2)) == UC_ERR_OK) {
                        uc_mem_write(uc, ptr + 0x10u, q2, sizeof(q2));   // overlay
                        uc_mem_write(uc, ptr + 0x0cu, &cur, 4);          // hw_current
                    }
                    usb_raise_irq(uc);
                }
                u32 nx = 0;
                if (uc_mem_read(uc, cur, &nx, 4) != UC_ERR_OK) break;
                if (nx & 1u) break;
                cur = nx & ~0x1fu;
            }
        }
        entry = w[0];                                  // proximo na cadeia do frame
    }
}

void usb_engine_run(uc_engine* uc) {
    const u32 usbcmd = g_usb_regs[(0x140u & 0x1ffu) >> 2];
    const u32 list = g_usb_regs[(0x158u & 0x1ffu) >> 2] & ~0x1fu;
    // DIAGNOSTICO (ZEEBO_USB_ENGINE_LOG): conta as vezes em que havia lista mas o
    // motor desistiu por ASE=0. Se o SETUP perdido cair nessa janela, a hipotese
    // (a) -- HCD desliga ASE durante unlink/relink -- esta' confirmada.
    if (!(usbcmd & 0x20u)) {
        if (list) {
            static u32 skipped = 0;
            if (g_usb_engine_log && (skipped++ < 8u))
                std::printf("[usb-ase] motor pulou: ASE=0 com ASYNCLISTADDR=0x%08x (skip #%u)\n",
                            list, skipped);
        }
        return;                                       // Async Schedule Enable (ASE)
    }
    if (!list) return;

    bool any = false;
    u32 qh = list;
    for (int n = 0; n < 32 && qh; ++n) {              // a lista e' circular: limite duro
        u32 w[12];
        if (uc_mem_read(uc, qh, w, sizeof(w)) != UC_ERR_OK) break;
        // info1 bits 8-11 = endpoint; bit 15 = "Head of Reclamation"; EP 0 = control.
        const u32 ep = (w[1] >> 8) & 0xfu;
        const bool is_control = (ep == 0);
        const bool is_intr    = (ep == 1);
        if (g_usb_engine_log) {
            static u32 seen_ep[16] = {0};
            if (ep < 16u && seen_ep[ep]++ < 3u)
                std::printf("[usb-ep] qh=0x%08x visita EP%u (control=%d intr=%d) fila_hid=%zu\n",
                            qh, ep, (int)is_control, (int)is_intr, g_hid_reports.size());
        }
        // O overlay e' uma COPIA do qTD corrente. Ao completar o overlay o hardware
        // real escreve o status de volta no qTD apontado por hw_current -- e e' esse
        // qTD que o ehci_irq/qh_completions varre (lista de software). Completar so'
        // o overlay deixava o qTD do HCD Active para sempre: a IRQ 47 chegava, o
        // handler nao achava trabalho terminado e o URB estourava (-110).
        const u32 cur_qtd = w[3] & ~0x1fu;
        // Overlay do qH: hw_qtd_next em +0x10, hw_token em +0x18, hw_buf[] em +0x1c.
        // Um qTD avulso tem token em +0x08 e buffers em +0x0c, ou seja o overlay se
        // comporta como um qTD que comecasse em qh+0x10. Passar qh+0x10 direto lia o
        // token 8 bytes adiante e os buffers vinham zerados (buf0=0) -- era por isso
        // que o SETUP nunca era decodificado e o HCD via EPROTO (-71).
        if (usb_run_qtd(uc, qh + 0x10u, is_control, is_intr)) {
            any = true;
            if (cur_qtd) {                            // espelha o status no qTD real
                u32 ovtok = 0;
                if (uc_mem_read(uc, qh + 0x18u, &ovtok, 4) == UC_ERR_OK)
                    uc_mem_write(uc, cur_qtd + 0x08u, &ovtok, 4);
            }
        }
        // Quando o HCD enfileira um URB novo num qH ja' linkado, ele sobrescreve o
        // qTD "dummy" no lugar: o novo qTD e' alcancavel por hw_current (+0x0c) e nao
        // pelo overlay.next_qtd, que continua Terminate. Varrer so' o overlay fazia o
        // motor perder o GET_DESCRIPTOR de config inteiro (descritor/all -> -110).
        {
            u32 cur = w[3] & ~0x1fu;
            for (int k = 0; k < 32 && cur && !(cur & 1u); ++k) {
                if (usb_run_qtd(uc, cur, is_control, is_intr)) any = true;
                u32 nx2 = 0;
                if (uc_mem_read(uc, cur, &nx2, 4) != UC_ERR_OK) break;
                if (nx2 & 1u) break;
                cur = nx2 & ~0x1fu;
            }
        }
        u32 qtd = w[4] & ~0x1fu;                      // overlay.next_qtd
        for (int k = 0; k < 32 && qtd && !(qtd & 1u); ++k) {
            if (usb_run_qtd(uc, qtd, is_control, is_intr)) any = true;
            u32 nx = 0;
            if (uc_mem_read(uc, qtd, &nx, 4) != UC_ERR_OK) break;
            if (nx & 1u) break;                       // Terminate
            qtd = nx & ~0x1fu;
        }
        const u32 nx = w[0];
        if (nx & 1u) break;
        qh = nx & ~0x1fu;
        if (qh == list) break;                        // deu a volta
    }

    if (any) usb_raise_irq(uc);
}

// ---------------------------------------------------------------------------
// Lista assincrona do EHCI: o HCD deixa qH/qTD (48/32 bytes) na RAM do guest e o
// hardware os executa. Sem isso o kernel fica em polling logo apos "new USB bus
// registered". Aqui so' OBSERVAMOS: caminha a lista, imprime a transferencia ativa
// (endpoint, PID, bytes, buffer) e o comeco do buffer -- e' o setup packet da
// enumeracao, que diz o que emular em seguida.
// Layout do qH: 0x00 link, 0x04 endpoint characteristics, 0x08 capabilities,
// 0x0c current qTD, 0x10+ overlay (next/alt/token/buffer[5]); token: bit7 Active,
// bits 8-9 PID (0=OUT 1=IN 2=SETUP), bits 16-30 total bytes.
static bool g_usb_async_log = false;

void usb_async_poll(uc_engine* uc) {
    u32 list = g_usb_regs[(0x158u & 0x1ffu) >> 2] & ~0x1fu;   // ASYNCLISTADDR
    // Com ZEEBO_USB_ASYNC=1 imprime a estrutura crua da lista (capped): sem isso nao da'
    // para saber se o HCD chegou a submeter, se o QH tem qTD ativo e onde ele esta'.
    static int prints = 0;
    static u32 last_list = 0xffffffffu;
    static u32 calls = 0;
    // Amostra periodica (a lista muda rapido: o HCD escreve ASYNCLISTADDR antes de
    // montar os qTDs, entao imprimir so' na mudanca pegava a lista vazia).
    const bool sample = (list && ((calls++ & 7u) == 0u) && prints < 24) || (list != last_list && prints < 6);
    if (sample) {
        ++prints;
        last_list = list;
        std::printf("[usb-async] ASYNCLISTADDR=0x%08x USBCMD=0x%08x USBSTS=0x%08x\n",
                    list, g_usb_regs[(0x140u & 0x1ffu) >> 2], g_usb_regs[(0x144u & 0x1ffu) >> 2]);
        if (list) {
            u32 w[8];
            if (uc_mem_read(uc, list, w, sizeof(w)) == UC_ERR_OK)
                std::printf("[usb-async]   qh0: next=0x%08x info1=0x%08x cur=0x%08x next_qtd=0x%08x tok=0x%08x buf0=0x%08x\n",
                            w[0], w[1], w[3], w[4], w[6], w[7]);
        }
        std::fflush(stdout);
    }
    if (!list) return;
    // qTD recem-alocado (capturado no retorno de ehci_qtd_alloc em qh_urb_transaction):
    // mostra a transferencia que o HCD esta' montando. Num control transfer o primeiro
    // qTD e' o SETUP, entao o buffer dele traz o setup packet.
    static u32 last_reported = 0;
    if (g_usb_last_qtd && g_usb_last_qtd != last_reported) {
        last_reported = g_usb_last_qtd;
        u32 q[2] = {0, 0};
        if (uc_mem_read(uc, g_usb_last_qtd + 0x08u, q, sizeof(q)) == UC_ERR_OK) {
            const u32 tok = q[0], buf = q[1];
            std::printf("[usb-qtd] qTD@0x%08x (ret pc=0x%08x) tok=0x%08x active=%u pid=%u bytes=%u buf=0x%08x\n",
                        g_usb_last_qtd, g_usb_last_qtd_pc, tok, (tok >> 7) & 1u, (tok >> 8) & 3u,
                        (tok >> 16) & 0x7fffu, buf);
            unsigned char b[8] = {0};
            if (buf && uc_mem_read(uc, buf, b, sizeof(b)) == UC_ERR_OK)
                std::printf("[usb-qtd]   dados: %02x %02x %02x %02x %02x %02x %02x %02x\n",
                            b[0], b[1], b[2], b[3], b[4], b[5], b[6], b[7]);
            std::fflush(stdout);
        }
    }
    // Varredura que roda em TODA amostragem (barata: ate' 4 QHs x 2 candidatos) e procura
    // qTD ativo -- URB submetido e ainda nao completado, que e' o momento em que o
    // "hardware" (nosso modelo) teria de executar a transferencia. Nota: a qtd_list de
    // software do HCD nao e' alcancavel por ASYNCLISTADDR (que aponta para o
    // struct ehci_qh_hw, bloco DMA so' de hardware), entao o qTD em voo e' procurado no
    // overlay do QH (qh+0x10..0x1c, mesmo layout de um qTD) e no qTD corrente.
    static int act_seen = 0;
    if (act_seen < 8) {
        u32 qh = list;
        for (int n = 0; n < 4 && qh; ++n) {
            u32 w[8];
            if (uc_mem_read(uc, qh, w, sizeof(w)) != UC_ERR_OK) break;
            u32 cand[2] = { qh + 0x10u, w[3] };
            for (int c = 0; c < 2; ++c) {
                if (!cand[c]) continue;
                const u32 base = c ? (cand[c] & ~0x1fu) : cand[c];
                u32 q[8];
                if (uc_mem_read(uc, base, q, sizeof(q)) != UC_ERR_OK) continue;
                const u32 t = q[2];
                if (!((t >> 7) & 1u)) continue;
                ++act_seen;
                std::printf("[usb-async] ATIVO qh[%d]@0x%08x via %s: tok=0x%08x pid=%u bytes=%u buf0=0x%08x\n",
                            n, qh, c ? "hw_current" : "overlay", t, (t >> 8) & 3u, (t >> 16) & 0x7fffu, q[3]);
                unsigned char b[8] = {0};
                if (q[3] && uc_mem_read(uc, q[3], b, sizeof(b)) == UC_ERR_OK)
                    std::printf("[usb-async]   setup/dados: %02x %02x %02x %02x %02x %02x %02x %02x\n",
                                b[0], b[1], b[2], b[3], b[4], b[5], b[6], b[7]);
                std::fflush(stdout);
            }
            const u32 nx = w[0];
            qh = (nx & 1u) ? 0u : (nx & ~0x1fu);
        }
    }
}

// ---------------------------------------------------------------------------
// Le' o framebuffer do guest e o converte de volta para TEXTO usando a propria fonte
// 8x16 do kernel (simbolo fontdata_8x16, VA 0xc02352b8 -> PA 0x102352b8). Serve para
// conferir se o que o shell escreve no console de VT chega ao FB, e para depurar os
// dois paineis lado a lado sem depender de olhar a imagem.
// Conferido contra o System.map do #21 (c02352b8 r fontdata_8x16). Se um rebuild
// mover o simbolo, o decodificador passa a ler lixo e "some" o texto -- o guard
// abaixo (fb_check_font) grita em vez de deixar o instrumento mentir.
static const u32 FONT_PA = 0x102352b8u;

// Guard barato contra VA podre: a font 8x16 do kernel comeca com o glifo 0 (todo
// zero) seguido de bytes nao-triviais; se a janela inteira vier zerada ou 0xff, o
// endereco esta' errado. Roda uma vez, so' quando o decodificador e' usado.

// ---------------------------------------------------------------------------
// Validacao do mapeamento PC<->simbolo.
//
// ARMADILHA: rodar isto antes de uc_emu_start nao vale NADA. Nesse instante a
// memoria ainda contem o zImage *comprimido*; o texto do kernel so' existe depois
// que o decompressor roda e salta para a entry em 0xc0008000. O check antigo
// comparava contra lixo comprimido e "passava" sem significar nada.
//
// Os VAs vem do System.map do kernel em uso e APODRECEM a cada rebuild:
//   grep -E ' (handshake|ehci_qtd_alloc|qh_urb_transaction|cfb_imageblit)$' System.map
// Os bytes esperados vem do proprio vmlinux:
//   arm-linux-gnueabi-objdump -s -j .text --start-address=0xVA --stop-address=0xVA+16 vmlinux

static void pc_check_run(uc_engine* uc) {
    if (g_pc_check_done) return;
    g_pc_check_done = true;

    struct Sym { const char* nome; u32 va; u32 w0; };
    // primeira palavra (little-endian) de cada funcao, conferida no vmlinux do #21
    static const Sym syms[] = {
        {"handshake",          0xc01893dcu, 0xe92d45f8u},
        {"ehci_qtd_alloc",     0xc018e91cu, 0xe92d4030u},
        {"qh_urb_transaction", 0xc018ed88u, 0xe92d4ff0u},
        {"cfb_imageblit",      0xc012b8fcu, 0xe92d4ff0u},
    };

    int ok = 0, bad = 0;
    for (const Sym& s : syms) {
        const u32 pa = s.va - 0xc0000000u + 0x10000000u;
        u32 got = 0;
        const uc_err e = uc_mem_read(uc, pa, &got, sizeof(got));
        const bool hit = (e == UC_ERR_OK) && (got == s.w0);
        hit ? ++ok : ++bad;
        std::printf("[pc-check] %-20s VA=0x%08x PA=0x%08x esperado=%08x lido=%08x %s\n",
                    s.nome, s.va, pa, s.w0, got,
                    hit ? "OK" : (e == UC_ERR_OK ? "DIVERGE" : "ERRO DE LEITURA"));
    }
    std::printf("[pc-check] %d OK, %d divergencia(s)%s\n", ok, bad,
                bad ? "  -- VA podre (rebuild?) ou kernel ainda comprimido" : "");
    if (bad == 0)
        std::printf("[pc-check] mapeamento PC<->simbolo confiavel: "
                    "instrumento por PC vale para este kernel.\n");
    std::fflush(stdout);
}


// Sonda: so' roda o check quando o texto descomprimido ja' esta' no lugar. Assim o
// resultado nao depende de acertar um PC de latch, e nunca compara contra o zImage
// ainda compactado (foi exatamente esse o defeito do instrumento antigo).
static void pc_check_poll(uc_engine* uc) {
    // O decompressor escreve o texto em ordem crescente de endereco, entao um unico
    // simbolo-sentinela pega o kernel a meio caminho (medido: handshake ja' correto
    // enquanto qh_urb_transaction ainda era lixo). Exigimos que TODOS os simbolos
    // estejam no lugar antes de reportar -- so' ai' a descompressao terminou.
    static const struct { u32 va; u32 w0; } sentinelas[] = {
        {0xc012b8fcu, 0xe92d4ff0u},   // cfb_imageblit
        {0xc01893dcu, 0xe92d45f8u},   // handshake
        {0xc018e91cu, 0xe92d4030u},   // ehci_qtd_alloc
        {0xc018ed88u, 0xe92d4ff0u},   // qh_urb_transaction  (o mais alto)
    };
    for (const auto& s : sentinelas) {
        u32 probe = 0;
        if (uc_mem_read(uc, s.va - 0xc0000000u + 0x10000000u, &probe, 4) != UC_ERR_OK)
            return;
        if (probe != s.w0) return;    // ainda comprimido / descompressao em curso
    }
    pc_check_run(uc);
}
static bool fb_font_looks_sane(uc_engine* uc) {
    u8 probe[256] = {0};
    if (uc_mem_read(uc, FONT_PA, probe, sizeof(probe)) != UC_ERR_OK) return false;
    unsigned zero = 0, ff = 0;
    for (unsigned char b : probe) { if (b == 0x00) ++zero; if (b == 0xff) ++ff; }
    return zero < sizeof(probe) && ff < sizeof(probe);
}

void fb_decode_text(uc_engine* uc) {
    static bool font_checked = false;
    if (!font_checked) {
        font_checked = true;
        if (!fb_font_looks_sane(uc))
            std::printf("[fb-text] AVISO: fontdata_8x16 em 0x%08x nao parece uma font "
                        "(VA mudou no rebuild? conferir no System.map)\n", FONT_PA);
    }
    const u32 half_bytes = (u32)FB_XRES * FB_YRES * 2u;      // 691200
    std::vector<u8> buf(half_bytes * 2u);
    if (uc_mem_read(uc, FB_PA, buf.data(), buf.size()) != UC_ERR_OK) return;
    size_t ink[2] = {0, 0};
    for (int h = 0; h < 2; ++h) {
        const u8* p = buf.data() + (size_t)h * half_bytes;
        for (u32 i = 0; i + 1 < half_bytes; i += 2) if (p[i] | p[i + 1]) ++ink[h];
    }
    const int half = (ink[1] > ink[0]) ? 1 : 0;
    const u8* fb = buf.data() + (size_t)half * half_bytes;
    std::vector<u8> font(4096);
    if (uc_mem_read(uc, FONT_PA, font.data(), font.size()) != UC_ERR_OK) return;
    const int cols = FB_XRES / 8, rows = FB_YRES / 16;
    // Compara as duas ordens de bits (o dumper do fbcon pode escrever com bit7 ou bit0
    // a' esquerda) e usa a que casar mais celulas com a fonte.
    auto decode = [&](bool msb_left, std::vector<std::string>& out) -> int {
        out.assign(rows, std::string(cols, ' '));
        int matched = 0;
        for (int r = 0; r < rows; ++r) {
            for (int c = 0; c < cols; ++c) {
                u8 pat[16];
                bool blank = true;
                for (int y = 0; y < 16; ++y) {
                    u8 b = 0;
                    for (int x = 0; x < 8; ++x) {
                        const u32 off = (((u32)(r * 16 + y) * FB_XRES) + (u32)(c * 8 + x)) * 2u;
                        const u16 px = (u16)(fb[off] | (fb[off + 1] << 8));
                        if (px) { b |= msb_left ? (u8)(0x80u >> x) : (u8)(1u << x); blank = false; }
                    }
                    pat[y] = b;
                }
                if (blank) continue;
                for (int g = 0; g < 256; ++g) {
                    if (std::memcmp(pat, &font[(size_t)g * 16], 16) == 0) {
                        out[r][c] = (g >= 32 && g < 127) ? (char)g : '.';
                        ++matched;
                        break;
                    }
                }
            }
        }
        return matched;
    };
    std::vector<std::string> txt_a, txt_b;
    const int na = decode(true, txt_a);
    const int nb = decode(false, txt_b);
    const std::vector<std::string>& txt = (nb > na) ? txt_b : txt_a;
    // Guarda o que foi decodificado: e' o sinal de "shell pronta" para o gatilho de
    // digitacao (o prompt vive no VT, nao no console capturado pela UART).
    g_fb_lines = txt;
    std::printf("[fb-text] metade=%d ink=%zu/%zu %dx%d celulas; casou bit7=%d bit0=%d -> usando %s\n",
                half, ink[0], ink[1], cols, rows, na, nb, (nb > na) ? "bit0-esquerda" : "bit7-esquerda");
    int last = -1;
    for (int r = 0; r < rows; ++r)
        if (txt[r].find_first_not_of(' ') != std::string::npos) last = r;
    const int first = (last > 24) ? last - 24 : 0;
    for (int r = first; r <= last; ++r) {
        std::string l = txt[r];
        while (!l.empty() && l.back() == ' ') l.pop_back();
        if (!l.empty()) std::printf("[fb-text] %2d|%s\n", r, l.c_str());
    }
    std::fflush(stdout);
}

int main(int argc, char** argv) {
    if (argc > 1 && std::string(argv[1]) == "selftest") return run_selftest();
    if (std::getenv("ZEEBO_VIC_TEST")) return vic_test() == 0 ? 0 : 1;
    // A tabela de teclas nao depende de kernel nenhum: tem que ser decidida AQUI,
    // antes do SKIP(77) da imagem -- antes ela ficava depois e o alvo do Makefile
    // nem chegava a executa-la sem ZEEBO_KERNEL.
    if (std::getenv("ZEEBO_KEY_TEST")) return key_test() == 0 ? 0 : 1;
    if (std::getenv("ZEEBO_VEC_TEST")) vec_test();

    std::printf("=== Test boot de kernel Linux no MSM7201A (Zeebo) ===\n");

    const char* env = std::getenv("ZEEBO_KERNEL");
    std::string path = env ? env : "images/zImage";
    auto img = read_file(path);
    if (img.empty()) {
        std::printf("SKIP (exit 77): nenhuma imagem de kernel em '%s'.\n", path.c_str());
        std::printf("  Defina ZEEBO_KERNEL=<caminho> ou coloque um zImage em images/.\n");
        return 77;
    }

    Verdict v;
    auto k = identify(img);
    v.kind = k.label();
    v.l1 = (k.kind != KernelImage::None);
    std::printf("[img] %s: %zu bytes, tipo=%s\n", path.c_str(), img.size(), v.kind.c_str());
    if (!v.l1) {
        std::printf("FAIL: imagem nao reconhecida como kernel ARM.\n");
        report(v);
        return 1;
    }

    uc_engine* uc = nullptr;
    if (uc_open(UC_ARCH_ARM, UC_MODE_ARM, &uc) != UC_ERR_OK) {
        std::printf("FAIL: uc_open\n"); return 1;
    }
    // MODELO DE CPU: sem isto o Unicorn usa o default (Cortex-A15, ARMv7),
    // e o kernel aborta com "unrecognized/unsupported processor variant
    // (0x412fc0f1)". O Zeebo e ARM1136 -- ver notes/ARM_CPU_WAS_WRONG.md.
    uc_ctl_set_cpu_model(uc, UC_CPU_ARM_1136);
    uc_mem_map(uc, APPS_RAM_PHYS, APPS_RAM_SIZE, UC_PROT_ALL);
    // Mapear tambem o espelho virtual de memoria linear do kernel:
    // PAGE_OFFSET = 0xc0000000, mapeando 64MB (0xc0000000 ate 0xc4000000)
    // O Unicorn host uc_mem_read/write requer mapeamento para VAs quando a MMU do Unicorn
    // nao estiver fazendo walk completo no modo interpretado para o host API.
    // uc_mem_map(uc, 0x00000000u, 0x10000000u, UC_PROT_ALL); // 256MB user space
    // (espelho de 96MB em 0xc0000000 removido -- ver comentario do GPT/CSR abaixo)
    // Mas o espaco de PERIFERICOS fisicos 0xC0000000+ precisa de memoria de host:
    // o VIC fica em PA 0xC0000000 (VA 0xE0000000) e o GPT/DGT em PA 0xC0100000.
    uc_mem_map(uc, PERIPH_BASE, PERIPH_SIZE, UC_PROT_ALL);
    uc_mem_map(uc, 0x9c000000u, 0x100000u, UC_PROT_ALL); // ioremap virtual region
    uc_mem_map(uc, 0xffff0000u, 0x10000u, UC_PROT_ALL); // High vectors page (64KB)
    // MDP/TVENC: o driver de framebuffer escreve nos registradores do MDP. Sem
    // este mapeamento o acesso vira UC_ERR_MAP e mata o boot (visto em
    // 0xaa200060). O espaco de FB memoria fica em PA 0x15000000 (dentro da RAM
    // mapeada de 96MB, fora dos 64MB que o kernel gerencia).
    uc_mem_map(uc, MDP_BASE, MDP_SIZE, UC_PROT_ALL);
    // USB HS: o OTG/EHCI ioremappam este bloco e leem os registradores; sem o
    // mapeamento o acesso vira UC_ERR_MAP e mata o boot (visto no MDP tambem).
    uc_mem_map(uc, 0xa0800000u, 0x1000u, UC_PROT_ALL);
    g_usb_log = (std::getenv("ZEEBO_USB_LOG") != nullptr);
    g_usb_engine_log = (std::getenv("ZEEBO_USB_ENGINE_LOG") != nullptr);
    g_usb_async_log = (std::getenv("ZEEBO_USB_ASYNC") != nullptr);
    uc_hook hu_w = 0, hu_r = 0;
    uc_hook_add(uc, &hu_w, UC_HOOK_MEM_WRITE, (void*)on_usb_write, nullptr, 0xa0800000u, 0xa0801000u);
    uc_hook_add(uc, &hu_r, UC_HOOK_MEM_READ,  (void*)on_usb_read,  nullptr, 0xa0800000u, 0xa0801000u);
    g_mdp_log = (std::getenv("ZEEBO_MDP_LOG") != nullptr);
    uc_hook hmw = 0, hmr = 0;
    uc_hook_add(uc, &hmw, UC_HOOK_MEM_WRITE, (void*)on_mdp_write, nullptr, MDP_BASE, MDP_BASE + MDP_SIZE);
    uc_hook_add(uc, &hmr, UC_HOOK_MEM_READ, (void*)on_mdp_read, nullptr, MDP_BASE, MDP_BASE + MDP_SIZE);
    uc_mem_map(uc, TVENC_BASE, TVENC_SIZE, UC_PROT_ALL);
    // Diagnostico: onde o kernel escreve o framebuffer (acima da RAM dele).
    uc_hook hfp = 0;
    uc_hook_add(uc, &hfp, UC_HOOK_MEM_WRITE, (void*)on_fbprobe_write, nullptr,
                0x14000000u, 0x16000000u);
    // NAO mapear espelho de 0xc0000000+: os hooks do Unicorn reportam o endereco
    // FISICO dos acessos do guest, e 0xC0100000 (PA do GPT) cai exatamente ali --
    // mapear/hookar esse espaco corromperia RAM do kernel. O dump do log do kernel
    // nao depende dele: guest_read_bytes percorre as tabelas de pagina na mao.
    // GPT/DGT em PA 0xC0100000 (VA 0xE0001000 no iotable). O hook ve o PA.
    uc_mem_map(uc, CSR_PA, CSR_SIZE, UC_PROT_ALL);
    uc_mem_map(uc, CSR_BASE, CSR_SIZE, UC_PROT_ALL);
    g_timer_log = (std::getenv("ZEEBO_TIMER_LOG") != nullptr);
    // O clockevent virtual fica LIGADO por padrao: sem ele o fbcon/hrtimer trava o
    // boot (jiffies nao avancam) e o "sleep" nunca retorna. ZEEBO_NOTIMER=1 desliga.
    g_timer_on  = (std::getenv("ZEEBO_NOTIMER") == nullptr);
    uc_hook hcw = 0, hcr = 0;
    uc_hook_add(uc, &hcw, UC_HOOK_MEM_WRITE, (void*)on_csr_write, nullptr, CSR_PA, CSR_PA + CSR_SIZE);
    uc_hook_add(uc, &hcr, UC_HOOK_MEM_READ, (void*)on_csr_read, nullptr, CSR_PA, CSR_PA + CSR_SIZE);
    for (u32 ub : UART_BASES) uc_mem_map(uc, ub, UART_SIZE, UC_PROT_ALL);
    uc_mem_map(uc, VIC_BASE, VIC_SIZE, UC_PROT_ALL);
    // MSM MMIO ranges: atencao: UART1_BASE = 0xa9a00000 (size 4KB) ja foi mapeada acima.
    // Nao sobrepor com UART1_BASE.
    uc_err err;
    err = uc_mem_map(uc, 0xa9200000u, 0x800000u, UC_PROT_ALL); // 0xa9200000 ate 0xa9a00000 (GPIOs, etc)
    if (err) std::printf("uc_mem_map 0xa9200000 failed: %d\n", err);
    err = uc_mem_map(uc, 0xa9a10000u, 0x5f0000u, UC_PROT_ALL); // acima da UART1
    if (err) std::printf("uc_mem_map 0xa9a10000 failed: %d\n", err);
    err = uc_mem_map(uc, 0xa8000000u, 0x1000000u, UC_PROT_ALL); // 0xa8000000 - 0xa9000000 (CLK_CTL, etc)
    if (err) std::printf("uc_mem_map 0xa8000000 failed: %d\n", err);
    err = uc_mem_map(uc, 0x01f00000u, 0x100000u, UC_PROT_ALL);  // SMEM (Shared RAM 1MB)
    if (err) std::printf("uc_mem_map 0x01f00000 failed: %d\n", err);
    // Inicializar proc_comm e SMEM Shared Structure
    // struct smem_shared {
    //   proc_comm[4] (4 * 16 bytes = 64 bytes = 0x40)
    //   version[32]  (32 * 4 = 128 bytes = 0x80)
    //   heap_info    (4 * 4 = 16 bytes = 0x10)
    //   heap_toc[512] (512 * 16 bytes = 8192 bytes = 0x2000)
    // };
    // SMEM_SMSM_SHARED_STATE = 0.
    // Offset do heap_toc = 64 + 128 + 16 = 208 = 0xd0.
    // heap_toc[0]: allocated = 1, offset = 0x4000, size = SMSM_V1_SIZE (32 bytes).
    u32 pcom_ready = 1;
    uc_mem_write(uc, 0x01f00000u + 0x14, &pcom_ready, 4);

    // Inicia smem_shared na base 0x01f00000:
    // r3 em c00141bc aponta para [0xe0100600] que e 0x01f00600 fisico!
    // 0xe0100600 = MSM_SHARED_RAM_BASE + 0x600.
    // Ele le r3+32 (offset 0x620), r3+36 (offset 0x624), r3+40 (offset 0x628).
    // cmp r3, #0 -> se [0x01f00620] == 0 pula para c00142b0 (loop infinito: b c00142b0)!
    // E compara r4 (size) com 16 ou 32: sub r4, #16; bic r4, #16; cmp r4, #0!
    // Entao [0x01f00620] precisa ser != 0 (ponteiro), e [0x01f00628] (size) precisa ser 16 ou 32!
    // E [0x01f00624] (offset) e o endereco relativo.
    u32 heap_entry_ptr = 0x01f04000u; // endereco do estado
    u32 heap_entry_off = 0x4000u;      // offset
    u32 heap_entry_sz  = 32u;          // SMSM_V1_SIZE (32)
    uc_mem_write(uc, 0x01f00000u + 0x620, &heap_entry_ptr, 4);
    uc_mem_write(uc, 0x01f00000u + 0x624, &heap_entry_off, 4);
    uc_mem_write(uc, 0x01f00000u + 0x628, &heap_entry_sz, 4);

    // Inicializar estado do modem em smsm state: SMSM_INIT | SMSM_SMDINIT | SMSM_RPCINIT | SMSM_RUN
    // state[1] e o estado do MODEM
    u32 modem_state[8] = {0};
    modem_state[1] = 0x00000001 | 0x00000008 | 0x00000020 | 0x00000100;
    uc_mem_write(uc, 0x01f00000u + 0x4000, modem_state, sizeof(modem_state));
    uc_mem_write(uc, 0xe0100000u + 0x14, &pcom_ready, 4);
    uc_mem_write(uc, 0xe0100000u + 0x620, &heap_entry_ptr, 4);
    uc_mem_write(uc, 0xe0100000u + 0x624, &heap_entry_off, 4);
    uc_mem_write(uc, 0xe0100000u + 0x628, &heap_entry_sz, 4);
    uc_mem_write(uc, 0xe0100000u + 0x4000, modem_state, sizeof(modem_state));
    err = uc_mem_map(uc, 0xa0000000u, 0x1000000u, UC_PROT_ALL); // 0xa0000000 - 0xa1000000 (HSUSB, SDC, etc)
    if (err) std::printf("uc_mem_map 0xa0000000 failed: %d\n", err);
    err = uc_mem_map(uc, 0xe0000000u, 0x1000000u, UC_PROT_ALL); // MSM virtual IO aliases se acessados diretos
    if (err) std::printf("uc_mem_map 0xe0000000 failed: %d\n", err);

    uc_hook hw = 0, hr = 0, hc = 0;
    g_uart_log = (std::getenv("ZEEBO_UART_LOG") != nullptr);   // traco dos acessos a UART
    // Entrada de teclado -> RX da UART: pipe/arquivo entra de uma vez; tty usa select.
    if (!isatty(0)) {
        char c = 0;
        while (read(0, &c, 1) == 1) g_rx_buf.push_back(c);
        if (!g_rx_buf.empty())
            std::printf("[rx] %zu bytes de entrada do host na fila da UART\n", g_rx_buf.size());
    } else {
        g_stdin_tty = true;
    }
    g_irq_log  = (std::getenv("ZEEBO_IRQ_LOG") != nullptr);    // traco de VIC/IRQ
#if defined(ZEEBO_SDL)
    if (std::getenv("ZEEBO_SDL")) sdl_open();                  // janela Wayland do console
#endif
    // VIC: modelo dos registradores usados pelo entry-macro e pelo irq_chip.
    uc_hook hvw = 0, hvr = 0;
    uc_hook_add(uc, &hvw, UC_HOOK_MEM_WRITE, (void*)on_vic_write, nullptr,
                VIC_BASE, VIC_BASE + VIC_SIZE);
    uc_hook_add(uc, &hvr, UC_HOOK_MEM_READ, (void*)on_vic_read, nullptr,
                VIC_BASE, VIC_BASE + VIC_SIZE);
    // UART1 (0xa9a00000), UART2/ttyMSM2 (0xa9c00000) e UART3 (0xa9e00000):
    // o console do kernel pode cair em qualquer uma delas.
    for (u32 ub : UART_BASES) {
        uc_hook_add(uc, &hw, UC_HOOK_MEM_WRITE, (void*)on_uart_write, nullptr,
                    ub, ub + UART_SIZE);
        uc_hook_add(uc, &hr, UC_HOOK_MEM_READ, (void*)on_uart_read, nullptr,
                    ub, ub + UART_SIZE);
    }
    uc_hook hs1 = 0, hs2 = 0;
    uc_hook_add(uc, &hs1, UC_HOOK_MEM_WRITE, (void*)on_smem_write, nullptr,
                0x01f00000u, 0x01f00000u + 0x100);
    uc_hook_add(uc, &hs2, UC_HOOK_MEM_WRITE, (void*)on_smem_write, nullptr,
                0xe0100000u, 0xe0100000u + 0x100);
    uc_hook_add(uc, &hc, UC_HOOK_CODE, (void*)on_code, nullptr, 1, 0);
    uc_hook hm = 0;
    uc_hook_add(uc, &hm, UC_HOOK_MEM_INVALID, (void*)on_mem_invalid, nullptr, 1, 0);
    // g_probe = (std::getenv("ZEEBO_UART_PROBE") != nullptr);
    g_probe = true;
    uc_hook hp = 0;
    uc_hook_add(uc, &hp, UC_HOOK_MEM_WRITE, (void*)on_any_write, nullptr, 1, 0);
    uc_hook hpc = 0;
    uc_hook_add(uc, &hpc, UC_HOOK_CODE, (void*)on_code_probe, nullptr, 1, 0);
    uc_hook hun = 0;
    uc_hook_add(uc, &hun, UC_HOOK_MEM_FETCH_UNMAPPED | UC_HOOK_MEM_READ_UNMAPPED |
                           UC_HOOK_MEM_WRITE_UNMAPPED, (void*)on_fault_entry, nullptr, 1, 0);
    uc_hook hint = 0;
    uc_hook_add(uc, &hint, UC_HOOK_INTR, (void*)on_intr, nullptr, 1, 0);



    uc_mem_write(uc, KERNEL_LOAD, img.data(), img.size());

    // Contrato de boot ARM Linux: r0=0, r1=machine id, r2=ponteiro ATAGS/DTB.
    //
    // MACHINE ID: o proprio kernel lista o que aceita quando r1 esta errado:
    //   "ID (hex) NAME / 0000059f Halibut Board (QCT SURF7200A)"
    // 0x59f = MACH_TYPE_HALIBUT, a mesma board do log de boot do TripleOxygen
    // ("Machine: Zeebo" era um board custom derivado do halibut).
    constexpr u32 MACH_TYPE_HALIBUT = 0x59fu;
    const u32 atags = APPS_RAM_PHYS + 0x100u;

    // ATAGs minimas: o kernel sem elas nao sabe quanta RAM existe.
    // Formato: cada tag = {u32 size_em_words, u32 tag_id, payload...}.
    // Valores vindos do log real: mem=64M na base fisica 0x10000000,
    // console ttyMSM0,115200n8.
    {
        std::vector<u32> t;
        // ATAG_CORE (0x54410001): flags, pagesize, rootdev
        t.push_back(5); t.push_back(0x54410001u);
        t.push_back(0); t.push_back(4096); t.push_back(0);
        // ATAG_MEM (0x54410002): size, start  -- 64MB @ 0x10000000
        t.push_back(4); t.push_back(0x54410002u);
        t.push_back(64u * 1024u * 1024u); t.push_back(APPS_RAM_PHYS);
        // ATAG_CMDLINE (0x54410009)
        // zeebo_usb=1 liga o bring-up do host controller EHCI no kernel. O caminho de
        // transferencia JA' esta' modelado (assincrono + periodico, com HID entregando
        // tecla na shell); o parametro so' existe para nao gastar budget nos alvos que
        // nao precisam de USB.
        std::string cmdline = "console=ttyMSM2,115200n8 earlyprintk=msm_serial,0xa9c00000 mem=64M lpj=2629632 init=/init";
        // [2026-09-13] EHCI fica FORA por padrao; ZEEBO_USB=1 liga. NAO existe
        // ZEEBO_NOUSB -- o default foi invertido quando a enumeracao travava em laco,
        // poluia o console e queimava budget, e o nome antigo ficou so' na lembranca.
        // Com o HID entregando tecla na shell ligar deixou de ser risco, mas segue OFF
        // porque nao e' preciso para os outros alvos: quem precisa (test-linux-hid) e'
        // que pede.
        g_usb_on = (std::getenv("ZEEBO_USB") != nullptr);
        if (std::getenv("ZEEBO_HID_TYPE")) {
            const char* at = std::getenv("ZEEBO_HID_AT");
            g_hid_type_at = at ? std::strtoull(at, nullptr, 0) : 380000000ull;
            // Com marcador explicito a digitacao espera por ele e ignora o numero.
            if (const char* w = std::getenv("ZEEBO_HID_WAIT")) g_hid_type_at = 0, g_hid_wait = w;
            if (const char* m = std::getenv("ZEEBO_HID_MARGIN")) g_hid_wait_margin = std::strtoull(m, nullptr, 0);
        }
        if (g_usb_on) cmdline += " zeebo_usb=1";
        // Epoch de RTC: o 3.4.113 do console sobe sem CONFIG_RTC_CLASS e a placa nao
        // tem RTC modelado, entao o kernel comeca em 1970 e `date` mente. Nao ha
        // hardware para emular aqui -- passamos a hora do host pela cmdline e o
        // /init aplica com `date -s`. O timer (GPT/DGT) ja' e' correto; o que
        // faltava era so' o ponto de partida. ZEEBO_EPOCH=0 desliga.
        {
            const char* ep = std::getenv("ZEEBO_EPOCH");
            const long epoch = ep ? std::atol(ep) : (long)std::time(nullptr);
            if (epoch > 0) cmdline += " zeebo_epoch=" + std::to_string(epoch);
        }
        size_t clen = cmdline.size() + 1;
        size_t cwords = (clen + 3) / 4;
        t.push_back(static_cast<u32>(2 + cwords)); t.push_back(0x54410009u);
        size_t base = t.size();
        t.resize(base + cwords, 0);
        std::memcpy(&t[base], cmdline.c_str(), clen);
        // ATAG_NONE (0x00000000) encerra a lista
        t.push_back(0); t.push_back(0);
        uc_mem_write(uc, atags, t.data(), t.size() * sizeof(u32));
    }

    u32 r0 = 0, r1 = MACH_TYPE_HALIBUT, r2 = atags, sp = APPS_RAM_PHYS + 0x4000000u;
    uc_reg_write(uc, UC_ARM_REG_R0, &r0);
    uc_reg_write(uc, UC_ARM_REG_R1, &r1);
    uc_reg_write(uc, UC_ARM_REG_R2, &r2);
    uc_reg_write(uc, UC_ARM_REG_SP, &sp);

    std::printf("[boot] carregado em 0x%08x, entry=0x%08x, budget=%llu insn\n",
                KERNEL_LOAD, KERNEL_LOAD, (unsigned long long)kInsnBudget);
    // Validacao do mapeamento PC<->simbolo: le' o texto do kernel direto da memoria do
    // guest (VA 0xc0xxxxxx -> PA = VA - 0xc0000000 + 0x10000000) para comparar com o
    // objdump do vmlinux do container. Se nao bater, instrumento por PC nao vale.
    g_pc_check_on = (std::getenv("ZEEBO_PC_CHECK") != nullptr);
    // (o pc-check real dispara no hook, na entry descomprimida -- ver pc_check_run())

    uc_err e = uc_emu_start(uc, KERNEL_LOAD, 0, 0, kInsnBudget);
    std::printf("[boot] parou: %s (%d) apos %llu instrucoes, last_pc=0x%08x\n",
                uc_strerror(e), (int)e, (unsigned long long)g_insn, g_last_pc);
    v.insn = g_insn;
    dump_kernel_log(uc, "fim da execucao");
#if defined(ZEEBO_SDL)
    sdl_fb_dump(uc);          // memoria de FB do guest -> /tmp/zeebo_fb.bmp
    if (std::getenv("ZEEBO_FB_TEXT")) fb_decode_text(uc);   // estado final do FB, em texto
    sdl_shot_save();          // prova visual do que a janela desenhou
#endif
    std::printf("[fbprobe] escritas na faixa do FB: %llu (nao-zero: %llu)\n",
                (unsigned long long)g_fb_writes, (unsigned long long)g_fb_writes_nz);
    std::printf("[draw] cfb_imageblit=%u fbcon_putcs=%u bit_putcs=%u fbcon_init=%u fbcon_switch=%u cfb_fillrect=%u\n",
                g_draw_cnt[0], g_draw_cnt[1], g_draw_cnt[2], g_draw_cnt[3], g_draw_cnt[4], g_draw_cnt[5]);
    {   // pseudo_palette do msm_fb (static unsigned PP[16] em msm_fb.c).
        // ATENCAO: o VA sai do System.map e MUDA a cada rebuild do kernel. Estava
        // fixo em 0xc03f45d4 (kernel antigo) e virou 0xc05b36b4 no #21 -- o valor
        // velho lia memoria qualquer e imprimia "PP = e7fddef0 x16" (um ponteiro),
        // o que parecia paleta corrompida e nao era. Conferir com:
        //   grep ' PP$' /work/k6/linux-3.4.113/System.map
        // PHYS_OFFSET 0x10000000 = PAGE_OFFSET 0xc0000000, dai o VA-0xb0000000.
        constexpr u32 kPPVirt = 0xc05b36b4u;
        u32 pp[16] = {0};
        if (uc_mem_read(uc, kPPVirt - 0xb0000000u, pp, sizeof(pp)) == UC_ERR_OK) {
            std::printf("[fb] pseudo_palette PP =");
            for (int i = 0; i < 16; ++i) std::printf(" %08x", pp[i]);
            std::printf("\n");
        }
    }
    if (g_probe) {
        std::printf("[probe] paginas de PC mais visitadas (top 8):\n");
        {
            std::vector<std::pair<u32,long>> pv(g_pc_hist.begin(), g_pc_hist.end());
            std::sort(pv.begin(), pv.end(),
                      [](auto& a, auto& b){ return a.second > b.second; });
            for (size_t i = 0; i < pv.size() && i < 8; ++i)
                std::printf("   PC 0x%08x  %ld amostras\n", pv[i].first, pv[i].second);
        }
        std::printf("[probe] paginas MMIO escritas (top 12):\n");
        std::vector<std::pair<u32,int>> v(g_hi_writes.begin(), g_hi_writes.end());
        std::sort(v.begin(), v.end(),
                  [](auto& a, auto& b){ return a.second > b.second; });
        for (size_t i = 0; i < v.size() && i < 12; ++i)
            std::printf("   0x%08x  %d escritas\n", v[i].first, v[i].second);
    }
    if (g_fault_seen) {
        const char* tn = (g_fault_type == UC_MEM_WRITE_UNMAPPED) ? "WRITE" :
                         (g_fault_type == UC_MEM_READ_UNMAPPED)  ? "READ"  : "FETCH";
        std::printf("[falha] %s em 0x%08llx (PC=0x%08x) -- regiao nao mapeada\n",
                    tn, (unsigned long long)g_fault_addr, g_fault_pc);
    }
    u32 cur_r4 = 0, cur_lr = 0, cur_sp = 0, cur_cpsr = 0, cur_r7 = 0;
    uc_reg_read(uc, UC_ARM_REG_R4, &cur_r4);
    uc_reg_read(uc, UC_ARM_REG_LR, &cur_lr);
    uc_reg_read(uc, UC_ARM_REG_SP, &cur_sp);
    uc_reg_read(uc, UC_ARM_REG_CPSR, &cur_cpsr);
    uc_reg_read(uc, UC_ARM_REG_R7, &cur_r7);
    std::printf("[boot] parou: %s (%d) apos %llu instrucoes, last_pc=0x%08x, r4=0x%08x, r7=0x%08x, lr=0x%08x, sp=0x%08x, cpsr=0x%08x\n",
                uc_strerror(e), (int)e, (unsigned long long)g_insn, g_last_pc, cur_r4, cur_r7, cur_lr, cur_sp, cur_cpsr);
    u32 high_vecs[8] = {0};
    uc_err vec_r_err = uc_mem_read(uc, 0xffff0000u, high_vecs, sizeof(high_vecs));
    std::printf("[vectors] vec_r_err=%d, 0xffff0000[0..7]: 0x%08x 0x%08x 0x%08x 0x%08x 0x%08x 0x%08x 0x%08x 0x%08x\n",
                vec_r_err, high_vecs[0], high_vecs[1], high_vecs[2], high_vecs[3],
                high_vecs[4], high_vecs[5], high_vecs[6], high_vecs[7]);

    v.l2 = (g_insn >= 100000000ULL);
    v.l3 = (g_console.find("Uncompressing Linux") != std::string::npos) ||
           (g_console.find("booting the kernel") != std::string::npos) ||
           (g_console.find("Booting Linux") != std::string::npos);

    report(v);

    if (v.all()) {
        std::printf("\n=== PASS: o kernel Linux fala pelo console. ===\n");
        uc_close(uc);
        return 0;
    }
    std::printf("\n=== FAIL (RED esperado): o kernel ainda nao chega ao console. ===\n");
    uc_close(uc);
    return 1;
}
