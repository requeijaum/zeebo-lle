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

#include "zeebo_l4_mmu.h"
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

    printf("\n%s\n", falhas ? "RESULTADO: FALHOU" : "RESULTADO: PASSOU");
    return falhas ? 1 : 0;
}
