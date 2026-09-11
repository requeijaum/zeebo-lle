// test_l4_base_ext_share.cpp — OKL4 base→extension PD domain/window sharing.
//
// CAUSAL MODEL (measured, QW99):
//   * Every MapControl source page (e.g. the compressed scatterload source that
//     covers 0xb04151a4) is registered by the kernel against the BASE PD space
//     (measured sid=0x80000100).
//   * The consumer thread that runs the second scatterload lives in an
//     EXTENSION space (measured sid=0x8000c001) created by ThreadControl with
//     Pager = the base space (measured pager=0x80000100).
//   * In OKL4 an extension space shares the base PD's mappings (map window /
//     shared domain). The emulator's SpaceManager treated each SID as fully
//     isolated, so the extension space could not see the base space's source
//     page -> the decoder read flat/zero backing -> length underflow ->
//     0xb0400064..6c infinite loop.
//
// This test proves the missing SEMANTIC OPERATION generally (no hardcoded SIDs
// or pages): after linking an extension space to its base space, a VA present
// ONLY in the base space resolves — for the extension space — to the BASE
// space's host backing; while VAs owned by the extension itself still win, and
// isolation to unrelated spaces holds.
//
// Positive control (default): base-linking is applied -> extension sees base.
// Negative control (argv "buggy"): base-linking skipped (old isolated model) ->
// extension cannot resolve the base-only page (RED reproduced).
//
// Build: g++ -std=c++23 test_l4_base_ext_share.cpp
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
    printf("== L4 base->extension PD domain sharing%s ==\n",
           buggy ? " (buggy/negative)" : "");

    // Abstract SIDs — the mechanism must not depend on their concrete values.
    const u32 BASE = 0xA1000100;   // base PD (owns the shared source)
    const u32 EXT  = 0xA100C001;   // extension PD (consumer runs here)
    const u32 OTHER= 0xA1010001;   // unrelated PD (must NOT see BASE)

    // A source page present ONLY in the base space.
    const u64 SRC_VA = 0xB0410000; u8 src_backing[0x1000];
    memset(src_backing, 0xAB, sizeof src_backing);
    // A page the extension owns itself, aliasing a VA the base also has.
    const u64 OWN_VA = 0xB0300000;
    u8 base_at_own[0x1000]; memset(base_at_own, 0x11, sizeof base_at_own);
    u8 ext_at_own[0x1000];  memset(ext_at_own,  0x22, sizeof ext_at_own);

    SpaceManager mgr;
    mgr.record(BASE, SRC_VA, 0x1000, 0, src_backing);
    mgr.record(BASE, OWN_VA, 0x1000, 0, base_at_own);
    mgr.record(EXT,  OWN_VA, 0x1000, 0, ext_at_own);
    mgr.record(OTHER,0xB0900000, 0x1000, 0, nullptr);

    // The semantic operation the emulator was missing: bind the extension space
    // to its base space (ThreadControl Pager == base space id).
    if (!buggy) mgr.link_base(EXT, BASE);

    // 1) Extension resolves the base-only source page -> BASE backing.
    u8* r_src = mgr.resolve_host(EXT, SRC_VA);
    printf("  EXT resolve(SRC_VA)=%p (base_backing=%p)\n",
           (void*)r_src, (void*)src_backing);
    CHECK(r_src == src_backing);

    // 2) Extension's OWN mapping shadows the base's at the same VA (ext wins).
    u8* r_own = mgr.resolve_host(EXT, OWN_VA);
    CHECK(r_own == ext_at_own);
    CHECK(r_own != base_at_own);

    // 3) Isolation: an unrelated space must NOT see the base source page.
    u8* r_other = mgr.resolve_host(OTHER, SRC_VA);
    CHECK(r_other == nullptr);

    // 4) The base space itself resolves its own source (unchanged).
    CHECK(mgr.resolve_host(BASE, SRC_VA) == src_backing);

    if (g_fail == 0) { printf("ALL TESTS PASSED\n"); return 0; }
    printf("%d CHECK(s) FAILED\n", g_fail); return 1;
}
