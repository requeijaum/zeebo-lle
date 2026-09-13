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
static u64 g_insn   = 0;
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

static void on_code(uc_engine* uc, u64 addr, u32 size, void* ud) {
    (void)uc; (void)size; (void)ud;
    ++g_insn;
    if (g_progress && g_progress_every && (g_insn % g_progress_every) == 0ull) {
        u32 cpsr = 0;
        uc_reg_read(uc, UC_ARM_REG_CPSR, &cpsr);
        std::printf("[prog] insn=%llu pc=0x%08x cpsr=0x%08x\n",
                    (unsigned long long)g_insn, (u32)addr, cpsr);
        std::fflush(stdout);
    }
    if (g_trace) {
        g_pc_ring[g_pc_pos & 15u] = (u32)addr;
        ++g_pc_pos;
    }
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
static void on_intr(uc_engine* uc, u32 intno, void* ud) {
    (void)ud;
    if (intno != 2) {                                // 2 = SVC no Unicorn/ARM
        return;
    }
    u32 cpsr = 0, pc = 0;
    uc_reg_read(uc, UC_ARM_REG_CPSR, &cpsr);
    uc_reg_read(uc, UC_ARM_REG_PC, &pc);
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
    zeebo_msm::g_exc_vector_base = 0x00000000u;

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

    // ------- hooks de periferico (o modelo vem do header compartilhado) -------
    uc_hook h = 0;
    uc_hook_add(uc, &h, UC_HOOK_MEM_WRITE, (void*)zeebo_msm::on_vic_write, nullptr,
                zeebo_msm::VIC_BASE, zeebo_msm::VIC_BASE + zeebo_msm::VIC_SIZE);
    uc_hook_add(uc, &h, UC_HOOK_MEM_READ,  (void*)zeebo_msm::on_vic_read,  nullptr,
                zeebo_msm::VIC_BASE, zeebo_msm::VIC_BASE + zeebo_msm::VIC_SIZE);
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
    uc_hook_add(uc, &h, UC_HOOK_CODE, (void*)on_code, nullptr, 1, 0);

    // ------- entrada de teclado (mesma convencao do harness de Linux) -------
    if (!isatty(0)) {
        char c = 0;
        while (read(0, &c, 1) == 1) zeebo_msm::g_rx_buf.push_back(c);
    }
    g_insn = 0;
    const u32 entry = zeebo_msm::APPS_RAM_PHYS;     // o port linka o _start aqui
    std::printf("[xv6] carregado em 0x%08x; rodando...\n", entry);
    uc_mem_write(uc, entry, img.data(), img.size());
    uc_err e = uc_emu_start(uc, entry, 0, 0, g_budget);
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
                uc_strerror(e), (int)e, (unsigned long long)g_insn, pc_stop);
    std::printf("[xv6] aborts=%u; vetores=%u\n",
                zeebo_msm::g_abort_count, zeebo_msm::g_exc_vector_base);
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
