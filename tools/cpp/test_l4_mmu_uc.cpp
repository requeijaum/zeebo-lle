// test_l4_mmu_uc.cpp — integração: uc_mem_map_ptr + PhysPool + VTLB LUT.
// Valida o caminho estilo Dolphin/PCSX2: um pool físico de host mapeado em
// múltiplos VAs do guest, com tradução O(1) via LUT casando com uc_mem_read.
// Compila: g++ -std=c++23 -DZEEBO_L4_MMU_WITH_UNICORN test_l4_mmu_uc.cpp -lunicorn
#include "zeebo_l4_mmu.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>

using namespace zeebo_l4;
static int g_fail = 0;
#define CHECK(c) do{ if(!(c)){ printf("  FAIL: %s (line %d)\n",#c,__LINE__); g_fail++; } }while(0)

// Encoders (espelham test_l4_mmu.cpp).
static u32 make_phys_desc(u64 phys, l4attrib_e a){ return (((u32)(phys>>10))<<6)|((u32)a&0x3f); }
static u32 make_fpage(u64 va,u32 sz,bool r,bool w,bool x){
    u32 raw=0; raw|=(x?1u:0)<<0; raw|=(w?1u:0)<<1; raw|=(r?1u:0)<<2;
    raw|=(sz&0x3f)<<4; raw|=((u32)(va>>10)&0x3fffff)<<10; return raw;
}

int main(){
    printf("== L4 MMU aliasing (uc_mem_map_ptr) integration ==\n");
    uc_engine* uc; CHECK(uc_open(UC_ARCH_ARM, UC_MODE_ARM, &uc)==UC_ERR_OK);

    // Pool física contígua: 96MB de APPS_RAM em phys 0x10000000.
    const u64 PHYS_BASE=0x10000000, POOL=96u*1024*1024;
    u8* host=(u8*)aligned_alloc(0x1000, POOL); memset(host,0,POOL);
    PhysPool pool{ PHYS_BASE, POOL, host };
    VtlbLut lut;

    // Duas fpages sobre o MESMO phys (identidade + espaço de usuário) = aliasing.
    MapItem it1 = decode_item(make_phys_desc(PHYS_BASE,l4mem_cached),
                              make_fpage(0xb0000000,16,true,true,true));  // 64KB
    MapItem it2 = decode_item(make_phys_desc(PHYS_BASE,l4mem_cached),
                              make_fpage(0xd0000000,16,true,true,true));  // alias

    CHECK(map_one_aliased(uc,it1,pool,&lut)==UC_ERR_OK);
    CHECK(map_one_aliased(uc,it2,pool,&lut)==UC_ERR_OK);

    // Guest escreve em VA1; alias VA2 e host_ptr direto veem o mesmo valor.
    u32 w=0xcafebabe;
    CHECK(uc_mem_write(uc,0xb0001000,&w,4)==UC_ERR_OK);
    u32 r=0; CHECK(uc_mem_read(uc,0xd0001000,&r,4)==UC_ERR_OK);
    printf("  aliasing guest VA1->VA2: 0x%08x %s\n", r, r==w?"OK":"DIVERGE");
    CHECK(r==w);

    // VTLB LUT casa com uc_mem_read SEM chamar uc_mem_read (O(1) host-side).
    u32 lr=0; CHECK(lut.read_u32(0xb0001000,&lr) && lr==w);
    CHECK(lut.translate(0xd0001000)==host+0x1000); // alias -> mesmo host offset
    printf("  VTLB translate == pool host_ptr: %s\n", lut.translate(0xb0001000)==host+0x1000?"OK":"NO");

    // BrewLoader-style: escreve via LUT, guest ve via Unicorn.
    CHECK(lut.write_u32(0xb0002000,0x11223344));
    u32 gr=0; CHECK(uc_mem_read(uc,0xb0002000,&gr,4)==UC_ERR_OK && gr==0x11223344);
    printf("  LUT write -> guest read: 0x%08x %s\n", gr, gr==0x11223344?"OK":"DIVERGE");

    // Fallback: phys fora da pool -> UC_ERR_ARG (chamador usa map_one anonimo).
    MapItem itx = decode_item(make_phys_desc(0xaa600000,l4mem_io),
                              make_fpage(0xe0000000,12,true,true,false));
    CHECK(map_one_aliased(uc,itx,pool,&lut)==UC_ERR_ARG);
    printf("  fallback (phys fora da pool) -> UC_ERR_ARG OK\n");

    uc_close(uc); free(host);
    if(g_fail==0){ printf("ALL TESTS PASSED\n"); return 0; }
    printf("%d CHECK(s) FAILED\n", g_fail); return 1;
}
