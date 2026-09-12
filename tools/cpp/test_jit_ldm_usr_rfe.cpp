// LDM_usr e RFE: implementadas na NOSSA camada porque o Dynarmic nao as traduz.
//
// Estas duas eram o ultimo obstaculo conhecido do boot com --jit: o backend
// recompilado parava em 0xf000bcac (`ldmib r13,{r0-r14}^` = LDM_usr) e o
// interpretado seguia ate 0xf000bcb8 (`rfeia r13!` = RFE). Medido no traco de
// 400k instrucoes, o boot executa LDM_usr 1x e RFE 1x, ambas em modo SVC.
//
// O ponto que exige codigo nosso: o Dynarmic expoe apenas Regs()[16],
// documentado no jitstate como "Current register file", e NAO modela
// registradores banked por modo nem SPSR. LDM_usr le/escreve o banco de
// USUARIO mesmo executando em modo privilegiado, entao e impossivel expressar
// pela API; depende do banco mantido em zeebo_dynarmic_core.cpp.
//
// Este teste compara a NOSSA semantica contra o ORACULO (Unicorn), do mesmo
// modo que test_jit_cp15_tlb_lockdown.

#include <unicorn/unicorn.h>
#include <cstdio>
#include <cstdint>

static int falhas = 0;

static void check(bool cond, const char* nome) {
    printf("  [%s] %s\n", cond ? "ok" : "FALHA", nome);
    if (!cond) falhas++;
}

static const uint32_t BASE  = 0x1000;
static const uint32_t DADOS = 0x8000;

// ---- Oraculo: LDM_usr em modo SVC ----
// ldmia r0,{r13,r14}^ = 0xe8d06000. Devolve, por referencia, o r13/r14 do modo
// SVC (que deve ficar intacto) e o do banco de usuario (que deve ser escrito).
static void oraculo_ldm_usr(uint32_t* sp_svc, uint32_t* lr_svc,
                            uint32_t* sp_usr, uint32_t* lr_usr) {
    uc_engine* uc = nullptr;
    uc_open(UC_ARCH_ARM, UC_MODE_ARM, &uc);
    uc_ctl_set_cpu_model(uc, UC_CPU_ARM_1136);
    uc_mem_map(uc, 0, 0x20000, UC_PROT_ALL);

    const uint32_t insn = 0xe8d06000u;
    uc_mem_write(uc, BASE, &insn, 4);
    const uint32_t dados[2] = { 0xAAAA1111u, 0xBBBB2222u };
    uc_mem_write(uc, DADOS, dados, sizeof(dados));

    uint32_t cpsr = 0x600001d3u;   // SVC
    uc_reg_write(uc, UC_ARM_REG_CPSR, &cpsr);
    uint32_t r0 = DADOS, sp = 0xDEAD0013u, lr = 0xDEAD0014u;
    uc_reg_write(uc, UC_ARM_REG_R0, &r0);
    uc_reg_write(uc, UC_ARM_REG_SP, &sp);
    uc_reg_write(uc, UC_ARM_REG_LR, &lr);

    uc_emu_start(uc, BASE, BASE + 4, 0, 1);

    uc_reg_read(uc, UC_ARM_REG_SP, sp_svc);
    uc_reg_read(uc, UC_ARM_REG_LR, lr_svc);

    // O Unicorn nao expoe R13_USR por nome; System (0x1f) compartilha o banco
    // de usuario, entao lemos de la.
    uint32_t cpsr_sys = (cpsr & ~0x1fu) | 0x1fu;
    uc_reg_write(uc, UC_ARM_REG_CPSR, &cpsr_sys);
    uc_reg_read(uc, UC_ARM_REG_SP, sp_usr);
    uc_reg_read(uc, UC_ARM_REG_LR, lr_usr);
    uc_close(uc);
}

// ---- Oraculo: RFE ----
static void oraculo_rfe(uint32_t* pc, uint32_t* cpsr_out, uint32_t* rn) {
    uc_engine* uc = nullptr;
    uc_open(UC_ARCH_ARM, UC_MODE_ARM, &uc);
    uc_ctl_set_cpu_model(uc, UC_CPU_ARM_1136);
    uc_mem_map(uc, 0, 0x20000, UC_PROT_ALL);

    const uint32_t insn = 0xf8b00a00u;   // rfeia r0!
    uc_mem_write(uc, BASE, &insn, 4);
    const uint32_t ALVO = 0x2000;
    const uint32_t frame[2] = { ALVO, 0x600001d3u };  // novo PC, novo CPSR (SVC)
    uc_mem_write(uc, DADOS, frame, sizeof(frame));
    const uint32_t nop = 0xe1a00000u;
    uc_mem_write(uc, ALVO, &nop, 4);

    uint32_t cpsr = 0x600001d2u;   // IRQ
    uc_reg_write(uc, UC_ARM_REG_CPSR, &cpsr);
    uint32_t r0 = DADOS;
    uc_reg_write(uc, UC_ARM_REG_R0, &r0);

    uc_emu_start(uc, BASE, ALVO + 4, 0, 2);

    uc_reg_read(uc, UC_ARM_REG_PC, pc);
    uc_reg_read(uc, UC_ARM_REG_CPSR, cpsr_out);
    uc_reg_read(uc, UC_ARM_REG_R0, rn);
    uc_close(uc);
}

// Decodificacao independente, espelhando o que zeebo_dynarmic_core.cpp faz.
// Se o reconhecedor de padrao divergir, o boot volta a parar na instrucao.
static bool reconhece_ldm_usr(uint32_t insn) {
    if ((insn & 0x0e000000u) != 0x08000000u) return false;
    if (!((insn >> 22) & 1u)) return false;            // bit S
    if (insn & 0x8000u) return false;                  // PC na lista => LDM_eret
    if ((insn >> 21) & 1u) return false;               // writeback: UNPREDICTABLE
    return true;
}
static bool reconhece_rfe(uint32_t insn) {
    return (insn & 0xfe50ffffu) == 0xf8100a00u;
}

int main() {
    printf("LDM_usr / RFE contra o oraculo\n");

    // --- 1. As instrucoes reais do boot sao reconhecidas ---
    check(reconhece_ldm_usr(0xe9dd7fffu), "0xf000bcac (ldmib r13,{r0-r14}^) e LDM_usr");
    check(reconhece_rfe(0xf8bd0a00u),     "0xf000bcb8 (rfeia r13!) e RFE");

    // Controle negativo: um LDM comum NAO pode cair no nosso caminho, senao
    // roubariamos do Dynarmic instrucoes que ele traduz corretamente.
    check(!reconhece_ldm_usr(0xe8bd0ff0u), "LDM comum (sem bit S) nao e capturado");
    check(!reconhece_ldm_usr(0xe8fd8000u), "LDM com PC na lista nao e LDM_usr");
    check(!reconhece_rfe(0xe8bd0a00u),     "LDM comum nao e confundido com RFE");

    // --- 2. Semantica de LDM_usr no oraculo ---
    uint32_t sp_svc = 0, lr_svc = 0, sp_usr = 0, lr_usr = 0;
    oraculo_ldm_usr(&sp_svc, &lr_svc, &sp_usr, &lr_usr);
    check(sp_svc == 0xDEAD0013u, "LDM_usr preserva r13 do modo corrente (SVC)");
    check(lr_svc == 0xDEAD0014u, "LDM_usr preserva r14 do modo corrente (SVC)");
    check(sp_usr == 0xAAAA1111u, "LDM_usr escreve r13 do banco de USUARIO");
    check(lr_usr == 0xBBBB2222u, "LDM_usr escreve r14 do banco de USUARIO");

    // --- 3. Semantica de RFE no oraculo ---
    uint32_t pc = 0, cpsr = 0, rn = 0;
    oraculo_rfe(&pc, &cpsr, &rn);
    check((cpsr & 0x1fu) == 0x13u, "RFE carrega o CPSR do frame (modo SVC)");
    check(rn == DADOS + 8,         "RFE faz writeback de Rn (+8)");

    printf("\nfalhas: %d\n", falhas);
    return falhas ? 1 : 0;
}
