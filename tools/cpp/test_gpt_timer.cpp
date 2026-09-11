// test_gpt_timer.cpp — Bug 5: GPT counter advances from emulated progress and
// its MATCH compare raises a VIC line (host-only, deterministic).
//
// Positive: an enabled timer with MATCH=1000 advanced by emulated instruction
// counts eventually crosses MATCH exactly once, and that crossing is routed to
// a VIC line which then asserts. Reads do NOT advance the counter — only
// advance() (driven by emulated progress) does.
//
// Negative mutation (argv[1]=="buggy"): simulate the old behaviour where the
// counter only moves on reads / never fires -> a deadline busy-wait would spin
// forever. We model that as "advance is a no-op" and assert the line NEVER
// asserts, proving the positive gate is meaningful.

#include "zeebo_gpt_timer.h"
#include "zeebo_vic_irq.h"
#include <cstdio>
#include <string>

using namespace zeebo;
static int failures = 0;
#define CHECK(c, msg) do { if (!(c)) { printf("FAIL: %s\n", msg); ++failures; } \
                           else { printf("ok: %s\n", msg); } } while (0)

static const unsigned GPT_VIC_LINE = 8; // INT_GP_TIMER (model)

int main(int argc, char** argv) {
    bool buggy = (argc > 1 && std::string(argv[1]) == "buggy");

    GptTimer t;
    VicState vic;
    vic.enable_line(GPT_VIC_LINE);

    // Disabled timer must not advance/fire.
    CHECK(!t.advance(500), "disabled timer does not fire");
    CHECK(t.count == 0, "disabled timer counter stays 0");

    t.enable();
    t.set_match(1000);

    // Reading state (count) never advances it.
    uint32_t observed = t.count;
    (void)observed;
    CHECK(t.count == 0, "reading count does not advance counter");

    // Drive from emulated progress: 3 slices of 400 emulated ticks each = 1200,
    // crossing MATCH=1000 during the third slice.
    int fires = 0;
    uint32_t slices[] = {400, 400, 400};
    for (uint32_t s : slices) {
        uint32_t adv = buggy ? 0 : s; // buggy: counter never advances
        if (t.advance(adv)) {
            fires++;
            vic.raise_line(GPT_VIC_LINE); // route timer -> VIC
        }
    }

    if (buggy) {
        // MUTATION: counter never advances (old "reads-only / never fires" bug).
        // Assert the CORRECT outcome so the buggy variant FAILS (RED reproduced):
        // a real timer MUST advance and fire, letting a deadline wait terminate.
        CHECK(t.count == 1200, "buggy: stalled counter must be caught (RED)");
        CHECK(fires == 1, "buggy: missing MATCH fire must be caught (RED)");
        CHECK(vic.irq_asserted(), "buggy: absent VIC assertion must be caught (RED)");
    } else {
        CHECK(t.count == 1200, "counter advanced deterministically to 1200");
        CHECK(fires == 1, "MATCH fired exactly once across the crossing");
        CHECK(vic.irq_asserted(), "timer crossing asserted the VIC line");
        CHECK(vic.active_line() == (int)GPT_VIC_LINE, "active VIC line is the GPT line");

        // Further advance without re-arm must not fire again (one-shot).
        bool again = t.advance(2000);
        CHECK(!again, "no re-fire until match re-armed");

        // Re-arm (guest writes MATCH) then cross again.
        t.set_match(t.count + 100);
        CHECK(t.advance(200), "re-armed match fires on next crossing");
    }

    if (failures) { printf("\n%d CHECK(s) FAILED\n", failures); return 1; }
    printf("\nALL PASS (%s)\n", buggy ? "buggy-negative" : "positive");
    return 0;
}
