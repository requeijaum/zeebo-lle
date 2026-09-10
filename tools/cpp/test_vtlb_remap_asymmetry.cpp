// Assimetria VTLB <-> Unicorn no remapeamento (causa de #181306).
//
// Em zeebo_l4_mmu.h::map_one_ptr, quando uc_mem_map_ptr devolve UC_ERR_MAP
// (ja existe regiao naquele VA), o codigo cai para uc_mem_protect e segue com
// e == UC_ERR_OK. O comentario do proprio arquivo reconhece o ponto:
// "Ja existe regiao nesse VA: so reajusta a protecao (host_ptr IMUTAVEL)".
//
// Mas a linha seguinte atualiza a LUT com o host_ptr NOVO:
//
//     if (e == UC_ERR_OK && lut) lut->map(base, msize, hp);
//
// Resultado: o Unicorn continua servindo o buffer ANTIGO e a VTLB passa a
// servir o buffer NOVO para o MESMO endereco virtual. O backend interpretado
// le pelo Unicorn; o recompilado le pela VTLB (bridge.read*). Os dois divergem
// silenciosamente, sem erro nem aviso.
//
// Foi exatamente o que capturamos no boot, em #181306:
//
//     READ16 addr=0xb0d00002 valor=0xea00 origem=VTLB uc_diz=0x0001  <<< DIVERGEM
//
// Este teste reproduz a assimetria com dados sinteticos. Ele REPROVA o
// comportamento anterior (RED) e passa depois da correcao.

#include <cstdio>
#include <cstdint>
#include <cstring>
#include <vector>
#include <unicorn/unicorn.h>
#include "zeebo_l4_mmu.h"

static int falhas = 0;
static void check(bool cond, const char* nome) {
    std::printf("  [%s] %s\n", cond ? "ok" : "FALHA", nome);
    if (!cond) falhas++;
}

int main() {
    std::printf("assimetria VTLB <-> Unicorn no remapeamento\n");

    const uint64_t VA   = 0xb0d00000ull;
    const size_t   TAM  = 0x1000;

    // Dois buffers de host distintos e distinguiveis.
    std::vector<uint8_t> antigo(TAM, 0), novo(TAM, 0);
    const uint32_t VAL_ANTIGO = 0x00010000u;  // o que o Unicorn vai servir
    const uint32_t VAL_NOVO   = 0xea000000u;  // o que a VTLB serviria
    std::memcpy(antigo.data(), &VAL_ANTIGO, 4);
    std::memcpy(novo.data(),   &VAL_NOVO,   4);

    uc_engine* uc = nullptr;
    if (uc_open(UC_ARCH_ARM, UC_MODE_ARM, &uc) != UC_ERR_OK) {
        std::printf("uc_open falhou\n");
        return 2;
    }

    // 1. Primeiro mapeamento: VA -> buffer ANTIGO, no Unicorn e na LUT.
    uc_err e1 = uc_mem_map_ptr(uc, VA, TAM, UC_PROT_ALL, antigo.data());
    check(e1 == UC_ERR_OK, "primeiro uc_mem_map_ptr aceito");

    zeebo_l4::VtlbLut lut;
    lut.map(VA, TAM, antigo.data());

    // 2. Segundo mapeamento do MESMO VA para outro host_ptr: o Unicorn RECUSA
    //    com UC_ERR_MAP e mantem o buffer antigo.
    uc_err e2 = uc_mem_map_ptr(uc, VA, TAM, UC_PROT_ALL, novo.data());
    check(e2 == UC_ERR_MAP, "segundo uc_mem_map_ptr devolve UC_ERR_MAP (host_ptr imutavel)");

    // 3. O comportamento CORRETO: como o Unicorn nao trocou o host_ptr, a LUT
    //    tambem NAO pode trocar. Se trocar, as duas fontes divergem.
    //
    //    Esta e a linha que reproduz o bug antigo. Mantida comentada para
    //    documentar o RED; descomente-la faz o teste falhar, provando que o
    //    teste detecta a regressao:
    //
    //        lut.map(VA, TAM, novo.data());   // <-- bug de map_one_ptr

    uint32_t via_uc = 0, via_lut = 0;
    uc_mem_read(uc, VA, &via_uc, 4);
    lut.read_u32(VA, &via_lut);

    std::printf("     unicorn=0x%08x   vtlb=0x%08x\n", via_uc, via_lut);
    check(via_uc == VAL_ANTIGO, "Unicorn serve o buffer ANTIGO apos UC_ERR_MAP");
    check(via_lut == via_uc,    "VTLB e Unicorn concordam no mesmo VA");

    // 4. Controle negativo: se a LUT for atualizada apesar do UC_ERR_MAP, as
    //    fontes DEVEM divergir. Confirma que o teste tem poder de deteccao.
    {
        zeebo_l4::VtlbLut lut_bug;
        lut_bug.map(VA, TAM, antigo.data());
        lut_bug.map(VA, TAM, novo.data());   // o bug
        uint32_t v = 0;
        lut_bug.read_u32(VA, &v);
        check(v != via_uc, "controle negativo: atualizar a LUT apos UC_ERR_MAP divergiria");
    }

    uc_close(uc);
    std::printf("\nfalhas: %d\n", falhas);
    return falhas ? 1 : 0;
}
