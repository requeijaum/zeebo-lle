// probe_map_ptr.cpp — experimento: uc_mem_map_ptr suporta aliasing?
// Mapeia UM buffer de host em DOIS VAs distintos do guest e verifica que
// escrever num VA reflete no outro (aliasing físico) e no host_ptr direto.
#include <unicorn/unicorn.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>

static int fails = 0;
#define CHECK(c) do{ if(!(c)){ printf("FAIL: %s (line %d)\n", #c, __LINE__); fails++; } }while(0)

int main() {
    uc_engine* uc;
    uc_err e = uc_open(UC_ARCH_ARM, UC_MODE_ARM, &uc);
    CHECK(e == UC_ERR_OK);

    const size_t SZ = 0x10000; // 64KB pool física de host
    uint8_t* pool = (uint8_t*)aligned_alloc(0x1000, SZ);
    memset(pool, 0, SZ);

    const uint64_t VA1 = 0xb0000000;
    const uint64_t VA2 = 0xd0000000; // alias do MESMO pool
    int prot = UC_PROT_READ | UC_PROT_WRITE | UC_PROT_EXEC;

    e = uc_mem_map_ptr(uc, VA1, SZ, prot, pool);
    printf("[1] map_ptr VA1=0x%llx -> %s\n",(unsigned long long)VA1, uc_strerror(e));
    CHECK(e == UC_ERR_OK);

    e = uc_mem_map_ptr(uc, VA2, SZ, prot, pool); // MESMO ptr, VA diferente
    printf("[2] map_ptr VA2=0x%llx (alias) -> %s\n",(unsigned long long)VA2, uc_strerror(e));
    bool alias_ok = (e == UC_ERR_OK);
    CHECK(alias_ok);

    // Escreve via guest em VA1, lê via guest em VA2 e via host_ptr direto.
    uint32_t w = 0xdeadbeef;
    CHECK(uc_mem_write(uc, VA1 + 0x100, &w, 4) == UC_ERR_OK);

    uint32_t r2 = 0, rhost = 0;
    if (alias_ok) {
        CHECK(uc_mem_read(uc, VA2 + 0x100, &r2, 4) == UC_ERR_OK);
        printf("[3] aliasing: VA2 le 0x%08x (esperado 0xdeadbeef) %s\n",
               r2, r2 == w ? "OK" : "DIVERGE");
        CHECK(r2 == w);
    }
    memcpy(&rhost, pool + 0x100, 4);
    printf("[4] host_ptr direto le 0x%08x (esperado 0xdeadbeef) %s\n",
           rhost, rhost == w ? "OK" : "DIVERGE");
    CHECK(rhost == w);

    // Caminho inverso: escreve no host_ptr, guest ve? (base do VTLB LUT)
    uint32_t h2 = 0x12345678;
    memcpy(pool + 0x200, &h2, 4);
    uint32_t rg = 0;
    CHECK(uc_mem_read(uc, VA1 + 0x200, &rg, 4) == UC_ERR_OK);
    printf("[5] host->guest: VA1 le 0x%08x (esperado 0x12345678) %s\n",
           rg, rg == h2 ? "OK" : "DIVERGE");
    CHECK(rg == h2);

    uc_close(uc);
    free(pool);
    printf(fails ? "PROBE FAILED (%d)\n" : "PROBE OK\n", fails);
    return fails ? 1 : 0;
}
