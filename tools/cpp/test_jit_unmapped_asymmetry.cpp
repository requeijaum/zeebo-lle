// Prova a ASSIMETRIA de mapeamento sob demanda entre os dois backends.
//
// Descoberta (divergencia #23726): os dois backends emitem as mesmas escritas
// em 0xf401ffc0, mas so o interpretado consegue relê-las.
//
// O motivo nao e semantica de instrucao — e o caminho de tratamento de acesso
// a memoria NAO MAPEADA:
//
//   * Interpretado: o acesso do codigo emulado dispara UC_HOOK_MEM_*_UNMAPPED,
//     e o handler c0_unmapped_hook faz "map dynamically to continue discovery"
//     (uc_mem_map da pagina) e devolve true. A escrita entao ACONTECE.
//
//   * Recompilado: a escrita passa por bridge.write32, que chama
//     uc_mem_write() — API EXTERNA, que NAO dispara hooks. Logo o handler de
//     unmapped nunca roda, ninguem mapeia a pagina, e a escrita e perdida
//     (uc_mem_write devolve UC_ERR_WRITE_UNMAPPED = 7, retorno descartado).
//
// Este teste reproduz exatamente essa assimetria num engine isolado, sem
// depender do firmware. E o contrato que o backend recompilado precisa passar
// a respeitar: uma escrita em pagina nao mapeada tem de ser tratada, nao
// engolida.

#include <unicorn/unicorn.h>
#include <cstdio>
#include <cstdint>

static int falhas = 0;

static void check(bool cond, const char* nome) {
    printf("  [%s] %s\n", cond ? "ok" : "FALHA", nome);
    if (!cond) falhas++;
}

// Espelha o "map dynamically to continue discovery" do c0_unmapped_hook.
static bool unmapped_hook(uc_engine* uc, uc_mem_type type, uint64_t addr,
                          int size, int64_t value, void* ud) {
    (void)type; (void)size; (void)value;
    int* chamou = (int*)ud;
    (*chamou)++;
    uc_mem_map(uc, addr & ~0xFFFULL, 0x1000, UC_PROT_ALL);
    return true;  // continua a execucao
}

int main() {
    printf("== assimetria de mapeamento sob demanda entre os backends ==\n");

    const uint32_t ALVO = 0xf401ffc0;   // endereco real da divergencia #23726
    const uint32_t VALOR = 0x10090001;  // valor real que o boot grava

    // ---------------------------------------------------------------
    // Caminho RECOMPILADO: escrita via API externa (bridge.write32).
    // ---------------------------------------------------------------
    {
        uc_engine* uc = nullptr;
        uc_open(UC_ARCH_ARM, UC_MODE_ARM, &uc);

        int chamou = 0;
        uc_hook h;
        uc_hook_add(uc, &h, UC_HOOK_MEM_WRITE_UNMAPPED,
                    (void*)unmapped_hook, &chamou, 1, 0);

        uint32_t v = VALOR;
        uc_err e = uc_mem_write(uc, ALVO, &v, 4);

        check(e != UC_ERR_OK,
              "recompilado: uc_mem_write em pagina nao mapeada FALHA");
        check(e == UC_ERR_WRITE_UNMAPPED,
              "recompilado: o erro e exatamente UC_ERR_WRITE_UNMAPPED (7)");
        check(chamou == 0,
              "recompilado: o hook de unmapped NAO e chamado (API externa)");

        // O sintoma de #23726: a releitura devolve zero, nao o valor gravado.
        uint32_t lido = 0xdeadbeef;
        uc_mem_read(uc, ALVO, &lido, 4);
        check(lido != VALOR,
              "recompilado: releitura NAO devolve o valor (escrita perdida)");

        uc_close(uc);
    }

    // ---------------------------------------------------------------
    // Caminho INTERPRETADO: a escrita parte do codigo emulado, entao o
    // hook de unmapped roda e mapeia a pagina sob demanda.
    // ---------------------------------------------------------------
    {
        uc_engine* uc = nullptr;
        uc_open(UC_ARCH_ARM, UC_MODE_ARM, &uc);

        int chamou = 0;
        uc_hook h;
        uc_hook_add(uc, &h, UC_HOOK_MEM_WRITE_UNMAPPED,
                    (void*)unmapped_hook, &chamou, 1, 0);

        // Codigo: r0 = ALVO ; r1 = VALOR ; str r1,[r0]
        const uint32_t CODE_BASE = 0x1000;
        uc_mem_map(uc, CODE_BASE, 0x1000, UC_PROT_ALL);
        const uint8_t code[] = {
            0x00, 0x00, 0x80, 0xe5,  // str r0, [r0]  (placeholder, sobrescrito)
        };
        // str r1,[r0] = 0xe5801000
        const uint32_t insn = 0xe5801000;
        uc_mem_write(uc, CODE_BASE, &insn, 4);
        (void)code;

        uint32_t r0 = ALVO, r1 = VALOR;
        uc_reg_write(uc, UC_ARM_REG_R0, &r0);
        uc_reg_write(uc, UC_ARM_REG_R1, &r1);

        uc_err e = uc_emu_start(uc, CODE_BASE, CODE_BASE + 4, 0, 1);

        check(e == UC_ERR_OK,
              "interpretado: a execucao do store conclui sem erro");
        check(chamou == 1,
              "interpretado: o hook de unmapped E chamado e mapeia a pagina");

        uint32_t lido = 0;
        uc_mem_read(uc, ALVO, &lido, 4);
        check(lido == VALOR,
              "interpretado: releitura devolve o valor gravado (0x10090001)");

        uc_close(uc);
    }

    printf("\n  => A assimetria e REAL: o mesmo store persiste num backend e\n");
    printf("     se perde no outro, sem que ninguem reporte erro.\n");

    printf("\n%s\n", falhas ? "RESULTADO: FALHOU" : "RESULTADO: PASSOU");
    return falhas ? 1 : 0;
}
