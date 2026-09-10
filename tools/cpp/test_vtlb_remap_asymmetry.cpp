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
// map_one_aliased() e todo o bloco de mapeamento vivem atras deste guard
// (zeebo_l4_mmu.h:277-536). Sem defini-lo, o teste compilaria sem enxergar a
// funcao de producao -- que foi justamente a falha original deste arquivo.
#define ZEEBO_L4_MMU_WITH_UNICORN 1
#include "zeebo_l4_mmu.h"

static int falhas = 0;
static void check(bool cond, const char* nome) {
    std::printf("  [%s] %s\n", cond ? "ok" : "FALHA", nome);
    if (!cond) falhas++;
}

// Monta um MapItem com fpage de 4KB (size_log2=12) rwx, apontando para `phys`.
// Encoding conforme zeebo_l4_mmu.h: fpage bits[0:2]=rwx, bits[4:9]=size_log2,
// vaddr nos bits altos; PhysDesc = phys_base >> 6 (granularidade de 64B).
static zeebo_l4::MapItem mk_item(uint64_t va, uint64_t phys) {
    const uint32_t rwx = 7u;                 // r|w|x
    const uint32_t sz  = 12u;                // 2^12 = 4KB
    uint32_t fp = (uint32_t)(va & ~0xFFFu) | (sz << 4) | rwx;
    uint32_t pd = (uint32_t)(phys >> 6);
    return zeebo_l4::decode_item(pd, fp);
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

    // Este teste chama a FUNCAO DE PRODUCAO map_one_aliased(). Antes ele
    // reimplementava o comportamento correto a mao (com a linha do bug
    // comentada), entao passaria verde mesmo se zeebo_l4_mmu.h regredisse --
    // provava o harness, nao o codigo. Agora a regressao e detectada de fato.

    // A pool fisica precisa cobrir dois enderecos fisicos distintos, para que
    // duas fpages sobre o MESMO VA resolvam para host_ptrs diferentes.
    const uint64_t PHYS_ANTIGO = 0x00000000ull;
    const uint64_t PHYS_NOVO   = 0x00001000ull;
    std::vector<uint8_t> poolbuf(2 * TAM, 0);
    std::memcpy(poolbuf.data(),       &VAL_ANTIGO, 4);   // phys 0x0000
    std::memcpy(poolbuf.data() + TAM, &VAL_NOVO,   4);   // phys 0x1000

    zeebo_l4::PhysPool pool;
    pool.phys_base = PHYS_ANTIGO;
    pool.size = 2 * TAM;
    pool.host = poolbuf.data();

    zeebo_l4::VtlbLut lut;

    // 1. Primeira fpage: VA -> phys ANTIGO. O Unicorn adota este host_ptr.
    zeebo_l4::MapItem it1 = mk_item(VA, PHYS_ANTIGO);
    uc_err e1 = zeebo_l4::map_one_aliased(uc, it1, pool, &lut);
    check(e1 == UC_ERR_OK, "primeira map_one_aliased aceita (host_ptr adotado)");

    // 2. Segunda fpage: MESMO VA, phys NOVO. O uc_mem_map_ptr interno devolve
    //    UC_ERR_MAP e o Unicorn MANTEM o buffer antigo (host_ptr e imutavel).
    //    A funcao de producao nao pode atualizar a LUT neste caso.
    zeebo_l4::MapItem it2 = mk_item(VA, PHYS_NOVO);
    uc_err e2 = zeebo_l4::map_one_aliased(uc, it2, pool, &lut);
    check(e2 == UC_ERR_OK, "segunda map_one_aliased retorna OK (protecao reajustada)");

    uint32_t via_uc = 0, via_lut = 0;
    uc_mem_read(uc, VA, &via_uc, 4);
    lut.read_u32(VA, &via_lut);

    std::printf("     unicorn=0x%08x   vtlb=0x%08x\n", via_uc, via_lut);
    check(via_uc == VAL_ANTIGO, "Unicorn serve o buffer ANTIGO apos UC_ERR_MAP");
    check(via_lut == via_uc,    "VTLB e Unicorn concordam no mesmo VA");

    // 3. Controle positivo do instrumento: a pool/LUT REALMENTE distinguem os
    //    dois buffers. Sem isto, "concordam" poderia ser trivialmente verdade.
    check(pool.host_of(PHYS_ANTIGO) != pool.host_of(PHYS_NOVO),
          "controle positivo: os dois phys resolvem para host_ptrs distintos");
    {
        zeebo_l4::VtlbLut lut_ctl;
        lut_ctl.map(VA, TAM, pool.host_of(PHYS_NOVO));
        uint32_t v = 0;
        lut_ctl.read_u32(VA, &v);
        check(v == VAL_NOVO, "controle positivo: a LUT serviria VAL_NOVO se atualizada");
        check(v != via_uc,   "controle negativo: atualizar a LUT apos UC_ERR_MAP divergiria");
    }

    uc_close(uc);
    std::printf("\nfalhas: %d\n", falhas);
    return falhas ? 1 : 0;
}
