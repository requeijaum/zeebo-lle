// Prova que o caminho de escrita do backend recompilado perde escritas
// silenciosamente quando o endereco nao esta mapeado.
//
// Contexto (divergencia #23726 do boot): o Core0 executa, no MESMO endereco
// 0xf401ffc0, esta sequencia:
//
//     pc=0xf000a32c  str  0x00000001
//     pc=0xf000a35c  str  0x10090001     <- valor que o boot espera reler
//     ...            ldr  -> interpretado le 0x10090001, recompilado le 0
//
// O vigia (--watch-writes) mostrou que os DOIS backends emitem as duas
// escritas com os mesmos valores. Ou seja: o defeito nao esta na semantica do
// store nem na do load — a escrita simplesmente NAO PERSISTE no caminho do
// recompilado, e a leitura seguinte devolve zero.
//
// A causa e que `bridge.write32` faz:
//
//     s->vtlb_.write_u32(addr, val);              // retorna bool — DESCARTADO
//     if (s->core0_.uc) uc_mem_write(...);        // retorna uc_err — DESCARTADO
//
// Se o endereco nao estiver mapeado, as duas falham e ninguem percebe: o
// firmware segue como se tivesse gravado. Este teste trava esse contrato.
//
// NOTA DE HONESTIDADE: este teste prova a PERDA SILENCIOSA da escrita, que e
// um defeito real e demonstravel. Ele NAO prova, por si so, que corrigi-lo faz
// o boot avancar — o mapeamento de 0xf401f000 e uma questao separada.

// LIMITACAO CONHECIDA E DECLARADA: bridge.write32 e um lambda definido dentro
// de ZeeboLLESystem (zeebo_lle_main.cpp:1341), sem linkagem externa -- nao da
// para chama-lo daqui sem extrair a funcao. A versao anterior deste teste
// exercitava APENAS VtlbLut e passaria verde mesmo com a bridge regredida.
//
// Enquanto a extracao nao acontece, este teste exercita o MECANISMO REAL de
// que a bridge depende: uc_mem_write() e API externa e NAO dispara hooks,
// entao sem o retry apos UC_ERR_WRITE_UNMAPPED a escrita se perde em silencio.
// Isso e testavel contra o Unicorn de verdade, e e a substancia de #23726.
#define ZEEBO_L4_MMU_WITH_UNICORN 1
#include "zeebo_l4_mmu.h"
#include <unicorn/unicorn.h>
#include <cstdio>
#include <cstdint>

static int falhas = 0;

static void check(bool cond, const char* nome) {
    printf("  [%s] %s\n", cond ? "ok" : "FALHA", nome);
    if (!cond) falhas++;
}

int main() {
    printf("== escrita perdida silenciosamente no caminho do recompilado ==\n");

    zeebo_l4::VtlbLut lut;
    static uint8_t pagina[0x1000] = {0};

    // Controle positivo: numa pagina mapeada, escrever e reler devolve o valor.
    const uint64_t VA_OK = 0x10000000;
    lut.map(VA_OK, 0x1000, pagina);
    check(lut.write_u32(VA_OK + 0x40, 0x10090001),
          "controle positivo: escrita em pagina mapeada e ACEITA");
    uint32_t lido = 0;
    check(lut.read_u32(VA_OK + 0x40, &lido) && lido == 0x10090001,
          "controle positivo: releitura devolve o valor escrito");

    // O caso real: 0xf401ffc0 nao esta mapeado.
    const uint64_t VA_BOOT = 0xf401ffc0;
    const bool aceitou = lut.write_u32(VA_BOOT, 0x10090001);
    check(!aceitou,
          "a VTLB REJEITA a escrita em 0xf401ffc0 (pagina nao mapeada)");

    // E o ponto central: a releitura devolve 0, exatamente o sintoma de #23726.
    uint32_t relido = 0xdeadbeef;
    const bool leu = lut.read_u32(VA_BOOT, &relido);
    check(!leu, "a releitura de 0xf401ffc0 tambem falha");

    // O contrato que a bridge violava: quem ignora o retorno de write_u32 nao
    // consegue distinguir "gravei" de "perdi a escrita". Este e o bug.
    check(aceitou == false && leu == false,
          "escrita e leitura falham JUNTAS => ignorar o retorno perde o dado");

    // ---- O mecanismo real da bridge, contra o Unicorn ----------------------
    // uc_mem_write() e API externa: NAO dispara UC_HOOK_MEM_WRITE, logo o
    // c0_unmapped_hook (que mapearia a pagina sob demanda no caminho
    // interpretado) nunca roda aqui. Sem o retry explicito, a escrita some.
    {
        uc_engine* uc = nullptr;
        if (uc_open(UC_ARCH_ARM, UC_MODE_ARM, &uc) != UC_ERR_OK) {
            printf("  uc_open falhou\n");
            return 2;
        }
        const uint64_t VA = 0xf401ffc0ull;
        const uint32_t VAL = 0x10090001u;

        // 1. Sem retry: a escrita em pagina nao mapeada FALHA.
        uc_err e_sem = uc_mem_write(uc, VA, &VAL, 4);
        check(e_sem == UC_ERR_WRITE_UNMAPPED,
              "sem retry: uc_mem_write devolve WRITE_UNMAPPED (escrita perdida)");

        // 2. Controle positivo do instrumento: a releitura confirma que nada
        //    foi gravado -- nao basta confiar no codigo de erro.
        uint32_t antes = 0xdeadbeefu;
        uc_err e_rd = uc_mem_read(uc, VA, &antes, 4);
        check(e_rd != UC_ERR_OK,
              "controle positivo: a leitura tambem falha (pagina inexistente)");

        // 3. Com o retry (o que a bridge de producao faz): mapeia e regrava.
        uc_mem_map(uc, VA & ~0xFFFULL, 0x1000, UC_PROT_ALL);
        uc_err e_com = uc_mem_write(uc, VA, &VAL, 4);
        check(e_com == UC_ERR_OK, "com retry: a escrita e ACEITA");

        uint32_t depois = 0;
        check(uc_mem_read(uc, VA, &depois, 4) == UC_ERR_OK && depois == VAL,
              "com retry: a releitura devolve o valor gravado (sintoma de #23726 sanado)");

        // 4. O contrato violado: ignorar o retorno torna (1) e (3)
        //    indistinguiveis para o chamador.
        check(e_sem != e_com,
              "ignorar o retorno de uc_mem_write confunde escrita perdida com gravada");

        uc_close(uc);
    }

    printf("\n%s\n", falhas ? "RESULTADO: FALHOU" : "RESULTADO: PASSOU");
    return falhas ? 1 : 0;
}
