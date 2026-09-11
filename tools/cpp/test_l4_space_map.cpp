// test_l4_space_map.cpp — Bug 1: mapeamentos L4 por space_id.
// Cada L4_SpaceId_t tem seu PRÓPRIO conjunto de traduções VA->host (VTLB).
// A MESMA VA em espaços diferentes deve resolver para host_ptr diferentes,
// e uma revogação num espaço NÃO afeta o outro.
// Compila: g++ -std=c++23 -DZEEBO_L4_MMU_WITH_UNICORN test_l4_space_map.cpp -lunicorn
#include "zeebo_l4_mmu.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>

using namespace zeebo_l4;
static int g_fail = 0;
#define CHECK(c) do{ if(!(c)){ printf("  FAIL: %s (line %d)\n",#c,__LINE__); g_fail++; } }while(0)

int main(){
    printf("== L4 per-space_id mappings ==\n");
    SpaceMap spaces;

    // Espaço 1 e 2 são distintos; obter LUTs distintas.
    VtlbLut& s1 = spaces.lut_for(1);
    VtlbLut& s2 = spaces.lut_for(2);
    CHECK(&s1 != &s2);
    // Reobter o mesmo id devolve a MESMA LUT.
    CHECK(&spaces.lut_for(1) == &s1);
    CHECK(spaces.space_count() == 2);

    // Backing físico distinto por espaço.
    u8* h1=(u8*)calloc(0x2000,1);
    u8* h2=(u8*)calloc(0x2000,1);

    // Mesma VA 0xb0001000 mapeada em cada espaço para host diferente.
    s1.map(0xb0001000, 0x1000, h1);
    s2.map(0xb0001000, 0x1000, h2);

    CHECK(s1.translate(0xb0001000) == h1);
    CHECK(s2.translate(0xb0001000) == h2);
    CHECK(s1.translate(0xb0001000) != s2.translate(0xb0001000));

    // Escrita num espaço não vaza para o outro.
    CHECK(s1.write_u32(0xb0001000, 0xAAAAAAAA));
    CHECK(s2.write_u32(0xb0001000, 0xBBBBBBBB));
    u32 v1=0,v2=0;
    CHECK(s1.read_u32(0xb0001000,&v1) && v1==0xAAAAAAAA);
    CHECK(s2.read_u32(0xb0001000,&v2) && v2==0xBBBBBBBB);

    // Revogação (unmap) num espaço não afeta o outro.
    s1.unmap(0xb0001000, 0x1000);
    CHECK(!s1.is_mapped(0xb0001000));
    CHECK(s2.is_mapped(0xb0001000));

    // handle_map_control roteia pela LUT do space_id passado (via SpaceMap).
    const u64 PHYS_BASE=0x10000000, POOL=1u*1024*1024;
    u8* host=(u8*)aligned_alloc(0x1000, POOL); memset(host,0,POOL);
    PhysPool pool{ PHYS_BASE, POOL, host };
    uc_engine* uc; CHECK(uc_open(UC_ARCH_ARM, UC_MODE_ARM, &uc)==UC_ERR_OK);

    // MRs para um map de RAM em space 7.
    auto make_phys=[&](u64 phys,l4attrib_e a){ return (u32)((((u32)(phys>>6))<<6)|((u32)a&0x3f)); };
    auto make_fpage=[&](u64 va,u32 sz,bool r,bool w,bool x){
        u32 raw=0; raw|=(x?1u:0)<<0; raw|=(w?1u:0)<<1; raw|=(r?1u:0)<<2;
        raw|=(sz&0x3f)<<4; raw|=((u32)(va>>10)&0x3fffff)<<10; return raw; };
    u32 utcb=0x1000; CHECK(uc_mem_map(uc,utcb,0x1000,UC_PROT_ALL)==UC_ERR_OK);
    u32 mr0=make_phys(PHYS_BASE,l4mem_cached), mr1=make_fpage(0xc0000000,16,true,true,false);
    uc_mem_write(uc, utcb+64+0*4, &mr0,4);
    uc_mem_write(uc, utcb+64+1*4, &mr1,4);
    u32 ctrl=0; // count=1
    handle_map_control(uc, utcb, /*space_id=*/7, ctrl, nullptr, &pool, &spaces);
    // Space 7 agora conhece a tradução; spaces 1/2 não.
    CHECK(spaces.lut_for(7).is_mapped(0xc0000000));
    CHECK(!spaces.lut_for(1).is_mapped(0xc0000000));

    uc_close(uc); free(host); free(h1); free(h2);
    if(g_fail==0){ printf("ALL TESTS PASSED\n"); return 0; }
    printf("%d CHECK(s) FAILED\n", g_fail); return 1;
}
