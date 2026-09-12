// qdsp5_doorbell_probe.h — opt-in diagnostic RPC liveness probe on the A2M
// doorbell. See QDSP5_TODO.md Phase Q1.5:
//   "Replace the dummy 0x42/0x1b59 injection in c0_mem_hook ...; keep the dummy
//    behind a --probe-rpc flag for liveness testing."
//
// PRODUCTION-BUG FIX: previously zeebo_lle_main.cpp injected fabricated 0x42
// ONCRPC packets with proc 0x1b59 on EVERY doorbell, unconditionally. But per
// QDSP5_TODO.md §3.1/§3.2/§11.1 the value 0x1b59 is the RPC *success status
// code* (session 2zz literal pool), NOT a procedure ID — and the payload is a
// fabricated dummy carrying no real command semantics. Firing it on every
// doorbell corrupts the live AMSS ONCRPC queue on production doorbells.
//
// This gate makes the injection an explicit, OFF-by-default diagnostic probe.
#pragma once

#include <cstdlib>
#include <vector>
#include <unicorn/unicorn.h>
#include "zeebo_smd_bridge_unified.h"

namespace zeebo_qdsp5 {

// True only when the operator explicitly opts in via ZEEBO_QDSP5_RPC_PROBE.
// Defaults FALSE so production doorbells never mutate the ONCRPC queue.
inline bool rpc_probe_enabled() {
    const char* e = std::getenv("ZEEBO_QDSP5_RPC_PROBE");
    // Safety-critical opt-in: only the exact documented value enables the
    // fabricated diagnostic packets. Values such as "off" must stay false.
    return e && e[0] == '1' && e[1] == '\0';
}

// The diagnostic liveness probe. NO-OP unless the probe is explicitly enabled.
// When enabled it injects the (still-fabricated, clearly-flagged) 0x42 dummy
// packets to prove the SMD wakeup path — this is a probe, not real audio.
inline void doorbell_maybe_inject(UnifiedSMDBridge& smd, uc_engine* uc) {
    if (!rpc_probe_enabled()) return;   // production default: do nothing
    // Diagnostic ONLY. 0x1b59 is the RPC success STATUS code (not a proc) and
    // the 0x42 payload is fabricated; gated so it can never touch a real queue
    // unless an operator opts in for liveness testing.
    std::vector<u8> dummy_payload(16, 0x42);
    smd.inject_packet(uc, 0x30000013, 0x1b59, dummy_payload);
    smd.inject_packet(uc, 0x3000000a, 0x02, dummy_payload);
}

} // namespace zeebo_qdsp5
