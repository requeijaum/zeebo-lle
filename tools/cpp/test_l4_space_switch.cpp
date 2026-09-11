// test_l4_space_switch.cpp — Bug 1 (runtime): TRUE address-space switching.
//
// Proves that activating a target SID between uc_emu_start slices actually
// changes the physical backing seen at the SAME virtual address — both for
// EXECUTED instructions and for data READS — and that a write in one space
// does NOT leak into the other (isolation).
//
// Positive control (default): SpaceManager::activate() genuinely unmaps the
// outgoing space's regions and (re)maps the incoming space's regions, so the
// same VA decodes different code / reads different data per SID.
//
// Negative control (argv "buggy"): activation is skipped (the old flat-mapping
// behaviour where a single Unicorn view is shared by all spaces). The same VA
// then keeps the first space's bytes, so the isolation assertions MUST fail.
//
// Build: g++ -std=c++23 -DZEEBO_L4_MMU_WITH_UNICORN test_l4_space_switch.cpp -lunicorn
#include "zeebo_l4_mmu.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

using namespace zeebo_l4;
static int g_fail = 0;
#define CHECK(c) do{ if(!(c)){ printf("  FAIL: %s (line %d)\n",#c,__LINE__); g_fail++; } }while(0)

int main(int argc, char** argv) {
    const bool buggy = (argc > 1 && std::string(argv[1]) == "buggy");
    printf("== L4 runtime address-space switching%s ==\n", buggy ? " (buggy/negative)" : "");

    uc_engine* uc = nullptr;
    CHECK(uc_open(UC_ARCH_ARM, UC_MODE_ARM, &uc) == UC_ERR_OK);
    if (!uc) { printf("no uc\n"); return 1; }

    const u64 VA   = 0x10000000;
    const u64 SIZE = 0x1000;

    // Two distinct physical backings, one per space, aliased at the SAME VA.
    u8* host1 = (u8*)aligned_alloc(0x1000, SIZE); memset(host1, 0, SIZE);
    u8* host2 = (u8*)aligned_alloc(0x1000, SIZE); memset(host2, 0, SIZE);

    // ARM: "mov r0, #0x11" = e3a00011 ; "mov r0, #0x22" = e3a00022 ; bx lr = e12fff1e
    auto put = [](u8* h, u32 movword){
        u32 code[2] = { movword, 0xe12fff1e };
        memcpy(h, code, sizeof code);
    };
    put(host1, 0xe3a00011); // space 1 -> r0 = 0x11
    put(host2, 0xe3a00022); // space 2 -> r0 = 0x22
    // Distinguishing data word right after the code, same VA in both spaces.
    u32 d1 = 0xAAAA0001, d2 = 0xBBBB0002;
    memcpy(host1 + 0x100, &d1, 4);
    memcpy(host2 + 0x100, &d2, 4);

    SpaceManager mgr;
    mgr.record(/*sid=*/1, VA, SIZE, /*prot=*/UC_PROT_ALL, host1);
    mgr.record(/*sid=*/2, VA, SIZE, /*prot=*/UC_PROT_ALL, host2);

    auto activate = [&](u32 sid){
        if (buggy) {
            // Broken behaviour: map space 1 once and never switch backing.
            static bool once = false;
            if (!once) { mgr.activate(uc, 1); once = true; }
            return;
        }
        CHECK(mgr.activate(uc, sid) == UC_ERR_OK);
    };

    auto run_and_read = [&](u32 sid, u32* out_r0, u32* out_data){
        activate(sid);
        u32 zero = 0; uc_reg_write(uc, UC_ARM_REG_R0, &zero);
        uc_err e = uc_emu_start(uc, VA, VA + 8, 0, 2); // mov ; bx lr
        (void)e;
        uc_reg_read(uc, UC_ARM_REG_R0, out_r0);
        uc_mem_read(uc, VA + 0x100, out_data, 4);
    };

    u32 r0_a=0, dat_a=0, r0_b=0, dat_b=0;
    run_and_read(1, &r0_a, &dat_a);
    run_and_read(2, &r0_b, &dat_b);

    printf("  space1: r0=0x%08x data=0x%08x | space2: r0=0x%08x data=0x%08x\n",
           r0_a, dat_a, r0_b, dat_b);

    // Executed bytes changed across SID activation.
    CHECK(r0_a == 0x11);
    CHECK(r0_b == 0x22);
    CHECK(r0_a != r0_b);
    // Read bytes at the same VA changed across SID activation.
    CHECK(dat_a == 0xAAAA0001);
    CHECK(dat_b == 0xBBBB0002);
    CHECK(dat_a != dat_b);

    // Isolation: write into the ACTIVE space's VA touches only that backing.
    mgr.activate(uc, 1);
    u32 w = 0xDEADBEEF; uc_mem_write(uc, VA + 0x200, &w, 4);
    u32 chk1=0, chk2=0;
    memcpy(&chk1, host1 + 0x200, 4);
    memcpy(&chk2, host2 + 0x200, 4);
    CHECK(chk1 == 0xDEADBEEF);   // landed in space 1
    CHECK(chk2 == 0x00000000);   // did NOT leak into space 2

    uc_close(uc);
    free(host1); free(host2);
    if (g_fail == 0) { printf("ALL TESTS PASSED\n"); return 0; }
    printf("%d CHECK(s) FAILED\n", g_fail); return 1;
}
