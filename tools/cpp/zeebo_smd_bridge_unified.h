// zeebo_smd_bridge_unified.h — extracted from zeebo_lle_main.cpp so the
// production ONCRPC/SMD injection path is testable in isolation.
//
// Contains ONLY the AMSS messaging structs/constants and the UnifiedSMDBridge
// injector that the orchestrator uses. No behavior change: this is the exact
// code that previously lived inline in zeebo_lle_main.cpp.
#pragma once

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>
#include <unicorn/unicorn.h>

// These aliases are identical redeclarations when included after
// zeebo_lle_main.cpp's own `using` block, and self-sufficient in tests.
using u8  = uint8_t;
using u32 = uint32_t;

// ---- Shared Memory SMD / ONCRPC Subsystem (Zeebo AMSS Messaging) ----
enum {
    AMSS_SMD_CHANNEL_ADDR   = 0x1755d1dc,
    AMSS_RPC_QUEUE_HEAD     = 0x17571748,
    AMSS_RPC_PACKET_BUFFER  = 0x177f2000,
};

#pragma pack(push, 1)
struct smd_half_channel {
    u8 state;
    u8 busy;
    u8 error;
    u8 link_status;
    u32 read_ptr;
    u32 write_ptr;
};

struct oncrpc_packet_header {
    u32 xid;
    u32 msg_type;       // 0 = CALL
    u32 rpc_version;    // 2
    u32 program;        // e.g., QDSP service
    u32 version;        // service version
    u32 procedure;      // Procedure ID (+0x20 offset)
    u32 cred_flavor;
    u32 cred_length;
    u32 verf_flavor;
    u32 verf_length;
};

struct oncrpc_queue_node {
    u32 next;
    u32 prev;
    u32 packet_addr;
    u32 packet_len;
    u32 status;
};
#pragma pack(pop)

class UnifiedSMDBridge {
public:
    UnifiedSMDBridge() : packets_injected_(0) {}

    void inject_packet(uc_engine* uc, u32 program, u32 proc_id, const std::vector<u8>& payload) {
        if (!uc) return;
        oncrpc_packet_header hdr{};
        hdr.xid = 0x12345678 + packets_injected_;
        hdr.msg_type = 0; // CALL
        hdr.rpc_version = 2;
        hdr.program = program; // Official MSM Audio/QDSP service (AUDMGR or ADSPRTOSATOM)
        hdr.version = 1;
        hdr.procedure = proc_id;

        u32 packet_target = AMSS_RPC_PACKET_BUFFER + (packets_injected_ * 0x500);
        uc_mem_write(uc, packet_target, &hdr, sizeof(hdr));
        if (!payload.empty()) {
            uc_mem_write(uc, packet_target + 0x80, payload.data(), payload.size());
        }

        oncrpc_queue_node node{};
        node.next = 0;
        node.prev = 0;
        node.packet_addr = packet_target;
        node.packet_len = sizeof(hdr) + 0x80 + payload.size();
        node.status = 1;

        u32 node_target = packet_target + 0x400;
        uc_mem_write(uc, node_target, &node, sizeof(node));

        u32 head = 0, tail = 0, count = 0;
        uc_mem_read(uc, AMSS_RPC_QUEUE_HEAD + 0, &head, 4);
        uc_mem_read(uc, AMSS_RPC_QUEUE_HEAD + 4, &tail, 4);
        uc_mem_read(uc, AMSS_RPC_QUEUE_HEAD + 8, &count, 4);

        if (count == 0) {
            head = node_target;
            tail = node_target;
        } else {
            uc_mem_write(uc, tail + 0, &node_target, 4);
            node.prev = tail;
            uc_mem_write(uc, node_target, &node, sizeof(node));
            tail = node_target;
        }
        count++;

        uc_mem_write(uc, AMSS_RPC_QUEUE_HEAD + 0, &head, 4);
        uc_mem_write(uc, AMSS_RPC_QUEUE_HEAD + 4, &tail, 4);
        uc_mem_write(uc, AMSS_RPC_QUEUE_HEAD + 8, &count, 4);

        smd_half_channel ch{};
        uc_mem_read(uc, AMSS_SMD_CHANNEL_ADDR, &ch, sizeof(ch));
        ch.state = 2; // SMD_SS_OPENED
        ch.link_status = 3; // SMD_SS_FLUSHING
        ch.busy = 0;
        ch.error = 0;
        uc_mem_write(uc, AMSS_SMD_CHANNEL_ADDR, &ch, sizeof(ch));

        packets_injected_++;
        printf("[SMD/ONCRPC] Injected packet #%u (Prog 0x%08x, Proc 0x%x) -> AMSS Queue (Total %u)\n",
               packets_injected_, program, proc_id, count);
    }

    u32 injected() const { return packets_injected_; }

private:
    u32 packets_injected_;
};
