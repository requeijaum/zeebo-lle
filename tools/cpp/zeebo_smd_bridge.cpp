// zeebo_smd_bridge.cpp — Phase 1: Shared Memory SMD/SMSM Bridge & Synthetic ONCRPC Ingestion
// Bridges Qualcomm MSM7201A Application processor (ARM11) and Modem (ARM9/AMSS).
// Base SMEM: 0x01F00000 (2MB)
// Models SMSM state progression (SMSM_SMDINIT = 0x00000008), ProcComm commands, and ONCRPC dispatch.

#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <cstring>
#include <vector>
#include <map>
#include <unicorn/unicorn.h>

using u8  = uint8_t;
using u16 = uint16_t;
using u32 = uint32_t;
using u64 = uint64_t;

enum {
    SMEM_BASE               = 0x01f00000,
    SMEM_SIZE               = 0x00200000, // 2MB
    
    // SMSM State flags
    SMSM_INIT               = 0x00000001,
    SMSM_OSENTERED          = 0x00000002,
    SMSM_SMDINIT            = 0x00000008,
    SMSM_RPCINIT            = 0x00000020,
    
    // SMD channel states
    SMD_SS_CLOSED           = 0,
    SMD_SS_OPENING          = 1,
    SMD_SS_OPENED           = 2,
    SMD_SS_FLUSHING         = 3,
    
    // MSM ProcComm Command offsets
    APP_COMMAND             = 0x00,
    APP_STATUS              = 0x04,
    APP_DATA1               = 0x08,
    APP_DATA2               = 0x0C,
    MDM_COMMAND             = 0x10,
    MDM_STATUS              = 0x14,
    MDM_DATA1               = 0x18,
    MDM_DATA2               = 0x1C,
    
    PCOM_CMD_IDLE           = 0x0,
    PCOM_CMD_DONE           = 0x1,
    PCOM_READY              = 0x1,
    PCOM_CMD_SUCCESS        = 0x3,
    
    // ONCRPC queue addresses in AMSS
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
    // Payload starts at offset +0x80
};

struct oncrpc_queue_node {
    u32 next;
    u32 prev;
    u32 packet_addr;
    u32 packet_len;
    u32 status;
};
#pragma pack(pop)

class VirtualSMDBridge {
public:
    VirtualSMDBridge(uc_engine* uc) : uc_(uc) {
        apps_smsm_state_ = SMSM_INIT | SMSM_OSENTERED | SMSM_SMDINIT | SMSM_RPCINIT;
        modem_smsm_state_ = 0;
        packets_injected_ = 0;
    }

    void init_smem() {
        std::vector<u8> zero(SMEM_SIZE, 0);
        uc_mem_write(uc_, SMEM_BASE, zero.data(), zero.size());

        // Initialize ProcComm status
        u32 ready = PCOM_READY;
        uc_mem_write(uc_, SMEM_BASE + MDM_STATUS, &ready, 4);

        printf("[SMD Bridge] Shared Memory initialized at 0x%08x (2MB)\n", SMEM_BASE);
        printf("[SMD Bridge] Apps SMSM state set to 0x%08x (SMSM_SMDINIT | SMSM_RPCINIT active)\n", apps_smsm_state_);
    }

    void inject_oncrpc_packet(u32 procedure_id, const std::vector<u8>& payload) {
        // Build ONCRPC CALL packet
        oncrpc_packet_header hdr{};
        hdr.xid = 0x12345678 + packets_injected_;
        hdr.msg_type = 0; // CALL
        hdr.rpc_version = 2;
        hdr.program = 0x30000060; // MSM Audio/QDSP service
        hdr.version = 1;
        hdr.procedure = procedure_id;

        u32 packet_target = AMSS_RPC_PACKET_BUFFER + (packets_injected_ * 0x500);
        
        // Write packet header
        uc_mem_write(uc_, packet_target, &hdr, sizeof(hdr));

        // Write payload at +0x80
        if (!payload.empty()) {
            uc_mem_write(uc_, packet_target + 0x80, payload.data(), payload.size());
        }

        // Build queue node
        oncrpc_queue_node node{};
        node.next = 0;
        node.prev = 0;
        node.packet_addr = packet_target;
        node.packet_len = sizeof(hdr) + 0x80 + payload.size();
        node.status = 1;

        u32 node_target = packet_target + 0x400;
        uc_mem_write(uc_, node_target, &node, sizeof(node));

        // Link into queue head at 0x17571748
        u32 head = 0, tail = 0, count = 0;
        uc_mem_read(uc_, AMSS_RPC_QUEUE_HEAD + 0, &head, 4);
        uc_mem_read(uc_, AMSS_RPC_QUEUE_HEAD + 4, &tail, 4);
        uc_mem_read(uc_, AMSS_RPC_QUEUE_HEAD + 8, &count, 4);

        if (count == 0) {
            head = node_target;
            tail = node_target;
        } else {
            // Append to tail
            uc_mem_write(uc_, tail + 0, &node_target, 4); // tail->next = node_target
            node.prev = tail;
            uc_mem_write(uc_, node_target, &node, sizeof(node));
            tail = node_target;
        }
        count++;

        uc_mem_write(uc_, AMSS_RPC_QUEUE_HEAD + 0, &head, 4);
        uc_mem_write(uc_, AMSS_RPC_QUEUE_HEAD + 4, &tail, 4);
        uc_mem_write(uc_, AMSS_RPC_QUEUE_HEAD + 8, &count, 4);

        // Update SMD channel structure state to SMD_SS_FLUSHING / OPENED (3)
        smd_half_channel ch{};
        uc_mem_read(uc_, AMSS_SMD_CHANNEL_ADDR, &ch, sizeof(ch));
        ch.state = SMD_SS_OPENED;
        ch.link_status = SMD_SS_FLUSHING; // state 3 triggers flush/packet ingestion
        ch.busy = 0;
        ch.error = 0;
        uc_mem_write(uc_, AMSS_SMD_CHANNEL_ADDR, &ch, sizeof(ch));

        packets_injected_++;
        printf("[SMD Bridge] Injected ONCRPC packet #%u (Proc 0x%x, len %u) -> Queue Head 0x%08x (Total: %u)\n",
               packets_injected_, procedure_id, (u32)payload.size(), AMSS_RPC_QUEUE_HEAD, count);
    }

    u32 packets_injected() const { return packets_injected_; }

private:
    uc_engine* uc_;
    u32 apps_smsm_state_;
    u32 modem_smsm_state_;
    u32 packets_injected_;
};

int main(int argc, char** argv) {
    printf("===================================================================\n");
    printf("  ZEEBO SMD/SMSM BRIDGE: Synthetic ONCRPC Ingestion Test          \n");
    printf("===================================================================\n");

    uc_engine* uc = nullptr;
    uc_err err = uc_open(UC_ARCH_ARM, UC_MODE_ARM, &uc);
    if (err != UC_ERR_OK) {
        printf("[Fatal] uc_open failed: %s\n", uc_strerror(err));
        return 1;
    }

    // Map SMEM and AMSS memory regions
    uc_mem_map(uc, SMEM_BASE, SMEM_SIZE, UC_PROT_ALL);
    uc_mem_map(uc, 0x17500000, 0x00400000, UC_PROT_ALL); // AMSS data space (0x17500000..0x17900000)

    VirtualSMDBridge bridge(uc);
    bridge.init_smem();

    // Initialize AMSS RPC queue head at 0x17571748 with empty state
    u32 q_init[8] = {0, 0, 0, 0, 0, 0, 0, 1}; // init flag at +0x1c = 1
    uc_mem_write(uc, AMSS_RPC_QUEUE_HEAD, q_init, sizeof(q_init));

    // Initialize AMSS SMD half-channel structure at 0x1755d1dc
    smd_half_channel ch_init = {SMD_SS_OPENED, 0, 0, SMD_SS_OPENED, 0, 0};
    uc_mem_write(uc, AMSS_SMD_CHANNEL_ADDR, &ch_init, sizeof(ch_init));

    printf("[SMD Bridge] AMSS Queue Head and SMD channel initialized.\n");

    // Test injecting QDSP Audio Post-Processor command packet
    std::vector<u8> dummy_audpp_payload = {0x01, 0x02, 0x03, 0x04, 0x10, 0x20, 0x30, 0x40};
    bridge.inject_oncrpc_packet(0x1b59, dummy_audpp_payload);

    // Verify queue status
    u32 head = 0, count = 0;
    uc_mem_read(uc, AMSS_RPC_QUEUE_HEAD + 0, &head, 4);
    uc_mem_read(uc, AMSS_RPC_QUEUE_HEAD + 8, &count, 4);
    printf("[SMD Bridge] Post-injection Verification: Head = 0x%08x, Count = %u\n", head, count);

    // Verify channel structure status byte
    smd_half_channel ch_verify{};
    uc_mem_read(uc, AMSS_SMD_CHANNEL_ADDR, &ch_verify, sizeof(ch_verify));
    printf("[SMD Bridge] Channel link_status = %u (expected %u), state = %u\n",
           ch_verify.link_status, SMD_SS_FLUSHING, ch_verify.state);

    if (count == 1 && ch_verify.link_status == SMD_SS_FLUSHING) {
        printf("[SMD Bridge] SUCCESS: Synthetic ONCRPC packet ingested and SMD channel prepared for dispatch.\n");
    } else {
        printf("[SMD Bridge] FAILURE: Queue or channel state mismatch.\n");
    }

    uc_close(uc);
    return 0;
}
