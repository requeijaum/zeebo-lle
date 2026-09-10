// Regressao: uc_ctl_set_cpu_model DEVE ser chamado ANTES de uc_ctl_tlb_mode.
//
// Com a ordem invertida o Unicorn devolve UC_ERR_ARG em set_cpu_model e ignora
// o modelo em silencio. O Core1 ficava no CPU default em vez de ARM926 e
// rejeitava `mrc p15,0,apsr_nzcv,c7,c14,3` (test-and-clean dcache, so existe no
// ARM9) com UC_ERR_INSN_INVALID -- travando o boot em 0xf001833c.
//
// Este teste falha no codigo ANTIGO (ordem invertida) e passa no novo.
#include <unicorn/unicorn.h>
#include <cstdio>

// mrc p15, 0, apsr_nzcv, c7, c14, 3
static const unsigned char MRC_C7_C14_3[] = {0x7e, 0xff, 0x17, 0xee};

static int falhas = 0;

static void checa(bool cond, const char* msg) {
    if (!cond) { std::printf("  [FALHA] %s\n", msg); falhas++; }
    else       { std::printf("  [ok] %s\n", msg); }
}

// Monta um Core1 com a ordem pedida e devolve os tres codigos de erro.
static void monta(bool modelo_primeiro, uc_err* e_model, uc_err* e_tlb, uc_err* e_exec) {
    uc_engine* uc = nullptr;
    uc_open(UC_ARCH_ARM, UC_MODE_ARM, &uc);

    if (modelo_primeiro) {
        *e_model = uc_ctl_set_cpu_model(uc, UC_CPU_ARM_926);
        *e_tlb   = uc_ctl_tlb_mode(uc, UC_TLB_VIRTUAL);
    } else {
        *e_tlb   = uc_ctl_tlb_mode(uc, UC_TLB_VIRTUAL);
        *e_model = uc_ctl_set_cpu_model(uc, UC_CPU_ARM_926);
    }

    uc_mem_map(uc, 0xf0018000, 0x1000, UC_PROT_ALL);
    uc_mem_write(uc, 0xf001833c, MRC_C7_C14_3, sizeof(MRC_C7_C14_3));

    // Mesmo CPSR observado no boot real na barreira (SVC, ARM).
    unsigned cpsr = 0x600000d3;
    uc_reg_write(uc, UC_ARM_REG_CPSR, &cpsr);

    *e_exec = uc_emu_start(uc, 0xf001833c, 0xf001833c + 4, 0, 1);
    uc_close(uc);
}

int main() {
    std::printf("[test_uc_cpu_model_order]\n");

    // Controle negativo: a ordem ERRADA precisa mesmo falhar. Se este bloco
    // passar, o teste perdeu o poder de detectar a regressao.
    uc_err m_bad, t_bad, x_bad;
    monta(false, &m_bad, &t_bad, &x_bad);
    std::printf("  ordem invertida: set_cpu_model=%s tlb=%s exec=%s\n",
                uc_strerror(m_bad), uc_strerror(t_bad), uc_strerror(x_bad));
    checa(m_bad != UC_ERR_OK,
          "controle negativo: set_cpu_model falha quando vem depois de tlb_mode");
    checa(x_bad == UC_ERR_INSN_INVALID,
          "controle negativo: mrc c7,c14,3 e rejeitada com o modelo ignorado");

    // Ordem correta: tudo OK e a instrucao do ARM9 executa.
    uc_err m_ok, t_ok, x_ok;
    monta(true, &m_ok, &t_ok, &x_ok);
    std::printf("  ordem correta:   set_cpu_model=%s tlb=%s exec=%s\n",
                uc_strerror(m_ok), uc_strerror(t_ok), uc_strerror(x_ok));
    checa(m_ok == UC_ERR_OK, "set_cpu_model(ARM926) aceito");
    checa(t_ok == UC_ERR_OK, "tlb_mode(VIRTUAL) aceito depois do modelo");
    checa(x_ok == UC_ERR_OK, "mrc p15,0,apsr_nzcv,c7,c14,3 executa no ARM926");

    if (falhas) { std::printf("[test_uc_cpu_model_order] %d FALHA(S)\n", falhas); return 1; }
    std::printf("[test_uc_cpu_model_order] OK\n");
    return 0;
}
