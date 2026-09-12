// test_qdsp5_doorbell_probe.cpp — integration test for the QDSP5 RPC-probe gate.
//
// Proves the production bug is fixed: an A2M doorbell must NOT mutate the AMSS
// ONCRPC queue when the diagnostic probe is OFF (default), and MUST inject the
// liveness packets only when the probe is explicitly ON.
//
// Drives the EXACT production injector (UnifiedSMDBridge) through the same gate
// (zeebo_qdsp5::doorbell_maybe_inject) the orchestrator now calls, against a
// real Unicorn engine with the AMSS queue/packet/channel regions mapped.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <unicorn/unicorn.h>

#include "zeebo_smd_bridge_unified.h"
#include "qdsp5_doorbell_probe.h"

static int failures = 0;
#define CHECK(cond, msg) do { \
    if (cond) { printf("ok: %s\n", msg); } \
    else { printf("FAIL: %s\n", msg); ++failures; } \
} while (0)

// Map a page-aligned window covering [addr, addr+len) into uc.
static void map_cover(uc_engine* uc, uint64_t addr, uint64_t len) {
    uint64_t start = addr & ~0xFFFULL;
    uint64_t end   = (addr + len + 0xFFF) & ~0xFFFULL;
    uc_mem_map(uc, start, end - start, UC_PROT_ALL);
}

static uc_engine* make_engine() {
    uc_engine* uc = nullptr;
    if (uc_open(UC_ARCH_ARM, UC_MODE_ARM, &uc) != UC_ERR_OK) {
        printf("FAIL: uc_open\n"); exit(2);
    }
    // Map the three AMSS regions the injector touches. Packet buffer needs room
    // for several 0x500-strided packets plus the +0x400 node + node struct.
    map_cover(uc, AMSS_SMD_CHANNEL_ADDR, sizeof(smd_half_channel));
    map_cover(uc, AMSS_RPC_QUEUE_HEAD, 0x40);
    map_cover(uc, AMSS_RPC_PACKET_BUFFER, 0x4000);
    return uc;
}

// Read the ONCRPC queue count (+0x08) — the mutation witness.
static u32 queue_count(uc_engine* uc) {
    u32 c = 0;
    uc_mem_read(uc, AMSS_RPC_QUEUE_HEAD + 8, &c, 4);
    return c;
}

int main() {
    // Textual false values must never opt into a queue-corrupting diagnostic.
    setenv("ZEEBO_QDSP5_RPC_PROBE", "off", 1);
    CHECK(!zeebo_qdsp5::rpc_probe_enabled(), "probe remains OFF when env=off");
    setenv("ZEEBO_QDSP5_RPC_PROBE", "false", 1);
    CHECK(!zeebo_qdsp5::rpc_probe_enabled(), "probe remains OFF when env=false");

    // --- Case 1: probe OFF (production default) -> NO queue mutation ---
    unsetenv("ZEEBO_QDSP5_RPC_PROBE");
    {
        uc_engine* uc = make_engine();
        UnifiedSMDBridge smd;
        u32 count_before = queue_count(uc);
        smd_half_channel ch_before{};
        uc_mem_read(uc, AMSS_SMD_CHANNEL_ADDR, &ch_before, sizeof(ch_before));

        CHECK(!zeebo_qdsp5::rpc_probe_enabled(), "probe defaults OFF (no env)");

        // Simulate many production doorbells.
        for (int i = 0; i < 32; ++i)
            zeebo_qdsp5::doorbell_maybe_inject(smd, uc);

        u32 count_after = queue_count(uc);
        smd_half_channel ch_after{};
        uc_mem_read(uc, AMSS_SMD_CHANNEL_ADDR, &ch_after, sizeof(ch_after));

        CHECK(count_before == 0, "queue count starts at 0");
        CHECK(count_after == 0, "probe OFF: doorbell does NOT mutate ONCRPC queue count");
        CHECK(smd.injected() == 0, "probe OFF: bridge injected 0 packets over 32 doorbells");
        CHECK(std::memcmp(&ch_before, &ch_after, sizeof(ch_before)) == 0,
              "probe OFF: SMD half-channel untouched (no fabricated FLUSHING)");
        uc_close(uc);
    }

    // --- Case 2: probe ON -> positive instrument control (injection happens) ---
    setenv("ZEEBO_QDSP5_RPC_PROBE", "1", 1);
    {
        uc_engine* uc = make_engine();
        UnifiedSMDBridge smd;
        CHECK(zeebo_qdsp5::rpc_probe_enabled(), "probe enabled when env=1");

        zeebo_qdsp5::doorbell_maybe_inject(smd, uc);  // one doorbell

        // Each doorbell injects exactly two liveness packets (AUDMGR + ADSPRTOS).
        CHECK(smd.injected() == 2, "probe ON: one doorbell injects 2 liveness packets");
        CHECK(queue_count(uc) == 2, "probe ON: ONCRPC queue count advanced to 2");

        smd_half_channel ch{};
        uc_mem_read(uc, AMSS_SMD_CHANNEL_ADDR, &ch, sizeof(ch));
        CHECK(ch.state == 2 && ch.link_status == 3,
              "probe ON: SMD channel driven to OPENED/FLUSHING (wakeup proven)");
        uc_close(uc);
    }
    unsetenv("ZEEBO_QDSP5_RPC_PROBE");

    if (failures == 0) { printf("\nALL PASS (QDSP5 doorbell probe gate)\n"); return 0; }
    printf("\n%d CHECK(S) FAILED\n", failures);
    return 1;
}
