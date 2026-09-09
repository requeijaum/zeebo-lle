// test_threadspace_frame_resume.cpp — QW26 regression: ThreadControl (0x08) and
// SpaceControl (0x18) syscall stub frame restoration.
//
// Real firmware stubs (1.1.2_APPS.bin, VA 0xb0000000 = fileoff 0x30000):
//
//   ThreadControl (syscall 0x08, stub 0xb000c798):
//     0xb000c798: push {r4-r8, sb, sl, fp, lr}   (e92d4ff0)
//     0xb000c79c: ldr  r4, [sp, #0x24]           (e59d4024)
//     0xb000c7a0: ldr  r5, [sp, #0x28]           (e59d5028)
//     0xb000c7a4: ldr  r6, [sp, #0x2c]           (e59d602c)
//     0xb000c7a8: mov  ip, sp                     (e1a0c00d)
//     0xb000c7ac: mvn  sp, #0xf7                  (e3e0d0f7)  sp=0xffffff08, id=0x08
//     0xb000c7b0: svc  #0x1408                    (ef001408)
//     0xb000c7b4: pop  {r4-r8, sb, sl, fp, pc}    (e8bd8ff0)
//
//   SpaceControl (syscall 0x18, stub 0xb000c944):
//     0xb000c944: push {r4-r8, sb, sl, fp, lr}   (e92d4ff0)
//     0xb000c948: mov  ip, sp                     (e1a0c00d)
//     0xb000c94c: mvn  sp, #0xe7                  (e3e0d0e7)  sp=0xffffff18, id=0x18
//     0xb000c950: svc  #0x1418                    (ef001418)
//     0xb000c954: ldr  r2, [sp, #0x24]           (e59d2024)
//     0xb000c958: cmp  r2, #0                      (e3520000)
//     0xb000c95c: strne r1, [r2]                   (15821000)
//     0xb000c960: pop  {r4-r8, sb, sl, fp, pc}    (e8bd8ff0)
//
// UC_HOOK_INTR delivers PC at svc+4 (the epilogue). Resuming at target_pc = LR
// (the else fallback in zeebo_lle_main.cpp) jumps straight to the caller WITHOUT
// executing pop (and, for SpaceControl, without the ldr/cmp/strne writeback),
// destroying callee-saved r4-r11 and dropping the output write.
//
// Resuming at target_pc = pc (with SP restored to IP) lets the firmware's own
// epilogue pop the frame and (for 0x18) perform the output store.
//
// Usage:
//   ./test_threadspace_frame_resume        -> resume at pc (must PASS -> GREEN)
//   ./test_threadspace_frame_resume buggy  -> resume at lr (must FAIL -> RED)

#include <unicorn/unicorn.h>
#include <cstdio>
#include <cstdint>
#include <cstring>

using u32 = uint32_t;

static bool g_buggy_mode = false;

static void intr_hook(uc_engine* uc, uint32_t intno, void* ud) {
    (void)intno; (void)ud;
    u32 pc = 0, ip = 0, lr = 0;
    uc_reg_read(uc, UC_ARM_REG_PC, &pc);
    uc_reg_read(uc, UC_ARM_REG_R12, &ip);
    uc_reg_read(uc, UC_ARM_REG_LR, &lr);

    // Simulate host syscall handler clobbering callee-saved registers.
    u32 clob = 0xdeadbeef;
    uc_reg_write(uc, UC_ARM_REG_R4, &clob);
    uc_reg_write(uc, UC_ARM_REG_R5, &clob);
    uc_reg_write(uc, UC_ARM_REG_R6, &clob);
    uc_reg_write(uc, UC_ARM_REG_R7, &clob);
    // r1 = handler-provided output value for SpaceControl writeback.
    u32 out_val = 0xcafe1234;
    uc_reg_write(uc, UC_ARM_REG_R1, &out_val);

    u32 target_pc;
    if (g_buggy_mode) {
        target_pc = lr & ~1u;            // else fallback: skips the epilogue
    } else {
        target_pc = pc;                  // fix: resume at svc+4 epilogue
    }
    if (ip) uc_reg_write(uc, UC_ARM_REG_SP, &ip);
    uc_reg_write(uc, UC_ARM_REG_PC, &target_pc);
}

// Runs one stub. Returns true on contract satisfied.
static bool run_stub(const char* name, const u32* code, size_t code_words,
                     u32 svc_word_idx, bool check_output) {
    uc_engine* uc = nullptr;
    if (uc_open(UC_ARCH_ARM, UC_MODE_ARM, &uc) != UC_ERR_OK) {
        fprintf(stderr, "FAIL(%s): uc_open\n", name); return false;
    }
    const u32 STUB   = 0x1000;
    const u32 CALLER = 0x2000;
    const u32 STACK  = 0x3000;
    const u32 OUTPTR = 0x4000;         // where SpaceControl must store r1
    const u32 EXPECT_R4 = 0xb0d00000;
    const u32 EXPECT_OUT = 0xcafe1234;
    (void)svc_word_idx;

    uc_mem_map(uc, 0x0000, 0x6000, UC_PROT_ALL);
    uc_mem_write(uc, STUB, code, code_words * 4);
    u32 caller_code[] = { 0xe1a00000, 0xe1a00000 };
    uc_mem_write(uc, CALLER, caller_code, sizeof(caller_code));

    // Stack: after `push {9 regs}` sp drops 0x24. mov ip,sp captures that.
    // The stub reads args at [sp,#0x24] == the word at the pre-push sp.
    u32 sp = STACK + 0x800;
    // Pre-push sp points at the first stack arg. For SpaceControl [sp+0x24]
    // must hold the output pointer.
    u32 out_ptr = OUTPTR;
    uc_mem_write(uc, sp, &out_ptr, 4);   // becomes [ip+0x24] after push
    uc_reg_write(uc, UC_ARM_REG_SP, &sp);
    uc_reg_write(uc, UC_ARM_REG_R4, &EXPECT_R4);
    uc_reg_write(uc, UC_ARM_REG_R5, &EXPECT_R4);
    uc_reg_write(uc, UC_ARM_REG_R6, &EXPECT_R4);
    uc_reg_write(uc, UC_ARM_REG_R7, &EXPECT_R4);
    u32 caller_lr = CALLER;
    uc_reg_write(uc, UC_ARM_REG_LR, &caller_lr);
    u32 zero = 0;
    uc_mem_write(uc, OUTPTR, &zero, 4);

    uc_hook h;
    uc_hook_add(uc, &h, UC_HOOK_INTR, (void*)intr_hook, nullptr, 0, ~0ull);
    uc_err err = uc_emu_start(uc, STUB, CALLER + 4, 0, 30);
    if (err != UC_ERR_OK)
        fprintf(stderr, "[info %s] uc_emu_start err=%u (%s)\n", name, err, uc_strerror(err));

    u32 r4 = 0, r5 = 0, r6 = 0, r7 = 0, pc = 0, stored = 0;
    uc_reg_read(uc, UC_ARM_REG_R4, &r4);
    uc_reg_read(uc, UC_ARM_REG_R5, &r5);
    uc_reg_read(uc, UC_ARM_REG_R6, &r6);
    uc_reg_read(uc, UC_ARM_REG_R7, &r7);
    uc_reg_read(uc, UC_ARM_REG_PC, &pc);
    uc_mem_read(uc, OUTPTR, &stored, 4);
    uc_close(uc);

    bool regs_ok = (r4 == EXPECT_R4 && r5 == EXPECT_R4 && r6 == EXPECT_R4 && r7 == EXPECT_R4);
    bool out_ok = !check_output || (stored == EXPECT_OUT);
    printf("  [%s] mode=%s pc=0x%08x r4=0x%08x r5=0x%08x r6=0x%08x r7=0x%08x stored=0x%08x\n",
           name, g_buggy_mode ? "buggy" : "fixed", pc, r4, r5, r6, r7, stored);
    if (!regs_ok) { printf("  FAIL(%s): callee-saved corrupted\n", name); return false; }
    if (!out_ok)  { printf("  FAIL(%s): output writeback missing (got 0x%08x)\n", name, stored); return false; }
    return true;
}

int main(int argc, char** argv) {
    if (argc > 1 && strcmp(argv[1], "buggy") == 0) g_buggy_mode = true;

    // ThreadControl 0xb000c798 (exact bytes)
    u32 thread_code[] = {
        0xe92d4ff0, // push {r4-r8, sb, sl, fp, lr}
        0xe59d4024, // ldr r4, [sp, #0x24]
        0xe59d5028, // ldr r5, [sp, #0x28]
        0xe59d602c, // ldr r6, [sp, #0x2c]
        0xe1a0c00d, // mov ip, sp
        0xe3e0d0f7, // mvn sp, #0xf7
        0xef001408, // svc #0x1408
        0xe8bd8ff0  // pop {r4-r8, sb, sl, fp, pc}
    };
    // SpaceControl 0xb000c944 (exact bytes)
    u32 space_code[] = {
        0xe92d4ff0, // push {r4-r8, sb, sl, fp, lr}
        0xe1a0c00d, // mov ip, sp
        0xe3e0d0e7, // mvn sp, #0xe7
        0xef001418, // svc #0x1418
        0xe59d2024, // ldr r2, [sp, #0x24]
        0xe3520000, // cmp r2, #0
        0x15821000, // strne r1, [r2]
        0xe8bd8ff0  // pop {r4-r8, sb, sl, fp, pc}
    };

    printf("mode=%s\n", g_buggy_mode ? "buggy" : "fixed");
    // NOTE: ThreadControl overwrites r4-r6 from stack args via its own ldr;
    // check only that the epilogue ran (pc returned to caller) by verifying r7
    // (a genuine callee-saved not reloaded by the stub) survived. We reuse
    // run_stub but relax: for ThreadControl the ldr reloads r4/r5/r6, so its
    // EXPECT for those equals the stack args (0). Handle separately.
    bool ok = true;

    // SpaceControl: r4-r7 are pure callee-saved -> full contract + output store.
    ok &= run_stub("SpaceControl", space_code, 8, 3, /*check_output=*/true);

    // ThreadControl: r7,r8,sb,sl,fp are callee-saved (r4/r5/r6 reloaded by ldr
    // from stack args). We validate r7 survives the epilogue pop.
    // run_stub sets stack args at [sp+0x24..] = {out_ptr(0x4000),0,0}; the stub
    // reloads r4=0x4000,r5=0,r6=0 then pops r7 from frame. We check r7 only.
    {
        uc_engine* uc = nullptr;
        uc_open(UC_ARCH_ARM, UC_MODE_ARM, &uc);
        const u32 STUB=0x1000, CALLER=0x2000, STACK=0x3000, EXPECT=0xb0d00000;
        uc_mem_map(uc, 0, 0x6000, UC_PROT_ALL);
        uc_mem_write(uc, STUB, thread_code, sizeof(thread_code));
        u32 cc[]={0xe1a00000,0xe1a00000}; uc_mem_write(uc, CALLER, cc, sizeof(cc));
        u32 sp=STACK+0x800;
        uc_reg_write(uc, UC_ARM_REG_SP, &sp);
        uc_reg_write(uc, UC_ARM_REG_R7, &EXPECT);
        u32 lr=CALLER; uc_reg_write(uc, UC_ARM_REG_LR, &lr);
        uc_hook h; uc_hook_add(uc, &h, UC_HOOK_INTR, (void*)intr_hook, nullptr, 0, ~0ull);
        uc_emu_start(uc, STUB, CALLER+4, 0, 30);
        u32 r7=0, pc=0; uc_reg_read(uc, UC_ARM_REG_R7, &r7); uc_reg_read(uc, UC_ARM_REG_PC, &pc);
        uc_close(uc);
        bool tok = (r7 == EXPECT);
        printf("  [ThreadControl] mode=%s pc=0x%08x r7=0x%08x (expect=0x%08x)\n",
               g_buggy_mode?"buggy":"fixed", pc, r7, EXPECT);
        if (!tok) printf("  FAIL(ThreadControl): callee-saved r7 corrupted\n");
        ok &= tok;
    }

    if (!ok) { printf("RED: contract violated (%s mode)\n", g_buggy_mode?"buggy":"fixed"); return 1; }
    printf("PASS: both stubs restored callee-saved frame + writeback\n");
    return 0;
}
