// test_l4_mmu_revoke.cpp — Bug 7: semântica real de revogação rwx=0.
// Uma fpage com rwx=0 sobre uma VA já mapeada deve REVOGAR o acesso
// (UC_PROT_NONE), não forçar leitura (o antigo `if(prot==0)prot=1`).
// Compila: g++ -std=c++23 -DZEEBO_L4_MMU_WITH_UNICORN test_l4_mmu_revoke.cpp -lunicorn
#include "zeebo_l4_mmu.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>

using namespace zeebo_l4;
static int g_fail = 0;
#define CHECK(c) do{ if(!(c)){ printf("  FAIL: %s (line %d)\n",#c,__LINE__); g_fail++; } }while(0)

static u32 make_phys_desc(u64 phys, l4attrib_e a){ return (((u32)(phys>>6))<<6)|((u32)a&0x3f); }
static u32 make_fpage(u64 va,u32 sz,bool r,bool w,bool x){
    u32 raw=0; raw|=(x?1u:0)<<0; raw|=(w?1u:0)<<1; raw|=(r?1u:0)<<2;
    raw|=(sz&0x3f)<<4; raw|=((u32)(va>>10)&0x3fffff)<<10; return raw;
}

int main(){
    printf("== L4 MMU rwx=0 revocation ==\n");
    uc_engine* uc; CHECK(uc_open(UC_ARCH_ARM, UC_MODE_ARM, &uc)==UC_ERR_OK);

    // 1) Mapeia RW, escreve, lê de volta: controle positivo.
    MapItem rw = decode_item(make_phys_desc(0,l4mem_cached),
                             make_fpage(0xb0000000,16,true,true,false)); // 64KB, r+w
    CHECK(map_one(uc, rw)==UC_ERR_OK);
    u32 w=0xdeadbeef;
    CHECK(uc_mem_write(uc,0xb0001000,&w,4)==UC_ERR_OK);
    u32 r=0; CHECK(uc_mem_read(uc,0xb0001000,&r,4)==UC_ERR_OK && r==w);

    // 2) Revoga: rwx=0 sobre a MESMA VA. Deve virar UC_PROT_NONE.
    // NOTA: uc_mem_read/uc_mem_write (API de host) IGNORAM as permissões do
    // guest; só acessos EMULADOS as respeitam. Por isso verificamos a proteção
    // efetiva da região via uc_mem_regions (o que uma instrução ARM veria).
    MapItem revoke = decode_item(make_phys_desc(0,l4mem_cached),
                                 make_fpage(0xb0000000,16,false,false,false));
    CHECK(map_one(uc, revoke)==UC_ERR_OK);
    auto prot_at = [&](u64 addr)->int{
        uc_mem_region* rg=nullptr; uint32_t n=0; int p=-1;
        if (uc_mem_regions(uc,&rg,&n)==UC_ERR_OK){
            for(uint32_t i=0;i<n;i++) if(addr>=rg[i].begin && addr<=rg[i].end){ p=rg[i].perms; break; }
            uc_free(rg);
        }
        return p;
    };
    int pr = prot_at(0xb0001000);
    printf("  pos-revoke prot=%d (esperado 0 = UC_PROT_NONE)\n", pr);
    CHECK(pr == 0); // UC_PROT_NONE: nem leitura nem escrita para o guest

    // 3) Controle: re-conceder r deixa legível de novo.
    MapItem regrant = decode_item(make_phys_desc(0,l4mem_cached),
                                  make_fpage(0xb0000000,16,true,false,false));
    CHECK(map_one(uc, regrant)==UC_ERR_OK);
    CHECK(prot_at(0xb0001000) == UC_PROT_READ);

    // 4) Revogação whole-space rwx=0 sobre AS com regiões mapeadas: revoga tudo.
    MapItem ws = decode_item(make_phys_desc(0,l4mem_default),
                             make_fpage(0,32,false,false,false)); // 2^32, rwx=0
    CHECK(revoke_whole_space(uc, ws.fpage)==UC_ERR_OK);
    int pr4 = prot_at(0xb0001000);
    printf("  pos-whole-space-revoke prot=%d (esperado 0)\n", pr4);
    CHECK(pr4 == 0);

    uc_close(uc);
    if(g_fail==0){ printf("ALL TESTS PASSED\n"); return 0; }
    printf("%d CHECK(s) FAILED\n", g_fail); return 1;
}
