// test_exregs_ig_naming.cpp — QW42: L4_ExchangeRegisters resume mode fix.
//
// Fact (proven by real boot execution + ZEEBO_FULL_TRACE + Capstone
// disassembly, see ROADMAP QW42): the AMSS `ig_naming` service embeds a LOCAL
// COPY of the L4_ExchangeRegisters trap-stack stub (ARM code) inside the
// range 0xb0100000-0xb0120000, distinct from the fixed kernel stub at
// 0xb000c758. QW41's `apply_tbit(pc)` classifier treats ANY pc outside
// 0xb0000000-0xb0020000 as Thumb, which is correct for AMSS/BREW app code but
// WRONG for this local ARM copy inside ig_naming: real boot showed the T-bit
// flipping 0->1 exactly at the transition 0xb0102c28->0xb0102c2c (this SVC's
// resume), and Unicorn subsequently mis-decoding real ARM as Thumb, stalling
// at pc=0xb010333a (2 bytes inside an unrelated `bl` instruction).
//
// The fix: for the L4_ExchangeRegisters case specifically, also treat callers
// in [0xb0100000, 0xb0120000) as ARM (like the fixed kernel stub), in addition
// to the fixed stub range. This must NOT touch resume behavior for callers
// outside this ig_naming range (that's the pre-existing generic apply_tbit
// path, exercised by test_svc_thumb_resume.cpp and unaffected here).
//
// Usage:
//   ./test_exregs_ig_naming        -> fixed (ig_naming range resumed as ARM) -> PASS/GREEN
//   ./test_exregs_ig_naming buggy  -> buggy (generic apply_tbit forces Thumb) -> FAIL/RED

#include <unicorn/unicorn.h>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <cstdlib>

using u32 = uint32_t;

static bool g_buggy = false;
static bool g_intr_fired = false;

// Mirrors the real caller_is_kernel_stub / local_arm_copy decision made in
// zeebo_lle_main.cpp's L4_ExchangeRegisters (syscall == 0x0c) resume path.
static void intr_hook(uc_engine* uc, uint32_t intno, void* user) {
    (void)intno; (void)user;
    g_intr_fired = true;
    u32 pc = 0;
    uc_reg_read(uc, UC_ARM_REG_PC, &pc);
    u32 target_pc = pc;

    bool caller_is_kernel_stub = (pc >= 0xb0000000u && pc < 0xb0020000u);
    bool local_arm_copy = (pc >= 0xb0100000u && pc < 0xb0120000u);

    if (!g_buggy) {
        // QW42 real fix.
        target_pc = (caller_is_kernel_stub || local_arm_copy) ? (pc & ~1u) : (pc | 1u);
    } else {
        // QW41-only generic classifier (the bug QW42 exposes): anything
        // outside the fixed kernel stub range is forced Thumb, even the
        // ig_naming local ARM copy.
        target_pc = caller_is_kernel_stub ? (pc & ~1u) : (pc | 1u);
    }

    uc_reg_write(uc, UC_ARM_REG_PC, &target_pc);
}

static void code_hook(uc_engine* uc, uint64_t address, uint32_t size, void* user) {
    (void)size; (void)user;
    static int count = 0;
    count++;
    if (count >= 4) uc_emu_stop(uc);
    (void)address;
}

int main(int argc, char** argv) {
    g_buggy = (argc > 1 && std::strcmp(argv[1], "buggy") == 0);

    uc_engine* uc = nullptr;
    uc_err err = uc_open(UC_ARCH_ARM, UC_MODE_ARM, &uc);
    if (err != UC_ERR_OK) { printf("uc_open failed: %d\n", err); return 1; }

    // Map a page inside the real ig_naming local-ARM-copy range.
    uint32_t base = 0xb0102c00;
    uc_mem_map(uc, base & ~0xFFFu, 0x1000, UC_PROT_ALL);

    // Layout at base (ARM, matches real svc-resume site 0xb0102c2c):
    //   svc #0x140c    (ARM, ef00140c)
    //   mov r1, #0x99  (ARM sentinel, e3a01099) -> writes magic to r1
    //   str r1, [r0]   (ARM, e5801000) -> stores magic to [r0]
    //   svc #0xff      (ARM stop trap, ef0000ff)
    uint32_t entry = base + 0x2c; // 0xb0102c2c
    uint8_t code[] = {
        0x0c, 0x14, 0x00, 0xef, // svc #0x140c
        0x99, 0x10, 0xa0, 0xe3, // mov r1, #0x99
        0x00, 0x10, 0x80, 0xe5, // str r1, [r0]
        0xff, 0x00, 0x00, 0xef, // svc #0xff
    };
    uc_mem_write(uc, entry, code, sizeof(code));

    // Scratch output word at r0.
    uint32_t scratch_addr = base + 0x100;
    uc_mem_write(uc, scratch_addr, "\x00\x00\x00\x00", 4);
    uint32_t r0 = scratch_addr;
    uc_reg_write(uc, UC_ARM_REG_R0, &r0);

    uc_hook h_intr, h_code;
    uc_hook_add(uc, &h_intr, UC_HOOK_INTR, (void*)intr_hook, nullptr, 1, 0);
    uc_hook_add(uc, &h_code, UC_HOOK_CODE, (void*)code_hook, nullptr, 1, 0);

    u32 start = entry; // ARM mode, bit0 clear
    err = uc_emu_start(uc, start, base + 0xFFF, 0, 0);

    uint32_t scratch = 0;
    uc_mem_read(uc, scratch_addr, (uint8_t*)&scratch, 4);

    printf("mode=%s intr_fired=%d emu_err=%d scratch=0x%08x\n",
           g_buggy ? "buggy" : "fixed", g_intr_fired, (int)err, scratch);

    uc_close(uc);

    bool ok = g_intr_fired && (scratch == 0x99);
    if (!g_buggy) {
        if (ok) {
            printf("PASS: ig_naming local ARM copy correctly resumed in ARM mode\n");
            return 0;
        }
        printf("FAIL: fixed mode did not resume correctly\n");
        return 1;
    } else {
        // Convention (see other QW test harnesses): "buggy" mode must exit
        // non-zero when it correctly reproduces the RED case (Makefile's
        // negative-mutation check expects the buggy invocation to FAIL).
        if (!ok) {
            printf("OK: buggy generic-Thumb classifier fails as expected (RED reproduced)\n");
            return 1;
        }
        printf("UNEXPECTED: buggy mode passed (test doesn't discriminate)\n");
        return 0;
    }
}
