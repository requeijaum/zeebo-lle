// Isola o caso `ldr pc,[pc,r12,lsl#2]` (jump table) servido pelo hook de leitura
// do Split I/D, para responder: escrever na memoria DENTRO do hook, numa
// instrucao cujo destino e o PC, faz o Unicorn parar em silencio?
//
// Contexto medido no boot do Core1: a execucao termina sempre em exatamente
// 73550 insns, na instrucao f0004f74 = `ldrls pc,[pc,r12,lsl#2]`, com r12=3 e
// alvo 0xf0004f8c (endereco VALIDO). uc_emu_start devolve UC_ERR_OK sem erro.
//
// Este teste NAO assume culpa do hook: compara as tres situacoes e deixa os
// numeros decidirem.
#include <unicorn/unicorn.h>
#include <cstdio>
#include <cstring>
#include <cstdint>
#include <vector>

typedef uint32_t u32;
typedef uint8_t  u8;

static const u32 BASE = 0xf0000000;
static const u32 SIZE = 0x00010000;

struct Ctx {
    std::vector<u8> pristine;
    bool serve_from_hook;   // hook faz uc_mem_write durante a leitura
    long reads = 0;
};

static void read_hook(uc_engine* uc, uc_mem_type type, uint64_t addr,
                      int size, int64_t, void* ud) {
    if (type != UC_MEM_READ) return;
    Ctx* c = (Ctx*)ud;
    u32 a = (u32)addr;
    if (a < BASE || a >= BASE + SIZE) return;
    c->reads++;
    if (!c->serve_from_hook) return;
    u32 off = a - BASE, n = (u32)size;
    if (off + n <= c->pristine.size())
        uc_mem_write(uc, a, &c->pristine[off], n);   // o que o fix faz hoje
}

// Monta: cmp r12,#3 ; ldrls pc,[pc,r12,lsl#2] ; <tabela> ; alvo: mov r0,#0xAA ; b .
// Retorna quantas insns executaram e o r0 final.
static void run(bool com_hook, bool serve, const char* rotulo,
                uint64_t* out_insns, u32* out_r0, const char** out_err) {
    uc_engine* uc = nullptr;
    uc_open(UC_ARCH_ARM, UC_MODE_ARM, &uc);
    uc_ctl_set_cpu_model(uc, UC_CPU_ARM_926);
    uc_mem_map(uc, BASE, SIZE, UC_PROT_ALL);

    Ctx c;
    c.pristine.assign(SIZE, 0);
    c.serve_from_hook = serve;

    const u32 start = BASE + 0x4f70;
    u32 insn_cmp = 0xe35c0003;   // cmp r12, #3
    u32 insn_ldr = 0x979ff10c;   // ldrls pc,[pc,r12,lsl#2]
    memcpy(&c.pristine[start - BASE + 0], &insn_cmp, 4);
    memcpy(&c.pristine[start - BASE + 4], &insn_ldr, 4);

    // tabela em pc+8 = start+0xc ; entrada [3] -> alvo
    const u32 tab = start + 0xc;
    const u32 alvo = tab + 0x10;
    for (u32 k = 0; k < 4; k++) {
        u32 v = alvo;
        memcpy(&c.pristine[tab - BASE + 4*k], &v, 4);
    }
    u32 insn_mov = 0xe3a000aa;   // mov r0, #0xaa
    u32 insn_b   = 0xeafffffe;   // b .
    memcpy(&c.pristine[alvo - BASE + 0], &insn_mov, 4);
    memcpy(&c.pristine[alvo - BASE + 4], &insn_b,   4);

    uc_mem_write(uc, BASE, c.pristine.data(), SIZE);

    u32 r12 = 3;
    uc_reg_write(uc, UC_ARM_REG_R12, &r12);
    u32 zero = 0;
    uc_reg_write(uc, UC_ARM_REG_R0, &zero);

    uc_hook h;
    if (com_hook)
        uc_hook_add(uc, &h, UC_HOOK_MEM_READ, (void*)read_hook, &c, BASE, BASE + SIZE - 1);

    // conta instrucoes executadas
    static uint64_t contador;
    contador = 0;
    uc_hook hc;
    uc_hook_add(uc, &hc, UC_HOOK_CODE,
                (void*)+[](uc_engine*, uint64_t, uint32_t, void*) { contador++; },
                nullptr, BASE, BASE + SIZE - 1);

    uc_err e = uc_emu_start(uc, start, 0, 0, 20);
    uc_reg_read(uc, UC_ARM_REG_R0, out_r0);
    *out_insns = contador;
    *out_err = uc_strerror(e);
    printf("  [%-34s] insns=%2llu r0=0x%02x leituras_hook=%ld err=%s\n",
           rotulo, (unsigned long long)contador, *out_r0, c.reads, *out_err);
    uc_close(uc);
}

int main() {
    printf("== test_jt_pc_load_hook ==\n");
    printf("ldrls pc,[pc,r12,lsl#2] com r12=3 -> alvo valido (mov r0,#0xaa; b .)\n");

    uint64_t i_sem, i_passivo, i_serve;
    u32 r_sem, r_passivo, r_serve;
    const char *e_sem, *e_passivo, *e_serve;

    run(false, false, "sem hook (referencia)",        &i_sem,     &r_sem,     &e_sem);
    run(true,  false, "hook passivo (so observa)",    &i_passivo, &r_passivo, &e_passivo);
    run(true,  true,  "hook serve (uc_mem_write)",    &i_serve,   &r_serve,   &e_serve);

    bool ok = true;

    // Referencia: o salto tem de funcionar e chegar no alvo.
    if (r_sem != 0xaa) {
        printf("FALHA[referencia]: sem hook o jump table nao chegou no alvo (r0=0x%02x)\n", r_sem);
        ok = false;
    } else {
        printf("OK[referencia]: sem hook o jump table chega no alvo\n");
    }

    // A pergunta do teste: servir pelo hook muda o resultado?
    if (r_serve != r_sem || i_serve != i_sem) {
        printf("VEREDITO: servir a leitura no hook ALTERA a execucao "
               "(insns %llu -> %llu, r0 0x%02x -> 0x%02x)\n",
               (unsigned long long)i_sem, (unsigned long long)i_serve, r_sem, r_serve);
        printf("  => o fix do literal pool quebra loads cujo destino e o PC\n");
        ok = false;
    } else {
        printf("OK: servir a leitura no hook NAO altera o jump table\n");
        printf("  => a parada em 73550 insns tem OUTRA causa; hipotese REFUTADA\n");
    }

    printf("%s\n", ok ? "TESTE_EXIT=0" : "TESTE_EXIT=1");
    return ok ? 0 : 1;
}
