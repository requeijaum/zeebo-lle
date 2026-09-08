// test_intr_pc_resume.cpp — QW17 regression: PC-resume convention in c0_intr_hook.
//
// Fact (proven by notes/boot-investigation/intr_pc_witness.py and skill notes):
//   On ARM Unicorn, UC_HOOK_INTR for an `svc` delivers the callback with PC ALREADY
//   pointing at the instruction FOLLOWING the svc (PC == svc + 4).
//   Therefore a handler that resumes at `pc + 4` lands at svc + 8, silently skipping
//   exactly one guest instruction (the sentinel immediately after the svc).
//   The correct resume is `target_pc = pc` (== svc + 4).
//
// This test reproduces the exact mechanism under Unicorn with a two-instruction
// layout: [svc] [sentinel-store]. The sentinel writes a magic value to memory. If the
// resume convention is correct (offset 0), the sentinel runs and the magic appears.
// If the buggy convention is used (offset 4, == production before QW17), the sentinel
// is skipped and the magic is absent.
//
// Usage:
//   ./test_intr_pc_resume        -> resume at pc      (correct, must PASS -> GREEN)
//   ./test_intr_pc_resume 4      -> resume at pc + 4  (buggy,   must FAIL -> RED evidence)
//
// The default (no arg) mirrors the fixed production convention, so this binary is the
// GREEN gate. Running with `4` reproduces the pre-fix RED.

#include <unicorn/unicorn.h>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <cstdlib>

using u32 = uint32_t;

static u32 g_resume_offset = 0; // 0 = correct (pc); 4 = buggy (pc + 4)
static bool g_intr_fired = false;

static void intr_hook(uc_engine* uc, uint32_t intno, void* user) {
    (void)intno; (void)user;
    g_intr_fired = true;
    u32 pc = 0;
    uc_reg_read(uc, UC_ARM_REG_PC, &pc);
    // Under UC_HOOK_INTR for svc, `pc` already == svc + 4.
    u32 target_pc = pc + g_resume_offset;
    uc_reg_write(uc, UC_ARM_REG_PC, &target_pc);
}

int main(int argc, char** argv) {
    if (argc > 1) g_resume_offset = (u32)strtoul(argv[1], nullptr, 0);

    const u32 BASE     = 0x1000;
    const u32 SENTINEL_ADDR = 0x2000; // where the sentinel store lands
    const u32 MAGIC    = 0xC0FFEE11;

    uc_engine* uc = nullptr;
    if (uc_open(UC_ARCH_ARM, UC_MODE_ARM, &uc) != UC_ERR_OK) {
        fprintf(stderr, "FAIL: uc_open\n"); return 2;
    }
    uc_mem_map(uc, 0x0000, 0x10000, UC_PROT_ALL);

    // ARM code at BASE:
    //   0x1000: svc #0x14          -> traps to intr_hook; on return pc==0x1004
    //   0x1004: (sentinel) ldr r1,[pc,#..]; str r1,[r0]  -- but keep it simple:
    //           mov r1, #0 ; the real sentinel is a store of MAGIC to SENTINEL_ADDR.
    // We encode the sentinel as a small routine:
    //   0x1004: movw r0, #0x2000        (load SENTINEL_ADDR low)
    //   0x1008: movt r0, #0x0000
    //   0x100c: ldr  r1, [pc, #..]      -> MAGIC constant
    //   0x1010: str  r1, [r0]
    //   0x1014: svc  #0x99              -> second trap used only to stop cleanly
    // Simpler & robust: use a literal pool. Assemble bytes explicitly.

    // Hand-assembled ARM (little-endian). We avoid movw/movt (not supported by the
    // default Unicorn ARM CPU) and load both the sentinel address and the magic from
    // a literal pool via pc-relative ldr.
    //
    // Layout:
    // 0x1000 svc #0x14        (word0)               -> UC_HOOK_INTR fires, pc==0x1004
    // 0x1004 ldr r0,[pc,#8]   (word1) sentinel start -> r0 = SENTINEL_ADDR (0x1014)
    // 0x1008 ldr r1,[pc,#8]   (word2)               -> r1 = MAGIC          (0x1018)
    // 0x100c str r1,[r0]      (word3)               -> *SENTINEL_ADDR = MAGIC
    // 0x1010 svc #0x99        (word4) stop trap
    // 0x1014 SENTINEL_ADDR    (word5) literal
    // 0x1018 MAGIC            (word6) literal
    // ldr @0x1004: pc=0x100c, +8 = 0x1014 (word5). ldr @0x1008: pc=0x1010, +8 = 0x1018.
    u32 code[] = {
        0xEF000014,    // 0x1000 svc #0x14
        0xE59F0008,    // 0x1004 ldr r0, [pc, #8]   -> loads SENTINEL_ADDR
        0xE59F1008,    // 0x1008 ldr r1, [pc, #8]   -> loads MAGIC
        0xE5801000,    // 0x100c str r1, [r0]
        0xEF000099,    // 0x1010 svc #0x99 (stop)
        SENTINEL_ADDR, // 0x1014 literal
        MAGIC          // 0x1018 literal
    };
    uc_mem_write(uc, BASE, code, sizeof(code));

    // Pre-clear sentinel target.
    u32 zero = 0;
    uc_mem_write(uc, SENTINEL_ADDR, &zero, 4);

    uc_hook h;
    uc_hook_add(uc, &h, UC_HOOK_INTR, (void*)intr_hook, nullptr, 0, ~0ull);

    // Run until the stop trap (svc #0x99) at 0x1014. We stop when the second intr fires.
    // Simplest: run with a count cap; the second svc will re-enter intr_hook and we
    // detect completion by checking memory afterwards. Use uc_emu_start until 0x1014.
    uc_err err = uc_emu_start(uc, BASE, 0x1010, 0, 0);
    if (err != UC_ERR_OK && err != UC_ERR_HOOK) {
        // err may be non-zero if it ran into the stop svc; that's acceptable.
        fprintf(stderr, "[info] uc_emu_start err=%u (%s)\n", err, uc_strerror(err));
    }

    u32 got = 0;
    uc_mem_read(uc, SENTINEL_ADDR, &got, 4);
    uc_close(uc);

    if (!g_intr_fired) {
        fprintf(stderr, "FAIL: intr hook never fired (test setup broken)\n");
        return 2;
    }

    bool sentinel_ran = (got == MAGIC);
    printf("resume_offset=%u intr_fired=%d sentinel_ran=%d (got=0x%08x expect=0x%08x)\n",
           g_resume_offset, (int)g_intr_fired, (int)sentinel_ran, got, MAGIC);

    if (!sentinel_ran) {
        printf("FAIL: sentinel instruction after svc was SKIPPED "
               "(resume convention wrong: target_pc = pc + %u)\n", g_resume_offset);
        return 1;
    }
    printf("PASS: sentinel executed; syscall pc-resume is correct (target_pc = pc)\n");
    return 0;
}
