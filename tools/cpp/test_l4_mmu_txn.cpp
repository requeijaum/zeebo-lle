// test_l4_mmu_txn.cpp — QW13: regressão TRANSACIONAL de L4_MapControl sob Unicorn.
// ---------------------------------------------------------------------------
// Fixa a transação COMPLETA da syscall MapControl no espaço real do Unicorn,
// não apenas a decodificação de bits (isso já é coberto por test_l4_mmu*):
//
//   1. MRs INICIAIS do UTCB (bytes crus escritos em utcb_base+0x40, +0x44, ...).
//   2. ENTRADAS da syscall (space_id em r0, control em r1).
//   3. TRANSAÇÃO de map/callback: regiões efetivamente criadas no Unicorn,
//      em ORDEM, com base/tamanho/proteção.
//   4. MRs de RETORNO: bytes exatos de phys_desc/fpage reescritos no UTCB.
//   5. Resultado r0 da syscall.
//   6. Reexecução do mempool_init do Iguana: reler MR[1] (offset 0x44) para
//      extrair size_log2 e AVANÇAR o ponteiro do pool (lsl r4,r4,size_log2).
//
// Este é o exato mecanismo do bug do Passo 13: se o dispatcher não reescrever
// os MRs de volta no UTCB, o mempool_init relê 0 em MR[1], size_log2=0, e o
// avanço do pool colapsa (r4<<0), travando o laço em 0xb000d860. A regressão
// aqui FALHA por esse mecanismo exato (ver test_l4_mmu_txn_mut.cpp / mutação).
//
// Casos cobertos:
//   A. fpage válida de 1MiB (size_log2=20): mapeamento real + MR round-trip.
//   B. múltiplos itens com item malformado (nil) no meio: ordem, skip do
//      malformado, avanço correto dos itens válidos.
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

// Encoders idênticos aos headers OKL4 (espelham test_l4_mmu.cpp).
static u32 make_phys_desc(u64 phys, l4attrib_e a){ return (((u32)(phys>>10))<<6)|((u32)a&0x3f); }
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

// ---------------------------------------------------------------------------
// Reexecução byte-exata do laço mempool_init (@0xb000d864..0xb000d89c):
//   size_log2 = (MR[2*i+1] >> 4) & 0x3f;  pool_next = pool_cur << size_log2 ... 
// Modelamos o avanço do ponteiro do pool virtual: r4 começa em 1 e é deslocado
// (lsl r4, r4, size_log2). Se o MR não foi reescrito, size_log2=0 => sem avanço.
static u64 mempool_advance_from_utcb(uc_engine* uc,u32 utcb,u32 item){
    u32 fp = read_mr(uc, utcb, item*2u+1u);
    u32 s  = (fp >> 4) & 0x3fu;
    return (u64)1u << (s & 63);
}

int main(){
    printf("== QW13 L4_MapControl transactional regression ==\n");
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

        // (3)+(4)+(5) Dispatch: cria mapeamento real e reescreve MRs.
        std::vector<MapItem> items;
        u32 r0 = handle_map_control(uc, UTCB_BASE, space_id, control, &items);

        // (5) resultado r0 não-nulo (sucesso).
        CHECK(r0==1);
        // itens decodificados na ordem.
        CHECK(items.size()==1);
        CHECK(items[0].phys.phys_base()==PHYS);
        CHECK(items[0].fpage.vaddr()==VA);
        CHECK(items[0].fpage.size_bytes()==0x100000);

        // (3) TRANSAÇÃO de map: região de 1MiB criada em VA, RWX.
        auto regs = regions_of(uc);
        CHECK(has_region(regs, VA, 0x100000, UC_PROT_READ|UC_PROT_WRITE|UC_PROT_EXEC));

        // (4) MRs de RETORNO: bytes exatos reescritos (round-trip).
        CHECK(read_mr(uc,UTCB_BASE,0)==mr_phys);
        CHECK(read_mr(uc,UTCB_BASE,1)==mr_fpage);

        // (6) mempool_init relê MR[1] -> size_log2=20 -> avanço = 1<<20.
        //     ESTE é o mecanismo do bug do Passo 13. Sem write-back, MR[1]=lixo.
        CHECK(mempool_advance_from_utcb(uc,UTCB_BASE,0)==((u64)1<<20));

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
    //   item1: MALFORMADO (fpage nil raw=0) -> skip, sem região, MR ecoado
    //   item2:   4KiB válido @0xb0400000
    // Fixa ORDEM, skip do malformado e AVANÇO de cada item válido.
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

        // TODOS os MRs (inclusive o malformado) ecoados byte-a-byte, em ordem.
        CHECK(read_mr(uc,UTCB_BASE,0)==mr_phys0); CHECK(read_mr(uc,UTCB_BASE,1)==mr_fp0);
        CHECK(read_mr(uc,UTCB_BASE,2)==mr_phys1); CHECK(read_mr(uc,UTCB_BASE,3)==mr_fp1);
        CHECK(read_mr(uc,UTCB_BASE,4)==mr_phys2); CHECK(read_mr(uc,UTCB_BASE,5)==mr_fp2);

        // mempool advance por item: item0 -> 1<<20, item2 -> 1<<12.
        CHECK(mempool_advance_from_utcb(uc,UTCB_BASE,0)==((u64)1<<20));
        CHECK(mempool_advance_from_utcb(uc,UTCB_BASE,2)==((u64)1<<12));

        uc_mem_unmap(uc, VA0, 0x100000);
        uc_mem_unmap(uc, VA2, 0x1000);
    }

    // =====================================================================
    // CASO C — fpage de ESPAÇO INTEIRO (size_log2>=32): no-op de sucesso.
    //   Reproduz o descritor que travava o Core 0 com UC_ERR_NOMEM:
    //   va=0xb0d00000, phys=0x100000000, size=2^32, rwx=6.
    //   Exige: (a) r0 sucesso, (b) NENHUMA região criada (não passa 2^32 ao
    //   uc_mem_map), (c) MR ecoado.
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

        // (c) MR ecoado.
        CHECK(read_mr(uc,UTCB_BASE,0)==mr_phys);
        CHECK(read_mr(uc,UTCB_BASE,1)==mr_fpage);
    }

    uc_close(uc);
    if(g_fail==0){ printf("ALL TESTS PASSED\n"); return 0; }
    printf("%d CHECK(s) FAILED\n", g_fail); return 1;
}
