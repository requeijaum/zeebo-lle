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
#include <vector>
#include <map>
#include <algorithm>
#include <unicorn/unicorn.h>

using u8 = uint8_t; using u32 = uint32_t; using u64 = uint64_t; using i64 = int64_t;

namespace {

// --- mapa MSM7201A (mesmas constantes do emulador principal) ---
constexpr u32 APPS_RAM_PHYS = 0x10000000u;
constexpr u32 APPS_RAM_SIZE = 0x06000000u;   // 96MB
constexpr u32 UART1_BASE    = 0xa9a00000u;
constexpr u32 UART_SIZE     = 0x00010000u;
constexpr u32 UART_OFF_TF   = 0x000cu;       // TX FIFO
constexpr u32 UART_OFF_SR   = 0x0008u;       // status; bit2 = TX_READY
constexpr u32 VIC_BASE      = 0xc0000000u;
constexpr u32 VIC_SIZE      = 0x00200000u;   // 2MB: o VIC do MSM vai ate 0xc01xxxxx

// Onde o zImage ARM e tipicamente carregado: base da RAM + 0x8000.
constexpr u32 KERNEL_LOAD   = APPS_RAM_PHYS + 0x8000u;

std::string g_console;      // tudo que o guest escreveu na UART1
u64 g_insn = 0;

void on_uart_write(uc_engine* uc, uc_mem_type type, uint64_t addr,
                   int size, int64_t value, void* ud) {
    (void)uc; (void)type; (void)size; (void)ud;
    const u32 off = static_cast<u32>(addr - UART1_BASE);
    if (off == UART_OFF_TF || off == 0x00) {
        const char c = static_cast<char>(value & 0xff);
        if (c == '\n' || c == '\r' || (c >= 0x20 && c < 0x7f)) g_console.push_back(c);
    }
}

bool on_uart_read(uc_engine* uc, uc_mem_type type, uint64_t addr,
                  int size, int64_t value, void* ud) {
    (void)type; (void)size; (void)value; (void)ud;
    const u32 off = static_cast<u32>(addr - UART1_BASE);
    if (off == UART_OFF_SR) {
        // TX_READY sempre alto: o kernel nunca fica preso esperando a FIFO.
        u32 sr = (1u << 2);
        uc_mem_write(uc, UART1_BASE + UART_OFF_SR, &sr, 4);
    }
    return true;
}

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
    std::printf("  L2 CPU executa >=%lluk insn  : %-5s (%llu instrucoes)\n",
                (unsigned long long)(kInsnBudget/1000), v.l2 ? "PASS" : "FAIL",
                (unsigned long long)v.insn);
    std::printf("  L3 console emite assinatura  : %-5s\n", v.l3 ? "PASS" : "FAIL");
    if (!g_console.empty()) {
        std::printf("\n  --- console UART1 (%zu bytes) ---\n", g_console.size());
        std::printf("%s\n", g_console.substr(0, 2000).c_str());
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
    uc_mem_map(uc, UART1_BASE, UART_SIZE, UC_PROT_ALL);
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
    // fora da RAM de aplicacao: candidato a MMIO
    if (a < APPS_RAM_PHYS || a >= APPS_RAM_PHYS + APPS_RAM_SIZE)
        g_hi_writes[a & 0xFFFFF000u]++;
}


// Instrumento (ZEEBO_UART_PROBE): amostra o PC ao longo da execucao para
// distinguir "progredindo" de "preso em laco".
static std::map<u32,long> g_pc_hist;
static u64 g_icount = 0;
static void on_code_probe(uc_engine* uc, u64 addr, u32 size, void* user) {
    (void)uc;(void)size;(void)user;
    ++g_icount;
    if (!g_probe) return;
    if ((g_icount & 0xFFFF) == 0) g_pc_hist[static_cast<u32>(addr)]++;  // PC exato
}

int main(int argc, char** argv) {
    if (argc > 1 && std::string(argv[1]) == "selftest") return run_selftest();

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
    uc_mem_map(uc, UART1_BASE, UART_SIZE, UC_PROT_ALL);
    uc_mem_map(uc, VIC_BASE, VIC_SIZE, UC_PROT_ALL);

    uc_hook hw = 0, hr = 0, hc = 0;
    uc_hook_add(uc, &hw, UC_HOOK_MEM_WRITE, (void*)on_uart_write, nullptr,
                UART1_BASE, UART1_BASE + UART_SIZE);
    uc_hook_add(uc, &hr, UC_HOOK_MEM_READ, (void*)on_uart_read, nullptr,
                UART1_BASE, UART1_BASE + UART_SIZE);
    uc_hook_add(uc, &hc, UC_HOOK_CODE, (void*)on_code, nullptr, 1, 0);
    uc_hook hm = 0;
    uc_hook_add(uc, &hm, UC_HOOK_MEM_INVALID, (void*)on_mem_invalid, nullptr, 1, 0);
    g_probe = (std::getenv("ZEEBO_UART_PROBE") != nullptr);
    uc_hook hp = 0;
    if (g_probe)
        uc_hook_add(uc, &hp, UC_HOOK_MEM_WRITE, (void*)on_any_write, nullptr, 1, 0);
    uc_hook hpc = 0;
    if (g_probe)
        uc_hook_add(uc, &hpc, UC_HOOK_CODE, (void*)on_code_probe, nullptr, 1, 0);

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
        const char* cmdline = "console=ttyMSM0,115200n8 mem=64M rdinit=/bin/sh";
        size_t clen = std::strlen(cmdline) + 1;
        size_t cwords = (clen + 3) / 4;
        t.push_back(static_cast<u32>(2 + cwords)); t.push_back(0x54410009u);
        size_t base = t.size();
        t.resize(base + cwords, 0);
        std::memcpy(&t[base], cmdline, clen);
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

    uc_err e = uc_emu_start(uc, KERNEL_LOAD, 0, 0, kInsnBudget);
    v.insn = g_insn;
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
    std::printf("[boot] parou: %s (%d) apos %llu instrucoes\n",
                uc_strerror(e), (int)e, (unsigned long long)g_insn);

    v.l2 = (g_insn >= kInsnBudget);
    v.l3 = (g_console.find("Uncompressing Linux") != std::string::npos) ||
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
