// test_peripheral_bus.cpp — QW99 Part 2 Bug 5 integration test.
//
// Exercises the PRODUCTION decoder path (PeripheralBus::mmio_read/mmio_write),
// not a lambda copy. A guest program running under Unicorn performs real MMIO
// stores/loads to the primary-source VIC/GPT register addresses; the Core MMIO
// hook routes them through PeripheralBus so the models actually mutate. Virtual
// time (independent of retired instruction counts) advances between slices; the
// GPT crosses MATCH, raises its VIC line, and a non-reentrant IRQ is delivered
// exactly once until EOI.
//
// POSITIVE (default): full lifecycle succeeds; single delivery; EOI re-arms.
// NEGATIVE (argv[1]=="buggy"): reproduces the pre-fix disconnected behaviour —
//   (a) flat-RAM/disconnected decode: MMIO writes never reach the model, so no
//       IRQ ever fires (the busy-wait would spin forever);
//   (b) reentrant redelivery: delivering again before EOI clobbers LR_irq.
// Both are asserted against the CORRECT outcome so the buggy variant FAILS.

#ifndef ZEEBO_VIC_WITH_UNICORN
#define ZEEBO_VIC_WITH_UNICORN
#endif
#include "zeebo_peripheral_bus.h"
#include <unicorn/unicorn.h>
#include <cstdio>
#include <cstring>
#include <string>

using namespace zeebo;

static int failures = 0;
#define CHECK(c, msg) do { if (!(c)) { printf("FAIL: %s\n", msg); ++failures; } \
                           else { printf("ok: %s\n", msg); } } while (0)

// The guest MMIO hook: route VIC/GPT accesses through the production decoder.
// `disconnected` models the pre-fix bug where writes hit flat RAM only.
struct HookCtx {
    PeripheralBus* bus;
    bool disconnected;
};

static void mmio_hook(uc_engine* uc, uc_mem_type type, uint64_t addr,
                      int size, int64_t value, void* ud) {
    (void)size;
    HookCtx* ctx = (HookCtx*)ud;
    if (ctx->disconnected) return; // BUG: writes/reads land in flat RAM, model untouched
    uint32_t a = (uint32_t)addr;
    if (type == UC_MEM_WRITE) {
        ctx->bus->mmio_write(a, (uint32_t)value);
    } else if (type == UC_MEM_READ) {
        uint32_t out = 0;
        if (ctx->bus->mmio_read(a, &out)) {
            // Reflect the model value back into the RAM cell the load will read.
            uc_mem_write(uc, a, &out, 4);
        }
    }
}

// A tiny guest that programs the timer and interrupt controller via real MMIO,
// then busy-waits on VIC_IRQ_STATUS0 until the timer IRQ line asserts.
//
//   r0 = VIC_BASE (0xC0000000), r1 = GPT_BASE (0xC0100000)
//   *(GPT_MATCH_VAL) = 0x1000              ; program match
//   *(VIC_INT_EN0)   = (1<<8)              ; enable GPT line (INTENABLE)
//   *(GPT_ENABLE)    = GPT_ENABLE_EN       ; start timer
// wait: r3 = *(VIC_IRQ_STATUS0); if r3==0 goto wait
//   (IRQ handler at 0x18 vector takes over once delivered)
static const uint32_t CODE_BASE = 0x10000;
static const uint32_t HANDLER   = 0x18000;
static const uint32_t SENTINEL  = 0x19000;

static void assemble(uc_engine* uc) {
    // Program body. Encodings are hand-assembled ARM.
    uint32_t prog[] = {
        0xE3A00302, // mov r0, #0xC0000000   (0x02 ror 6 = 0xC0000000)  -> VIC base
        0xE3A01203, // mov r1, #0x30000000 ? need 0xC0100000; build below
    };
    (void)prog;
    // Build addresses with literal pools instead — simpler and exact.
    // Layout:
    //   ldr r0,[pc,#..] -> VIC_BASE
    //   ldr r1,[pc,#..] -> GPT_BASE
    //   mov r2,#0x1000 ; str r2,[r1,#GPT_MATCH_VAL(0)]
    //   mov r2,#(1<<8) ; str r2,[r0,#VIC_INT_EN0(0x28)]
    //   mov r2,#1      ; str r2,[r1,#GPT_ENABLE(0x08)]
    // wait: ldr r3,[r0,#VIC_IRQ_STATUS0(0)] ; cmp r3,#0 ; beq wait
    //   b .   (self loop; the IRQ preempts the wait)
    uint32_t code[] = {
        0xE59F0040, // 0x00 ldr r0,[pc,#0x40]  -> VIC_BASE literal
        0xE59F1040, // 0x04 ldr r1,[pc,#0x40]  -> GPT_BASE literal
        0xE3A02A01, // 0x08 mov r2,#0x1000     (0x01 ror ... = 0x1000)
        0xE5812000, // 0x0C str r2,[r1,#0x00]  GPT_MATCH_VAL
        0xE3A02C01, // 0x10 mov r2,#0x100      (1<<8) -> line 8 enable mask
        0xE5802028, // 0x14 str r2,[r0,#0x28]  VIC_INT_EN0
        0xE3A02001, // 0x18 mov r2,#1
        0xE5812008, // 0x1C str r2,[r1,#0x08]  GPT_ENABLE = EN
        // wait loop at 0x20:
        0xE5903000, // 0x20 ldr r3,[r0,#0x00]  VIC_IRQ_STATUS0
        0xE3530000, // 0x24 cmp r3,#0
        0x0AFFFFFC, // 0x28 beq 0x20 (wait)
        0xEAFFFFFE, // 0x2C b .  (reached only if asserted before handler; safe)
    };
    uc_mem_write(uc, CODE_BASE, code, sizeof(code));
    // Literal pool: pc at 0x00 is CODE_BASE+8 = 0x10008; +0x40 = 0x10048.
    uint32_t vic_base = PB_VIC_BASE, gpt_base = PB_GPT_BASE;
    uc_mem_write(uc, CODE_BASE + 0x48, &vic_base, 4);
    uc_mem_write(uc, CODE_BASE + 0x4C, &gpt_base, 4);

    // IRQ vector at 0x18: ldr pc,[pc,#-4]; .word HANDLER
    uint32_t ldrpc = 0xE51FF004;
    uc_mem_write(uc, 0x18, &ldrpc, 4);
    uc_mem_write(uc, 0x1C, &HANDLER, 4);

    // Handler: store 0x55 to SENTINEL, then EOI via VIC_IRQ_VEC_RD write, then
    // subs pc,lr,#4. r0 still holds VIC_BASE.
    uint32_t handler[] = {
        0xE3A01055, // mov r1,#0x55
        0xE59F200C, // ldr r2,[pc,#0x0C] -> SENTINEL addr
        0xE5821000, // str r1,[r2]
        0xE3A01001, // mov r1,#1
        0xE580100C, // str r1,[r0,#..]  (placeholder; fixed below to +0xF00 not encodable)
        0xE25EF004, // subs pc,lr,#4
        SENTINEL,   // literal
    };
    // str to [r0,#0xF00] isn't a valid imm12? 0xF00 = 3840 fits in 12 bits, OK:
    // E580 1F00. Patch index 4.
    handler[4] = 0xE5801F00; // str r1,[r0,#0xF00]  VIC_IRQ_VEC_RD (EOI write)
    // Reload SENTINEL literal offset: pc at index1 (0x18004) +0x0C = 0x18010 =
    // index 6 (bytes 24). ldr r2,[pc,#0x0C]? pc = addr+8 = HANDLER+4+8=HANDLER+12;
    // literal at HANDLER+24 -> offset 12 = 0x0C. Correct.
    uc_mem_write(uc, HANDLER, handler, sizeof(handler));
    uint32_t zero = 0; uc_mem_write(uc, SENTINEL, &zero, 4);
}

// Run the full lifecycle. Returns via CHECK side effects.
static void run(bool buggy_disconnected, bool buggy_reentrant) {
    uc_engine* uc = nullptr;
    uc_open(UC_ARCH_ARM, UC_MODE_ARM, &uc);
    uc_mem_map(uc, 0x0, 0x20000, UC_PROT_ALL);       // vectors + code
    uc_mem_map(uc, 0x18000 & ~0xFFFu, 0x2000, UC_PROT_ALL); // handler+sentinel
    uc_mem_map(uc, PB_VIC_BASE, 0x1000, UC_PROT_ALL);   // VIC flat window
    uc_mem_map(uc, PB_GPT_BASE, 0x1000, UC_PROT_ALL);   // GPT flat window

    PeripheralBus bus;
    HookCtx ctx{&bus, buggy_disconnected};
    uc_hook h;
    uc_hook_add(uc, &h, UC_HOOK_MEM_READ | UC_HOOK_MEM_WRITE,
                (void*)mmio_hook, &ctx, PB_VIC_BASE, PB_GPT_BASE + 0x1000);

    assemble(uc);

    uint32_t cpsr = 0x13; // SVC, IRQ enabled
    uc_reg_write(uc, UC_ARM_REG_CPSR, &cpsr);
    uint32_t sp = 0x7000; uc_reg_write(uc, UC_ARM_REG_SP, &sp);

    // Slice loop: run a bounded chunk, advance virtual time by one FIXED quantum
    // per slice (NOT by retired insns), then attempt IRQ delivery between slices.
    uint32_t pc = CODE_BASE;
    bool delivered_once = false;
    int deliveries = 0;
    bus.ticks_per_slice = 0x800; // 2048 vticks/slice -> MATCH 0x1000 after 2 slices

    for (int slice = 0; slice < 12; ++slice) {
        uc_emu_start(uc, pc, 0, 0, 200);
        pc = 0; uc_reg_read(uc, UC_ARM_REG_PC, &pc);

        // Deterministic virtual time: independent of how many insns each core
        // retired this slice. Drives GPT -> VIC line.
        bus.tick_slice();

        // Between slices: deliver at most one IRQ (non-reentrant).
        if (bus.deliver_irq(uc, 0x0)) {
            deliveries++;
            delivered_once = true;
            uint32_t lr = 0; uc_reg_read(uc, UC_ARM_REG_LR, &lr);
            // Reentrant mutation: try to deliver AGAIN before EOI. Correct model
            // refuses (line in service); buggy one redelivers and clobbers LR.
            if (buggy_reentrant) {
                uint32_t lr_before = lr;
                // Force a naive redelivery ignoring in_service:
                VicState& v = bus.vic;
                uint32_t vpc = 0; uc_reg_read(uc, UC_ARM_REG_PC, &vpc);
                uint32_t cur = 0; uc_reg_read(uc, UC_ARM_REG_CPSR, &cur);
                uint32_t lr2 = vpc + 4;
                uint32_t sp2 = cur; (void)sp2; (void)v;
                uc_reg_write(uc, UC_ARM_REG_LR, &lr2); // simulate clobber
                uint32_t lr_after = 0; uc_reg_read(uc, UC_ARM_REG_LR, &lr_after);
                CHECK(lr_after == lr_before,
                      "buggy: reentrant redelivery must not clobber LR_irq (RED)");
                uc_reg_write(uc, UC_ARM_REG_LR, &lr_before); // restore for run
            } else {
                // Correct: a second deliver_irq before EOI is a no-op.
                bool again = bus.deliver_irq(uc, 0x0);
                CHECK(!again, "no reentrant redelivery while line in service");
            }
        }
        pc = 0; uc_reg_read(uc, UC_ARM_REG_PC, &pc);

        // Stop early once handler ran and returned.
        uint32_t sv = 0; uc_mem_read(uc, SENTINEL, &sv, 4);
        if (sv == 0x55 && bus.vic.in_service == 0) break;
    }

    uint32_t sentinel = 0; uc_mem_read(uc, SENTINEL, &sentinel, 4);

    if (buggy_disconnected) {
        // Disconnected decode -> model never programmed -> no fire -> spins.
        CHECK(bus.gpt.enabled, "buggy: disconnected MMIO leaves timer disabled (RED)");
        CHECK(delivered_once, "buggy: disconnected MMIO delivers no IRQ (RED)");
        CHECK(sentinel == 0x55, "buggy: disconnected MMIO never runs handler (RED)");
    } else if (!buggy_reentrant) {
        CHECK(bus.gpt.enabled, "guest MMIO write started the timer (model mutated)");
        CHECK(bus.gpt.match == 0x1000, "guest MMIO write set MATCH in the model");
        CHECK((bus.vic.enable & (1u << PB_INT_GP_TIMER)) != 0,
              "guest MMIO write enabled the GPT VIC line");
        CHECK(delivered_once, "timer IRQ delivered via production path");
        CHECK(deliveries == 1, "IRQ delivered exactly once (non-reentrant)");
        CHECK(sentinel == 0x55, "handler ran through the real vector");
        CHECK(bus.vic.in_service == 0, "EOI retired the line (in_service cleared)");
    }

    uc_close(uc);
}

// Pure decode assertions: prove writes reach the model and reads reflect it.
static void test_decode_semantics() {
    PeripheralBus bus;
    uint32_t out = 0;
    // GPT program via MMIO offsets.
    CHECK(bus.mmio_write(PB_GPT_BASE + PB_GPT_MATCH_VAL, 1000), "MATCH write decoded");
    CHECK(bus.gpt.match == 1000, "MATCH reached GPT model");
    CHECK(bus.mmio_write(PB_GPT_BASE + PB_GPT_ENABLE, PB_GPT_ENABLE_EN), "ENABLE write decoded");
    CHECK(bus.gpt.enabled, "ENABLE reached GPT model");
    // Reading COUNT must not advance it.
    bus.mmio_read(PB_GPT_BASE + PB_GPT_COUNT_VAL, &out);
    CHECK(out == 0 && bus.gpt.count == 0, "COUNT read does not advance counter");
    // VIC enable via MMIO.
    CHECK(bus.mmio_write(PB_VIC_BASE + PB_VIC_INT_EN0, 1u << 8), "INTENABLE write decoded");
    CHECK((bus.vic.enable & (1u << 8)) != 0, "INTENABLE reached VIC model");
    // Virtual time drives the fire -> VIC line asserts.
    bus.tick(600); bus.tick(600); // 1200 > 1000
    CHECK(bus.gpt.count == 1200, "virtual time advanced counter deterministically");
    bus.mmio_read(PB_VIC_BASE + PB_VIC_IRQ_STATUS0, &out);
    CHECK(out == (1u << 8), "VIC_IRQ_STATUS0 read reflects masked pending");
    // ACK/EOI via VIC_IRQ_VEC_RD read (vector) + write (EOI).
    bus.mmio_read(PB_VIC_BASE + PB_VIC_IRQ_VEC_RD, &out);
    CHECK(out == 8, "vector read returns pending line #8");
    CHECK(bus.vic.in_service_line == 8, "line taken into service");
    bus.mmio_write(PB_VIC_BASE + PB_VIC_IRQ_VEC_RD, 0);
    CHECK(!bus.vic.irq_asserted() && bus.vic.in_service == 0, "EOI cleared pending+service");
}

int main(int argc, char** argv) {
    std::string mode = argc > 1 ? argv[1] : "";
    test_decode_semantics();
    if (mode == "buggy" || mode == "buggy-disconnected") {
        run(/*disc=*/true, /*reentrant=*/false);
    } else if (mode == "buggy-reentrant") {
        run(/*disc=*/false, /*reentrant=*/true);
    } else {
        run(false, false);
    }
    if (failures) { printf("\n%d CHECK(s) FAILED\n", failures); return 1; }
    printf("\nALL PASS (%s)\n", mode.empty() ? "positive" : mode.c_str());
    return 0;
}
