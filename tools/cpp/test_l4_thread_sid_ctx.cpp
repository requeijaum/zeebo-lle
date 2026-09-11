// test_l4_thread_sid_ctx.cpp — Bug 1/2: ABI-correct SID association + full ctx.
//
// Positive controls:
//   * on_thread_control(dest, SpaceSpecifier=r1,...) associates SID from r1
//     (ThreadControl ABI), NOT from ExchangeRegisters r5 (which is UserDefHandle).
//   * cpu_context_read/write round-trip the FULL ARM context (r0..r12,SP,LR,
//     PC,CPSR incl. Thumb T-bit) through a real Unicorn engine.
//
// Negative control (argv "buggy"): mimic the OLD bug — take the SID from the
// ExchangeRegisters r5 (UserDefHandle) instead of ThreadControl r1. The SID
// then equals the UserDefHandle and the assertion MUST fail.
//
// Build: g++ -std=c++23 -DZEEBO_L4_MMU_WITH_UNICORN test_l4_thread_sid_ctx.cpp -lunicorn
#include "zeebo_l4_thread.h"
#include <cstdio>
#include <cstring>
#include <string>

using namespace zeebo_l4;
static int g_fail=0;
#define CHECK(c) do{ if(!(c)){ printf("  FAIL: %s (line %d)\n",#c,__LINE__); g_fail++; } }while(0)

int main(int argc, char** argv){
    const bool buggy = (argc>1 && std::string(argv[1])=="buggy");
    printf("== L4 ABI SID + full context%s ==\n", buggy?" (buggy/negative)":"");
    ThreadTable tt;

    const uint32_t dest = 0x20;
    const uint32_t SPACE_SPECIFIER = 7;      // r1 of ThreadControl -> the real SID
    const uint32_t USER_DEF_HANDLE = 0x1234; // r5 of ExchangeRegisters -> NOT the SID

    // Thread created & started via ExchangeRegisters (r5 = UserDefHandle).
    uint32_t ctrl = EXREGS_CTRL_SP|EXREGS_CTRL_IP|EXREGS_CTRL_DELIVER;
    tt.on_exchange_registers(dest, ctrl, 0xb01ffff0, 0xb0100000, 0);
    tt.set_user_def_handle(dest, USER_DEF_HANDLE);

    if (buggy) {
        // OLD BUG: SID taken from ExchangeRegisters r5 (UserDefHandle).
        tt.set_thread_space(dest, USER_DEF_HANDLE);
    } else {
        // CORRECT: SID comes from ThreadControl SpaceSpecifier (r1).
        tt.on_thread_control(dest, SPACE_SPECIFIER, /*sched=*/0x40, /*pager=*/0x50);
    }

    const ThreadInfo* t = tt.get_thread(dest);
    CHECK(t != nullptr);
    CHECK(t->space_id == SPACE_SPECIFIER);      // fails in buggy mode
    CHECK(t->user_def_handle == USER_DEF_HANDLE);
    CHECK(t->space_id != t->user_def_handle);   // fails in buggy mode

    // ThreadControl with space_specifier==0 must NOT clobber existing SID.
    tt.on_thread_control(dest, 0, 0x41, 0x51);
    CHECK(tt.get_thread(dest)->space_id == SPACE_SPECIFIER);
    CHECK(tt.get_thread(dest)->scheduler == 0x41);

    // Full-context round-trip through a real Unicorn engine.
    uc_engine* uc=nullptr;
    CHECK(uc_open(UC_ARCH_ARM, UC_MODE_ARM, &uc)==UC_ERR_OK);
    if (uc) {
        uint32_t stk=0x8000; uc_mem_map(uc, stk, 0x1000, UC_PROT_ALL);
        CpuContext in{};
        for(int i=0;i<13;i++) in.r[i]=0xC0DE0000+i;
        in.sp=0x8100; in.lr=0xb0100abc; in.pc=0xb0100200;
        in.cpsr=0x60000030; // Thumb (T=bit5) + condition flags
        cpu_context_write(uc, in);
        CpuContext out{};
        cpu_context_read(uc, &out);
        for(int i=0;i<13;i++) CHECK(out.r[i]==in.r[i]);
        CHECK(out.sp==in.sp);
        CHECK(out.lr==in.lr);
        CHECK((out.pc & ~1u)==(in.pc & ~1u));
        CHECK(((out.cpsr>>5)&1u)==1u); // Thumb mode preserved
        uc_close(uc);
    }

    if(g_fail==0){ printf("ALL TESTS PASSED\n"); return 0; }
    printf("%d CHECK(s) FAILED\n", g_fail); return 1;
}
