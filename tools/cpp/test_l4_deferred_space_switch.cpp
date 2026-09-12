// test_l4_deferred_space_switch.cpp — Bug (MMU invariant): address-space
// activation requested from INSIDE a Unicorn hook must be DEFERRED and applied
// only BETWEEN uc_emu_start slices.
//
// Concrete bug: c0_intr_hook (cases L4_Ipc / L4_ThreadSwitch) called
// SpaceManager::activate() directly, which runs uc_mem_unmap/uc_mem_map_ptr.
// zeebo_l4_mmu.h documents the hard invariant that activate() must NEVER run
// inside a code/mem/intr hook — remapping during TB translation corrupts the
// translation cache (SIGSEGV/hang). The fix mirrors the existing deferred TB
// invalidation queue: the hook only QUEUEs the switch (queue_activate), and the
// scheduler loop drains it between slices (drain_activate).
//
// This test drives REAL Unicorn with a genuine UC_HOOK_INTR. It proves:
//   GREEN (default): the hook only queues — active_sid() and the physical
//   backing are UNTOUCHED while the slice is in flight (the code executing right
//   after the svc still reads the OUTGOING space's bytes). The switch becomes
//   visible only after the between-slice drain_activate().
//   RED (argv "buggy"): the pre-fix behaviour calls activate() directly inside
//   the hook. The switch then takes effect mid-slice (active_sid changes and the
//   post-svc load reads the incoming space) — violating the invariant. The
//   invariant assertions MUST fail (or the unsafe in-hook remap crashes).
//
// Build: g++ -std=c++23 -DZEEBO_L4_MMU_WITH_UNICORN test_l4_deferred_space_switch.cpp -lunicorn
#include "zeebo_l4_mmu.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

using namespace zeebo_l4;
static int g_fail = 0;
#define CHECK(c) do{ if(!(c)){ printf("  FAIL: %s (line %d)\n",#c,__LINE__); g_fail++; } }while(0)

static bool g_buggy = false;

struct HookCtx {
    SpaceManager* mgr;
    u32 next_sid;
    // Observed INSIDE the hook, right after the switch decision:
    u32 sid_in_hook = 0;
    bool ran = false;
};

// Simulates the c0_intr_hook syscall dispatcher deciding to switch space.
static void intr_hook(uc_engine* uc, uint32_t intno, void* ud) {
    (void)intno;
    HookCtx* h = (HookCtx*)ud;
    h->ran = true;
    if (g_buggy) {
        // PRE-FIX (unsafe): remap the address space DIRECTLY from inside the hook.
        h->mgr->activate(uc, h->next_sid);
    } else {
        // FIXED: only enqueue. No uc_mem_unmap/map_ptr here.
        h->mgr->queue_activate(h->next_sid);
    }
    h->sid_in_hook = h->mgr->active_sid();
}

int main(int argc, char** argv) {
    g_buggy = (argc > 1 && std::string(argv[1]) == "buggy");
    printf("== L4 deferred address-space switch%s ==\n", g_buggy ? " (buggy/RED)" : "");

    uc_engine* uc = nullptr;
    CHECK(uc_open(UC_ARCH_ARM, UC_MODE_ARM, &uc) == UC_ERR_OK);
    if (!uc) { printf("no uc\n"); return 1; }

    const u64 VA   = 0x10000000;
    const u64 SIZE = 0x1000;

    u8* host1 = (u8*)aligned_alloc(0x1000, SIZE); memset(host1, 0, SIZE);
    u8* host2 = (u8*)aligned_alloc(0x1000, SIZE); memset(host2, 0, SIZE);

    // Code at VA:  svc #0 ; ldr r1,[pc,#0xf4] (-> VA+0x100) ; bx lr
    //   svc #0            = 0xef000000
    //   ldr r1,[pc,#0xf4] = 0xe59f10f4   (pc = VA+8+... ; pc points 8 ahead: VA+0x8, +0xf4 = VA+0xfc? )
    // To be robust across pc-offset arithmetic we instead load an absolute value
    // written just after the code and use a fixed literal. Simpler: put the data
    // word at VA+0x100 and compute the literal offset precisely.
    //   Instruction at VA+4 (the ldr) has pc == VA+4+8 == VA+0xc.
    //   Target VA+0x100 => imm = 0x100 - 0xc = 0xf4.
    auto put = [](u8* h, u32 dataword){
        u32 code[3] = { 0xef000000u /*svc #0*/, 0xe59f10f4u /*ldr r1,[pc,#0xf4]*/, 0xe12fff1eu /*bx lr*/ };
        memcpy(h, code, sizeof code);
        memcpy(h + 0x100, &dataword, 4);
    };
    put(host1, 0xAAAA0001);
    put(host2, 0xBBBB0002);

    SpaceManager mgr;
    mgr.record(/*sid=*/1, VA, SIZE, UC_PROT_ALL, host1);
    mgr.record(/*sid=*/2, VA, SIZE, UC_PROT_ALL, host2);

    // Bring up space 1 (between-slice, legitimately) so a slice can run.
    CHECK(mgr.activate(uc, 1) == UC_ERR_OK);
    CHECK(mgr.active_sid() == 1);

    HookCtx hctx{ &mgr, /*next_sid=*/2 };
    uc_hook hh;
    CHECK(uc_hook_add(uc, &hh, UC_HOOK_INTR, (void*)intr_hook, &hctx, 1, 0) == UC_ERR_OK);

    // Run ONE slice: svc (fires hook) ; ldr r1 ; bx lr.
    u32 lr = 0; uc_reg_write(uc, UC_ARM_REG_LR, &lr); // bx lr -> 0 ends slice
    u32 r1z = 0; uc_reg_write(uc, UC_ARM_REG_R1, &r1z);
    uc_err e = uc_emu_start(uc, VA, 0, 0, 3);
    (void)e;
    CHECK(hctx.ran);

    u32 r1_after_slice = 0;
    uc_reg_read(uc, UC_ARM_REG_R1, &r1_after_slice);
    u32 sid_after_slice = mgr.active_sid();

    printf("  in-hook active_sid=0x%x | post-svc ldr r1=0x%08x | post-slice active_sid=0x%x\n",
           hctx.sid_in_hook, r1_after_slice, sid_after_slice);

    // INVARIANT 1: the hook must NOT have switched the space (still SID 1 while
    // the slice was in flight). Pre-fix activate()-in-hook makes this SID 2 -> RED.
    CHECK(hctx.sid_in_hook == 1);
    CHECK(sid_after_slice == 1);
    // INVARIANT 2: the instruction executing right after the svc in the SAME
    // slice still saw the OUTGOING space's backing. Pre-fix mid-slice remap makes
    // it read space 2 -> RED.
    CHECK(r1_after_slice == 0xAAAA0001);
    // In the fixed path a switch must have been QUEUED for the drain.
    if (!g_buggy) {
        CHECK(mgr.has_pending_activate());
        CHECK(mgr.pending_activate_sid() == 2);
    }

    // BETWEEN-SLICE DRAIN: now (engine quiescent) the queued switch is applied.
    CHECK(mgr.drain_activate(uc) == UC_ERR_OK);
    CHECK(mgr.active_sid() == 2);
    CHECK(!mgr.has_pending_activate());

    // After the drain, the SAME VA now reads space 2's backing.
    u32 dat2 = 0; uc_mem_read(uc, VA + 0x100, &dat2, 4);
    printf("  after drain: active_sid=0x%x data@VA+0x100=0x%08x\n", mgr.active_sid(), dat2);
    CHECK(dat2 == 0xBBBB0002);

    uc_close(uc);
    free(host1); free(host2);
    if (g_fail == 0) { printf("ALL TESTS PASSED\n"); return 0; }
    printf("%d CHECK(s) FAILED\n", g_fail); return 1;
}
