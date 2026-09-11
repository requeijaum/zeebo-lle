// test_smd_bridge_bounds.cpp — bug 9 regression: SMD packet payload must not
// overlap the queue node placed at slot + 0x400.
//
// The ONCRPC packet slot layout is:
//   [+0x000 .. +0x080)  header
//   [+0x080 .. +0x400)  payload   (max 0x380 bytes)
//   [+0x400 .. )        queue node
//
// A payload > 0x380 would spill into the queue node. The fixed
// inject_oncrpc_packet() must REJECT such a packet (return false) BEFORE any
// write, leaving the queue node region untouched, while still accepting a
// maximum-size valid payload.

#define ZEEBO_SMD_BRIDGE_NO_MAIN
#include "zeebo_smd_bridge.cpp"

#include <cassert>

static int failures = 0;
#define CHECK(cond, msg) do { \
    if (!(cond)) { printf("  FAIL: %s\n", msg); ++failures; } \
    else         { printf("  ok:   %s\n", msg); } \
} while (0)

int main() {
    printf("== test_smd_bridge_bounds (bug 9) ==\n");

    uc_engine* uc = nullptr;
    uc_err err = uc_open(UC_ARCH_ARM, UC_MODE_ARM, &uc);
    assert(err == UC_ERR_OK);
    uc_mem_map(uc, SMEM_BASE, SMEM_SIZE, UC_PROT_ALL);
    uc_mem_map(uc, 0x17500000, 0x00400000, UC_PROT_ALL);

    VirtualSMDBridge bridge(uc);
    bridge.init_smem();
    u32 q_init[8] = {0, 0, 0, 0, 0, 0, 0, 1};
    uc_mem_write(uc, AMSS_RPC_QUEUE_HEAD, q_init, sizeof(q_init));
    smd_half_channel ch_init = {SMD_SS_OPENED, 0, 0, SMD_SS_OPENED, 0, 0};
    uc_mem_write(uc, AMSS_SMD_CHANNEL_ADDR, &ch_init, sizeof(ch_init));

    const u32 slot0      = AMSS_RPC_PACKET_BUFFER;               // packet #0 slot
    const u32 node0_addr = slot0 + VirtualSMDBridge::NODE_OFFSET;

    // Pre-fill the node region with a known sentinel so we can detect any
    // partial write from a rejected injection.
    u8 sentinel[sizeof(oncrpc_queue_node)];
    memset(sentinel, 0xAB, sizeof(sentinel));
    uc_mem_write(uc, node0_addr, sentinel, sizeof(sentinel));

    // --- 1) Oversize payload must be rejected, no writes, node untouched ---
    std::vector<u8> oversize(VirtualSMDBridge::PAYLOAD_MAX + 1, 0xCC);
    bool ok = bridge.inject_oncrpc_packet(0x1b59, oversize);
    CHECK(!ok, "oversize payload (0x381) is rejected (returns false)");
    CHECK(bridge.packets_injected() == 0, "no packet counted after rejection");

    u8 node_after[sizeof(oncrpc_queue_node)];
    uc_mem_read(uc, node0_addr, node_after, sizeof(node_after));
    CHECK(memcmp(node_after, sentinel, sizeof(sentinel)) == 0,
          "queue node region UNTOUCHED after rejected oversize injection");

    u32 count = 0;
    uc_mem_read(uc, AMSS_RPC_QUEUE_HEAD + 8, &count, 4);
    CHECK(count == 0, "queue count still 0 after rejection");

    // --- 2) Maximum valid payload (0x380) is accepted and does NOT overlap ---
    std::vector<u8> maxpay(VirtualSMDBridge::PAYLOAD_MAX, 0x5A);
    ok = bridge.inject_oncrpc_packet(0x1b59, maxpay);
    CHECK(ok, "max-size payload (0x380) is accepted (returns true)");
    CHECK(bridge.packets_injected() == 1, "one packet counted");

    // Payload last byte at slot+0x80+0x37F; node starts at slot+0x400.
    // Verify the byte just before the node is our payload, and node was written.
    u8 last_payload = 0;
    uc_mem_read(uc, slot0 + VirtualSMDBridge::PAYLOAD_OFFSET +
                    VirtualSMDBridge::PAYLOAD_MAX - 1, &last_payload, 1);
    CHECK(last_payload == 0x5A, "last payload byte sits exactly at slot+0x3FF");

    oncrpc_queue_node node{};
    uc_mem_read(uc, node0_addr, &node, sizeof(node));
    CHECK(node.packet_addr == slot0, "queue node packet_addr correct (not clobbered by payload)");
    uc_mem_read(uc, AMSS_RPC_QUEUE_HEAD + 8, &count, 4);
    CHECK(count == 1, "queue count is 1 after valid injection");

    uc_close(uc);

    if (failures == 0) { printf("PASS: SMD packet/node bounded layout holds.\n"); return 0; }
    printf("FAILURE: %d check(s) failed.\n", failures);
    return 1;
}
