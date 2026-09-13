// test_weird_xv6.cpp — roda um SO bare-metal (xv6-ARM portado para o Zeebo) no modelo
// REAL do MSM7201A, nao num runner que reimplementa o MMIO.
//
// Por que isto existe: os SOs de `zeebo_weird_os` rodam em runners Python que
// REIMPLEMENTAM os perifericos (`run_payload.py` modela so' UART1 + um GPT constante).
// Payload verde ali prova o runner, nao o emulador. Aqui o SO roda contra o mesmo nucleo
// que o harness de boot de Linux usa (zeebo_msm_soc.h): UART de verdade, VIC de 2 bancos,
// o walker VA->PA pelas tabelas do PROPRIO guest e a entrada de excecao que o Unicorn nao
// faz. E' o que transforma um payload em teste do emulador.
//
// Uso:
//   ZEEBO_XV6_KERNEL=<kernel.bin> ./test_weird_xv6      # 0 = passou, 1 = RED, 77 = SKIP
//   ZEEBO_BUDGET=<instrucoes>                            # teto de emulacao
//
// O binario do SO NAO e' versionado neste repo (imagem de SO nao entra no git): passe o
// caminho por env. Sem imagem, sai 77 = SKIP e o alvo do Makefile trata.
#include <chrono>
#include <csignal>
#include <cstring>
#include <ctime>
#include <execinfo.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <ucontext.h>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include <unicorn/unicorn.h>

#include "zeebo_msm_soc.h"

using u8  = std::uint8_t;
using u32 = std::uint32_t;
using u64 = std::uint64_t;
using i64 = std::int64_t;

// --- o que o SO precisa: quantas instrucoes ja' rodaram + o teto ------------
// O contador e' o COMPARTILHADO (zeebo_msm::g_icount), o mesmo que o modelo do GPT/DGT le'
// para converter instrucoes em tempo do guest. Ter dois contadores seria divergencia certa.
using zeebo_msm::g_icount;
static u64 g_budget = 200000000ull;

// --- teto de aborts, como no harness de Linux: fault em laco e' bug, nao boot
static u32 g_abort_limit_hit = 0;

// Anel dos ultimos PCs: quando o SO "trava", o que responde a pergunta e' ONDE ele repete.
// Sem isso o veredito so' diz que consumiu o orcamento. Liga com ZEEBO_XV6_TRACE=1.
static bool g_trace = false;
static u32  g_pc_ring[16] = {0};
static u32  g_pc_pos = 0;

// Progresso periodico (ZEEBO_XV6_PROGRESS=1): imprime PC + contagem de tempo em tempo.
// Existe porque um run que NAO termina nao imprime veredito nenhum -- sem isto, "nao
// terminou" nao diz onde ele esta. Sobrevive ao kill (linha a linha com fflush).
static bool g_progress = false;
static u64  g_progress_every = 2000000ull;

// Dump de estado por sinal: quando o emulador trava SEM executar instrucao nao ha' hook
// nem progresso para consultar (e o gdb nao anexa sem ptrace_scope). Com isto, um SIGTERM
// imprime PC/contagem/anel no instante exato -- que e' o que diz ONDE ele esta'.
static uc_engine* g_uc_global = nullptr;
static u32 g_kpt_mem_addr = 0;   // endereco do kpt_mem do guest (vem do .nm, para o dump)
static int g_hb_ms = 0;   // ZEEBO_XV6_HB=<ms> liga o batimento
// Caminha a tabela do guest e PROCURA CICLO. O host backtrace mostrou o emulador girando
// em get_phys_addr_arm/tb_htable_lookup (pagina de CODIGO): walk que nao termina e' o
// sintoma de descritor ciclico/espurio. Aqui eu leio os descritores e denuncio o ciclo.
static void dump_walk(uc_engine* uc, u32 va) {
    uc_arm_cp_reg r0 = {15, 0, 0, 2, 0, 0, 0, 0};
    uc_arm_cp_reg r1 = {15, 0, 0, 2, 0, 0, 1, 0};
    uc_arm_cp_reg rc = {15, 0, 0, 2, 0, 0, 2, 0};
    if (uc_reg_read(uc, UC_ARM_REG_CP_REG, &r0) != UC_ERR_OK) return;
    uc_reg_read(uc, UC_ARM_REG_CP_REG, &r1);
    uc_reg_read(uc, UC_ARM_REG_CP_REG, &rc);
    u32 ttbr0 = (u32)r0.val, ttbr1 = (u32)r1.val, ttbcr = (u32)rc.val;
    u32 n = ttbcr & 7u;
    u32 split = (n == 0u) ? 0u : (0x80000000u >> (n - 1u));
    u32 pgdb = (((n == 0u) || (va < split)) ? ttbr0 : ttbr1) & 0xffffc000u;
    std::printf("[WALK] va=0x%08x ttbr0=0x%08x ttbr1=0x%08x ttbcr=0x%08x pgdb=0x%08x\n",
                va, ttbr0, ttbr1, ttbcr, pgdb);
    // Lista as ENTRADAS PRESENTES da L1: e' o mapa da tabela, e diz na hora se o kernel
    // (VA >= 0x10000000, indice >= 257) esta' mapeado no pgdir que esta' ativo.
    {
        u32 presentes = 0, mostrados = 0;
        for (u32 i = 0; i < 4096u; ++i) {
            u32 e = 0;
            if (uc_mem_read(uc, pgdb + i * 4u, &e, 4) != UC_ERR_OK) break;
            if ((e & 3u) == 0u) continue;
            ++presentes;
            if (mostrados < 10u) {
                std::printf("[WALK] L1[%u] (VA 0x%08x+) = 0x%08x tipo=%u\n",
                            i, i << 20, e, e & 3u);
                ++mostrados;
            }
        }
        std::printf("[WALK] entradas presentes na L1: %u de 4096\n", presentes);
    }
    u32 l1addr = pgdb + ((va >> 20) & 0xfffu) * 4u;
    u32 l1 = 0;
    if (uc_mem_read(uc, l1addr, &l1, 4) != UC_ERR_OK) { std::printf("[WALK] l1 ilegivel\n"); return; }
    std::printf("[WALK] L1@0x%08x = 0x%08x (tipo=%u)\n", l1addr, l1, l1 & 3u);
    if ((l1 & 3u) != 1u) return;
    u32 l2 = l1 & 0xfffffc00u;
    u32 pte = 0;
    u32 pteaddr = l2 + ((va >> 12) & 0xffu) * 4u;
    if (uc_mem_read(uc, pteaddr, &pte, 4) != UC_ERR_OK) { std::printf("[WALK] L2 ilegivel\n"); return; }
    std::printf("[WALK] L2@0x%08x PTE@0x%08x = 0x%08x (tipo=%u)\n", l2, pteaddr, pte, pte & 3u);
    // CICLO: algum descritor da L2 apontando de volta para a L1 ou para ela mesma?
    u32 selfhits = 0, l1hits = 0;
    for (u32 i = 0; i < 256u; ++i) {
        u32 e = 0;
        if (uc_mem_read(uc, l2 + i * 4u, &e, 4) != UC_ERR_OK) break;
        if ((e & 3u) == 1u) {
            if ((e & 0xfffffc00u) == l2) { if (selfhits < 3u) std::printf("[WALK] ** CICLO: L2[%u]=0x%08x aponta para SI mesma\n", i, e); ++selfhits; }
            if ((e & 0xfffffc00u) == pgdb) { if (l1hits < 3u) std::printf("[WALK] ** CICLO: L2[%u]=0x%08x aponta para a L1\n", i, e); ++l1hits; }
        }
    }
    std::printf("[WALK] resumo: auto-referencias=%u referencias-a-L1=%u\n", selfhits, l1hits);
}

// Batimento independente do progresso do guest: se o contador esta' congelado, o hb mostra;
// se o hb mostra avancar, quem mente e' o instrumento de progresso. Um contador parado com o
// host queimando CPU significa laco DENTRO do motor, e nao trabalho lento.
static void on_heartbeat(int sig) {
    (void)sig;
    u32 pc = 0;
    if (g_uc_global) uc_reg_read(g_uc_global, UC_ARM_REG_PC, &pc);
    char b[128];
    int n = std::snprintf(b, sizeof(b), "[hb] insn=%llu pc=0x%08x\n",
                          (unsigned long long)g_icount, pc);
    if (n > 0) std::fwrite(b, 1, (size_t)n, stdout);
    std::fflush(stdout);
}

// Caminha com um pgdb EXPLICITO e mostra o PA mapeado + os primeiros bytes. Serve para
// comparar a visao do processo e a do kernel no MESMO VA (o port põe vetores e codigo de
// usuario em VA 0; se as duas visoes diferem, o vetor do SVC cai em codigo de usuario).
static void dump_va(uc_engine* uc, u32 pgdb, u32 va, const char* tag) {
    u32 l1 = 0, pte = 0, pa = 0, w = 0;
    if (uc_mem_read(uc, pgdb + ((va >> 20) & 0xfffu) * 4u, &l1, 4) != UC_ERR_OK) {
        std::printf("[VA %s] va=0x%08x l1 ilegivel\n", tag, va); return;
    }
    if ((l1 & 3u) == 2u) {   // SECAO de 1 MB direto no L1: a traducao existe, e' so' ler
        const u32 sec_pa = (l1 & 0xfff00000u) | (va & 0xfffffu);
        std::printf("[VA %s] va=0x%08x SECAO -> pa=0x%08x palavras:", tag, va, sec_pa);
        for (u32 k = 0; k < 4u; ++k) {
            u32 w2 = 0;
            if (uc_mem_read(uc, sec_pa + k * 4u, &w2, 4) == UC_ERR_OK) std::printf(" %08x", w2);
        }
        std::printf("%s\n", (sec_pa == (va & 0xfffff000u) || 1) ? "" : "");
        return;
    }
    if ((l1 & 3u) != 1u) { std::printf("[VA %s] va=0x%08x l1=0x%08x (sem traducao)\n", tag, va, l1); return; }
    u32 l2 = l1 & 0xfffffc00u;
    if (uc_mem_read(uc, l2 + ((va >> 12) & 0xffu) * 4u, &pte, 4) != UC_ERR_OK) {
        std::printf("[VA %s] va=0x%08x l2 ilegivel\n", tag, va); return;
    }
    if ((pte & 3u) == 0u) { std::printf("[VA %s] va=0x%08x l2@0x%08x PTE=0x%08x => AUSENTE\n", tag, va, l2, pte); return; }
    // ATENCAO ao contexto: em L1, tipo 2 = SECAO (1 MB); em L2, tipo 2 = PAGINA PEQUENA
    // (4 KB). Aqui o descritor veio de uma L2, entao vale a formula de pagina -- foi um erro
    // meu tratar como secao e ler 0x14f00000 em vez de 0x14ff1000.
    pa = (pte & 0xfffff000u) | (va & 0xfffu);
    std::printf("[VA %s] va=0x%08x pgdb=0x%08x PTE=0x%08x pa=0x%08x palavras:",
                tag, va, pgdb, pte, pa);
    for (u32 k = 0; k < 4u; ++k) {
        u32 w2 = 0;
        if (uc_mem_read(uc, pa + k * 4u, &w2, 4) == UC_ERR_OK) std::printf(" %08x", w2);
    }
    std::printf("\n");
    (void)w;
}

static void on_signal_dump(int sig, siginfo_t* si, void* uctx) {
    (void)sig; (void)si;
    // O PC do GUEST diz onde o guest esta'. O PC do HOST diz onde o EMULADOR esta' -- e e' esse
    // que importa quando o contador de instrucoes para: gira dentro de uma chamada do motor.
    void* host_pc = nullptr;
    if (uctx) {
        ucontext_t* c = (ucontext_t*)uctx;
        host_pc = (void*)(uintptr_t)c->uc_mcontext.gregs[REG_RIP];
    }
    {
        char b[160];
        int n = std::snprintf(b, sizeof(b), "[HOST] pc=%p\n", host_pc);
        if (n > 0) std::fwrite(b, 1, (size_t)n, stdout);
    }
    {
        void* bt[20];
        int n = backtrace(bt, 20);
        backtrace_symbols_fd(bt, n, 1);
    }
    std::fflush(stdout);
    u32 pc = 0, cpsr = 0;
    if (g_uc_global) {
        uc_reg_read(g_uc_global, UC_ARM_REG_PC, &pc);
        uc_reg_read(g_uc_global, UC_ARM_REG_CPSR, &cpsr);
    }
    char buf[256];
    int n = std::snprintf(buf, sizeof(buf),
                          "\n[SINAL] insn=%llu pc=0x%08x cpsr=0x%08x ult=",
                          (unsigned long long)g_icount, pc, cpsr);
    if (n > 0) { std::fwrite(buf, 1, (size_t)n, stdout); }
    for (u32 k = 0; k < 8u; ++k) {
        std::snprintf(buf, sizeof(buf), "%08x ", g_pc_ring[(g_pc_pos - 8u + k) & 15u]);
        std::fwrite(buf, 1, 9, stdout);
    }
    std::fwrite("\n", 1, 1, stdout);
    if (g_uc_global) {
        // Registradores no ponto de parada: a instrucao presa e' `ldr r2,[r3]` (kpt_alloc), entao
        // o que importa e' r3 -- o topo do freelist do pool de tabelas de pagina.
        {
            const char* nomes[] = {"r0", "r1", "r2", "r3", "r4", "r5", "sp", "lr"};
            const int regs[] = {UC_ARM_REG_R0, UC_ARM_REG_R1, UC_ARM_REG_R2, UC_ARM_REG_R3,
                                UC_ARM_REG_R4, UC_ARM_REG_R5, UC_ARM_REG_SP, UC_ARM_REG_LR};
            std::printf("[REGS]");
            for (int i = 0; i < 8; ++i) {
                u32 v = 0;
                if (uc_reg_read(g_uc_global, regs[i], &v) == UC_ERR_OK)
                    std::printf(" %s=0x%08x", nomes[i], v);
            }
            std::printf("\n");
            if (g_kpt_mem_addr != 0u) {
                u32 w0 = 0, w4 = 0, w8 = 0;
                uc_mem_read(g_uc_global, g_kpt_mem_addr + 0u, &w0, 4);
                uc_mem_read(g_uc_global, g_kpt_mem_addr + 4u, &w4, 4);
                uc_mem_read(g_uc_global, g_kpt_mem_addr + 8u, &w8, 4);
                std::printf("[kpt_mem @0x%08x] +0=0x%08x +4=0x%08x +8=0x%08x (freelist deveria ser "
                            "endereco de PAGINA)\n", g_kpt_mem_addr, w0, w4, w8);
            }
        }
        uc_arm_cp_reg q0 = {15, 0, 0, 2, 0, 0, 0, 0};
        uc_arm_cp_reg q1 = {15, 0, 0, 2, 0, 0, 1, 0};
        uc_reg_read(g_uc_global, UC_ARM_REG_CP_REG, &q0);
        uc_reg_read(g_uc_global, UC_ARM_REG_CP_REG, &q1);
        u32 procdb = (u32)q0.val & ~0x3fffu;    // L1 exige base 16 KB-alinhada
        u32 kerndb = (u32)q1.val & ~0x3fffu;
        std::printf("[PGDB] processo(TTBR0)=0x%08x kernel(TTBR1)=0x%08x\n", procdb, kerndb);
        // SCTLR: bit 0 = M (MMU ligada). Se estiver em 0, a execucao e' FISICA -- e um PC como
        // 0xc274 (ausente nas tabelas!) roda mesmo sem fault, caindo nos 1 MB baixos que o
        // harness mapeia. E' a hipotese a bater antes de qualquer outra.
        {
            uc_arm_cp_reg d = {15, 0, 0, 3, 0, 0, 0, 0};   // DACR: permissoes por dominio
            if (uc_reg_read(g_uc_global, UC_ARM_REG_CP_REG, &d) == UC_ERR_OK) {
                u32 dv = (u32)d.val;
                std::printf("[DACR] = 0x%08x  dominios: D0=%u D1=%u ... (0=sem acesso, "
                            "1=cliente, 3=gerente)\n", dv, dv & 3u, (dv >> 2) & 3u);
            }
        }
        {
            uc_arm_cp_reg s = {15, 0, 0, 1, 0, 0, 0, 0};
            if (uc_reg_read(g_uc_global, UC_ARM_REG_CP_REG, &s) == UC_ERR_OK) {
                u32 v = (u32)s.val;
                std::printf("[SCTLR] = 0x%08x  MMU(M bit0)=%u  V(vetores altos, bit13)=%u  "
                            "D(dcache)=%u  I(icache)=%u\n", v, v & 1u, (v >> 13) & 1u,
                            (v >> 2) & 1u, (v >> 12) & 1u);
            }
        }
        dump_va(g_uc_global, procdb, pc, "processo");
        dump_va(g_uc_global, procdb, 0u, "processo@0");
        dump_va(g_uc_global, kerndb, 0u, "kernel@0");
        dump_va(g_uc_global, kerndb, 0xffff0000u, "kernel@alto");
    }
    std::fflush(stdout);
    std::_Exit(2);
}

// Primeiras instrucoes em MODO USUARIO: o initcode faz ldr/ldr/mov/svc, entao um SVC teria de
// aparecer em 4 instrucoes. Se o que aparece aqui nao e' essa sequencia, o trap-return nao
// aterrissou onde o port acha que aterrissou -- e' a pergunta que sobrou.
// ESTAGNACAO POR TRADUCAO VELHA: medido em `kpt_alloc+0x30` (`ldr r2,[r3]` -- codigo normal, com
// traducao identidade valida): o Unicorn chama o hook de codigo para o MESMO PC milhoes de vezes
// sem commitar a instrucao. Aqui eu detecto a repeticao no hook e REMOVO a traducao daquela
// pagina entre fatias (`uc_ctl_remove_cache`, que nao pode ser chamado de dentro do hook). Se o
// guest destravar, era TB velha; se nao, esta' medido que nao era.
static u64 g_rep_pc = 0;
static u32 g_rep_n = 0;
static u32 g_rep_fixes = 0;
static u64 g_stall_pc = 0;

static int g_first_user_logged = 0;
static int g_fetch_logged = 0;
static int g_first_user_n = 0;

static void on_code(uc_engine* uc, u64 addr, u32 size, void* ud) {
    (void)uc; (void)size; (void)ud;
    ++g_icount;
    // Tick do timer: e' o que faz o GPT virar interrupcao no VIC (linha 7). Periodicidade de
    // 64 instrucoes e' a mesma do harness de boot de Linux.
    if ((g_icount & 0x3Fu) == 0u) zeebo_msm::timer_refresh_irq();
    // DIAGNOSTICO (ZEEBO_TIMER_LOG): mostra a cadeia inteira do tick -- contagem do GPT,
    // match, enable, pendente, enable do VIC e o I-bit do guest.
    if (zeebo_msm::g_timer_log) {   // sem filtro: quero ver a serie inteira, inclusive a queda
        static u32 diag = 0;
        if ((g_icount & 0x3FFFu) == 0u && diag++ < 40u) {
            u32 cpsr = 0;
            uc_reg_read(uc, UC_ARM_REG_CPSR, &cpsr);
            std::printf("[tick] cnt=%u match=%u en=%u pend0=0x%08x vicen0=0x%08x cpsr=0x%08x\n",
                        zeebo_msm::gpt_count_now(), zeebo_msm::g_gpt_match,
                        zeebo_msm::g_gpt_enable, zeebo_msm::g_vic_pending[0],
                        zeebo_msm::g_vic_en[0], cpsr);
            std::fflush(stdout);
        }
    }
    // Entrega de IRQ ao guest: o Unicorn NAO faz a entrada de excecao de IRQ. Sem isto o
    // pendente do VIC (o GPT na linha 7) fica la' para sempre e o SO nunca e' preemptado.
    zeebo_msm::deliver_irq(uc);
    if (g_progress && g_progress_every && (g_icount % g_progress_every) == 0ull) {
        u32 cpsr = 0;
        uc_reg_read(uc, UC_ARM_REG_CPSR, &cpsr);
        // O tempo de PAREDE na linha transforma uma corrida na CURVA de degradacao: da'
        // para ver onde a taxa cai e se a queda e' penhasco ou rampa.
        static const auto t0 = std::chrono::steady_clock::now();
        const long long ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                 std::chrono::steady_clock::now() - t0).count();
        std::printf("[prog] insn=%llu t=%lldms pc=0x%08x cpsr=0x%08x ult=",
                    (unsigned long long)g_icount, ms, (u32)addr, cpsr);
        for (u32 k = 0; k < 8u; ++k)
            std::printf("%08x ", g_pc_ring[(g_pc_pos - 8u + k) & 15u]);
        std::printf("\n");
        std::fflush(stdout);   // SEM ISTO o progresso fica no buffer do libc e a ultima
                               // amostra visivel mente: parece travamento onde so' ha'
                               // buffer. (Mesma classe da armadilha de instrumento.)
        std::fflush(stdout);
    }
    if (!g_first_user_logged && addr < 0x10000000ull) {
        g_first_user_logged = 1;   // marca a entrada em modo usuario
        g_first_user_n = 0;
    }
    if (g_first_user_logged && (g_first_user_n < 12)) {
        u32 cpsr0 = 0, ttbr0 = 0;
        uc_arm_cp_reg c0 = {15, 0, 0, 2, 0, 0, 0, 0};
        uc_reg_read(uc, UC_ARM_REG_CPSR, &cpsr0);
        uc_reg_read(uc, UC_ARM_REG_CP_REG, &c0);
        ttbr0 = (u32)c0.val;
        std::printf("[1o-usuario] #%d pc=0x%08llx cpsr=0x%08x ttbr0=0x%08x\n",
                    g_first_user_n, (unsigned long long)addr, cpsr0, ttbr0);
        std::fflush(stdout);
        ++g_first_user_n;
    }

    if (addr == g_rep_pc) {
        if (++g_rep_n >= 64u && g_stall_pc == 0) g_stall_pc = addr;
    } else {
        g_rep_pc = addr;
        g_rep_n = 0;
    }

    // Anel SEMPRE mantido (1 store): e' o que responde "onde" quando o run trava sem
    // executar instrucao. Entra na linha de progresso.
    g_pc_ring[g_pc_pos & 15u] = (u32)addr;
    ++g_pc_pos;
}

// Aborts -> entrada de excecao do hardware (vive no header). O xv6 e' identity-mapped
// com V=0, entao os vetores ficam em 0x00000000, NAO em 0xffff0000 como no Linux.
static bool on_abort(uc_engine* uc, uc_mem_type type, u64 addr, int size,
                     i64 value, void* user) {
    if (zeebo_msm::g_abort_count > 900u) {          // escada de boot, nao tempestade
        if (!g_abort_limit_hit) {
            g_abort_limit_hit = 1;
            std::printf("[xv6] parando: %u aborts (o SO nao esta' tratando)\n",
                        zeebo_msm::g_abort_count);
        }
        uc_emu_stop(uc);
        return true;
    }
    return zeebo_msm::on_fault_entry(uc, type, addr, size, value, user);
}

// SVC: o Unicorn entrega UC_HOOK_INTR com o PC JA' apontando para a instrucao seguinte
// (`skill: zeebo-lle-emulator`). A entrada de excecao do ARM1136 tem de ser feita aqui:
// CPSR -> SPSR_svc, modo SVC, LR = PC (o retorno e' a instrucao apos o SVC), PC = vetor+8.
// Le um simbolo do arquivo .nm do kernel (formato "<addr> T <nome>"). O flush_tlb do guest
// precisa ser achado assim porque esta versao do Unicorn NAO tem hook de instrucao para ARM
// (nao existe UC_ARM_INS_MCR), entao nao da' para interceptar o MCR de CP15 em si.
static u32 nm_symbol(const std::string& nm_path, const std::string& want) {
    FILE* f = std::fopen(nm_path.c_str(), "r");
    if (!f) return 0;
    char line[512];
    u32 found = 0;
    while (std::fgets(line, sizeof(line), f)) {
        unsigned long a = 0;
        char tipo = 0, nome[256];
        nome[0] = 0;
        if (std::sscanf(line, "%lx %c %255s", &a, &tipo, nome) == 3) {
            if (want == nome) { found = (u32)a; break; }
        }
    }
    std::fclose(f);
    return found;
}

// CP15 / TLB: o `flush_tlb()` do guest escreve o registrador de invalidacao de TLB (c8,c7,0)
// -- e o Unicorn NAO honra isso sozinho. Sem esta traducao, a traducao de um VA fica CACHEADA
// quando o pgdir muda por `switchuvm`, e o CPU continua usando o mapa antigo. So' morde o xv6
// porque o codigo de usuario vive em VA 0, cujo mapeamento ja' estava cacheado com a SECAO do
// kernel (VA 0 -> PA 0): medido, as primeiras instrucoes "de usuario" eram um avanco linear por
// 0x00,0x04,0x08,0x0c... em 1 MB de zeros -- nunca o `svc` que esta' no initcode.
// Aqui: qualquer MCR de CP15 em c2 (TTBR: troca de tabela) ou c8 (manutencao de TLB) invalida o
// TLB do Unicorn, preservando a semantica do guest.
// Busca de instrucao: o Unicorn entrega aqui o endereco FISICO da busca. Comparar com o PA que
// a tabela do processo diz para aquele VA e' o que separa "a CPU usa a tabela do processo" de
// "a CPU esta' usando traducao velha". E' a medicao que decide o caso.
static void on_fetch(uc_engine* uc, u64 phys, u32 size, void* ud) {
    (void)uc; (void)size; (void)ud;
    // Buscas em MODO USUARIO: o Unicorn entrega o endereco FISICO. O PA que a tabela do processo
    // diz para VA 0 e' 0x14ff1000 (onde o initcode foi copiado). Se as buscas de usuario vierem
    // de OUTRO lugar, a CPU nao esta' usando a tabela do processo -- e o de onde elas vem diz
    // qual traducao esta' em uso.
    if (g_fetch_logged >= 12) return;
    ++g_fetch_logged;
    std::printf("[fetch] #%d usuario: FISICO=0x%08llx (a tabela do processo diz 0x14ff1000 para VA 0)\n",
                g_fetch_logged, (unsigned long long)phys);
    std::fflush(stdout);
}

static int g_flush_hits = 0;

static void on_flush_tlb(uc_engine* uc, u64 addr, u32 size, void* ud) {
    (void)addr; (void)size; (void)ud;
    uc_ctl_flush_tlb(uc);
    ++zeebo_msm::g_tlb_flushes;
    ++g_flush_hits;
}

static void on_intr(uc_engine* uc, u32 intno, void* ud) {
    (void)ud;
    if (intno != 2) {                                // 2 = SVC no Unicorn/ARM
        return;
    }
    u32 cpsr = 0, pc = 0;
    uc_reg_read(uc, UC_ARM_REG_CPSR, &cpsr);
    uc_reg_read(uc, UC_ARM_REG_PC, &pc);
    // MARCO: o programa de usuario roda mas nada aparece. Se NENHUM SVC vier do espaco de
    // usuario, o que roda la' nao e' o programa (nenhum syscall acontece) -- e o alvo passa a
    // ser por que o codigo de usuario nao executa o `svc` do initcode.
    ++zeebo_msm::g_svc_total;
    if (pc < 0x10000000u) {
        ++zeebo_msm::g_svc_user;
        if (zeebo_msm::g_svc_user <= 5u) {
            std::printf("[svc] do USUARIO: pc=0x%08x cpsr=0x%08x (total usuario=%u)\n",
                        pc, cpsr, zeebo_msm::g_svc_user);
            std::fflush(stdout);
        }
    }
    const u32 svc_cpsr = (cpsr & ~0x3fu) | 0x13u | 0x80u;   // modo SVC, IRQ mascarada
    uc_reg_write(uc, UC_ARM_REG_CPSR, &svc_cpsr);
    uc_reg_write(uc, UC_ARM_REG_SPSR, &cpsr);
    uc_reg_write(uc, UC_ARM_REG_LR, &pc);                   // nao somar 4 de novo
    const u32 vec = zeebo_msm::g_exc_vector_base + 0x08u;
    uc_reg_write(uc, UC_ARM_REG_PC, &vec);
}

static std::vector<u8> read_file(const std::string& p) {
    std::vector<u8> out;
    if (FILE* f = std::fopen(p.c_str(), "rb")) {
        u8 buf[65536];
        size_t n;
        while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0)
            out.insert(out.end(), buf, buf + n);
        std::fclose(f);
    }
    return out;
}

int main(int argc, char** argv) {
    (void)argc; (void)argv;
    std::printf("=== xv6-zeebo no modelo REAL do MSM7201A (nao no runner Python) ===\n");

    const char* env = std::getenv("ZEEBO_XV6_KERNEL");
    const std::string path = env ? env : "xv6-kernel.bin";
    const std::vector<u8> img = read_file(path);
    if (img.empty()) {
        std::printf("SKIP (exit 77): imagem do xv6 ausente em '%s'.\n", path.c_str());
        std::printf("  Defina ZEEBO_XV6_KERNEL=<kernel.bin> (a imagem do SO nao e' versionada).\n");
        return 77;
    }
    if (const char* b = std::getenv("ZEEBO_BUDGET")) g_budget = std::strtoull(b, nullptr, 0);
    g_trace = (std::getenv("ZEEBO_XV6_TRACE") != nullptr);
    if (const char* p = std::getenv("ZEEBO_XV6_PROGRESS")) {
        g_progress = true;
        if (*p) g_progress_every = std::strtoull(p, nullptr, 0);
    }
    std::printf("[xv6] imagem: %s (%zu bytes); teto %llu instrucoes\n",
                path.c_str(), img.size(), (unsigned long long)g_budget);

    uc_engine* uc = nullptr;
    if (uc_open(UC_ARCH_ARM, UC_MODE_ARM, &uc) != UC_ERR_OK) {
        std::printf("FAIL: uc_open\n");
        return 1;
    }
    // ARM1136 e' a CPU do Zeebo; o default do Unicorn (Cortex-A15/ARMv7) faz o SO
    // abortar com "unrecognized processor variant".
    uc_ctl_set_cpu_model(uc, UC_CPU_ARM_1136);
    // Vetores BAIXOS: o xv6 roda com V=0 (identity map), ao contrario do Linux aqui.
    // xv6-zeebo usa vetores ALTOS (VEC_TBL = 0xFFFF0000, SCTLR.V = 1). Com base 0 o vetor do
    // SVC caia no VA 0 -- que no pgdir do processo e' o `initcode` -- e o motor travava.
    zeebo_msm::g_exc_vector_base = 0xFFFF0000u;

    // ------- mapa: o que um SO bare-metal no Zeebo pode tocar -------
    uc_mem_map(uc, zeebo_msm::APPS_RAM_PHYS, zeebo_msm::APPS_RAM_SIZE, UC_PROT_ALL);
    uc_mem_map(uc, 0x00000000u, 0x00100000u, UC_PROT_ALL);          // 1MB baixo: vetores baixos
    // Pagina de VETORES ALTOS. O port escreve a tabela em VEC_TBL=0xFFFF0000 (trap.c), e
    // sem esta pagina o store fica girando: o SO para DENTRO de trap_init sem fault visivel
    // nem hook de unmapped. O harness de Linux mapeia exatamente isto -- e' a mesma
    // necessidade, nao uma peculiaridade do xv6.
    uc_mem_map(uc, 0xffff0000u, 0x00010000u, UC_PROT_ALL);
    uc_mem_map(uc, zeebo_msm::PERIPH_BASE, zeebo_msm::PERIPH_SIZE, UC_PROT_ALL);  // VIC + GPT/DGT
    uc_mem_map(uc, 0xa9200000u, 0x00800000u, UC_PROT_ALL);          // GPIO e vizinhos
    for (u32 ub : zeebo_msm::UART_BASES) uc_mem_map(uc, ub, zeebo_msm::UART_SIZE, UC_PROT_ALL);
    // GPT/DGT (CSR): o kernel le/ escreve o timer por aqui. Os dois enderecos sao mapeados
    // porque o driver pode usar o VA do iotable ou o PA, dependendo de como foi escrito.
    uc_mem_map(uc, zeebo_msm::CSR_PA, zeebo_msm::CSR_SIZE, UC_PROT_ALL);
    uc_mem_map(uc, zeebo_msm::CSR_BASE, zeebo_msm::CSR_SIZE, UC_PROT_ALL);
    // Clockevent ligado: e' o que permite ao SO ser PREEMPTADO. Sem tick o scheduler gira.
    zeebo_msm::g_timer_on = (std::getenv("ZEEBO_NOTIMER") == nullptr);
    zeebo_msm::g_timer_log = (std::getenv("ZEEBO_TIMER_LOG") != nullptr);
    zeebo_msm::g_irq_log   = (std::getenv("ZEEBO_IRQ_LOG") != nullptr);

    // ------- hooks de periferico (o modelo vem do header compartilhado) -------
    uc_hook h = 0;
    uc_hook_add(uc, &h, UC_HOOK_MEM_WRITE, (void*)zeebo_msm::on_vic_write, nullptr,
                zeebo_msm::VIC_BASE, zeebo_msm::VIC_BASE + zeebo_msm::VIC_SIZE);
    uc_hook_add(uc, &h, UC_HOOK_MEM_READ,  (void*)zeebo_msm::on_vic_read,  nullptr,
                zeebo_msm::VIC_BASE, zeebo_msm::VIC_BASE + zeebo_msm::VIC_SIZE);
    uc_hook_add(uc, &h, UC_HOOK_MEM_WRITE, (void*)zeebo_msm::on_csr_write, nullptr,
                zeebo_msm::CSR_PA, zeebo_msm::CSR_PA + zeebo_msm::CSR_SIZE);
    uc_hook_add(uc, &h, UC_HOOK_MEM_READ,  (void*)zeebo_msm::on_csr_read,  nullptr,
                zeebo_msm::CSR_PA, zeebo_msm::CSR_PA + zeebo_msm::CSR_SIZE);
    uc_hook_add(uc, &h, UC_HOOK_MEM_WRITE, (void*)zeebo_msm::on_csr_write, nullptr,
                zeebo_msm::CSR_BASE, zeebo_msm::CSR_BASE + zeebo_msm::CSR_SIZE);
    uc_hook_add(uc, &h, UC_HOOK_MEM_READ,  (void*)zeebo_msm::on_csr_read,  nullptr,
                zeebo_msm::CSR_BASE, zeebo_msm::CSR_BASE + zeebo_msm::CSR_SIZE);
    for (u32 ub : zeebo_msm::UART_BASES) {
        uc_hook_add(uc, &h, UC_HOOK_MEM_WRITE, (void*)zeebo_msm::on_uart_write, nullptr,
                    ub, ub + zeebo_msm::UART_SIZE);
        uc_hook_add(uc, &h, UC_HOOK_MEM_READ,  (void*)zeebo_msm::on_uart_read,  nullptr,
                    ub, ub + zeebo_msm::UART_SIZE);
    }
    // Aborts: o Unicorn nao vetoriza; o header faz a entrada de excecao.
    uc_hook_add(uc, &h, UC_HOOK_MEM_FETCH_UNMAPPED, (void*)on_abort, nullptr, 1, 0);
    uc_hook_add(uc, &h, UC_HOOK_MEM_WRITE_UNMAPPED, (void*)on_abort, nullptr, 1, 0);
    uc_hook_add(uc, &h, UC_HOOK_MEM_READ_UNMAPPED,  (void*)on_abort, nullptr, 1, 0);
    // FALHA DE PROTECAO tambem precisa de tratador. Sem estes, uma escrita numa pagina
    // mapeada SO'-LEITURA (a AP da secao do guest) faz o Unicorn repetir a MESMA instrucao
    // para sempre: o PC fica preso, nao ha abort e o sintoma vira "consumiu o orcamento".
    // Medido: `str r2,[r3]` com r3=0xffff0000 repetindo 20M instrucoes.
    // LIGADOS por padrao: sem eles uma falha de protecao faz o Unicorn repetir a instrucao
    // para sempre (medido). ZEEBO_XV6_NOPROT=1 desliga, e serve de A/B: medidos os dois
    // lados, o custo deles e' ZERO (10 M instrucoes em 45 s com e sem) -- a lentidao que
    // apareceu depois nao e' deles.
    if (std::getenv("ZEEBO_XV6_NOPROT") == nullptr) {
        uc_hook_add(uc, &h, UC_HOOK_MEM_FETCH_PROT, (void*)on_abort, nullptr, 1, 0);
        uc_hook_add(uc, &h, UC_HOOK_MEM_WRITE_PROT, (void*)on_abort, nullptr, 1, 0);
        uc_hook_add(uc, &h, UC_HOOK_MEM_READ_PROT,  (void*)on_abort, nullptr, 1, 0);
    }
    uc_hook_add(uc, &h, UC_HOOK_INTR, (void*)on_intr, nullptr, 1, 0);
    uc_hook_add(uc, &h, UC_HOOK_MEM_FETCH, (void*)on_fetch, nullptr, 1, 0);
    // Invalidacao de TLB: o Unicorn nao honra o MCR c8 do guest. Hook de CODIGO na faixa do
    // `flush_tlb` do guest (endereco tirado do .nm ao lado da imagem) traduz a operacao.
    {
        // O .nm fica ao lado do .bin com o mesmo radical: kernel.bin -> kernel.nm
        std::string nm = path;
        const std::size_t dot = nm.rfind('.');
        const std::size_t slash = nm.find_last_of('/');
        if (dot != std::string::npos && (slash == std::string::npos || dot > slash))
            nm = nm.substr(0, dot) + ".nm";
        else
            nm += ".nm";
        // SEGURANCA DO INSTRUMENTO: eu ja' resolvi simbolo no .nm ERRADO (stale) e tirei
        // conclusao falsa de 2491 "acertos do flush_tlb" que talvez fossem outra funcao. Se o
        // .nm for mais antigo que o binario, o hook esta' apontando para o lugar errado.
        {
            struct stat sb_bin, sb_nm;
            if (::stat(path.c_str(), &sb_bin) == 0 && ::stat(nm.c_str(), &sb_nm) == 0) {
                if (sb_nm.st_mtime < sb_bin.st_mtime) {
                    std::printf("[xv6] AVISO: %s e' MAIS ANTIGO que %s -- os simbolos podem ser "
                                "de outro build e todo hook por endereco fica suspeito\n",
                                nm.c_str(), path.c_str());
                }
            }
        }
        g_kpt_mem_addr = nm_symbol(nm, "kpt_mem");
        const u32 ft = nm_symbol(nm, "flush_tlb");
        if (ft != 0u) {
            uc_hook_add(uc, &h, UC_HOOK_CODE, (void*)on_flush_tlb, nullptr, ft, ft + 128u);
            std::printf("[xv6] flush_tlb do guest em 0x%08x -- TLB do Unicorn ligado a ele\n", ft);
        } else {
            std::printf("[xv6] AVISO: nao achei flush_tlb no .nm (%s) -- TLB do Unicorn NAO "
                        "sera' invalidado quando o guest pedir\n", nm.c_str());
        }
    }
    uc_hook_add(uc, &h, UC_HOOK_CODE, (void*)on_code, nullptr, 1, 0);

    // ------- entrada de teclado (mesma convencao do harness de Linux) -------
    if (!isatty(0)) {
        char c = 0;
        while (read(0, &c, 1) == 1) zeebo_msm::g_rx_buf.push_back(c);
    }
    g_icount = 0;
    const u32 entry = zeebo_msm::APPS_RAM_PHYS;     // o port linka o _start aqui
    std::printf("[xv6] carregado em 0x%08x; rodando...\n", entry);
    uc_mem_write(uc, entry, img.data(), img.size());
    g_uc_global = uc;
    if (const char* hb = std::getenv("ZEEBO_XV6_HB")) g_hb_ms = std::atoi(hb);
    // Alavanca de diagnostico: o default do Unicorn e' UC_TLB_VIRTUAL (TLB virtual proprio);
    // UC_TLB_CPU usa o softmmu classico, com caminhada de tabela por acesso. O travamento
    // medido e' na GERACAO do bloco da primeira busca em modo usuario -- se o modo de TLB
    // muda o sintoma, o problema esta' na caminhada, nao no tradutor.
    // UC_TLB_CPU (o softmmu, com caminhada de tabela POR ACESSO) e' o CORRETO aqui.
    // CORRECAO de uma conclusao minha anterior ("virtual e' obrigatorio"): era o INVERSO.
    // Medido com o mesmo binario:
    //   VIRTUAL: o initcode NAO executa -- as primeiras "instrucoes de usuario" sao um avanco
    //            linear por 0x00,0x04,0x08,0x0c..., sem o `svc` que esta' em 0x0c, e nenhum SVC
    //            de usuario acontece em 250 M instrucoes. O TLB virtual serve traducao VELHA
    //            depois da troca de pgdir no `switchuvm`.
    //   CPU:     o initcode executa (0x00,0x04,0x08,0x0c=svc), o SVC e' tomado, o PC vai para o
    //            VETOR ALTO 0xFFFF0008 e o trap handler do guest roda. Custa ~10-100x mais.
    // "virtual parecia melhor" porque entregava 93 IRQs contra 1 -- e eu li isso como progresso
    // quando era o contrario: o modo CPU e' LENTO, nao travado.
    uc_ctl_tlb_mode(uc, UC_TLB_CPU);
    if (const char* tm = std::getenv("ZEEBO_TLB")) {   // alavanca de A/B
        if (std::strcmp(tm, "virtual") == 0) uc_ctl_tlb_mode(uc, UC_TLB_VIRTUAL);
    }
    struct sigaction sa;
    std::memset(&sa, 0, sizeof(sa));
    sa.sa_sigaction = on_signal_dump;
    sa.sa_flags = SA_SIGINFO;
    sigaction(SIGTERM, &sa, nullptr);
    sigaction(SIGINT, &sa, nullptr);
    if (g_hb_ms > 0) {
        std::signal(SIGALRM, on_heartbeat);
        struct itimerval it;
        it.it_interval.tv_sec = g_hb_ms / 1000;
        it.it_interval.tv_usec = (g_hb_ms % 1000) * 1000;
        it.it_value = it.it_interval;
        setitimer(ITIMER_REAL, &it, nullptr);
    }
    // O Unicorn restringe a execucao ao intervalo [begin, until). Com begin = endereco do kernel
    // (0x10000000) o retorno de trap do xv6 para o codigo de usuario -- que vive em VA 0, o
    // `initcode` -- CAI FORA da faixa e a emulacao encerra (medido: "parou: OK ... pc=0x0").
    // Solucao: FATIAS, cada uma comecando no PC atual, entao a faixa acompanha o guest quando
    // ele desce para o espaco de usuario. `g_icount` (contador de hooks) mede o progresso real,
    // entao o laco termina por orcamento, por erro, ou por ausencia de progresso.
    const unsigned long long SLICE = 4000000ull;
    unsigned long long feito = 0ull;
    // O ponto de entrada tem de ser ESCRITO no registrador: com fatias quem manda no PC e' o
    // proprio guest a partir daqui (antes, o PC vinha do argumento `begin` do uc_emu_start).
    uc_reg_write(uc, UC_ARM_REG_PC, &entry);
    uc_err e = UC_ERR_OK;
    int fatias = 0;
    while ((feito < (unsigned long long)g_budget) && (fatias < 10000)) {
        u32 pc_now = 0;
        unsigned long long antes = g_icount;
        unsigned long long n = ((unsigned long long)g_budget - feito) < SLICE
                                   ? ((unsigned long long)g_budget - feito) : SLICE;

        if (uc_reg_read(uc, UC_ARM_REG_PC, &pc_now) != UC_ERR_OK) break;
        // `until` = 0xFFFFFFFF (e nao 0): com begin=0 e until=0 o intervalo fica degenerado e o
        // Unicorn retorna sem executar NADA (medido: fatia em pc=0 -> 0 instrucoes). Uma faixa
        // real que cobre o espaco inteiro deixa a fatia executar a partir do PC atual.
        e = uc_emu_start(uc, (u64)pc_now, 0xFFFFFFFFull, 0, n);
        feito += (g_icount - antes);
        ++fatias;
        if (e != UC_ERR_OK) break;
        if (g_stall_pc != 0) {
            const u64 ini = g_stall_pc & ~0xfffull;
            uc_ctl_remove_cache(uc, ini, ini + 0x1000ull);
            ++g_rep_fixes;
            if (g_rep_fixes <= 3u) {
                std::printf("[xv6] estagnacao em 0x%08llx: traducao da pagina removida (#%u)\n",
                            (unsigned long long)g_stall_pc, g_rep_fixes);
                std::fflush(stdout);
            }
            g_stall_pc = 0;
            g_rep_pc = 0;
            g_rep_n = 0;
            continue;   // retoma a fatia a partir do PC atual
        }
        if (g_icount == antes) break;   // sem progresso: para em vez de girar
    }
    // O PC da parada e' o que diz ONDE o SO desistiu; sem ele o veredito so' aponta o
    // sintoma. Resolve-se contra o kernel.nm do mesmo build.
    u32 pc_stop = 0;
    uc_reg_read(uc, UC_ARM_REG_PC, &pc_stop);
    u32 r0s=0,r1s=0,r2s=0,r3s=0,r4s=0,cpsr_s=0;
    uc_reg_read(uc, UC_ARM_REG_R0, &r0s); uc_reg_read(uc, UC_ARM_REG_R1, &r1s);
    uc_reg_read(uc, UC_ARM_REG_R2, &r2s); uc_reg_read(uc, UC_ARM_REG_R3, &r3s);
    uc_reg_read(uc, UC_ARM_REG_R4, &r4s); uc_reg_read(uc, UC_ARM_REG_CPSR, &cpsr_s);
    std::printf("[xv6] r0=0x%08x r1=0x%08x r2=0x%08x r3=0x%08x r4=0x%08x cpsr=0x%08x\n",
                r0s, r1s, r2s, r3s, r4s, cpsr_s);
    std::printf("\n[xv6] parou: %s (%d) apos %llu instrucoes; pc=0x%08x\n",
                uc_strerror(e), (int)e, (unsigned long long)g_icount, pc_stop);
    std::printf("[xv6] SVCs: total=%u (do usuario=%u); invalidadas de TLB=%u; acertos do hook "
                "flush_tlb=%u\n", zeebo_msm::g_svc_total, zeebo_msm::g_svc_user,
                zeebo_msm::g_tlb_flushes, g_flush_hits);
    std::printf("[xv6] aborts=%u; vetores=%u; IRQs entregues=%u\n",
                zeebo_msm::g_abort_count, zeebo_msm::g_exc_vector_base,
                zeebo_msm::g_irq_delivered);
    if (g_trace) {
        std::printf("[xv6] ultimos PCs (mais antigo -> mais novo):");
        for (u32 i = 0; i < 16u; ++i)
            std::printf(" %08x", g_pc_ring[(g_pc_pos - 16u + i) & 15u]);
        std::printf("\n");
    }

    // ------- veredito: o oraculo vem do BINARIO, nao do desejo -------
    std::printf("\n---- console do guest (UART1) ----\n%s\n---- fim ----\n",
                zeebo_msm::g_console.c_str());
    struct Check { const char* s; } checks[] = {
        {"starting xv6 for ARM..."},
        {"xv6: enabling mmu..."},
        {"xv6: mmu on"},
        {"xv6: reached kmain (post-MMU)"},
    };
    int found = 0, total = 0;
    for (const Check& c : checks) {
        ++total;
        const bool ok = zeebo_msm::g_console.find(c.s) != std::string::npos;
        std::printf("  %s %s\n", ok ? "OK  " : "FALTA", c.s);
        if (ok) ++found;
    }
    uc_close(uc);
    if (found == total) {
        std::printf("=== PASS: %d/%d marcos do oraculo do xv6 ===\n", found, total);
        return 0;
    }
    std::printf("=== FAIL: %d/%d marcos (o SO nao chegou onde o oraculo espera) ===\n",
                found, total);
    return 1;
}
