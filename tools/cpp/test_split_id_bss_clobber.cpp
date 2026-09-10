// test_split_id_bss_clobber.cpp — QW56
//
// PROVA a causa raiz do panic "Failed to create root server TCB":
// o Split I/D do heap REX serve leituras a partir do shadow/pristino E reescreve
// a RAM do Unicorn, apagando variaveis globais do kernel (.bss) que ja' foram
// escritas. O pristino do .bss e' zero (sem filesz no ELF), entao o valor escrito
// pelo kernel desaparece.
//
// Este teste reproduz a mecanica em miniatura, sem depender da AMSS:
//   1. janela de "heap" com pristino zerado numa regiao que representa .bss
//   2. escreve um valor (como o init_tcb_allocator faz)
//   3. dispara uma LEITURA na janela (o gatilho)
//   4. exige que o valor escrito sobreviva
//
// RED esperado com a logica atual (serve pristino por cima): valor vira 0.
// GREEN apos restringir a janela ao .text executavel.
//
// exit 0 = passou | exit 1 = corrupcao detectada | exit 77 = SKIP (sem unicorn)

#include <cstdio>
#include <cstring>
#include <cstdint>
#include <vector>
#include <unicorn/unicorn.h>

typedef uint32_t u32;
typedef uint8_t  u8;

static const u32 HEAP_BASE = 0xf0000000u;
static const u32 HEAP_SIZE = 0x00200000u;   // 2MB — a janela real do emulador
static const u32 TEXT_FIM  = 0x0001a324u;   // filesz do seg1: fim do .text/.data
static const u32 BSS_VAR   = 0xf001a538u;   // campo +0xc da struct do alocador de TCB
static const u32 VALOR     = 0x00000100u;   // o que init_tcb_allocator escreve

static std::vector<u8> pristino;            // visao do ARQUIVO (.bss = zeros)
static std::vector<u8> shadow;              // visao de DADOS

static bool in_heap(u32 a) { return a >= HEAP_BASE && a < HEAP_BASE + HEAP_SIZE; }

// Reproduz c1_heap_read_hook: serve o shadow e REESCREVE a RAM do Unicorn.
static void read_hook(uc_engine* uc, uc_mem_type type, uint64_t addr,
                      int size, int64_t, void*) {
    if (type != UC_MEM_READ) return;
    u32 a = (u32)addr;
    if (!in_heap(a)) return;
    u32 off = a - HEAP_BASE;
    if (off + (u32)size > shadow.size()) return;

    // QW56: espelha a guarda de producao (c1_heap_read_hook / REX_KERNEL_FILESZ).
    // O `.bss` (>= TEXT_FIM) nunca pode ser servido do pristino: la' o pristino
    // e' zero por construcao e apagaria globais ja' escritas pelo kernel.
    // RED: comentar a linha abaixo faz o teste falhar (exit 1) — foi assim que a
    // causa do panic thread.cc:1273 foi provada.
    if (off >= TEXT_FIM) return;

    uc_mem_write(uc, a, &pristino[off], size);
}

static void write_hook(uc_engine*, uc_mem_type type, uint64_t addr,
                       int size, int64_t value, void*) {
    if (type != UC_MEM_WRITE) return;
    u32 a = (u32)addr;
    if (!in_heap(a)) return;
    u32 off = a - HEAP_BASE;
    if (off + (u32)size > shadow.size()) return;
    for (int i = 0; i < size; i++)
        shadow[off + i] = (u8)((value >> (8 * i)) & 0xff);
}

int main() {
    uc_engine* uc = nullptr;
    if (uc_open(UC_ARCH_ARM, UC_MODE_ARM, &uc) != UC_ERR_OK) {
        fprintf(stderr, "SKIP: unicorn indisponivel\n");
        return 77;
    }
    if (uc_mem_map(uc, HEAP_BASE, HEAP_SIZE, UC_PROT_ALL) != UC_ERR_OK) {
        fprintf(stderr, "SKIP: uc_mem_map falhou\n");
        uc_close(uc); return 77;
    }

    pristino.assign(HEAP_SIZE, 0);   // .bss = zeros no arquivo (fato do ELF)
    shadow = pristino;

    uc_hook h_r, h_w;
    uc_hook_add(uc, &h_r, UC_HOOK_MEM_READ,  (void*)read_hook,  nullptr, 1, 0);
    uc_hook_add(uc, &h_w, UC_HOOK_MEM_WRITE, (void*)write_hook, nullptr, 1, 0);

    // 1) o kernel (init_tcb_allocator) escreve na global: `str r3,[ip,#0xc]`
    //    Usamos codigo EMULADO para passar pelos hooks (uc_mem_write e API externa).
    const u32 CODE = 0xf0100000u;
    uc_mem_map(uc, CODE, 0x1000, UC_PROT_ALL);
    // r0=BSS_VAR, r1=VALOR, str r1,[r0] ; ldr r2,[r0] ; ldr r3,[r0+4] ; ldr r2,[r0]
    const u8 prog[] = {
        0x00, 0x10, 0x80, 0xe5,   // str  r1, [r0]
        0x04, 0x30, 0x90, 0xe5,   // ldr  r3, [r0, #4]   <- LEITURA vizinha (o gatilho)
        0x00, 0x20, 0x90, 0xe5,   // ldr  r2, [r0]       <- releitura da global
    };
    uc_mem_write(uc, CODE, prog, sizeof(prog));
    u32 r0 = BSS_VAR, r1 = VALOR;
    uc_reg_write(uc, UC_ARM_REG_R0, &r0);
    uc_reg_write(uc, UC_ARM_REG_R1, &r1);

    uc_err e = uc_emu_start(uc, CODE, CODE + sizeof(prog), 0, 0);
    if (e != UC_ERR_OK) {
        fprintf(stderr, "SKIP: uc_emu_start: %s\n", uc_strerror(e));
        uc_close(uc); return 77;
    }

    u32 lido = 0;
    uc_reg_read(uc, UC_ARM_REG_R2, &lido);

    u32 sh = 0;
    memcpy(&sh, &shadow[BSS_VAR - HEAP_BASE], 4);

    printf("escrito=0x%08x  shadow=0x%08x  lido_pelo_guest=0x%08x\n",
           VALOR, sh, lido);

    // CONTROLE POSITIVO do instrumento: o shadow TEM que ter registrado a escrita.
    // Se nem o shadow tiver o valor, o teste esta' medindo a coisa errada.
    if (sh != VALOR) {
        fprintf(stderr, "FALHA DE INSTRUMENTO: o shadow nao registrou a escrita "
                        "(0x%08x != 0x%08x) — teste invalido, nao conclua nada.\n",
                sh, VALOR);
        uc_close(uc); return 2;
    }

    uc_close(uc);

    if (lido != VALOR) {
        fprintf(stderr,
            "CORRUPCAO: o guest escreveu 0x%08x na global 0x%08x (.bss) e releu 0x%08x.\n"
            "O Split I/D serviu o pristino (zeros) por cima da escrita.\n"
            "E' esta a causa do panic 'Failed to create root server TCB'.\n",
            VALOR, BSS_VAR, lido);
        return 1;
    }

    printf("OK: a global sobreviveu a leitura vizinha na janela do Split I/D.\n");
    return 0;
}
