// test_zeetris_ishell.cpp — Zeetris lifecycle: prove VA-0 blocker is the missing
// IShell context argument, then implement the minimal interface model that
// advances execution past it. TDD, evidence-first, clean-room, no Dynarmic.
//
// Structure:
//   (A) RED reproducer: with r0=NULL (no context argument), AEEMod_Load faults
//       reading VA 0 at PC 0x123c1bdc — the measured prior failure.
//   (B) POSITIVE execution: with the minimal IShell env installed (valid `this`
//       whose word[0] -> vtable), execution ADVANCES past 0x123c1bdc and reaches
//       the module's FIRST interface dispatch: IShell vtable slot 2
//       (ISHELL_CreateInstance) carrying ClassID 0x0106e415.
//   (C) Negative control: an IShell object whose word[0]=0 (NULL vtable) faults
//       reading the vtable slot (VA 0x8) — proves the fix requires a REAL vtable,
//       not merely a non-null `this`. A mutant (argv "buggy") that pretends the
//       null-vtable case dispatches cleanly must go RED here.
//
// Gate: without the real zeetris.mod -> exit 77 (SKIP), never fabricated.
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>
#include <unicorn/unicorn.h>
#include "zeebo_brew_loader.h"
#include "zeebo_brew_ishell.h"

using u8  = uint8_t;
using u32 = uint32_t;
using zeebo::brew::BrewLoader;
namespace ish = zeebo::brew::ishell;

// Measured identity (from the real bytes; see zeebo_zeetris_lifecycle.h / trace).
static constexpr u32 ENTRY_VA        = 0x12000048u; // AEEMod_Load (RAW_BRANCH target)
static constexpr u32 LOAD_VA         = 0x12000000u;
static constexpr u32 STK             = 0x00200000u;
static constexpr u32 MOD_SIZE        = 3939404u;
static constexpr u32 CLSID           = 0x12345678u; // injection tag (MIF applet id)
static constexpr u32 NULL_DEREF_PC   = 0x123c1bdcu; // Thumb `ldr r1,[r0]` reading *pIShell
static constexpr u32 CREATE_CLSID    = 0x0106e415u; // ClassID the module asks IShell to create

static std::vector<u8> read_file(const std::string& p) {
    std::ifstream f(p, std::ios::binary | std::ios::ate);
    if (!f) return {};
    std::streamoff n = f.tellg();
    if (n <= 0) return {};
    std::vector<u8> d(static_cast<size_t>(n));
    f.seekg(0);
    f.read(reinterpret_cast<char*>(d.data()), n);
    if (static_cast<std::streamoff>(f.gcount()) != n) return {};
    return d;
}

// Shared observation state for a run.
struct RunObs {
    ish::IShellEnv env{};
    // fault
    bool  faulted    = false;
    u32   fault_va   = 0;
    u32   fault_pc   = 0;
    // interface dispatch
    bool  dispatched = false;   // module invoked an IShell vtable slot
    int   slot       = -1;
    u32   slot_r1    = 0;       // ClassID argument at dispatch
    u32   slot_r2    = 0;       // ppObj argument at dispatch
    bool  reached_null_deref = false;
    bool  passed_null_deref  = false; // executed at least one PC AFTER 0x123c1bde
    bool  service_slots = true; // if false: don't service dispatch (null-vtable ctrl)
};

static void code_hook(uc_engine* uc, uint64_t address, uint32_t, void* user) {
    auto* o = static_cast<RunObs*>(user);
    u32 a = static_cast<u32>(address);
    if (a == NULL_DEREF_PC) o->reached_null_deref = true;
    if (a > NULL_DEREF_PC && a < 0x123c1c40u) o->passed_null_deref = true;
    int s = ish::slot_of(o->env, a);
    if (s >= 0 && !o->dispatched) {
        o->dispatched = true;
        o->slot = s;
        uc_reg_read(uc, UC_ARM_REG_R1, &o->slot_r1);
        uc_reg_read(uc, UC_ARM_REG_R2, &o->slot_r2);
        // Service the call minimally: return 0 to caller (bx lr) so we observe
        // the dispatch without fabricating a real object.
        u32 lr = 0, zero = 0;
        uc_reg_read(uc, UC_ARM_REG_LR, &lr);
        uc_reg_write(uc, UC_ARM_REG_R0, &zero);
        uc_reg_write(uc, UC_ARM_REG_PC, &lr);
        uc_emu_stop(uc); // stop at first dispatch: contract observed
    }
}

static bool mem_hook(uc_engine* uc, uc_mem_type, uint64_t address, int, int64_t, void* user) {
    auto* o = static_cast<RunObs*>(user);
    o->faulted  = true;
    o->fault_va = static_cast<u32>(address);
    uc_reg_read(uc, UC_ARM_REG_PC, &o->fault_pc);
    uc_emu_stop(uc);
    return false;
}

// Run AEEMod_Load with a given r0 (pIShell). `service` controls whether vtable
// dispatch is intercepted+serviced. Returns observations.
static RunObs run(const std::vector<u8>& mod, u32 r0_ishell, bool install_env,
                  bool service_slots) {
    RunObs o; o.service_slots = service_slots;
    uc_engine* uc = nullptr;
    assert(uc_open(UC_ARCH_ARM, UC_MODE_ARM, &uc) == UC_ERR_OK);
    uc_mem_map(uc, 0x001F0000, 0x10000, UC_PROT_ALL); // stack
    if (install_env) {
        u32 obj = ish::install(uc, o.env);
        assert(obj == o.env.obj_va);
    }
    BrewLoader ld(uc);
    assert(ld.inject_bytes(mod, LOAD_VA, CLSID, "zeetris.mod"));
    uc_hook hc = 0, hm = 0;
    uc_hook_add(uc, &hc, UC_HOOK_CODE, (void*)code_hook, &o, 1, 0);
    uc_hook_add(uc, &hm, UC_HOOK_MEM_UNMAPPED, (void*)mem_hook, &o, 1, 0);
    u32 z = 0, sp = STK, lr = 0xF0F0F0F0u;
    uc_reg_write(uc, UC_ARM_REG_R0, &r0_ishell);
    uc_reg_write(uc, UC_ARM_REG_R1, &z);
    uc_reg_write(uc, UC_ARM_REG_R2, &z);
    uc_reg_write(uc, UC_ARM_REG_R3, &z);
    uc_reg_write(uc, UC_ARM_REG_SP, &sp);
    uc_reg_write(uc, UC_ARM_REG_LR, &lr);
    uc_emu_start(uc, ENTRY_VA, 0xF0F0F0F0u, 0, 300000);
    uc_hook_del(uc, hc);
    uc_hook_del(uc, hm);
    uc_close(uc);
    return o;
}

int main(int argc, char** argv) {
    const bool BUGGY = (argc > 1 && std::string(argv[1]) == "buggy");
    std::printf("=== Test Zeetris IShell lifecycle%s ===\n",
                BUGGY ? " [MUTANT]" : "");

    // Instrument self-check: IShellEnv slot mapping is exact and honest.
    {
        ish::IShellEnv e{};
        assert(ish::slot_of(e, e.sentinel_va) == 0);
        assert(ish::slot_of(e, e.sentinel_va + 8) == 2);
        assert(ish::slot_of(e, e.sentinel_va - 4) == -1);
        assert(ish::slot_of(e, e.obj_va) == -1);
        assert(ish::ISHELL_SLOT_CREATEINSTANCE == 2);
    }

    const char* menv = std::getenv("ZEETRIS_MOD");
    const std::string mod_path =
        menv ? menv : "/home/rafaelfrequiao/Downloads/mod/zeetris/zeetris.mod";
    std::vector<u8> mod = read_file(mod_path);
    if (mod.empty()) {
        std::printf("[SKIP] ROM real ausente (mod='%s').\n", mod_path.c_str());
        std::printf("=== Test Zeetris IShell: SKIP (exit 77) ===\n");
        return 77;
    }
    assert(mod.size() == MOD_SIZE);

    // ── (A) RED reproducer: NULL context argument → VA-0 read at 0x123c1bdc. ──
    {
        RunObs o = run(mod, /*r0=*/0, /*install_env=*/false, /*service=*/false);
        std::printf("[A/red] r0=NULL -> faulted=%s fault_va=0x%08x fault_pc=0x%08x "
                    "reached_null_deref=%s\n",
                    o.faulted ? "SIM" : "não", o.fault_va, o.fault_pc,
                    o.reached_null_deref ? "SIM" : "não");
        assert(o.reached_null_deref);
        assert(o.faulted);
        assert(o.fault_va == 0);
        assert(o.fault_pc == NULL_DEREF_PC);   // exact instruction proven
        assert(!o.dispatched);                 // never reached an interface call
        assert(!o.passed_null_deref);          // blocked exactly at the deref
    }

    // ── (B) POSITIVE: valid IShell context advances past the blocker and the
    //        module dispatches its first interface call = ISHELL_CreateInstance. ─
    {
        ish::IShellEnv e{};
        RunObs o = run(mod, /*r0=*/e.obj_va, /*install_env=*/true, /*service=*/true);
        std::printf("[B/exec] r0=IShell -> passed_null_deref=%s dispatched=%s "
                    "slot=%d r1(clsid)=0x%08x r2(ppObj)=0x%08x faulted=%s\n",
                    o.passed_null_deref ? "SIM" : "não",
                    o.dispatched ? "SIM" : "não", o.slot, o.slot_r1, o.slot_r2,
                    o.faulted ? "SIM" : "não");
        assert(o.reached_null_deref);          // same code path...
        assert(o.passed_null_deref);           // ...but now survives the deref
        assert(o.dispatched);                  // reached a real interface call
        assert(o.slot == ish::ISHELL_SLOT_CREATEINSTANCE); // slot 2
        assert(o.slot_r1 == CREATE_CLSID);     // ClassID the module wants created
        assert(!o.faulted);                    // no VA-0 fault before dispatch
    }

    // ── (C) Negative control: IShell object with word[0]=0 (NULL vtable). The
    //        module gets past the `this` deref but faults fetching the vtable
    //        slot (reading *(vtable+8)=*(0+8)=VA 8). Proves the fix needs a REAL
    //        vtable, not just a non-null pointer. ──────────────────────────────
    {
        // Install env but zero out word[0] so the vtable pointer is NULL.
        uc_engine* uc = nullptr;
        assert(uc_open(UC_ARCH_ARM, UC_MODE_ARM, &uc) == UC_ERR_OK);
        uc_mem_map(uc, 0x001F0000, 0x10000, UC_PROT_ALL);
        ish::IShellEnv e{};
        u32 obj = ish::install(uc, e);
        assert(obj == e.obj_va);
        u32 zero = 0;
        uc_mem_write(uc, e.obj_va, &zero, 4); // NULL vtable pointer
        BrewLoader ld(uc);
        assert(ld.inject_bytes(mod, LOAD_VA, CLSID, "zeetris.mod"));
        RunObs o; o.env = e;
        uc_hook hc = 0, hm = 0;
        uc_hook_add(uc, &hc, UC_HOOK_CODE, (void*)code_hook, &o, 1, 0);
        uc_hook_add(uc, &hm, UC_HOOK_MEM_UNMAPPED, (void*)mem_hook, &o, 1, 0);
        u32 z = 0, sp = STK, lr = 0xF0F0F0F0u, r0 = e.obj_va;
        uc_reg_write(uc, UC_ARM_REG_R0, &r0);
        uc_reg_write(uc, UC_ARM_REG_R1, &z);
        uc_reg_write(uc, UC_ARM_REG_R2, &z);
        uc_reg_write(uc, UC_ARM_REG_SP, &sp);
        uc_reg_write(uc, UC_ARM_REG_LR, &lr);
        uc_emu_start(uc, ENTRY_VA, 0xF0F0F0F0u, 0, 300000);
        uc_hook_del(uc, hc);
        uc_hook_del(uc, hm);
        uc_close(uc);
        const bool null_vtable_clean = (o.dispatched && !o.faulted);
        std::printf("[C/ctrl] word0=NULL -> passed_null_deref=%s dispatched=%s "
                    "faulted=%s fault_va=0x%08x\n",
                    o.passed_null_deref ? "SIM" : "não",
                    o.dispatched ? "SIM" : "não", o.faulted ? "SIM" : "não",
                    o.fault_va);
        if (BUGGY) {
            assert(null_vtable_clean && "MUTANT: null vtable should dispatch cleanly");
        } else {
            assert(o.passed_null_deref);  // got past the `this` deref
            assert(!o.dispatched);        // but NO clean interface dispatch
            assert(o.faulted);            // faulted reading the vtable slot
            assert(o.fault_va == 8);      // *(vtable=0 + 8) for slot 2
        }
    }

    std::printf("=== Test Zeetris IShell lifecycle: PASS "
                "(VA-0 = missing IShell context; interface model advances) ===\n");
    return 0;
}
