// test_l4_kip_trap.cpp — host-only Unicorn reproduction of the REAL
// L4_KernelInterface stub (0xb000c720) + the current c0_intr_hook KIP handler.
//
// Question under test: does the extra `uc_mem_write(sp+0/4/8)` in the 0xb4
// handler corrupt r4 (the page-cache pointer consumed by largest_aligned_fpage)?
//
// Mechanism check: at the intr hook the SP is the TRAP SP set by the stub's
// `mvn sp,#0x4b` (== 0xFFFFFFB4), NOT the caller frame. The caller's {r4,r5,r6}
// live at `ip` (== old SP). So sp+0/4/8 (0xFFFFFFB4/B8/BC) is scratch that the
// firmware never reads and can never alias the pushed r4 slot at [ip].
//
// Real stub bytes (confirmed from firmware 1.1.2_APPS.bin):
//   70402de9 push {r4,r5,r6,lr}
//   0040a0e1 mov  r4,r0
//   0150a0e1 mov  r5,r1
//   0260a0e1 mov  r6,r2
//   0dc0a0e1 mov  ip,sp
//   4bd0e0e3 mvn  sp,#0x4b
//   140000ef svc  #0x14
//   000054e3 cmp  r4,#0
//   00108415 strne r1,[r4]
//   00208515 strne r2,[r5]   (0xb000c744)
//   00308615 strne r3,[r6]   (0xb000c748)
//   7080bde8 pop  {r4,r5,r6,pc}
//
// Compile: g++ -std=c++23 test_l4_kip_trap.cpp -lunicorn
#include <unicorn/unicorn.h>
#include <cstdio>
#include <cstdint>
#include <cstring>

using u32 = uint32_t;
static int g_fail = 0;
#define CHECK(c) do{ if(!(c)){ printf("  FAIL: %s (line %d)\n",#c,__LINE__); g_fail++; } }while(0)

static const u32 KIP_BASE   = 0xb0043000;   // arbitrary KIP page addr for the test
static const u32 STUB_ADDR  = 0xb000c720;
static const u32 CALLER     = 0xb0002000;   // caller: bl STUB ; then loops
static const u32 STACK_TOP  = 0xb0060000;   // real caller stack top
static const u32 SENTINEL_R4= 0xcace0000;   // "page-cache pointer" we must preserve

// State the hook toggles when it fires so the test knows the trap happened.
struct HookState { bool fired=false; u32 trap_sp=0; };

// Replica of the CURRENT production 0xb4 handler branch (incl. the suspicious
// sp+0/4/8 writes), plus the shared epilogue (SP=ip, PC=pc == svc+4).
static void intr_hook(uc_engine* uc, uint32_t intno, void* ud){
    if (intno != 2) return;
    HookState* st = (HookState*)ud;
    u32 pc=0, sp=0, ip=0;
    uc_reg_read(uc, UC_ARM_REG_PC, &pc);
    uc_reg_read(uc, UC_ARM_REG_SP, &sp);
    uc_reg_read(uc, UC_ARM_REG_R12, &ip);
    u32 syscall = sp & 0xFF;
    if (syscall != 0xb4) return;   // only model KIP here
    st->fired = true;
    st->trap_sp = sp;

    u32 kip_r1 = 0x0000000c, kip_r2 = 0x00000002, kip_r3 = 0;
    u32 res_r0 = KIP_BASE;

    u32 r4=0,r5=0,r6=0;
    uc_reg_read(uc, UC_ARM_REG_R4, &r4);
    uc_reg_read(uc, UC_ARM_REG_R5, &r5);
    uc_reg_read(uc, UC_ARM_REG_R6, &r6);
    if (r4) uc_mem_write(uc, r4, &kip_r1, 4);
    if (r5) uc_mem_write(uc, r5, &kip_r2, 4);
    if (r6) uc_mem_write(uc, r6, &kip_r3, 4);

#ifndef DROP_SP_WRITE
    // === The block under test ===
    uc_mem_write(uc, sp + 0, &kip_r1, 4);
    uc_mem_write(uc, sp + 4, &kip_r2, 4);
    uc_mem_write(uc, sp + 8, &kip_r3, 4);
#endif

    uc_reg_write(uc, UC_ARM_REG_R0, &res_r0);
    uc_reg_write(uc, UC_ARM_REG_R1, &kip_r1);
    uc_reg_write(uc, UC_ARM_REG_R2, &kip_r2);
    uc_reg_write(uc, UC_ARM_REG_R3, &kip_r3);
    u32 target_pc = pc;                       // pc already == svc+4
    uc_reg_write(uc, UC_ARM_REG_PC, &target_pc);
    if (ip) uc_reg_write(uc, UC_ARM_REG_SP, &ip);   // restore caller frame ptr
}

static void put(uc_engine* uc, u32 addr, u32 w){ uc_mem_write(uc, addr, &w, 4); }

int main(){
    printf("== L4_KernelInterface trap-SP / r4 preservation (host-only) ==\n");
    uc_engine* uc;
    CHECK(uc_open(UC_ARCH_ARM, UC_MODE_ARM, &uc)==UC_ERR_OK);

    // Map firmware code window, caller stack, and the TRAP scratch page.
    uc_mem_map(uc, 0xb0000000, 0x00100000, UC_PROT_ALL);   // 1MB covers stub+caller+stack
    uc_mem_map(uc, 0xffff0000, 0x00010000, UC_PROT_ALL);   // trap scratch (0xFFFFFFB4)

    // --- Real stub bytes at 0xb000c720 ---
    const u32 stub[] = {
        0xe92d4070, // push {r4,r5,r6,lr}
        0xe1a04000, // mov r4,r0
        0xe1a05001, // mov r5,r1
        0xe1a06002, // mov r6,r2
        0xe1a0c00d, // mov ip,sp
        0xe3e0d04b, // mvn sp,#0x4b
        0xef000014, // svc #0x14
        0xe3540000, // cmp r4,#0
        0x15841000, // strne r1,[r4]
        0x15852000, // strne r2,[r5]
        0x15863000, // strne r3,[r6]
        0xe8bd8070, // pop {r4,r5,r6,pc}
    };
    for (unsigned i=0;i<sizeof(stub)/4;i++) put(uc, STUB_ADDR + i*4, stub[i]);

    // --- Caller: bl STUB ; then a self-branch to halt ---
    // bl offset = (STUB - (CALLER+8)) >> 2
    int32_t off = ((int32_t)STUB_ADDR - (int32_t)(CALLER + 8)) >> 2;
    u32 bl = 0xeb000000u | ((u32)off & 0x00ffffff);
    put(uc, CALLER + 0, bl);
    put(uc, CALLER + 4, 0xeafffffe);          // b . (infinite self-loop == done)

    HookState st;
    uc_hook h; uc_hook_add(uc, &h, UC_HOOK_INTR, (void*)intr_hook, &st, 1, 0);

    // Caller sets r4 = page-cache pointer sentinel, r0=0 (no out-ptr so strne skipped).
    u32 sp = STACK_TOP, zero = 0, sent = SENTINEL_R4;
    uc_reg_write(uc, UC_ARM_REG_SP, &sp);
    uc_reg_write(uc, UC_ARM_REG_R4, &sent);
    uc_reg_write(uc, UC_ARM_REG_R0, &zero);
    uc_reg_write(uc, UC_ARM_REG_R1, &zero);
    uc_reg_write(uc, UC_ARM_REG_R2, &zero);

    // Run: caller -> stub -> (trap) -> stub epilogue -> pop -> back to caller self-loop.
    uc_err e = uc_emu_start(uc, CALLER, CALLER+4 /*stop at self-loop*/, 0, 40);
    printf("  emu_start=%s hook_fired=%d trap_sp=0x%08x\n",
           uc_strerror(e), st.fired, st.trap_sp);
    CHECK(st.fired);
    CHECK(st.trap_sp == 0xffffffb4);          // mvn #0x4b -> proves trap SP, not caller frame

    u32 r4_after=0; uc_reg_read(uc, UC_ARM_REG_R4, &r4_after);
    printf("  r4 after L4_KernelInterface = 0x%08x (expect sentinel 0x%08x)\n",
           r4_after, SENTINEL_R4);
    CHECK(r4_after == SENTINEL_R4);           // page-cache pointer must survive the syscall

    // Independent proof the extra write landed in unread scratch, never the frame:
    // the caller's pushed r4 slot is at [ip] == old SP-16 == STACK_TOP-16.
    u32 pushed_r4=0; uc_mem_read(uc, STACK_TOP-16, &pushed_r4, 4);
    printf("  pushed r4 slot @[ip]=0x%08x (uncorrupted=%s)\n",
           pushed_r4, pushed_r4==SENTINEL_R4?"yes":"NO");
    CHECK(pushed_r4 == SENTINEL_R4);

    uc_close(uc);
    if (g_fail==0){ printf("ALL TESTS PASSED\n"); return 0; }
    printf("%d CHECK(s) FAILED\n", g_fail); return 1;
}
