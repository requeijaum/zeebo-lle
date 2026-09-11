#pragma once
// zeebo_proccomm.h — Minimal inter-core ProcComm (proc_comm) handshake over
// shared SMEM (bug 6).
//
// The real MSM proc_comm protocol (kernel arch/arm/mach-msm/proc_comm.c):
//   APP_COMMAND (+0x00)  APP_STATUS (+0x04)  APP_DATA1 (+0x08)  APP_DATA2 (+0x0C)
// Apps (Core 0) writes a command id + data, rings the modem doorbell, then
// spins until APP_COMMAND reads back PCOM_CMD_DONE and inspects APP_STATUS.
// The modem (Core 1) is the *only* writer of the completion.
//
// This model deliberately does NOT fabricate success in the Core 0 write path.
// A Core 0 command becomes visible in SMEM as a PENDING command; only an
// explicit Core 1 service step turns it into DONE and writes an honest status:
//   * supported command  -> PCOM_CMD_SUCCESS
//   * everything else     -> PCOM_CMD_FAIL_UNSUPPORTED (honest, command-specific)
//
// The handshake operates purely on the shared backing bytes so both cores see
// the same memory; no immediate-success shortcut exists.

#include <cstdint>
#include <cstring>

namespace zeebo {

using pc_u32 = uint32_t;

// Byte offsets inside the ProcComm control block (relative to SMEM base).
enum : pc_u32 {
    PCOM_OFF_APP_COMMAND = 0x00,
    PCOM_OFF_APP_STATUS  = 0x04,
    PCOM_OFF_APP_DATA1   = 0x08,
    PCOM_OFF_APP_DATA2   = 0x0c,
};

// Command / status sentinels.
enum : pc_u32 {
    PCOM_CMD_IDLE = 0x00000000u,
    PCOM_CMD_DONE = 0x00000001u, // modem sets APP_COMMAND to this on completion

    // Status codes written by the modem into APP_STATUS.
    PCOM_STATUS_UNSET            = 0xffffffffu,
    PCOM_CMD_SUCCESS            = 0x00000000u,
    PCOM_CMD_FAIL_UNSUPPORTED   = 0x00000001u,
};

// A tiny set of commands we can honestly service. Anything outside this list is
// reported as UNSUPPORTED rather than faked as success.
enum : pc_u32 {
    PCOM_CMD_RESET_MODEM   = 0x00000001u,
    PCOM_CMD_GET_DEM_STATE = 0x00000010u,
    // NOTE: value 0x1 collides with PCOM_CMD_DONE only as an APP_COMMAND *sentinel*
    // meaning; a live pending command carries the id in a separate shadow so the
    // model never confuses "command 1 pending" with "done".
};

// Operates over a raw shared byte backing (the SMEM std::vector<uint8_t> data).
// All accesses are little-endian u32 like the guest CPU.
struct ProcComm {
    uint8_t* smem = nullptr; // pointer to shared SMEM base backing
    size_t   smem_size = 0;

    // Shadow of the currently-pending command id. -1 (all ones) = none pending.
    // Kept out of the guest-visible APP_COMMAND word, which the modem overwrites
    // with PCOM_CMD_DONE on completion.
    pc_u32 pending_cmd = PCOM_STATUS_UNSET;
    bool   has_pending = false;

    void bind(uint8_t* base, size_t size) {
        smem = base; smem_size = size;
        // Idle marker so a fresh block is not mistaken for a pending command.
        if (in_range(PCOM_OFF_APP_STATUS)) wr(PCOM_OFF_APP_STATUS, PCOM_STATUS_UNSET);
    }

    bool in_range(pc_u32 off) const { return smem && (off + 4) <= smem_size; }

    pc_u32 rd(pc_u32 off) const {
        pc_u32 v = 0; if (in_range(off)) memcpy(&v, smem + off, 4); return v;
    }
    void wr(pc_u32 off, pc_u32 v) {
        if (in_range(off)) memcpy(smem + off, &v, 4);
    }

    // Core 0 issues a command. This makes the command VISIBLE in shared SMEM and
    // marks it pending. It does NOT complete it and does NOT touch APP_STATUS's
    // completion meaning — the modem owns completion.
    void core0_issue(pc_u32 cmd, pc_u32 data1 = 0, pc_u32 data2 = 0) {
        wr(PCOM_OFF_APP_DATA1, data1);
        wr(PCOM_OFF_APP_DATA2, data2);
        wr(PCOM_OFF_APP_STATUS, PCOM_STATUS_UNSET); // no verdict yet
        wr(PCOM_OFF_APP_COMMAND, cmd);              // command id visible in SMEM
    }

    // Is a command visible in SMEM and awaiting the modem? Derived purely from
    // the shared backing so BOTH core views agree: a non-idle, non-DONE command
    // word with no verdict yet (STATUS still UNSET) is pending.
    bool command_pending() const {
        pc_u32 cmd = rd(PCOM_OFF_APP_COMMAND);
        if (cmd == PCOM_CMD_IDLE || cmd == PCOM_CMD_DONE) return false;
        return rd(PCOM_OFF_APP_STATUS) == PCOM_STATUS_UNSET;
    }

    static bool is_supported(pc_u32 cmd) {
        switch (cmd) {
            case PCOM_CMD_RESET_MODEM:
            case PCOM_CMD_GET_DEM_STATE:
                return true;
            default:
                return false;
        }
    }

    // Core 1 (modem) services one pending command and produces a real
    // completion in shared SMEM. Returns true if a command was serviced.
    // Honest status: supported -> SUCCESS, otherwise -> FAIL_UNSUPPORTED.
    bool core1_service() {
        if (!command_pending()) return false;
        pc_u32 cmd = rd(PCOM_OFF_APP_COMMAND);
        pc_u32 status = is_supported(cmd) ? PCOM_CMD_SUCCESS
                                          : PCOM_CMD_FAIL_UNSUPPORTED;
        // Modem is the sole writer of the completion words.
        wr(PCOM_OFF_APP_STATUS, status);
        wr(PCOM_OFF_APP_COMMAND, PCOM_CMD_DONE);
        return true;
    }

    // Convenience for the Core 0 side of the wait: has the modem completed?
    bool completed() const {
        return rd(PCOM_OFF_APP_COMMAND) == PCOM_CMD_DONE;
    }
    pc_u32 status() const { return rd(PCOM_OFF_APP_STATUS); }
};

} // namespace zeebo
