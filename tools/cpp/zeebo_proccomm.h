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

    // Host-model "shadow" pending state, kept in the SAME shared backing so both
    // core views (Core0 issue, Core1 service) agree without touching the
    // guest-visible ProcComm words. The guest never reads these offsets; they
    // live past the guest ProcComm/MDM block (0x00..0x1f).
    //
    // QW99 Bug 6: the pending flag is the SOLE source of truth for
    // "a command awaits the modem". It must NOT be re-derived from APP_COMMAND,
    // because the id PCOM_CMD_RESET_MODEM (0x1) is numerically identical to the
    // completion sentinel PCOM_CMD_DONE (0x1) — deriving pending-ness from
    // APP_COMMAND would treat a freshly-issued RESET_MODEM as already done.
    PCOM_OFF_PEND_FLAG   = 0x20, // 1 = a command is pending in shared SMEM
    PCOM_OFF_PEND_CMD    = 0x24, // the pending command id (shadow, not APP_COMMAND)
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

    void bind(uint8_t* base, size_t size) {
        smem = base; smem_size = size;
        // Idle marker so a fresh block is not mistaken for a pending command.
        if (in_range(PCOM_OFF_APP_STATUS)) wr(PCOM_OFF_APP_STATUS, PCOM_STATUS_UNSET);
        // No command pending in a fresh block. Shadow lives in shared SMEM.
        if (in_range(PCOM_OFF_PEND_FLAG)) wr(PCOM_OFF_PEND_FLAG, 0);
    }

    bool in_range(pc_u32 off) const { return smem && (off + 4) <= smem_size; }

    pc_u32 rd(pc_u32 off) const {
        pc_u32 v = 0; if (in_range(off)) memcpy(&v, smem + off, 4); return v;
    }
    void wr(pc_u32 off, pc_u32 v) {
        if (in_range(off)) memcpy(smem + off, &v, 4);
    }

    // Core 0 issues a command. This makes the command VISIBLE in shared SMEM and
    // marks it pending via the shared shadow. It does NOT complete it and does
    // NOT touch APP_STATUS's completion meaning — the modem owns completion.
    void core0_issue(pc_u32 cmd, pc_u32 data1 = 0, pc_u32 data2 = 0) {
        wr(PCOM_OFF_APP_DATA1, data1);
        wr(PCOM_OFF_APP_DATA2, data2);
        wr(PCOM_OFF_APP_STATUS, PCOM_STATUS_UNSET); // no verdict yet
        wr(PCOM_OFF_APP_COMMAND, cmd);              // command id visible in SMEM
        // Shadow pending state in the SHARED backing so Core1's view agrees.
        // This is the ONLY authority for "pending" — decoupled from APP_COMMAND
        // so RESET_MODEM (0x1) is never confused with DONE (0x1).
        wr(PCOM_OFF_PEND_CMD, cmd);
        wr(PCOM_OFF_PEND_FLAG, 1);
    }

    // Is a command visible in SMEM and awaiting the modem? Derived from the
    // shared PEND_FLAG shadow, NOT from APP_COMMAND — so a pending RESET_MODEM
    // (id 0x1) is not mistaken for a completed handshake (DONE == 0x1).
    bool command_pending() const {
        return rd(PCOM_OFF_PEND_FLAG) == 1;
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
        // Read the command id from the shadow, not APP_COMMAND, so the identity
        // survives the DONE overwrite regardless of numeric collisions.
        pc_u32 cmd = rd(PCOM_OFF_PEND_CMD);
        pc_u32 status = is_supported(cmd) ? PCOM_CMD_SUCCESS
                                          : PCOM_CMD_FAIL_UNSUPPORTED;
        // Modem is the sole writer of the completion words.
        wr(PCOM_OFF_APP_STATUS, status);
        wr(PCOM_OFF_APP_COMMAND, PCOM_CMD_DONE);
        // Clear the shared pending shadow: the handshake is complete.
        wr(PCOM_OFF_PEND_FLAG, 0);
        return true;
    }

    // Convenience for the Core 0 side of the wait: has the modem completed?
    // A completion is DONE in APP_COMMAND with no command still pending.
    bool completed() const {
        return rd(PCOM_OFF_APP_COMMAND) == PCOM_CMD_DONE
               && rd(PCOM_OFF_PEND_FLAG) == 0;
    }
    pc_u32 status() const { return rd(PCOM_OFF_APP_STATUS); }
};

} // namespace zeebo
