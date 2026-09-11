// zeebo_brew_ishell.h — minimal faithful IShell interface/relocation model.
//
// PROVENANCE (measured, not assumed — see test_zeetris_ishell.cpp):
//   AEEMod_Load (entry 0x12000048, ARM) preserves r0..r2 (push {r0,r1,r2,r4,lr}),
//   reloads the ORIGINAL r0 at 0x12000180 (`ldr r0,[sp]`) and `bx r12` at
//   0x120001c8 into the module's own Thumb routine at 0x123c1bd0. There, at
//   0x123c1bdc, it executes Thumb `ldr r1,[r0]` — fetching the IShell VTABLE
//   POINTER from *pIShell. With r0=NULL (no context argument) this is a read
//   from VA 0: the lifecycle blocker. The fault is therefore NOT a missing
//   import/GOT relocation; it is the MISSING CONTEXT ARGUMENT r0 = pIShell.
//
//   With a valid IShell object (word[0] -> vtable), the routine advances past
//   0x123c1bdc and dispatches its FIRST real interface call at 0x123c1be4
//   (`blx <slot>`) through IShell vtable SLOT 2 (offset 0x8) with
//   r1 = ClassID 0x0106e415, r2 = ppObj — i.e. ISHELL_CreateInstance.
//
// This header builds that context: a guest IShell object whose word[0] points
// at a vtable of unique sentinels, so a code-hook can identify which interface
// slot the module invokes. It is an OBSERVATION instrument (clean-room): it
// hands the module a valid `this` pointer and records the interface contract
// the module requires. It does NOT fabricate method results beyond returning to
// the caller, and it never forces boot success or patches guest pointers blindly.
#pragma once
#include <cstdint>
#include <unicorn/unicorn.h>

namespace zeebo::brew::ishell {

using u32 = uint32_t;

// Guest layout for the minimal IShell environment. Addresses are chosen in a
// free region well away from the module (0x12000000) and stack (0x001F0000).
struct IShellEnv {
    u32 obj_va      = 0x40000000; // IShell object; word[0] = vtable pointer
    u32 vtable_va   = 0x40001000; // interface method table (contiguous fn ptrs)
    u32 sentinel_va = 0x50000000; // base of per-slot sentinel targets
    u32 region_base = 0x40000000; // mapped [region_base, region_base+region_size)
    u32 region_size = 0x00010000;
    u32 sent_base   = 0x50000000;
    u32 sent_size   = 0x00010000;
    u32 num_slots   = 64;
};

// Map the IShell object + vtable + sentinel region into `uc` and wire word[0].
// Each vtable slot i is set to sentinel_va + i*4, so a `blx [vtable+i*4]` lands
// on a unique, identifiable PC that a code-hook can map back to slot i.
// Returns the object VA to pass as r0 (pIShell). 0 on failure.
inline u32 install(uc_engine* uc, const IShellEnv& env = IShellEnv{}) {
    if (!uc) return 0;
    if (uc_mem_map(uc, env.region_base, env.region_size, UC_PROT_ALL) != UC_ERR_OK)
        return 0;
    if (uc_mem_map(uc, env.sent_base, env.sent_size, UC_PROT_ALL) != UC_ERR_OK)
        return 0;
    u32 vt = env.vtable_va;
    if (uc_mem_write(uc, env.obj_va, &vt, 4) != UC_ERR_OK) return 0;
    for (u32 i = 0; i < env.num_slots; ++i) {
        u32 s = env.sentinel_va + i * 4u;
        if (uc_mem_write(uc, env.vtable_va + i * 4u, &s, 4) != UC_ERR_OK) return 0;
    }
    return env.obj_va;
}

// Map a sentinel PC back to a vtable slot index; returns -1 if not a sentinel.
inline int slot_of(const IShellEnv& env, u32 pc) {
    if (pc < env.sentinel_va || pc >= env.sentinel_va + env.num_slots * 4u)
        return -1;
    return static_cast<int>((pc - env.sentinel_va) / 4u);
}

// The interface contract the module requires at this lifecycle stage, MEASURED.
// slot index within the IShell vtable (offset = slot*4).
enum : int {
    ISHELL_SLOT_CREATEINSTANCE = 2,   // offset 0x08; (this, ClassID r1, ppObj r2)
};

} // namespace zeebo::brew::ishell
