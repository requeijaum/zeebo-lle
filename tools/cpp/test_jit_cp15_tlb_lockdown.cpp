// TLB lockdown (CP15 c10) e um registrador "reads ignored": ler NAO altera o
// registrador de destino.
//
// Contexto (divergencia #67395 do boot, apos f4bac04):
//
//     #67392  mov  r0, #0xc0        -> r0 = 0xc0 nos DOIS backends
//     #67393  ...                      r0 = 0xc0 nos DOIS backends
//     #67394  MRC p15,0,r0,c10,c0,0 -> AQUI os caminhos se separam
//     #67395  ...                      r0 = 0xc0 (interp)  vs  0 (recompilado)
//
// Ou seja: r0 ja valia 0xc0 ANTES da leitura, e nenhuma escrita a c10 ocorreu
// antes no traco. O interpretado PRESERVA o valor de r0; o nosso banco CP15
// generico trata c10 como um registrador comum, devolve o conteudo do slot
// (zero, nunca escrito) e SOBRESCREVE r0 com 0.
//
// O fonte do QEMU (target/arm/helper.c) declara, para ARMv6/v7:
//
//     { .name = "TLB_LOCKDOWN", .cp = 15, .crn = 10, .crm = 0,
//       .opc1 = CP_ANY, .opc2 = CP_ANY, .access = PL1_RW, .type = ARM_CP_NOP }
//
// e cpregs.h define ARM_CP_NOP como
//
//     "Special: no change to PE state: writes ignored, reads ignored."
//
// Confirmado empiricamente: num engine limpo o oraculo devolve 0 para
// MRC c10 — nao 0xc0. Logo 0xc0 nao vem de um valor de reset; vem de r0 ter
// sido PRESERVADO. Este teste trava esse contrato.

#include <unicorn/unicorn.h>
#include <cstdio>
#include <cstdint>

static int falhas = 0;

static void check(bool cond, const char* nome) {
    printf("  [%s] %s\n", cond ? "ok" : "FALHA", nome);
    if (!cond) falhas++;
}

// Executa "mov rX,#imm ; MRC p15,0,rX,c10,crm,opc2" e devolve rX final.
static uint32_t roda_mrc_c10(uint32_t semente, unsigned crm, unsigned opc2) {
    uc_engine* uc = nullptr;
    uc_open(UC_ARCH_ARM, UC_MODE_ARM, &uc);
    uc_ctl_set_cpu_model(uc, UC_CPU_ARM_1136);

    const uint32_t BASE = 0x1000;
    uc_mem_map(uc, BASE, 0x1000, UC_PROT_ALL);

    // MRC p15,0,r0,c10,crm,opc2
    const uint32_t insn = 0xee100f10u | ((10u & 0xf) << 16) | ((opc2 & 7u) << 5) | (crm & 0xfu);
    uc_mem_write(uc, BASE, &insn, 4);

    uc_reg_write(uc, UC_ARM_REG_R0, &semente);
    uc_emu_start(uc, BASE, BASE + 4, 0, 1);

    uint32_t r0 = 0;
    uc_reg_read(uc, UC_ARM_REG_R0, &r0);
    uc_close(uc);
    return r0;
}

int main() {
    printf("== CP15 c10 (TLB lockdown): leitura nao altera o destino ==\n");

    // O caso exato do boot: r0 = 0xc0 antes da leitura.
    const uint32_t SEMENTE = 0x000000c0;
    uint32_t r0 = roda_mrc_c10(SEMENTE, 0, 0);
    check(r0 == SEMENTE,
          "MRC c10,c0,0 PRESERVA r0 = 0xc0 (nao zera)");

    // Controle: com outra semente o valor tambem tem de ser preservado.
    // Se o registrador tivesse conteudo proprio, o resultado seria o mesmo
    // para as duas sementes — este controle distingue "preserva" de
    // "devolve constante".
    const uint32_t OUTRA = 0xa5a5a5a5;
    uint32_t r0b = roda_mrc_c10(OUTRA, 0, 0);
    check(r0b == OUTRA,
          "controle: com outra semente, tambem PRESERVA (nao e constante fixa)");
    check(r0 != r0b,
          "controle negativo: as duas sementes dao resultados DIFERENTES");

    // Se o valor fosse um conteudo de registrador, ambas as leituras
    // devolveriam a mesma coisa (tipicamente 0). Provar que nao devolvem.
    check(r0b != 0,
          "a leitura NAO devolve o conteudo do slot (que seria 0)");

    printf("\n  => c10 e 'reads ignored': o destino fica intacto. Um banco CP15\n");
    printf("     generico que devolve o slot zerado QUEBRA este contrato.\n");

    printf("\n%s\n", falhas ? "RESULTADO: FALHOU" : "RESULTADO: PASSOU");
    return falhas ? 1 : 0;
}
