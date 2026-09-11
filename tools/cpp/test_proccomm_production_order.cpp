// test_proccomm_production_order.cpp — QW99 Bug 6 / AUDIO_TODO A11 regression.
//
// Reproduces the PRODUCTION scheduling ordering of zeebo_lle_main.cpp with a
// REAL Unicorn guest that executes an actual ARM `str` into APP_COMMAND, under
// the same UC_HOOK_MEM_WRITE the emulator installs. It proves the A11 fix:
//
//   * The write hook ONLY issues the command (visible + PENDING). It does NOT
//     service the modem and does NOT write DONE — so the guest's own str is the
//     last writer of SMEM+0x00 for that slice.
//   * Core1 (modem) service happens BETWEEN slices (engine quiescent), mirroring
//     the honest completion into the guest-visible mapping.
//   * A subsequent guest LOAD then observes DONE / SUCCESS.
//
// Modes:
//   (default)  — production ordering (issue-in-hook, service-between-slices):
//                MUST pass; guest observes DONE/SUCCESS.
//   sync       — MUTATION reproducing the A11 bug: the hook services the modem
//                AND writes DONE synchronously, then the guest str retires and
//                CLOBBERS DONE with the command id. MUST fail (command 0x1 read
//                back as 0x1, never DONE), proving the ordering matters on a
//                live Unicorn guest.

#include "zeebo_proccomm.h"
#include <unicorn/unicorn.h>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

using namespace zeebo;
static int failures = 0;
#define CHECK(c, msg) do { if (!(c)) { printf("FAIL: %s\n", msg); ++failures; } \
                           else { printf("ok: %s\n", msg); } } while (0)

static const uint64_t CODE_BASE = 0x00001000;
static const uint64_t SMEM_BASE = 0x01f00000;

// Shared model state visible to the hook.
struct Model {
    ProcComm pc;
    std::vector<uint8_t>* smem_backing;
    uc_engine* uc;
    bool sync_bug; // reproduce A11: service+DONE synchronously inside the hook
};

// Mirror of the production APP_COMMAND write handler.
static void mem_write_hook(uc_engine* uc, uc_mem_type, uint64_t addr,
                           int /*size*/, int64_t value, void* ud) {
    Model* m = (Model*)ud;
    if (addr != SMEM_BASE + 0x00) return;
    uint32_t cmd = (uint32_t)value;
    // Issue: command visible in the shared backing, PENDING. (Correct path.)
    m->pc.core0_issue(cmd);
    if (m->sync_bug) {
        // A11 BUG: service the modem and write DONE right here, inside the write
        // hook — BEFORE the guest str retires. Unicorn will then complete the
        // str and overwrite SMEM+0x00 with `cmd`, clobbering DONE.
        m->pc.core1_service();
        uint32_t st   = m->pc.status();
        uint32_t done = m->pc.rd(PCOM_OFF_APP_COMMAND);
        uc_mem_write(uc, SMEM_BASE + 0x04, &st, 4);
        uc_mem_write(uc, SMEM_BASE + 0x00, &done, 4);
    }
    // Correct path: do nothing else. The guest str owns this store; the modem
    // services between slices.
}

// Service one pending ProcComm command between slices (engine quiescent).
static void service_between_slices(Model* m) {
    if (!m->pc.command_pending()) return;
    if (m->pc.core1_service()) {
        uint32_t st   = m->pc.status();
        uint32_t done = m->pc.rd(PCOM_OFF_APP_COMMAND);
        uc_mem_write(m->uc, SMEM_BASE + 0x04, &st, 4);
        uc_mem_write(m->uc, SMEM_BASE + 0x00, &done, 4);
    }
}

int main(int argc, char** argv) {
    bool sync_bug = (argc > 1 && std::string(argv[1]) == "sync");

    uc_engine* uc = nullptr;
    if (uc_open(UC_ARCH_ARM, UC_MODE_ARM, &uc) != UC_ERR_OK) {
        printf("FAIL: uc_open\n"); return 1;
    }
    uc_mem_map(uc, CODE_BASE, 0x1000, UC_PROT_ALL);
    uc_mem_map(uc, SMEM_BASE, 0x1000, UC_PROT_ALL);

    // Shared SMEM backing that BOTH cores (model + guest view) agree on. Here the
    // guest view IS the Unicorn mapping; the model's ProcComm uses a host mirror
    // and publishes completion words into the Unicorn mapping, exactly like
    // production (smem_mem_ + uc_mem_write mirroring).
    std::vector<uint8_t> smem(0x1000, 0);
    ProcComm pc; pc.bind(smem.data(), smem.size());

    Model m{}; m.pc = pc; m.smem_backing = &smem; m.uc = uc; m.sync_bug = sync_bug;
    // Re-bind after copy so the hook writes the SAME backing we inspect.
    m.pc.bind(smem.data(), smem.size());

    uc_hook h;
    uc_hook_add(uc, &h, UC_HOOK_MEM_WRITE, (void*)mem_write_hook, &m,
                SMEM_BASE, SMEM_BASE + 0x100);

    // Guest program (ARM):
    //   Slice 1:  r0 = SMEM_BASE ; r1 = 0x10 (GET_DEM_STATE) ; str r1,[r0] ; nop-ish
    //   Slice 2:  ldr r2,[r0]     ; ldr r3,[r0,#4]        ; (r2=APP_COMMAND, r3=STATUS)
    // We split execution into two uc_emu_start slices with the modem service in
    // between, mirroring run_interleaved.
    uint32_t prog[] = {
        0xe59f0018, // ldr r0, [pc, #0x18]  -> SMEM_BASE
        0xe3a01010, // mov r1, #0x10        -> PCOM_CMD_GET_DEM_STATE (supported; id != DONE)
        0xe5801000, // str r1, [r0]         -> APP_COMMAND write (triggers hook)
        0xe1a00000, // nop (mov r0,r0)      -> slice-1 boundary padding
        // --- slice 2 continues here ---
        0xe5902000, // ldr r2, [r0]         -> read back APP_COMMAND
        0xe5903004, // ldr r3, [r0, #4]     -> read back APP_STATUS
        0xe1a00000, // nop
        0xe1a00000, // nop
        SMEM_BASE,  // literal pool @ pc(0x1000+8)+0x18 = 0x1020
    };
    uc_mem_write(uc, CODE_BASE, prog, sizeof(prog));

    // Slice 1: execute up to and including the str (4 instrs).
    uc_err e = uc_emu_start(uc, CODE_BASE, CODE_BASE + 4 * 4, 0, 0);
    CHECK(e == UC_ERR_OK, "slice 1 (guest str APP_COMMAND) executed");

    // Between slices: in the CORRECT path the command is still PENDING here — the
    // hook did not service it. Assert that explicitly (production ordering).
    if (!sync_bug) {
        CHECK(m.pc.command_pending(),
              "command remains PENDING after the guest str (hook did not service)");
        // The guest str is the last writer: SMEM+0x00 holds the command id (0x1),
        // NOT DONE — proving no synchronous fake completion happened.
        uint32_t app_cmd_now = 0;
        uc_mem_read(uc, SMEM_BASE + 0x00, &app_cmd_now, 4);
        CHECK(app_cmd_now == PCOM_CMD_GET_DEM_STATE,
              "guest view still shows command id (no premature DONE)");
    }

    // Modem service step (Core1), engine quiescent — the only DONE writer.
    service_between_slices(&m);

    // Slice 2: the guest loads APP_COMMAND / APP_STATUS.
    e = uc_emu_start(uc, CODE_BASE + 4 * 4, CODE_BASE + 8 * 4, 0, 0);
    CHECK(e == UC_ERR_OK, "slice 2 (guest read-back) executed");

    uint32_t r2 = 0, r3 = 0;
    uc_reg_read(uc, UC_ARM_REG_R2, &r2); // APP_COMMAND
    uc_reg_read(uc, UC_ARM_REG_R3, &r3); // APP_STATUS

    CHECK(r2 == PCOM_CMD_DONE,
          "guest read observes APP_COMMAND == DONE after modem service");
    CHECK(r3 == PCOM_CMD_SUCCESS,
          "guest read observes APP_STATUS == SUCCESS for GET_DEM_STATE");
    CHECK(!m.pc.command_pending(), "no command left pending after service");

    uc_close(uc);

    if (failures) {
        printf("\n%d CHECK(s) FAILED (%s)\n", failures, sync_bug ? "sync-bug" : "production");
        return 1;
    }
    printf("\nALL PASS (%s)\n", sync_bug ? "sync-bug" : "production");
    return 0;
}
