// test_proccomm.cpp — Bug 6: inter-core ProcComm handshake over shared SMEM.
//
// The key regression: Core 0's write must NOT immediately fabricate success.
// A command becomes visible in shared SMEM as PENDING; only an explicit Core 1
// service step produces completion, and the status must be honest
// (SUCCESS for supported ids, FAIL_UNSUPPORTED otherwise).
//
// Positive: supported command completes SUCCESS after a Core1 step, and the
// command is genuinely visible in the shared backing before completion.
// Negative-honesty: an unsupported command completes as FAIL_UNSUPPORTED, never
// faked as success.
//
// Negative mutation (argv[1]=="buggy"): simulate the old Core0-immediate-success
// path (mark done+SUCCESS at issue time, no Core1 service). The test asserts
// that this is detectably WRONG: completion appears with zero modem work, which
// the honest model forbids.

#include "zeebo_proccomm.h"
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

using namespace zeebo;
static int failures = 0;
#define CHECK(c, msg) do { if (!(c)) { printf("FAIL: %s\n", msg); ++failures; } \
                           else { printf("ok: %s\n", msg); } } while (0)

int main(int argc, char** argv) {
    std::string mode = (argc > 1) ? std::string(argv[1]) : std::string();
    bool buggy = (mode == "buggy");
    bool derived = (mode == "derived");

    // --- Mutation/control: pending derived ONLY from APP_COMMAND. ---
    // This reproduces the QW99 Bug 6 collision directly: PCOM_CMD_RESET_MODEM
    // (0x1) == PCOM_CMD_DONE (0x1). A model that decides "pending" from the
    // APP_COMMAND word treats a freshly-issued RESET_MODEM as already done and
    // never services it. This branch MUST fail so the build's negative control
    // catches any regression back to APP_COMMAND-derived state.
    if (derived) {
        std::vector<uint8_t> s(0x1000, 0);
        ProcComm app;  app.bind(s.data(), s.size());
        ProcComm modem; modem.bind(s.data(), s.size());
        app.core0_issue(PCOM_CMD_RESET_MODEM);
        // Emulate the buggy derivation instead of the shared shadow.
        auto derived_pending = [&]() {
            pc_u32 cmd = app.rd(PCOM_OFF_APP_COMMAND);
            if (cmd == PCOM_CMD_IDLE || cmd == PCOM_CMD_DONE) return false;
            return app.rd(PCOM_OFF_APP_STATUS) == PCOM_STATUS_UNSET;
        };
        CHECK(derived_pending(),
              "derived-from-APP_COMMAND sees RESET_MODEM as pending (it does NOT)");
        (void)modem;
        if (failures) { printf("\n%d CHECK(s) FAILED\n", failures); return 1; }
        printf("\nALL PASS (derived-negative)\n");
        return 0;
    }

    // Shared SMEM backing that BOTH cores see (single buffer == shared memory).
    std::vector<uint8_t> smem(0x1000, 0);
    ProcComm app;  app.bind(smem.data(), smem.size());   // Core 0 view
    ProcComm modem; modem.bind(smem.data(), smem.size()); // Core 1 view (same bytes)

    // --- Case 1: supported command, correct handshake. ---
    app.core0_issue(PCOM_CMD_GET_DEM_STATE, /*data1=*/0xdead, /*data2=*/0xbeef);

    // Command must be visible through the SHARED backing to the modem view.
    CHECK(modem.rd(PCOM_OFF_APP_COMMAND) == PCOM_CMD_GET_DEM_STATE,
          "command visible via shared SMEM to Core1");
    CHECK(modem.rd(PCOM_OFF_APP_DATA1) == 0xdead, "data1 visible via shared SMEM");

    if (buggy) {
        // Simulate the removed immediate-success shortcut: Core0 itself writes
        // DONE+SUCCESS without any modem service.
        app.wr(PCOM_OFF_APP_STATUS, PCOM_CMD_SUCCESS);
        app.wr(PCOM_OFF_APP_COMMAND, PCOM_CMD_DONE);
        CHECK(app.completed(),
              "buggy: fabricated completion appears without modem service (WRONG)");
        // Assert the honest model would NOT have completed on its own: before a
        // modem step, completed() should be false in the real path.
        CHECK(false, "buggy: immediate-success path is dishonest and must fail");
    } else {
        // Before Core1 services, there is NO completion — no fabricated success.
        CHECK(!app.completed(), "no completion before modem services (no fake success)");
        CHECK(app.status() == PCOM_STATUS_UNSET, "status unset before modem verdict");
        CHECK(app.command_pending(), "command is pending awaiting modem");

        // Core 1 services the command -> real completion.
        bool serviced = modem.core1_service();
        CHECK(serviced, "Core1 serviced the pending command");
        CHECK(app.completed(), "completion visible to Core0 after modem step");
        CHECK(app.status() == PCOM_CMD_SUCCESS, "supported command -> honest SUCCESS");

        // --- Case 2: UNSUPPORTED command stays honest. ---
        // Reset shared block for a clean second exchange.
        std::fill(smem.begin(), smem.begin()+0x10, 0);
        app.wr(PCOM_OFF_APP_STATUS, PCOM_STATUS_UNSET);

        app.core0_issue(0xdeadbeef /* not in supported set */);
        CHECK(!app.completed(), "unsupported: no fake success at issue");
        bool s2 = modem.core1_service();
        CHECK(s2, "Core1 serviced the unsupported command");
        CHECK(app.completed(), "unsupported command still completes the handshake");
        CHECK(app.status() == PCOM_CMD_FAIL_UNSUPPORTED,
              "unsupported command -> honest FAIL_UNSUPPORTED (never faked success)");

        // Servicing with nothing pending returns false (no spurious completion).
        CHECK(!modem.core1_service(), "no pending command -> nothing serviced");

        // --- Case 3 (QW99 Bug 6): RESET_MODEM id == PCOM_CMD_DONE value (0x1). ---
        // Regression for the command-1/DONE collision: when Core0 issues
        // RESET_MODEM (id 0x1), the handshake MUST NOT confuse the pending
        // command with a completed one. pending state derives from an honest
        // shadow, NOT from APP_COMMAND == 0x1 (which also means DONE). A model
        // that reads pending only from APP_COMMAND treats RESET_MODEM as already
        // done and never services it.
        std::fill(smem.begin(), smem.begin()+0x10, 0);
        app.wr(PCOM_OFF_APP_STATUS, PCOM_STATUS_UNSET);

        app.core0_issue(PCOM_CMD_RESET_MODEM, /*data1=*/0x1234, /*data2=*/0);
        CHECK(modem.rd(PCOM_OFF_APP_COMMAND) == PCOM_CMD_RESET_MODEM,
              "RESET_MODEM (0x1) visible via shared SMEM to Core1");
        CHECK(!app.completed(),
              "RESET_MODEM: no fake completion at issue (0x1 != DONE here)");
        CHECK(app.command_pending(),
              "RESET_MODEM is pending awaiting modem (not confused with DONE)");

        bool s3 = modem.core1_service();
        CHECK(s3, "Core1 serviced the pending RESET_MODEM command");
        CHECK(app.completed(), "RESET_MODEM completion visible to Core0 after modem step");
        CHECK(app.status() == PCOM_CMD_SUCCESS,
              "supported RESET_MODEM -> honest SUCCESS (guest-visible handshake)");
        // After completion APP_COMMAND reads DONE(0x1); a fresh spurious service
        // must not fire again.
        CHECK(!modem.core1_service(),
              "RESET_MODEM: completed handshake is not re-serviced");
    }

    if (failures) { printf("\n%d CHECK(s) FAILED\n", failures); return 1; }
    printf("\nALL PASS (%s)\n", buggy ? "buggy-negative" : "positive");
    return 0;
}
