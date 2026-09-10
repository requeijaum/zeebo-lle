// Regressao: no Split I/D do heap REX, um load PC-relative (literal pool) deve
// ler o CODIGO pristino, nao o shadow de dados.
//
// Motivacao (medido no boot real do Core1):
//   add_mapping (0xf0002abc) faz `ldr ip,[pc,#0xb0]` para buscar seu limite em
//   0xf0002b78. O heap kmem do OKL4 e' (f0000000, f0200000) -- ele COBRE o .text
//   do proprio kernel -- e os lacos de zeragem em f0002ca4/f000afcc zeram
//   f0000008..f00060b8, apagando esse literal no shadow de dados.
//   Em silicio nao existe Split I/D: o `ldr` PC-relative le o .text e enxerga
//   0x61. No nosso modelo ele era servido pelo shadow e lia 0 -> add_mapping
//   retornava 0 -> "Assertion r != 0 failed in file pistachio/arch/arm/src/init.cc".
//
// CONTROLE NEGATIVO embutido: com a politica antiga (literal servido pelo shadow)
// o teste DEVE falhar. Sem isso, ele nao provaria nada.
#include <unicorn/unicorn.h>
#include <cstdio>
#include <cstring>
#include <cstdint>
#include <vector>
#include <set>

typedef uint32_t u32;
typedef uint8_t  u8;

static const u32 BASE = 0xf0000000;
static const u32 SIZE = 0x00200000;

struct Ctx {
    std::vector<u8> pristine;   // visao de INSTRUCAO (.text real)
    std::vector<u8> shadow;     // visao de DADO (heap do REX)
    std::set<u32>   dirty;
    bool            literal_from_pristine; // politica sob teste
};

// Espelha c1_heap_read_hook: injeta a visao de dado antes da leitura.
static void read_hook(uc_engine* uc, uc_mem_type type, uint64_t addr,
                      int size, int64_t, void* ud) {
    if (type != UC_MEM_READ) return;
    Ctx* c = (Ctx*)ud;
    u32 a = (u32)addr;
    if (a < BASE || a >= BASE + SIZE) return;
    u32 off = a - BASE, n = (u32)size;
    if (off + n > c->shadow.size()) return;

    if (c->literal_from_pristine) {
        // Um load PC-relative busca uma constante embutida no .text.
        // Detecta decodificando a instrucao que esta executando.
        u32 pc = 0;
        uc_reg_read(uc, UC_ARM_REG_PC, &pc);
        u32 insn = 0;
        if (pc >= BASE && pc + 4 <= BASE + SIZE)
            memcpy(&insn, &c->pristine[pc - BASE], 4);
        const bool is_ldr_imm = ((insn & 0x0e000000u) == 0x04000000u);
        const bool rn_is_pc   = (((insn >> 16) & 0xfu) == 15u);
        const bool is_load    = ((insn >> 20) & 1u) != 0;
        if (is_ldr_imm && rn_is_pc && is_load) {
            uc_mem_write(uc, a, &c->pristine[off], n);   // le o .text
            return;
        }
    }
    uc_mem_write(uc, a, &c->shadow[off], n);
    for (u32 w = a & ~3u; w < a + n; w += 4) c->dirty.insert(w);
}

// Roda `ldr r0,[pc,#0x0]` + literal, com o literal ZERADO no shadow.
// Retorna o valor que r0 enxergou.
static u32 run(bool literal_from_pristine, const char* rotulo) {
    uc_engine* uc = nullptr;
    if (uc_open(UC_ARCH_ARM, UC_MODE_ARM, &uc) != UC_ERR_OK) {
        printf("  [%s] uc_open falhou\n", rotulo);
        return 0xdeadbeef;
    }
    uc_ctl_set_cpu_model(uc, UC_CPU_ARM_926);
    uc_mem_map(uc, BASE, SIZE, UC_PROT_ALL);

    Ctx c;
    c.pristine.assign(SIZE, 0);
    c.literal_from_pristine = literal_from_pristine;

    // .text: ldr r0,[pc,#0]; b .;  literal = 0x00000061
    const u32 code_at = 0xf0002abc;
    const u32 lit_at  = code_at + 8;          // pc(+8) + 0
    u32 insn_ldr = 0xe59f0000;                // ldr r0,[pc,#0]
    u32 insn_b   = 0xeafffffe;                // b .
    u32 lit_val  = 0x00000061;
    memcpy(&c.pristine[code_at - BASE + 0], &insn_ldr, 4);
    memcpy(&c.pristine[code_at - BASE + 4], &insn_b,   4);
    memcpy(&c.pristine[lit_at  - BASE],     &lit_val,  4);

    // shadow nasce como copia, e entao o heap do REX zera a faixa do .text
    // (exatamente o que os lacos f0002ca4/f000afcc fazem no boot real).
    c.shadow = c.pristine;
    u32 zero = 0;
    memcpy(&c.shadow[lit_at - BASE], &zero, 4);

    uc_mem_write(uc, BASE, c.pristine.data(), SIZE);

    uc_hook h;
    uc_hook_add(uc, &h, UC_HOOK_MEM_READ, (void*)read_hook, &c, BASE, BASE + SIZE - 1);

    uc_emu_start(uc, code_at, code_at + 4, 0, 1);   // executa so o ldr

    u32 r0 = 0;
    uc_reg_read(uc, UC_ARM_REG_R0, &r0);
    uc_close(uc);
    printf("  [%s] r0 = 0x%08x\n", rotulo, r0);
    return r0;
}

int main() {
    printf("== test_rex_literal_pool ==\n");
    printf("literal 0xf0002ac4 = 0x61 no .text, 0x00000000 no shadow de dados\n");

    const u32 antigo = run(false, "politica ANTIGA (literal <- shadow)");
    const u32 novo   = run(true,  "politica NOVA   (literal <- pristino)");

    bool ok = true;

    // CONTROLE NEGATIVO: a politica antiga TEM de falhar, senao o teste e' cego.
    if (antigo != 0x00000000) {
        printf("FALHA[controle negativo]: politica antiga devolveu 0x%08x, esperava 0x0\n", antigo);
        printf("  (se nao reproduz o bug, este teste nao prova nada)\n");
        ok = false;
    } else {
        printf("OK[controle negativo]: politica antiga le 0 -> reproduz o bug do boot\n");
    }

    if (novo != 0x00000061) {
        printf("FALHA: politica nova devolveu 0x%08x, esperava 0x61\n", novo);
        ok = false;
    } else {
        printf("OK: load PC-relative le o .text pristino (0x61)\n");
    }

    printf("%s\n", ok ? "TESTE_EXIT=0" : "TESTE_EXIT=1");
    return ok ? 0 : 1;
}
