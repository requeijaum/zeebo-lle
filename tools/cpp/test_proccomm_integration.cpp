// test_proccomm_integration.cpp — QW99 Bug 6 integration-level regression.
//
// Exercises the SAME shared-SMEM handshake the production emulator runs in
// zeebo_lle_main.cpp's APP_COMMAND write path:
//     proccomm_.core0_issue(cmd);
//     proccomm_.core1_service();
//     // mirror completion words (APP_COMMAND, APP_STATUS) into the guest view
//     uc_mem_write(core0, SMEM+0x04, status); uc_mem_write(core0, SMEM+0x00, done);
//
// Here a second byte buffer stands in for the guest's Unicorn-mapped view; we
// mirror exactly the two completion words production mirrors, then assert the
// GUEST observes an honest DONE + SUCCESS for RESET_MODEM (id 0x1), which the
// old APP_COMMAND-derived model could never deliver (0x1 read back as DONE).
//
// The `derived` mode reproduces the pre-fix collision and MUST fail.

#include "zeebo_proccomm.h"
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

using namespace zeebo;
static int failures = 0;
#define CHECK(c, msg) do { if (!(c)) { printf("FAIL: %s\n", msg); ++failures; } \
                           else { printf("ok: %s\n", msg); } } while (0)

// Little-endian u32 read from a raw byte buffer (guest CPU semantics).
static pc_u32 rdle(const std::vector<uint8_t>& b, size_t off) {
    pc_u32 v = 0; memcpy(&v, b.data() + off, 4); return v;
}

// Reproduce the production APP_COMMAND write handler for one command, over a
// shared SMEM backing, mirroring completion into a separate guest view buffer.
// If use_derived is true, pending is (wrongly) derived from APP_COMMAND — the
// pre-fix bug — so RESET_MODEM is never serviced.
static void run_production_command(std::vector<uint8_t>& smem,
                                   std::vector<uint8_t>& guest_view,
                                   ProcComm& pc, pc_u32 cmd, bool use_derived) {
    pc.core0_issue(cmd);
    bool serviced;
    if (use_derived) {
        pc_u32 c = pc.rd(PCOM_OFF_APP_COMMAND);
        bool pending = !(c == PCOM_CMD_IDLE || c == PCOM_CMD_DONE)
                       && pc.rd(PCOM_OFF_APP_STATUS) == PCOM_STATUS_UNSET;
        serviced = false;
        if (pending) serviced = pc.core1_service();
    } else {
        serviced = pc.core1_service();
    }
    (void)serviced;
    // Mirror the two completion words into the guest view, exactly like the
    // production uc_mem_write path (STATUS first, then COMMAND).
    pc_u32 st   = pc.rd(PCOM_OFF_APP_STATUS);
    pc_u32 done = pc.rd(PCOM_OFF_APP_COMMAND);
    memcpy(guest_view.data() + PCOM_OFF_APP_STATUS,  &st,   4);
    memcpy(guest_view.data() + PCOM_OFF_APP_COMMAND, &done, 4);
    (void)smem;
}

int main(int argc, char** argv) {
    bool derived = (argc > 1 && std::string(argv[1]) == "derived");

    // Single shared SMEM backing == production's smem_mem_.
    std::vector<uint8_t> smem(0x1000, 0);
    std::vector<uint8_t> guest_view(0x1000, 0xff); // guest's mapped SMEM view
    ProcComm pc; pc.bind(smem.data(), smem.size());

    // Guest (Core0) issues RESET_MODEM (id 0x1 — collides numerically with DONE).
    run_production_command(smem, guest_view, pc, PCOM_CMD_RESET_MODEM, derived);

    // The GUEST view must show an honest completed handshake.
    CHECK(rdle(guest_view, PCOM_OFF_APP_COMMAND) == PCOM_CMD_DONE,
          "guest observes APP_COMMAND == DONE after RESET_MODEM handshake");
    CHECK(rdle(guest_view, PCOM_OFF_APP_STATUS) == PCOM_CMD_SUCCESS,
          "guest observes APP_STATUS == SUCCESS for RESET_MODEM");
    CHECK(pc.completed(), "shared backing reports completed");
    CHECK(!pc.command_pending(), "no command left pending after service");

    if (failures) { printf("\n%d CHECK(s) FAILED\n", failures); return 1; }
    printf("\nALL PASS (%s)\n", derived ? "derived-negative" : "integration");
    return 0;
}
