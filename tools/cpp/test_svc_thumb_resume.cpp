// test_svc_thumb_resume.cpp — QW41: T-bit preservation on SVC resume.
//
// Fact (proven by real boot execution + Capstone disassembly, see ROADMAP QW41):
//   The real hardware exception entry for `svc` forces the CPU into ARM mode
//   (T-bit cleared in CPSR) regardless of the caller's original mode. The Zeebo
//   AMSS/BREW firmware (1.1.2_APPS.bin) is 100% Thumb code outside the fixed
//   L4 kernel stub range (0xb0000000-0xb0020000, which IS ARM). If the SVC
//   resume path in zeebo_lle_main.cpp's c0_intr_hook does not restore the T-bit
//   for non-kernel-stub callers, Unicorn decodes the resumed instruction stream
//   as ARM and immediately hits UC_ERR_INSN_INVALID on valid Thumb bytes
//   (observed for real at pc=0x1039322e, `cbz r4, ...` == 0xb1c4 as Thumd,
//   garbage as ARM).
//
// This test reproduces the exact mechanism: an `svc` instruction encoded in
// Thumb, located OUTSIDE the kernel stub range, followed by a valid Thumb
// instruction (a sentinel store). If T-bit restore is correct, the sentinel
// executes in Thumb and the magic value lands in memory. If the buggy
// convention (T-bit stays cleared / ARM) is used, Unicorn decodes the next
// halfword as leading ARM garbage and either faults or produces wrong
// behavior — sentinel value stays absent.
//
// Usage:
//   ./test_svc_thumb_resume        -> fixed (T-bit restored for non-kernel caller) -> PASS/GREEN
//   ./test_svc_thumb_resume buggy  -> buggy (T-bit never restored, forced ARM)      -> FAIL/RED

#include <unicorn/unicorn.h>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <cstdlib>

using u32 = uint32_t;

static bool g_buggy = false;
static bool g_intr_fired = false;
static int g_intr_count = 0;

// Simulates the fixed portion of zeebo_lle_main.cpp's c0_intr_hook SVC-resume
// logic relevant to QW41: decide T-bit based on whether the caller PC falls
// inside the fixed L4 kernel-stub range.
static void intr_hook(uc_engine* uc, uint32_t intno, void* user) {
    (void)intno; (void)user;
    g_intr_fired = true;
    g_intr_count++;
    u32 pc = 0;
    uc_reg_read(uc, UC_ARM_REG_PC, &pc);
    // Under UC_HOOK_INTR for Thumb svc, pc already points past the svc.
    u32 target_pc = pc;

    bool caller_is_kernel_stub = (pc >= 0xb0000000u && pc < 0xb0020000u);
    if (!g_buggy) {
        // QW41 real fix: writing CPSR bit 5 directly is NOT reliable in
        // Unicorn to switch decode mode — the only deterministic way is
        // writing PC with bit 0 set (real hardware BX convention).
        if (!caller_is_kernel_stub) {
            target_pc |= 1u; // restore Thumb (QW41 fix)
        } else {
            target_pc &= ~1u;
        }
    }
    // Buggy path: PC LSB untouched -> stays whatever hw-exception-entry
    // simulation left it at (cleared, i.e. ARM) -- reproduced by not fixing PC
    // and by the base memory image only containing Thumb after the svc.

    uc_reg_write(uc, UC_ARM_REG_PC, &target_pc);
}

static void code_hook(uc_engine* uc, uint64_t address, uint32_t size, void* user) {
    (void)uc; (void)size; (void)user;
    static int count = 0;
    count++;
    if (count >= 4) {
        uc_emu_stop(uc);
    }
    (void)address;
}

int main(int argc, char** argv) {
    g_buggy = (argc > 1 && std::strcmp(argv[1], "buggy") == 0);

    uc_engine* uc = nullptr;
    uc_err err = uc_open(UC_ARCH_ARM, UC_MODE_THUMB, &uc);
    if (err != UC_ERR_OK) { printf("uc_open failed: %d\n", err); return 1; }

    // Map a region OUTSIDE the kernel stub range (0xb000xxxx), like real AMSS code.
    uint32_t base = 0x10393200; // matches the real stall address's page
    uc_mem_map(uc, base & ~0xFFFu, 0x1000, UC_PROT_ALL);

    // Layout at base:
    //   svc #0      (Thumb, df 00)
    //   movs r1,#0x99  (sentinel, Thumb 21 99) -> writes magic to r1
    //   str r1,[r0]    (Thumb 60 01) -> stores magic to [r0]
    uint8_t code[] = {
        0x00, 0xdf,       // svc #0  (Thumb halfword 0xdf00, little-endian bytes)
        0x99, 0x21,       // movs r1, #0x99
        0x01, 0x60,       // str r1, [r0]
        0xff, 0xdf,       // svc #0xff (stop trap)
    };
    uc_mem_write(uc, base, code, sizeof(code));

    // Scratch memory for the store target.
    uint32_t scratch = 0x10394000;
    uc_mem_map(uc, scratch, 0x1000, UC_PROT_ALL);
    uint32_t zero = 0;
    uc_mem_write(uc, scratch, &zero, 4);

    uc_reg_write(uc, UC_ARM_REG_R0, &scratch);

    uc_hook h;
    uc_hook_add(uc, &h, UC_HOOK_INTR, (void*)intr_hook, nullptr, 1, 0);
    uc_hook hc;
    uc_hook_add(uc, &hc, UC_HOOK_CODE, (void*)code_hook, nullptr, 1, 0);

    // Entry: Thumb mode at base | 1 (BX convention for start address).
    uint32_t sp = 0x10395000;
    uc_reg_write(uc, UC_ARM_REG_SP, &sp);

    err = uc_emu_start(uc, base | 1, 0, 0, 0);
    // Any UC_ERR (e.g. INSN_INVALID) counts as failure to reach the sentinel.

    uint32_t result = 0;
    uc_mem_read(uc, scratch, &result, 4);

    printf("mode=%s intr_fired=%d emu_err=%d scratch=0x%08x\n",
           g_buggy ? "buggy" : "fixed", g_intr_fired, err, result);

    bool ok = g_intr_fired && (result == 0x99);
    if (g_buggy) {
        // Buggy convention is EXPECTED to fail to produce the sentinel value.
        // Convention (matches other QW test harnesses in this Makefile): exit
        // NONZERO when the mutant correctly reproduces the RED failure, so
        // `if ./test buggy; then FAIL else OK` in the Makefile works.
        if (!ok) {
            printf("OK: buggy convention fails as expected (RED reproduced)\n");
            return 1;
        }
        printf("UNEXPECTED: buggy convention produced correct sentinel\n");
        return 0;
    } else {
        if (ok) {
            printf("PASS: T-bit correctly restored; Thumb sentinel executed\n");
            return 0;
        }
        printf("FAIL: sentinel not observed (T-bit restore broken)\n");
        return 1;
    }
}
