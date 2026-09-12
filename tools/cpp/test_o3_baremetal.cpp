// test_o3_baremetal.cpp — Carga de trabalho ARM bare-metal REAL no MSM7201A.
//
// PROVENIENCIA
//   testdata/tripleoxygen/blinkenlichten.bin  (244 bytes)
//   sha256 611a1fbe3a7775d12102b9baf35addd41b85973e073f1dc6eab4a3e68630d438
//   Origem: https://www.tripleoxygen.net/files/devices/zeebo/code/blinkenlichten.bin
//   Autor: Fausto "TripleOxygen" -- homebrew bare-metal que RODA no Zeebo real.
//
// POR QUE ESTA CARGA
//   O pivo pedia um kernel Linux. O Fausto de fato bootou Linux 2.6.29-zeebo
//   (log em pastebin/pdVwuLUV, build #85, 07/ago/2011, bootloader "Zeeboot v0.1"),
//   mas NEM o zImage NEM o Zeeboot foram publicados -- varredura completa dos 194
//   arquivos de /files/devices/zeebo/ nao tem imagem de kernel.
//
//   blinkenlichten.bin e o substituto honesto: codigo ARM autoral, comprovadamente
//   executado no hardware alvo, pequeno o bastante para ser auditado inteiro (244B),
//   e que exercita exatamente o que queremos validar da CPU -- fetch/decode ARMv6,
//   branches, barrel shifter, e acesso a MMIO de GPIO.
//
//   Diferente do Zeetris, aqui o comportamento esperado e DERIVADO DO CODIGO, nao
//   de engenharia reversa: o binario foi desassemblado e o oraculo abaixo vem do
//   que as instrucoes dizem, nao do que eu gostaria que acontecesse.
//
// ORACULO (do desassembly, verificavel por qualquer um)
//   0x10  mov r0,#0x80000      \
//   0x14  subs r0,r0,#1         > delay loop: 0x80000 iteracoes
//   0x18  bne 0x14             /
//   0x1c  ldr r0,[pc,#0xcc]   -> literal @0xf0 = 0xa9200800  (GPIO)
//   0x20  ldr r1,[r0]
//   0x24  eor r1,r1,#0x8000   -> TOGGLE do bit 15
//   0x28  str r1,[r0]         -> escrita de volta no GPIO
//
// CRITERIOS
//   B1  CPU executa o delay loop inteiro (>= 0x80000 iteracoes) sem derail
//   B2  ocorre STORE em 0xa9200800 (o GPIO que o codigo endereca)
//   B3  o bit 15 realmente ALTERNA entre escritas consecutivas (>= 2 toggles)
//
// B3 e o criterio que vale: prova semantica (eor/str fazendo a coisa certa),
// nao apenas "a CPU nao crashou". Um emulador que ignorasse o EOR passaria em
// B1 e B2 e reprovaria em B3 -- que e o ponto.
//
// Compile e rode de dentro de tools/cpp/.

#include <unicorn/unicorn.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <string>
#include <vector>

typedef uint32_t u32;
typedef uint64_t u64;

// Mapa MSM7201A (mesmas constantes ja usadas pelo emulador principal).
static const u64 APPS_RAM_BASE = 0x10000000ULL;
static const u64 APPS_RAM_SIZE = 96ULL * 1024 * 1024;
static const u64 GPIO_BASE     = 0xa9200000ULL;   // janela que contem 0xa9200800
static const u64 GPIO_SIZE     = 0x00001000ULL;
static const u64 MMIO_BASE     = 0xb8000000ULL;   // o codigo toca 0xb8000308
static const u64 MMIO_SIZE     = 0x00001000ULL;

static const u64 LOAD_ADDR     = 0x10000000ULL;
static const u32 GPIO_WATCH    = 0xa9200800u;
static const u32 TOGGLE_BIT    = 0x00008000u;

struct Ctx {
    u64 insns = 0;
    std::vector<u32> gpio_writes;   // valores escritos em GPIO_WATCH
    u32 gpio_cell = 0;              // estado corrente da celula
};

static void hook_code(uc_engine*, u64, u32, void* user) {
    static_cast<Ctx*>(user)->insns++;
}

static void hook_mem_write(uc_engine*, uc_mem_type, u64 addr, int size,
                           int64_t value, void* user) {
    Ctx* c = static_cast<Ctx*>(user);
    if (addr == GPIO_WATCH && size == 4) {
        c->gpio_cell = static_cast<u32>(value);
        c->gpio_writes.push_back(c->gpio_cell);
    }
}

// Leitura do GPIO devolve o estado corrente (senao o EOR nunca alterna).
static bool hook_mem_read(uc_engine* uc, uc_mem_type, u64 addr, int size,
                          int64_t, void* user) {
    Ctx* c = static_cast<Ctx*>(user);
    if (addr == GPIO_WATCH && size == 4) {
        uc_mem_write(uc, addr, &c->gpio_cell, 4);
    }
    return true;
}

static bool read_file(const std::string& p, std::vector<uint8_t>& out) {
    FILE* f = fopen(p.c_str(), "rb");
    if (!f) return false;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    out.resize(static_cast<size_t>(n));
    size_t rd = fread(out.data(), 1, out.size(), f);
    fclose(f);
    return rd == out.size();
}

// Executa um blob ARM e devolve as metricas observadas.
static bool run_blob(const std::vector<uint8_t>& blob, Ctx& ctx, std::string& err) {
    uc_engine* uc = nullptr;
    if (uc_open(UC_ARCH_ARM, UC_MODE_ARM, &uc) != UC_ERR_OK) {
        err = "uc_open falhou";
        return false;
    }
    uc_mem_map(uc, APPS_RAM_BASE, APPS_RAM_SIZE, UC_PROT_ALL);
    uc_mem_map(uc, GPIO_BASE, GPIO_SIZE, UC_PROT_ALL);
    uc_mem_map(uc, MMIO_BASE, MMIO_SIZE, UC_PROT_ALL);

    if (uc_mem_write(uc, LOAD_ADDR, blob.data(), blob.size()) != UC_ERR_OK) {
        err = "uc_mem_write falhou";
        uc_close(uc);
        return false;
    }

    uc_hook h1, h2, h3;
    uc_hook_add(uc, &h1, UC_HOOK_CODE, (void*)hook_code, &ctx, 1, 0);
    uc_hook_add(uc, &h2, UC_HOOK_MEM_WRITE, (void*)hook_mem_write, &ctx, 1, 0);
    uc_hook_add(uc, &h3, UC_HOOK_MEM_READ, (void*)hook_mem_read, &ctx, 1, 0);

    // Limite generoso: o delay loop sozinho e ~1.5M instrucoes por volta.
    const u64 MAX_INSNS = 40ULL * 1000 * 1000;
    uc_err e = uc_emu_start(uc, LOAD_ADDR, LOAD_ADDR + blob.size(), 0, MAX_INSNS);
    if (e != UC_ERR_OK && e != UC_ERR_FETCH_UNMAPPED && e != UC_ERR_INSN_INVALID) {
        // O binario e um loop infinito; parar por limite/fetch e esperado.
        if (e != UC_ERR_OK) {
            // nao tratamos como erro fatal, apenas registramos
        }
    }
    uc_close(uc);
    return true;
}

int main(int argc, char** argv) {
    bool selftest = (argc > 1 && std::string(argv[1]) == "--selftest");

    std::string path = "../../testdata/tripleoxygen/blinkenlichten.bin";
    if (const char* p = getenv("ZEEBO_O3_BLINK")) path = p;

    std::vector<uint8_t> blob;
    if (!read_file(path, blob)) {
        // Tentativa a partir da raiz do repo.
        if (!read_file("testdata/tripleoxygen/blinkenlichten.bin", blob)) {
            fprintf(stderr,
                    "SKIP: blinkenlichten.bin nao encontrado (%s).\n"
                    "      Baixe de tripleoxygen.net/files/devices/zeebo/code/\n",
                    path.c_str());
            return 77;
        }
    }

    // ---- CONTROLE POSITIVO DO INSTRUMENTO ----
    // Blob sintetico que escreve 0x8000 e depois 0x0000 no GPIO observado.
    // Se o instrumento nao detectar ESTE toggle, ele nao pode ser usado como
    // veredito sobre o binario real.
    {
        const u32 pos[] = {
            0xe59f0018,  // ldr r0,[pc,#0x18]  -> 0xa9200800
            0xe3a01902,  // mov r1,#0x8000
            0xe5801000,  // str r1,[r0]
            0xe3a01000,  // mov r1,#0
            0xe5801000,  // str r1,[r0]
            0xe3a01902,  // mov r1,#0x8000
            0xe5801000,  // str r1,[r0]
            0xeafffffe,  // b .
            0xa9200800,  // literal
        };
        std::vector<uint8_t> pb(sizeof(pos));
        memcpy(pb.data(), pos, sizeof(pos));
        Ctx pc_ctx;
        std::string perr;
        run_blob(pb, pc_ctx, perr);
        size_t tog = 0;
        for (size_t i = 1; i < pc_ctx.gpio_writes.size(); ++i)
            if (((pc_ctx.gpio_writes[i] ^ pc_ctx.gpio_writes[i-1]) & TOGGLE_BIT) != 0) tog++;
        printf("[controle positivo] writes=%zu toggles=%zu\n",
               pc_ctx.gpio_writes.size(), tog);
        if (tog < 2) {
            fprintf(stderr,
                    "FALHA DE INSTRUMENTO: o detector nao viu toggles que existem "
                    "por construcao. Qualquer veredito sobre o binario real seria "
                    "sem valor.\n");
            return 1;
        }
        if (selftest) {
            printf("SELFTEST OK: instrumento detecta toggle real de GPIO.\n");
            return 0;
        }
    }

    // ---- CARGA REAL ----
    printf("== blinkenlichten.bin (%zu bytes) ==\n", blob.size());
    Ctx ctx;
    std::string err;
    if (!run_blob(blob, ctx, err)) {
        fprintf(stderr, "ERRO: %s\n", err.c_str());
        return 1;
    }

    size_t toggles = 0;
    for (size_t i = 1; i < ctx.gpio_writes.size(); ++i)
        if (((ctx.gpio_writes[i] ^ ctx.gpio_writes[i-1]) & TOGGLE_BIT) != 0) toggles++;

    bool b1 = ctx.insns >= 0x80000;
    bool b2 = !ctx.gpio_writes.empty();
    bool b3 = toggles >= 2;

    printf("  instrucoes executadas : %llu\n", (unsigned long long)ctx.insns);
    printf("  escritas em 0x%08x : %zu\n", GPIO_WATCH, ctx.gpio_writes.size());
    printf("  toggles do bit 15     : %zu\n", toggles);
    if (!ctx.gpio_writes.empty()) {
        printf("  primeiros valores     :");
        for (size_t i = 0; i < ctx.gpio_writes.size() && i < 6; ++i)
            printf(" 0x%08x", ctx.gpio_writes[i]);
        printf("\n");
    }
    printf("\n  B1 delay loop completo  : %s\n", b1 ? "PASS" : "FAIL");
    printf("  B2 store no GPIO        : %s\n", b2 ? "PASS" : "FAIL");
    printf("  B3 bit 15 alterna       : %s\n", b3 ? "PASS" : "FAIL");

    if (b1 && b2 && b3) {
        printf("\nGREEN: codigo ARM autoral do TripleOxygen executa com semantica correta.\n");
        return 0;
    }
    printf("\nRED: a CPU nao reproduz o comportamento derivado do desassembly.\n");
    return 1;
}
