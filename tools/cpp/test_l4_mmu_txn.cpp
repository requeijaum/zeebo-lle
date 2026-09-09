// test_l4_mmu_txn.cpp — QW13: cobertura TRANSACIONAL de L4_MapControl sob Unicorn.
// ---------------------------------------------------------------------------
// Fixa a transação de map da syscall MapControl no espaço REAL do Unicorn
// (não apenas a decodificação de bits, já coberta por test_l4_mmu*):
//
//   1. MRs INICIAIS do UTCB (bytes crus escritos em utcb_base+0x40, +0x44, ...).
//   2. ENTRADAS da syscall (space_id em r0, control em r1).
//   3. TRANSAÇÃO de map: regiões efetivamente criadas no Unicorn, em ORDEM,
//      com base/tamanho/proteção, incluindo escrita real de página (não NOP).
//   4. Resultado r0 da syscall.
//   5. Comportamento de itens malformados e de fpage de espaço inteiro.
//
// LIMITE HONESTO DESTA REGRESSÃO (não é o que o header do commit original
// afirmava): esta chamada black-box NÃO consegue distinguir a presença ou
// ausência do write-back idêntico dos MRs (echo neutro). Como o dispatcher
// relê os MRs que ele mesmo reescreveria com o MESMO valor, o UTCB fica
// byte-idêntico ao estado de entrada nos dois cenários. Empiricamente: remover
// o write_mr de produção NÃO faz nenhum CHECK abaixo falhar. Portanto NÃO
// afirmamos que este teste detecta o write-back ausente do UTCB nem que fecha
// o "Passo 13" (mempool_init relendo MR[1]). Provar esse mecanismo exige um
// harness com GUEST VIVO onde a entrada/saída da syscall pode ALTERAR os MRs
// (kernel escrevendo descritores efetivos diferentes dos de entrada), o que a
// produção atual — um shim de echo — não faz. QW13 permanece BLOQUEADO para
// esse escopo; o valor real desta regressão é a transação de map no Unicorn.
//
// Casos cobertos:
//   A. fpage válida de 1MiB (size_log2=20): mapeamento real + escrita de página.
//   B. múltiplos itens com item malformado (nil) no meio: ordem, skip do
//      malformado, sem região espúria em 0.
//   C. espaço inteiro (size_log2>=32): no-op de sucesso, SEM overflow 32-bit
//      e SEM região criada no Unicorn (não repassa 2^32 ao uc_mem_map).
//
// Sem boot sintético, sem critério de contagem de instruções: só bytes/regiões.
// Compila: g++ -std=c++23 -DZEEBO_L4_MMU_WITH_UNICORN test_l4_mmu_txn.cpp -lunicorn
#include "zeebo_l4_mmu.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>
#include <algorithm>

using namespace zeebo_l4;
static int g_fail = 0;
#define CHECK(c) do{ if(!(c)){ printf("  FAIL: %s (line %d)\n",#c,__LINE__); g_fail++; } }while(0)

// Encoders idênticos ao GUEST do Zeebo (phys_desc gran 64B; espelham test_l4_mmu.cpp).
static u32 make_phys_desc(u64 phys, l4attrib_e a){ return (((u32)(phys>>6))<<6)|((u32)a&0x3f); }
static u32 make_fpage(u64 va,u32 sz,bool r,bool w,bool x){
    u32 raw=0; raw|=(x?1u:0)<<0; raw|=(w?1u:0)<<1; raw|=(r?1u:0)<<2;
    raw|=(sz&0x3f)<<4; raw|=((u32)(va>>10)&0x3fffff)<<10; return raw;
}
static u32 make_control(u32 count,bool modify,bool query){
    u32 raw=(count-1)&0x3f; if(query)raw|=(1u<<30); if(modify)raw|=(1u<<31); return raw;
}

static constexpr u32 UTCB_BASE = 0x00040000;

// Lê o snapshot ordenado das regiões de memória do Unicorn.
struct Region { u64 base, end; u32 perms; };
static std::vector<Region> regions_of(uc_engine* uc){
    uc_mem_region* r=nullptr; uint32_t n=0;
    std::vector<Region> out;
    if(uc_mem_regions(uc,&r,&n)==UC_ERR_OK){
        for(uint32_t i=0;i<n;i++) out.push_back({r[i].begin, r[i].end, r[i].perms});
        uc_free(r);
    }
    std::sort(out.begin(),out.end(),[](const Region&a,const Region&b){return a.base<b.base;});
    return out;
}
static bool has_region(const std::vector<Region>& v,u64 base,u64 size,u32 perms){
    for(auto&r:v) if(r.base==base && (r.end-r.base+1)==size && r.perms==perms) return true;
    return false;
}
static bool any_covers(const std::vector<Region>& v,u64 addr){
    for(auto&r:v) if(addr>=r.base && addr<=r.end) return true;
    return false;
}

int main(){
    printf("== QW13 L4_MapControl transactional (map) regression ==\n");
    uc_engine* uc; CHECK(uc_open(UC_ARCH_ARM,UC_MODE_ARM,&uc)==UC_ERR_OK);
    // UTCB do thread corrente (1 página).
    CHECK(uc_mem_map(uc, UTCB_BASE, 0x1000, UC_PROT_READ|UC_PROT_WRITE)==UC_ERR_OK);

    // =====================================================================
    // CASO A — fpage válida de 1MiB (size_log2=20), item único.
    // =====================================================================
    {
        printf("-- A: single 1MiB fpage (size_log2=20) --\n");
        const u64 VA=0xb0100000, PHYS=0x10000000;
        const u32 mr_phys  = make_phys_desc(PHYS, l4mem_cached);
        const u32 mr_fpage = make_fpage(VA, 20, true,true,true);

        // (1) MRs INICIAIS: escreve os descritores de entrada no UTCB.
        write_mr(uc, UTCB_BASE, 0, mr_phys);
        write_mr(uc, UTCB_BASE, 1, mr_fpage);
        // Pino de bytes crus dos MRs iniciais (exato).
        CHECK(read_mr(uc,UTCB_BASE,0)==mr_phys);
        CHECK(read_mr(uc,UTCB_BASE,1)==mr_fpage);
        CHECK(mr_fpage==0xb0100147u); // va=0xb0100000,size=20,rwx=7 -> byte-exato

        // (2) Entradas da syscall.
        const u32 space_id=0x1, control=make_control(1,true,false);

        // (3)+(4) Dispatch: cria mapeamento real.
        std::vector<MapItem> items;
        u32 r0 = handle_map_control(uc, UTCB_BASE, space_id, control, &items);

        // (4) resultado r0 não-nulo (sucesso).
        CHECK(r0==1);
        // itens decodificados na ordem.
        CHECK(items.size()==1);
        CHECK(items[0].phys.phys_base()==PHYS);
        CHECK(items[0].fpage.vaddr()==VA);
        CHECK(items[0].fpage.size_bytes()==0x100000);

        // (3) TRANSAÇÃO de map: região de 1MiB criada em VA, RWX.
        auto regs = regions_of(uc);
        CHECK(has_region(regs, VA, 0x100000, UC_PROT_READ|UC_PROT_WRITE|UC_PROT_EXEC));

        // UTCB permanece byte-idêntico ao estado de entrada. NOTA: este CHECK
        // NÃO distingue write-back de no-op (echo neutro idêntico à entrada);
        // apenas garante que a transação não corrompeu os MRs.
        CHECK(read_mr(uc,UTCB_BASE,0)==mr_phys);
        CHECK(read_mr(uc,UTCB_BASE,1)==mr_fpage);

        // Prova de escrita real na página mapeada (não é NOP-slide).
        u32 wv=0xa5a5f00d, rv=0;
        CHECK(uc_mem_write(uc,VA+0x1234,&wv,4)==UC_ERR_OK);
        CHECK(uc_mem_read(uc,VA+0x1234,&rv,4)==UC_ERR_OK && rv==wv);

        // Limpa a região para o próximo caso.
        uc_mem_unmap(uc, VA, 0x100000);
    }

    // =====================================================================
    // CASO B — múltiplos itens + item MALFORMADO (nil) no meio.
    //   item0: 1MiB válido @0xb0200000
    //   item1: MALFORMADO (fpage nil raw=0) -> skip, sem região
    //   item2:   4KiB válido @0xb0400000
    // Fixa ORDEM e skip do malformado.
    // =====================================================================
    {
        printf("-- B: multi-item with malformed (nil) middle item --\n");
        const u64 VA0=0xb0200000, VA2=0xb0400000;
        const u32 mr_phys0 = make_phys_desc(0x10100000,l4mem_cached);
        const u32 mr_fp0   = make_fpage(VA0,20,true,true,true);   // 1MiB
        const u32 mr_phys1 = 0x0;                                 // malformado
        const u32 mr_fp1   = 0x0;                                 // fpage nil
        const u32 mr_phys2 = make_phys_desc(0x10300000,l4mem_writeback);
        const u32 mr_fp2   = make_fpage(VA2,12,true,true,false);  // 4KiB rw

        write_mr(uc,UTCB_BASE,0,mr_phys0); write_mr(uc,UTCB_BASE,1,mr_fp0);
        write_mr(uc,UTCB_BASE,2,mr_phys1); write_mr(uc,UTCB_BASE,3,mr_fp1);
        write_mr(uc,UTCB_BASE,4,mr_phys2); write_mr(uc,UTCB_BASE,5,mr_fp2);

        const u32 control=make_control(3,true,false);
        std::vector<MapItem> items;
        u32 r0 = handle_map_control(uc, UTCB_BASE, 0x1, control, &items);

        CHECK(r0==1);
        // ORDEM dos itens decodificados preservada.
        CHECK(items.size()==3);
        CHECK(items[0].fpage.vaddr()==VA0 && items[0].fpage.size_bytes()==0x100000);
        CHECK(items[1].fpage.is_nil());
        CHECK(items[2].fpage.vaddr()==VA2 && items[2].fpage.size_bytes()==0x1000);

        auto regs = regions_of(uc);
        // itens válidos mapeados...
        CHECK(has_region(regs, VA0, 0x100000, UC_PROT_READ|UC_PROT_WRITE|UC_PROT_EXEC));
        CHECK(has_region(regs, VA2, 0x1000,   UC_PROT_READ|UC_PROT_WRITE));
        // ...item malformado NÃO cria região (nil raw=0 -> vaddr 0).
        CHECK(!any_covers(regs, 0x0));

        uc_mem_unmap(uc, VA0, 0x100000);
        uc_mem_unmap(uc, VA2, 0x1000);
    }

    // =====================================================================
    // CASO C — fpage de ESPAÇO INTEIRO (size_log2>=32): no-op de sucesso.
    //   Reproduz o descritor que travava o Core 0 com UC_ERR_NOMEM:
    //   va=0xb0d00000, phys=0x100000000, size=2^32, rwx=6.
    //   Exige: (a) r0 sucesso, (b) NENHUMA região criada (não passa 2^32 ao
    //   uc_mem_map), (c) UTCB não corrompido.
    // =====================================================================
    {
        printf("-- C: whole-space fpage (size_log2>=32) no-op success --\n");
        const u32 mr_phys  = make_phys_desc(0x100000000ull, l4mem_io);
        const u32 mr_fpage = make_fpage(0xb0d00000,32,false,true,true);

        write_mr(uc,UTCB_BASE,0,mr_phys);
        write_mr(uc,UTCB_BASE,1,mr_fpage);

        auto before = regions_of(uc);
        std::vector<MapItem> items;
        u32 r0 = handle_map_control(uc, UTCB_BASE, 0x1, make_control(1,true,false), &items);
        auto after = regions_of(uc);

        // (a) sucesso.
        CHECK(r0==1);
        CHECK(items.size()==1);
        CHECK(items[0].fpage.is_whole_space());
        CHECK(items[0].fpage.size_bytes()==((u64)1<<32)); // satura, sem overflow

        // (b) NENHUMA nova região: whole-space não é repassado ao uc_mem_map.
        CHECK(after.size()==before.size());
        CHECK(!any_covers(after, 0xb0d00000)); // nada mapeado em 32-bit

        // (c) UTCB não corrompido (não distingue echo de no-op — ver nota do topo).
        CHECK(read_mr(uc,UTCB_BASE,0)==mr_phys);
        CHECK(read_mr(uc,UTCB_BASE,1)==mr_fpage);
    }

    // =====================================================================
    // CASO D — fpage de tamanho intermediário (size_log2=31, 2GiB) em va ALTO
    // que cruza a fronteira de 4GB do espaço ARM de 32 bits. O guard
    // whole-space (size_log2>=32) NÃO cobre este caso: size_log2=31 é uma fpage
    // legítima de 2GiB que, sobre base alta, faz `end` estourar 0x100000000 e
    // repassa um range >4GB ao uc_mem_map -> UC_ERR_NOMEM/ARG.
    // Exige: (a) r0 sucesso (controle/clamp, não erro), (b) NENHUMA região
    // criada acima de 0x100000000, (c) estado UTCB não corrompido.
    // =====================================================================
    {
        printf("-- D: size_log2=31 (2GiB) at high VA crossing 4GB --\n");
        const u64 VA = 0xC0000000ull, PHYS = 0x10000000ull;
        const u32 mr_phys  = make_phys_desc(PHYS, l4mem_cached);
        const u32 mr_fpage = make_fpage(VA, 31, true,true,true); // 2GiB por design

        write_mr(uc,UTCB_BASE,0,mr_phys);
        write_mr(uc,UTCB_BASE,1,mr_fpage);

        auto before = regions_of(uc);
        std::vector<MapItem> items;
        u32 r0 = handle_map_control(uc, UTCB_BASE, 0x1, make_control(1,true,false), &items);
        auto after = regions_of(uc);

        // (a) sucesso (não deve falhar com UC_ERR_NOMEM nem corromper a syscall).
        CHECK(r0==1);
        CHECK(items.size()==1);
        CHECK(items[0].fpage.vaddr()==VA);
        CHECK(items[0].fpage.size_bytes()==((u64)1<<31));
        CHECK(!items[0].fpage.is_whole_space());

        // (b) nenhuma região criada além da fronteira de 4GB (clamp/controle).
        for (auto& r : after) {
            CHECK(r.end <= 0xFFFFFFFFull);
            CHECK(r.base <  0x100000000ull);
        }

        // (c) UTCB intacto.
        CHECK(read_mr(uc,UTCB_BASE,0)==mr_phys);
        CHECK(read_mr(uc,UTCB_BASE,1)==mr_fpage);
    }

    uc_close(uc);
    if(g_fail==0){ printf("ALL TESTS PASSED\n"); return 0; }
    printf("%d CHECK(s) FAILED\n", g_fail); return 1;
}
