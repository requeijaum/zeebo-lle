// test_exregs_frame_resume.cpp — QW27 regression: ExchangeRegisters (0x0c),
// ThreadSwitch (0x04) and Schedule (0x10) syscall stub frame restoration and
// output writebacks in c0_intr_hook.
//
// Real firmware stubs (1.1.2_APPS.bin, VA 0xb0000000 = fileoff 0x30000):
//
//   ExchangeRegisters (syscall 0x0c, stub 0xb000c758) — TRAP-STACK epilogue:
//     0xb000c758: push {r4-r8, sb, sl, fp, lr}   (e92d4ff0)
//     0xb000c75c: ldr  r4, [sp, #0x24]           (e59d4024)
//     0xb000c760: ldr  r5, [sp, #0x28]           (e59d5028)
//     0xb000c764: ldr  r6, [sp, #0x2c]           (e59d602c)
//     0xb000c768: mov  ip, sp                     (e1a0c00d)
//     0xb000c76c: mvn  sp, #0xf3                  (e3e0d0f3)  sp=0xffffff0c
//     0xb000c770: svc  #0x140c                    (ef00140c)
//     0xb000c774: add  lr, sp, #0x30              (e28de030)  <-- svc+4
//     0xb000c778: ldm  lr, {r7,r8,sb,sl,fp,ip}   (e89e1f80)
//     0xb000c77c: str  r1, [r7]                    (e5871000)
//     0xb000c780: str  r2, [r8]                    (e5882000)
//     0xb000c784: str  r3, [sb]                    (e5893000)
//     0xb000c788: str  r4, [sl]                    (e58a4000)
//     0xb000c78c: str  r5, [fp]                    (e58b5000)
//     0xb000c790: str  r6, [ip]                    (e58c6000)
//     0xb000c794: pop  {r4-r8, sb, sl, fp, pc}    (e8bd8ff0)
//   Without SP=ip, `add lr,sp,#0x30` uses the trap sp (0xffffff0c) and the final
//   pop unwinds garbage -> PC=0x00000000.
//
//   ThreadSwitch (syscall 0x04, stub 0xb000c7b8):
//     0xb000c7b8: push {r4-r8, sb, sl, fp, lr}   (e92d4ff0)
//     0xb000c7bc: mov  ip, sp                     (e1a0c00d)
//     0xb000c7c0: mvn  sp, #0xfb                  (e3e0d0fb)
//     0xb000c7c4: svc  #0x1404                    (ef001404)
//     0xb000c7c8: pop  {r4-r8, sb, sl, fp, pc}    (e8bd8ff0)  <-- svc+4
//
//   Schedule (syscall 0x10, stub 0xb000c7cc):
//     0xb000c7cc: push {r4-r8, sb, sl, fp, lr}   (e92d4ff0)
//     0xb000c7d0: ldr  r4, [sp, #0x24]           (e59d4024)
//     0xb000c7d4: ldr  r5, [sp, #0x28]           (e59d5028)
//     0xb000c7d8: mov  ip, sp                     (e1a0c00d)
//     0xb000c7dc: mvn  sp, #0xef                  (e3e0d0ef)
//     0xb000c7e0: svc  #0x1410                    (ef001410)  svc; pc->svc+4
//     0xb000c7e4: ldr  r7, [sp, #0x2c]           (e59d702c)  <-- svc+4
//     0xb000c7e8: ldr  r8, [sp, #0x30]           (e59d8030)
//     0xb000c7ec: cmp  r7, #0                      (e3570000)
//     0xb000c7f0: strne r1, [r7]                   (15871000)
//     0xb000c7f4: cmp  r8, #0                      (e3580000)
//     0xb000c7f8: strne r2, [r8]                   (15882000)
//     0xb000c7fc: pop  {r4-r8, sb, sl, fp, pc}    (e8bd8ff0)
//
// UC_HOOK_INTR delivers PC at svc+4. The buggy path (else fallback: target_pc=LR)
// jumps straight to the caller, skipping the epilogue: no frame restore, no
// trap-stack output writebacks. The fix resumes at target_pc=pc with SP=ip.
//
// Usage:
//   ./test_exregs_frame_resume        -> resume at pc  (must PASS -> GREEN)
//   ./test_exregs_frame_resume buggy  -> resume at lr  (must FAIL -> RED)

#include <unicorn/unicorn.h>
#include <cstdio>
#include <cstdint>
#include <cstring>

using u32 = uint32_t;

static bool g_buggy_mode = false;

// Handler-provided output values (what the kernel would return in r1..r6).
static const u32 OUT1 = 0x11111111, OUT2 = 0x22222222, OUT3 = 0x33333333;
static const u32 OUT4 = 0x44444444, OUT5 = 0x55555555, OUT6 = 0x66666666;

static void intr_hook(uc_engine* uc, uint32_t intno, void* ud) {
    (void)intno; (void)ud;
    u32 pc = 0, ip = 0, lr = 0;
    uc_reg_read(uc, UC_ARM_REG_PC, &pc);
    uc_reg_read(uc, UC_ARM_REG_R12, &ip);
    uc_reg_read(uc, UC_ARM_REG_LR, &lr);

    // Host handler clobbers callee-saved and sets output registers r1..r6.
    u32 clob = 0xdeadbeef;
    uc_reg_write(uc, UC_ARM_REG_R4, &clob);
    uc_reg_write(uc, UC_ARM_REG_R5, &clob);
    uc_reg_write(uc, UC_ARM_REG_R6, &clob);
    uc_reg_write(uc, UC_ARM_REG_R7, &clob);
    uc_reg_write(uc, UC_ARM_REG_R8, &clob);
    u32 v1 = OUT1, v2 = OUT2, v3 = OUT3, v4 = OUT4, v5 = OUT5, v6 = OUT6;
    uc_reg_write(uc, UC_ARM_REG_R1, &v1);
    uc_reg_write(uc, UC_ARM_REG_R2, &v2);
    uc_reg_write(uc, UC_ARM_REG_R3, &v3);
    uc_reg_write(uc, UC_ARM_REG_R4, &v4);
    uc_reg_write(uc, UC_ARM_REG_R5, &v5);
    uc_reg_write(uc, UC_ARM_REG_R6, &v6);

    u32 target_pc;
    if (g_buggy_mode) {
        target_pc = lr & ~1u;            // else fallback: skips the epilogue
    } else {
        target_pc = pc;                  // fix: resume at svc+4 epilogue
        if (ip) uc_reg_write(uc, UC_ARM_REG_SP, &ip);
    }
    uc_reg_write(uc, UC_ARM_REG_PC, &target_pc);
}

static const u32 EXPECT = 0xb0d00000;   // callee-saved sentinel

// ---- ExchangeRegisters (0x0c): trap-stack writebacks + frame restore --------
static bool run_exregs() {
    uc_engine* uc = nullptr;
    if (uc_open(UC_ARCH_ARM, UC_MODE_ARM, &uc) != UC_ERR_OK) return false;
    const u32 STUB = 0x1000, CALLER = 0x2000, STACK = 0x8000;
    const u32 OUTBASE = 0x4000;         // 6 output cells: OUTBASE + i*4
    uc_mem_map(uc, 0, 0x10000, UC_PROT_ALL);

    u32 code[] = {
        0xe92d4ff0, 0xe59d4024, 0xe59d5028, 0xe59d602c,
        0xe1a0c00d, 0xe3e0d0f3, 0xef00140c, 0xe28de030,
        0xe89e1f80, 0xe5871000, 0xe5882000, 0xe5893000,
        0xe58a4000, 0xe58b5000, 0xe58c6000, 0xe8bd8ff0
    };
    uc_mem_write(uc, STUB, code, sizeof(code));
    u32 cc[] = { 0xe1a00000, 0xe1a00000 };
    uc_mem_write(uc, CALLER, cc, sizeof(cc));

    // pre-push sp = S. After push, ip=frame base=S-0x24. The stub reads
    // [ip+0x30..0x44] (=S+0x0c..S+0x20) as the 6 output pointers.
    u32 S = STACK;
    // args reloaded into r4/r5/r6 (later overwritten by pop) at [S],[S+4],[S+8]
    for (int i = 0; i < 3; i++) { u32 z = 0; uc_mem_write(uc, S + i*4, &z, 4); }
    // 6 output pointers at [S+0x0c .. S+0x20]
    for (int i = 0; i < 6; i++) {
        u32 p = OUTBASE + (u32)i*4;
        uc_mem_write(uc, S + 0x0c + (u32)i*4, &p, 4);
        u32 z = 0; uc_mem_write(uc, OUTBASE + (u32)i*4, &z, 4);
    }
    uc_reg_write(uc, UC_ARM_REG_SP, &S);
    // callee-saved seed (pushed to frame, restored by pop)
    u32 e = EXPECT;
    uc_reg_write(uc, UC_ARM_REG_R4, &e); uc_reg_write(uc, UC_ARM_REG_R5, &e);
    uc_reg_write(uc, UC_ARM_REG_R6, &e); uc_reg_write(uc, UC_ARM_REG_R7, &e);
    uc_reg_write(uc, UC_ARM_REG_R8, &e);
    u32 lr = CALLER; uc_reg_write(uc, UC_ARM_REG_LR, &lr);

    uc_hook h;
    uc_hook_add(uc, &h, UC_HOOK_INTR, (void*)intr_hook, nullptr, 0, ~0ull);
    uc_err err = uc_emu_start(uc, STUB, CALLER + 4, 0, 40);

    u32 pc = 0, r4 = 0, r5 = 0, r7 = 0, r8 = 0, out[6] = {0};
    uc_reg_read(uc, UC_ARM_REG_PC, &pc);
    uc_reg_read(uc, UC_ARM_REG_R4, &r4); uc_reg_read(uc, UC_ARM_REG_R5, &r5);
    uc_reg_read(uc, UC_ARM_REG_R7, &r7); uc_reg_read(uc, UC_ARM_REG_R8, &r8);
    for (int i = 0; i < 6; i++) uc_mem_read(uc, OUTBASE + (u32)i*4, &out[i], 4);
    uc_close(uc);

    bool ret_ok  = (pc == CALLER || pc == CALLER + 4);
    bool regs_ok = (r4 == EXPECT && r5 == EXPECT && r7 == EXPECT && r8 == EXPECT);
    bool wb_ok   = (out[0]==OUT1 && out[1]==OUT2 && out[2]==OUT3 &&
                    out[3]==OUT4 && out[4]==OUT5 && out[5]==OUT6);
    printf("  [ExchangeRegisters] mode=%s err=%u pc=0x%08x r4=0x%08x r7=0x%08x "
           "out=[%08x %08x %08x %08x %08x %08x]\n",
           g_buggy_mode?"buggy":"fixed", err, pc, r4, r7,
           out[0], out[1], out[2], out[3], out[4], out[5]);
    if (!ret_ok)  printf("  FAIL(ExchangeRegisters): did not return to caller (pc=0x%08x)\n", pc);
    if (!regs_ok) printf("  FAIL(ExchangeRegisters): callee-saved corrupted\n");
    if (!wb_ok)   printf("  FAIL(ExchangeRegisters): output writebacks missing\n");
    return ret_ok && regs_ok && wb_ok;
}

// ---- ThreadSwitch (0x04): pure frame restore --------------------------------
static bool run_threadswitch() {
    uc_engine* uc = nullptr;
    if (uc_open(UC_ARCH_ARM, UC_MODE_ARM, &uc) != UC_ERR_OK) return false;
    const u32 STUB = 0x1000, CALLER = 0x2000, STACK = 0x8000;
    uc_mem_map(uc, 0, 0x10000, UC_PROT_ALL);
    u32 code[] = { 0xe92d4ff0, 0xe1a0c00d, 0xe3e0d0fb, 0xef001404, 0xe8bd8ff0 };
    uc_mem_write(uc, STUB, code, sizeof(code));
    u32 cc[] = { 0xe1a00000, 0xe1a00000 };
    uc_mem_write(uc, CALLER, cc, sizeof(cc));
    u32 S = STACK; uc_reg_write(uc, UC_ARM_REG_SP, &S);
    u32 e = EXPECT;
    uc_reg_write(uc, UC_ARM_REG_R4, &e); uc_reg_write(uc, UC_ARM_REG_R5, &e);
    uc_reg_write(uc, UC_ARM_REG_R6, &e); uc_reg_write(uc, UC_ARM_REG_R7, &e);
    uc_reg_write(uc, UC_ARM_REG_R8, &e);
    u32 lr = CALLER; uc_reg_write(uc, UC_ARM_REG_LR, &lr);
    uc_hook h; uc_hook_add(uc, &h, UC_HOOK_INTR, (void*)intr_hook, nullptr, 0, ~0ull);
    uc_err err = uc_emu_start(uc, STUB, CALLER + 4, 0, 40);
    u32 pc=0, r4=0, r5=0, r7=0, r8=0;
    uc_reg_read(uc, UC_ARM_REG_PC, &pc);
    uc_reg_read(uc, UC_ARM_REG_R4, &r4); uc_reg_read(uc, UC_ARM_REG_R5, &r5);
    uc_reg_read(uc, UC_ARM_REG_R7, &r7); uc_reg_read(uc, UC_ARM_REG_R8, &r8);
    uc_close(uc);
    bool ok = ((pc == CALLER || pc == CALLER + 4) && r4==EXPECT && r5==EXPECT && r7==EXPECT && r8==EXPECT);
    printf("  [ThreadSwitch] mode=%s err=%u pc=0x%08x r4=0x%08x r7=0x%08x\n",
           g_buggy_mode?"buggy":"fixed", err, pc, r4, r7);
    if (!ok) printf("  FAIL(ThreadSwitch): frame not restored / no return\n");
    return ok;
}

// ---- Schedule (0x10): frame restore + two conditional writebacks ------------
static bool run_schedule() {
    uc_engine* uc = nullptr;
    if (uc_open(UC_ARCH_ARM, UC_MODE_ARM, &uc) != UC_ERR_OK) return false;
    const u32 STUB = 0x1000, CALLER = 0x2000, STACK = 0x8000, OUTBASE = 0x4000;
    uc_mem_map(uc, 0, 0x10000, UC_PROT_ALL);
    u32 code[] = {
        0xe92d4ff0, 0xe59d4024, 0xe59d5028, 0xe1a0c00d, 0xe3e0d0ef,
        0xef001410, 0xe59d702c, 0xe59d8030, 0xe3570000, 0x15871000,
        0xe3580000, 0x15882000, 0xe8bd8ff0
    };
    uc_mem_write(uc, STUB, code, sizeof(code));
    u32 cc[] = { 0xe1a00000, 0xe1a00000 };
    uc_mem_write(uc, CALLER, cc, sizeof(cc));
    // ip = frame base = S-0x24. r7 ptr read at [ip+0x2c]=[S+8], r8 ptr at [ip+0x30]=[S+0xc].
    u32 S = STACK;
    u32 p1 = OUTBASE, p2 = OUTBASE + 4, z = 0;
    uc_mem_write(uc, S + 0x08, &p1, 4);   // -> [ip+0x2c] : r7 writeback ptr
    uc_mem_write(uc, S + 0x0c, &p2, 4);   // -> [ip+0x30] : r8 writeback ptr
    uc_mem_write(uc, OUTBASE, &z, 4); uc_mem_write(uc, OUTBASE + 4, &z, 4);
    uc_reg_write(uc, UC_ARM_REG_SP, &S);
    u32 e = EXPECT;
    uc_reg_write(uc, UC_ARM_REG_R4, &e); uc_reg_write(uc, UC_ARM_REG_R5, &e);
    uc_reg_write(uc, UC_ARM_REG_R6, &e); uc_reg_write(uc, UC_ARM_REG_R7, &e);
    uc_reg_write(uc, UC_ARM_REG_R8, &e);
    u32 lr = CALLER; uc_reg_write(uc, UC_ARM_REG_LR, &lr);
    uc_hook h; uc_hook_add(uc, &h, UC_HOOK_INTR, (void*)intr_hook, nullptr, 0, ~0ull);
    uc_err err = uc_emu_start(uc, STUB, CALLER + 4, 0, 40);
    u32 pc=0, r4=0, r5=0, r6=0, o1=0, o2=0;
    uc_reg_read(uc, UC_ARM_REG_PC, &pc);
    uc_reg_read(uc, UC_ARM_REG_R4, &r4); uc_reg_read(uc, UC_ARM_REG_R5, &r5);
    uc_reg_read(uc, UC_ARM_REG_R6, &r6);
    uc_mem_read(uc, OUTBASE, &o1, 4); uc_mem_read(uc, OUTBASE + 4, &o2, 4);
    uc_close(uc);
    // r4/r5 reloaded from stack args (0) by the stub's own ldr then restored by
    // pop from the frame -> back to EXPECT. r6 is a pure callee-saved.
    bool ret_ok  = (pc == CALLER || pc == CALLER + 4);
    bool regs_ok = (r4==EXPECT && r5==EXPECT && r6==EXPECT);
    bool wb_ok   = (o1==OUT1 && o2==OUT2);
    printf("  [Schedule] mode=%s err=%u pc=0x%08x r4=0x%08x r6=0x%08x o1=0x%08x o2=0x%08x\n",
           g_buggy_mode?"buggy":"fixed", err, pc, r4, r6, o1, o2);
    if (!ret_ok)  printf("  FAIL(Schedule): did not return to caller\n");
    if (!regs_ok) printf("  FAIL(Schedule): callee-saved corrupted\n");
    if (!wb_ok)   printf("  FAIL(Schedule): conditional writebacks missing\n");
    return ret_ok && regs_ok && wb_ok;
}

int main(int argc, char** argv) {
    if (argc > 1 && strcmp(argv[1], "buggy") == 0) g_buggy_mode = true;
    printf("mode=%s\n", g_buggy_mode ? "buggy" : "fixed");
    bool ok = true;
    ok &= run_exregs();
    ok &= run_threadswitch();
    ok &= run_schedule();
    if (!ok) { printf("RED: QW27 contract violated (%s mode)\n", g_buggy_mode?"buggy":"fixed"); return 1; }
    printf("PASS: ExchangeRegisters/ThreadSwitch/Schedule frame restore + writebacks OK\n");
    return 0;
}
