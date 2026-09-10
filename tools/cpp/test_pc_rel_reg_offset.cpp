// Regressao: o filtro de "load PC-relative" do Split I/D deve aceitar TAMBEM a
// forma com offset por REGISTRADOR (jump table), nao so a forma imediata.
//
// Medido no boot do Core1: a execucao morria em exatamente 73550 insns na
// instrucao f0004f74 = 979ff10c = `ldrls pc,[pc,r12,lsl#2]` (jump table do
// kernel OKL4). O filtro antigo exigia (insn & 0x0e000000) == 0x04000000, que
// so casa com offset imediato; a forma registrador vale 0x06000000. Assim esse
// load era servido do SHADOW (zerado pelos lacos de heap) e o PC ia para 0.
//
// Controle negativo embutido: a politica antiga REPROVA (nao reconhece 979ff10c),
// a nova APROVA -- e nenhuma das duas pode aceitar store nem base != pc.
#include <cstdio>
#include <cstdint>

typedef uint32_t u32;

// Politica ANTIGA (com o bug): so offset imediato.
static bool pc_rel_antiga(u32 insn) {
    const bool is_ldr_imm = ((insn & 0x0e000000u) == 0x04000000u);
    const bool rn_is_pc   = (((insn >> 16) & 0xfu) == 15u);
    const bool is_load    = ((insn >> 20) & 1u) != 0;
    return is_ldr_imm && rn_is_pc && is_load;
}

// Politica NOVA: aceita imediato (0x04) e registrador (0x06).
static bool pc_rel_nova(u32 insn) {
    const u32 classe      = insn & 0x0e000000u;
    const bool is_ldr     = (classe == 0x04000000u) || (classe == 0x06000000u);
    const bool rn_is_pc   = (((insn >> 16) & 0xfu) == 15u);
    const bool is_load    = ((insn >> 20) & 1u) != 0;
    return is_ldr && rn_is_pc && is_load;
}

struct Caso { u32 insn; bool esperado; const char* desc; };

int main() {
    const Caso casos[] = {
        // o caso que quebrava o boot
        { 0x979ff10cu, true,  "ldrls pc,[pc,r12,lsl#2]  (jump table, reg)" },
        // a forma que ja funcionava
        { 0xe59fc0b0u, true,  "ldr ip,[pc,#0xb0]        (literal pool, imm)" },
        { 0xe51f0010u, true,  "ldr r0,[pc,#-0x10]       (imm negativo)" },
        // negativos: base != pc
        { 0xe5941000u, false, "ldr r1,[r4]              (base r4, nao pc)" },
        { 0xe7802103u, false, "str r2,[r0,r3,lsl#2]     (store, base r0)" },
        // negativo: store com base pc
        { 0xe58f0010u, false, "str r0,[pc,#0x10]        (store, base pc)" },
        // negativo: nao e' classe de load/store
        { 0xe3a03000u, false, "mov r3,#0                (nao e' ldr/str)" },
    };
    const int N = (int)(sizeof(casos) / sizeof(casos[0]));

    int falhas_nova = 0;
    printf("== test_pc_rel_reg_offset ==\n");
    for (int i = 0; i < N; i++) {
        bool n = pc_rel_nova(casos[i].insn);
        bool ok = (n == casos[i].esperado);
        if (!ok) falhas_nova++;
        printf("  [%s] %-42s nova=%d esperado=%d\n",
               ok ? "OK " : "FAIL", casos[i].desc, n, casos[i].esperado);
    }

    // Controle negativo: a politica antiga TEM de reprovar o jump table.
    bool antiga_pega_jt = pc_rel_antiga(0x979ff10cu);
    printf("\ncontrole negativo: politica ANTIGA reconhece o jump table? %s\n",
           antiga_pega_jt ? "SIM" : "NAO");
    if (antiga_pega_jt) {
        printf("FALHA[controle]: o teste nao distingue as politicas -- invalido\n");
        return 1;
    }
    printf("  => teste valido: reprova o codigo antigo, aprova o novo\n");

    if (falhas_nova) {
        printf("TESTE_EXIT=1 (%d falha(s))\n", falhas_nova);
        return 1;
    }
    printf("TESTE_EXIT=0\n");
    return 0;
}
