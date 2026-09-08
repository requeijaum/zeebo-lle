// test_l4_mmu.cpp — validação da decodificação de L4_MapControl (sem Unicorn).
// Compila: g++ -std=c++23 -O2 test_l4_mmu.cpp -o test_l4_mmu
//
// Constrói descritores reais bit-a-bit conforme os headers OKL4
// (map.h / fpage.h) e verifica que zeebo_l4_mmu.h os decodifica corretamente.
#include "zeebo_l4_mmu.h"
#include <cstdio>
#include <cassert>

using namespace zeebo_l4;

static int g_fail = 0;
#define CHECK(cond) do { if (!(cond)) { \
    printf("  FAIL: %s (line %d)\n", #cond, __LINE__); g_fail++; } } while (0)

// --- Encoders espelhando exatamente os headers OKL4 ------------------------

// phys_desc_t: attr[0..5], base[6..31] onde base = phys >> 10.
static u32 make_phys_desc(u64 phys, l4attrib_e attr) {
    u32 base = (u32)(phys >> 10);
    return (base << 6) | ((u32)attr & 0x3f);
}

// fpage_t::set(base, size, r, w, x):
//   raw=0; x.base = (base & mask) >> 10; x.size=size; rwx bits.
// Layout: exec[0] write[1] read[2] meta[3] size[4..9] base[10..31].
static u32 make_fpage(u64 vaddr, u32 size_log2, bool r, bool w, bool x) {
    u32 raw = 0;
    raw |= (x ? 1u : 0u) << 0;
    raw |= (w ? 1u : 0u) << 1;
    raw |= (r ? 1u : 0u) << 2;
    raw |= (size_log2 & 0x3f) << 4;
    raw |= ((u32)(vaddr >> 10) & 0x3fffff) << 10; // 22 bits de base
    return raw;
}

// map_control_t: n[0..5], q[30], m[31].
static u32 make_control(u32 count, bool modify, bool query) {
    u32 raw = (count - 1) & 0x3f;
    if (query)  raw |= (1u << 30);
    if (modify) raw |= (1u << 31);
    return raw;
}

int main() {
    printf("== L4_MapControl decode tests ==\n");

    // 1) map_control_t
    {
        MapControl c(make_control(3, /*modify*/true, /*query*/false));
        CHECK(c.count() == 3);
        CHECK(c.highest_item() == 2);
        CHECK(c.is_modify());
        CHECK(!c.is_query());

        MapControl q(make_control(1, false, true));
        CHECK(q.count() == 1);
        CHECK(q.is_query());
        CHECK(!q.is_modify());
    }

    // 2) phys_desc_t — RAM APPS física do Zeebo, cacheada.
    {
        u64 phys = 0x10000000;
        PhysDesc p(make_phys_desc(phys, l4mem_cached));
        CHECK(p.phys_base() == phys);
        CHECK(p.attributes() == l4mem_cached);
        CHECK(!p.is_device());

        // Device memory (MMIO), write-combined.
        PhysDesc io(make_phys_desc(0xaa600000, l4mem_io_combined));
        CHECK(io.phys_base() == 0xaa600000);
        CHECK(io.is_device());
    }

    // 3) fpage_t — página de 4KB (size_log2=12) rwx, VA de usuário Iguana.
    {
        u64 va = 0xb0001000;
        Fpage f(make_fpage(va, 12, true, true, true));
        CHECK(f.vaddr() == va);
        CHECK(f.size_log2() == 12);
        CHECK(f.size_bytes() == 4096);
        CHECK(f.is_read() && f.is_write() && f.is_execute());
        CHECK(f.rwx() == 7);
        CHECK(!f.is_nil());
        CHECK(fpage_to_uc_prot(f) == (1 | 2 | 4));

        // Página só-leitura de 1MB (size_log2=20).
        Fpage ro(make_fpage(0xb0100000, 20, true, false, false));
        CHECK(ro.size_bytes() == 0x100000);
        CHECK(ro.is_read() && !ro.is_write() && !ro.is_execute());
        CHECK(ro.rwx() == 4);
        CHECK(fpage_to_uc_prot(ro) == 1);

        // Página R+X (código), sem escrita.
        Fpage rx(make_fpage(0xf0000000, 12, true, false, true));
        CHECK(rx.rwx() == 5);
        CHECK(fpage_to_uc_prot(rx) == (1 | 4));

        // nil / complete
        CHECK(Fpage(0).is_nil());
    }

    // 4) Par completo (phys, fpage) via decode_item, como viria dos MRs.
    {
        u32 mr_phys  = make_phys_desc(0x10200000, l4mem_writeback);
        u32 mr_fpage = make_fpage(0xb0200000, 12, true, true, false);
        MapItem it = decode_item(mr_phys, mr_fpage);
        CHECK(it.phys.phys_base() == 0x10200000);
        CHECK(it.phys.attributes() == l4mem_writeback);
        CHECK(it.fpage.vaddr() == 0xb0200000);
        CHECK(it.fpage.size_bytes() == 4096);
        CHECK(it.fpage.rwx() == (2 | 4)); // write|read, sem exec
        CHECK(fpage_to_uc_prot(it.fpage) == (1 | 2));
    }

    // 5) Layout de MRs: simula o buffer do UTCB e confere a ordem
    //    MR[i*2]=phys, MR[i*2+1]=fpage (conforme pistachio/src/map.cc).
    {
        const u32 count = 2;
        u32 mr[IPC_NUM_MR] = {0};
        struct { u64 phys; u64 va; u32 sz; bool r,w,x; l4attrib_e a; } spec[count] = {
            { 0x10000000, 0xb0000000, 12, true, true,  true,  l4mem_cached },
            { 0xaa600000, 0xd0600000, 16, true, true,  false, l4mem_io     },
        };
        for (u32 i = 0; i < count; i++) {
            mr[i*2]   = make_phys_desc(spec[i].phys, spec[i].a);
            mr[i*2+1] = make_fpage(spec[i].va, spec[i].sz,
                                   spec[i].r, spec[i].w, spec[i].x);
        }
        for (u32 i = 0; i < count; i++) {
            MapItem it = decode_item(mr[i*2], mr[i*2+1]);
            CHECK(it.phys.phys_base()  == spec[i].phys);
            CHECK(it.phys.attributes() == spec[i].a);
            CHECK(it.fpage.vaddr()     == spec[i].va);
            CHECK(it.fpage.size_bytes()== ((u64)1 << spec[i].sz));
        }
    }

    if (g_fail == 0) {
        printf("ALL TESTS PASSED\n");
        return 0;
    }
    printf("%d CHECK(s) FAILED\n", g_fail);
    return 1;
}
