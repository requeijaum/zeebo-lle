// test_l4_base_ext_activate.cpp — PRODUCTION-PATH test of the OKL4
// base->extension SID sharing, driving the REAL SpaceManager::activate() on a
// REAL Unicorn engine (not the pure resolve_host() model).
//
// This closes the hard test gap from the adversarial review: the earlier
// test_l4_base_ext_share only exercised resolve_host(), never the production
// activate() layering/rebind that the interpreter actually runs.
//
// CAUSAL MODEL (measured, QW99): a source page owned ONLY by the base PD must
// be readable/executable from the EXTENSION space after link_base(). A static
// flat mapping pre-exists at the shared VA with the WRONG backing, so a bare
// reprotect cannot rebind — activate() must force unmap->map_ptr. When the
// extension is switched away, the prior space's correct contents must be
// visible again and no permanent hole may be left in the address space.
//
// Positive control (default): activate() layers base into extension and force-
// rebinds over the static map -> extension reads BASE backing; the base's own
// VA still reads base; switching back restores base contents; the static VA is
// never permanently destroyed.
//
// Negative/mutant control (argv "buggy"): the base linking is skipped (mutant
// disabling the activate layering) -> the extension reads the stale static/flat
// backing, so the base-shared assertions MUST fail (RED reproduced).
//
// Build: g++ -std=c++23 -DZEEBO_L4_MMU_WITH_UNICORN test_l4_base_ext_activate.cpp -lunicorn
#include "zeebo_l4_mmu.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

using namespace zeebo_l4;
static int g_fail = 0;
#define CHECK(c) do{ if(!(c)){ printf("  FAIL: %s (line %d)\n",#c,__LINE__); g_fail++; } }while(0)

static bool va_mapped(uc_engine* uc, u64 va) {
    uc_mem_region* regions = nullptr; uint32_t count = 0;
    if (uc_mem_regions(uc, &regions, &count) != UC_ERR_OK) return false;
    bool found = false;
    for (uint32_t i = 0; i < count; ++i)
        if (va >= regions[i].begin && va <= regions[i].end) { found = true; break; }
    uc_free(regions);
    return found;
}

int main(int argc, char** argv) {
    const bool buggy = (argc > 1 && std::string(argv[1]) == "buggy");
    printf("== L4 base->extension activate() on REAL Unicorn%s ==\n",
           buggy ? " (buggy/negative)" : "");

    uc_engine* uc = nullptr;
    CHECK(uc_open(UC_ARCH_ARM, UC_MODE_ARM, &uc) == UC_ERR_OK);
    if (!uc) { printf("no uc\n"); return 1; }

    // Abstract SIDs — mechanism must not depend on concrete values.
    const u32 BASE = 0xA1000100;
    const u32 EXT  = 0xA100C001;

    const u64 SIZE   = 0x1000;
    const u64 SRC_VA = 0xB0410000;  // page owned ONLY by BASE (the shared source)
    const u64 OWN_VA = 0xB0300000;  // VA both spaces have; EXT shadows BASE

    // Physical backings.
    u8* base_src = (u8*)aligned_alloc(0x1000, SIZE); memset(base_src, 0xAB, SIZE);
    u8* base_own = (u8*)aligned_alloc(0x1000, SIZE); memset(base_own, 0x11, SIZE);
    u8* ext_own  = (u8*)aligned_alloc(0x1000, SIZE); memset(ext_own,  0x22, SIZE);
    // Sentinel words to read back per backing.
    u32 w_src = 0xABCD0001; memcpy(base_src + 0x40, &w_src, 4);
    u32 w_bown= 0x11110002; memcpy(base_own + 0x40, &w_bown,4);
    u32 w_eown= 0x22220003; memcpy(ext_own  + 0x40, &w_eown,4);

    // A pre-existing STATIC flat mapping collides at SRC_VA with the WRONG
    // backing (Unicorn-owned RAM). activate() must force-rebind over it.
    CHECK(uc_mem_map(uc, SRC_VA, SIZE, UC_PROT_ALL) == UC_ERR_OK);
    u32 stale = 0xDEAD0000; uc_mem_write(uc, SRC_VA + 0x40, &stale, 4);

    SpaceManager mgr;
    mgr.record(BASE, SRC_VA, SIZE, UC_PROT_ALL, base_src);
    mgr.record(BASE, OWN_VA, SIZE, UC_PROT_ALL, base_own);
    mgr.record(EXT,  OWN_VA, SIZE, UC_PROT_ALL, ext_own);

    // THE production semantic operation under test.
    if (!buggy) mgr.link_base(EXT, BASE);

    // Activate BASE first (prior space), then EXT (target), like the scheduler.
    CHECK(mgr.activate(uc, BASE) == UC_ERR_OK);
    u32 r_base_src = 0, r_base_own = 0;
    uc_mem_read(uc, SRC_VA + 0x40, &r_base_src, 4);
    uc_mem_read(uc, OWN_VA + 0x40, &r_base_own, 4);
    printf("  BASE: src=0x%08x own=0x%08x\n", r_base_src, r_base_own);
    CHECK(r_base_src == 0xABCD0001);   // base sees its own source (rebound over static)
    CHECK(r_base_own == 0x11110002);

    // Switch to EXT: must read BASE's source (shared) and EXT's OWN (shadow).
    uc_err ae = mgr.activate(uc, EXT);
    if (!buggy) CHECK(ae == UC_ERR_OK);
    u32 r_ext_src = 0, r_ext_own = 0;
    uc_mem_read(uc, SRC_VA + 0x40, &r_ext_src, 4);
    uc_mem_read(uc, OWN_VA + 0x40, &r_ext_own, 4);
    printf("  EXT:  src=0x%08x own=0x%08x\n", r_ext_src, r_ext_own);
    CHECK(r_ext_src == 0xABCD0001);    // extension sees BASE backing at SRC_VA
    CHECK(r_ext_own == 0x22220003);    // extension's own shadows base at OWN_VA

    // Execute FROM the shared source in EXT to prove code fetch uses base
    // backing too. Put "mov r0,#0x5A ; bx lr" at SRC_VA and run it.
    u32 code[2] = { 0xe3a0005a, 0xe12fff1e };
    memcpy(base_src, code, sizeof code);
    u32 zero = 0; uc_reg_write(uc, UC_ARM_REG_R0, &zero);
    uc_emu_start(uc, SRC_VA, SRC_VA + 8, 0, 2);
    u32 r0 = 0; uc_reg_read(uc, UC_ARM_REG_R0, &r0);
    printf("  EXT exec@SRC_VA -> r0=0x%02x\n", r0);
    CHECK(r0 == 0x5A);                 // fetched from BASE backing via extension

    // Switch AWAY back to BASE: prior/static contents must be correct again and
    // the SRC_VA must still be mapped (no permanent hole).
    CHECK(mgr.activate(uc, BASE) == UC_ERR_OK);
    CHECK(va_mapped(uc, SRC_VA));      // never permanently destroyed
    u32 r_back_own = 0; uc_mem_read(uc, OWN_VA + 0x40, &r_back_own, 4);
    CHECK(r_back_own == 0x11110002);   // base OWN restored (ext shadow gone)
    // base source still correct (its own backing; note code was written at +0).
    u32 r_back_src = 0; uc_mem_read(uc, SRC_VA + 0x40, &r_back_src, 4);
    CHECK(r_back_src == 0xABCD0001);

    uc_close(uc);
    free(base_src); free(base_own); free(ext_own);
    if (g_fail == 0) { printf("ALL TESTS PASSED\n"); return 0; }
    printf("%d CHECK(s) FAILED\n", g_fail); return 1;
}
