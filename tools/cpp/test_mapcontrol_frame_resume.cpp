// test_mapcontrol_frame_resume.cpp — QW19 regression: MapControl stub frame restoration.
//
// Mechanism:
//   In OKL4 / Iguana OS, the MapControl stub at 0xb000c930 is:
//     0xb000c930: push {r4, r5, r6, r7, r8, sb, sl, fp, lr}
//     0xb000c934: mov ip, sp
//     0xb000c938: mvn sp, #0xeb
//     0xb000c93c: svc #0x1414
//     0xb000c940: pop {r4, r5, r6, r7, r8, sb, sl, fp, pc}
//
//   When UC_HOOK_INTR fires, PC is delivered at svc+4 (0xb000c940).
//   If the intr hook resumes at target_pc = LR (the old fallback in zeebo_lle_main.cpp),
//   it returns directly to the caller at 0xb000d6b8 WITHOUT executing the `pop` instruction.
//   As a result, all preserved registers {r4-r8, sb, sl, fp} remain destroyed by whatever
//   the syscall handler clobbered, and r4 (the virtual pool cursor) loses its value.
//
//   Resuming at target_pc = pc (with SP restored to IP) allows Unicorn to execute
//   the pop at 0xb000c940, correctly restoring all registers from the stack frame and
//   naturally popping PC = caller_lr.
//
// Usage:
//   ./test_mapcontrol_frame_resume        -> resume at pc (fixed convention, must PASS -> GREEN)
//   ./test_mapcontrol_frame_resume buggy  -> resume at lr (buggy convention, must FAIL -> RED)

#include <unicorn/unicorn.h>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <cstdlib>

using u32 = uint32_t;

static bool g_buggy_mode = false;
static bool g_intr_fired = false;

static void intr_hook(uc_engine* uc, uint32_t intno, void* ud) {
    (void)intno; (void)ud;
    g_intr_fired = true;
    u32 pc = 0, ip = 0, lr = 0;
    uc_reg_read(uc, UC_ARM_REG_PC, &pc);
    uc_reg_read(uc, UC_ARM_REG_R12, &ip);
    uc_reg_read(uc, UC_ARM_REG_LR, &lr);

    // Clobber r4 to simulate host syscall handler activity
    u32 clobbered = 0xdeadbeef;
    uc_reg_write(uc, UC_ARM_REG_R4, &clobbered);

    u32 target_pc = 0;
    if (g_buggy_mode) {
        // Old fallback: jumps straight to LR without popping the stack frame
        target_pc = lr & ~1u;
        if (ip) uc_reg_write(uc, UC_ARM_REG_SP, &ip);
        uc_reg_write(uc, UC_ARM_REG_PC, &target_pc);
    } else {
        // Fixed convention for syscall 0x14: resumes at pc (0xb000c940) to execute pop
        target_pc = pc;
        if (ip) uc_reg_write(uc, UC_ARM_REG_SP, &ip);
        uc_reg_write(uc, UC_ARM_REG_PC, &target_pc);
    }
}

int main(int argc, char** argv) {
    if (argc > 1 && strcmp(argv[1], "buggy") == 0) {
        g_buggy_mode = true;
    }

    uc_engine* uc = nullptr;
    if (uc_open(UC_ARCH_ARM, UC_MODE_ARM, &uc) != UC_ERR_OK) {
        fprintf(stderr, "FAIL: uc_open\n");
        return 2;
    }

    const u32 STUB_ADDR   = 0x1000;
    const u32 CALLER_ADDR = 0x2000;
    const u32 STACK_ADDR  = 0x3000;
    const u32 EXPECTED_R4 = 0xb0d00000;

    uc_mem_map(uc, 0x0000, 0x5000, UC_PROT_ALL);

    // Hand-assembled MapControl stub at 0x1000:
    // 0x1000: push {r4, r5, r6, r7, r8, sb, sl, fp, lr} (e92d4ff0)
    // 0x1004: mov ip, sp                                 (e1a0c00d)
    // 0x1008: mvn sp, #0xeb                              (e3e0d0eb)
    // 0x100c: svc #0x1414                                (ef001414)
    // 0x1010: pop {r4, r5, r6, r7, r8, sb, sl, fp, pc}  (e8bd8ff0)
    u32 stub_code[] = {
        0xe92d4ff0,
        0xe1a0c00d,
        0xe3e0d0eb,
        0xef001414,
        0xe8bd8ff0
    };
    uc_mem_write(uc, STUB_ADDR, stub_code, sizeof(stub_code));

    // Caller code at 0x2000 (mov r0, r0; nop):
    u32 caller_code[] = {
        0xe1a00000, // nop
        0xe1a00000  // nop
    };
    uc_mem_write(uc, CALLER_ADDR, caller_code, sizeof(caller_code));

    // Setup stack and initial registers
    u32 sp = STACK_ADDR + 0x800;
    uc_reg_write(uc, UC_ARM_REG_SP, &sp);
    uc_reg_write(uc, UC_ARM_REG_R4, &EXPECTED_R4);
    u32 caller_lr = CALLER_ADDR;
    uc_reg_write(uc, UC_ARM_REG_LR, &caller_lr);

    uc_hook h;
    uc_hook_add(uc, &h, UC_HOOK_INTR, (void*)intr_hook, nullptr, 0, ~0ull);

    // Execute through stub
    uc_err err = uc_emu_start(uc, STUB_ADDR, CALLER_ADDR + 4, 0, 20);
    if (err != UC_ERR_OK) {
        fprintf(stderr, "[info] uc_emu_start err=%u (%s)\n", err, uc_strerror(err));
    }

    u32 final_r4 = 0, final_pc = 0;
    uc_reg_read(uc, UC_ARM_REG_R4, &final_r4);
    uc_reg_read(uc, UC_ARM_REG_PC, &final_pc);
    uc_close(uc);

    if (!g_intr_fired) {
        fprintf(stderr, "FAIL: intr hook never fired\n");
        return 2;
    }

    bool r4_preserved = (final_r4 == EXPECTED_R4);
    printf("mode=%s intr_fired=%d final_pc=0x%08x final_r4=0x%08x (expect=0x%08x)\n",
           g_buggy_mode ? "buggy" : "fixed", (int)g_intr_fired, final_pc, final_r4, EXPECTED_R4);

    if (!r4_preserved) {
        printf("FAIL: r4 was corrupted by un-popped stack frame (got 0x%08x)\n", final_r4);
        return 1;
    }

    printf("PASS: r4 correctly restored via pop instruction at svc+4\n");
    return 0;
}
