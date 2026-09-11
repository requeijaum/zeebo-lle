// test_zeetris_live_shell.cpp — Can a REAL, live IShell context for the Zeetris
// lifecycle be derived from a running AppMgr boot? This test answers that with
// MEASUREMENT, not fabrication.
//
// The prior lifecycle work (test_zeetris_ishell.cpp) hands AEEMod_Load a
// SYNTHETIC IShell object at 0x40000000 whose word[0] points to a sentinel
// vtable. That advances the module past the VA-0 deref at 0x123c1bdc, but the
// `this` pointer and its vtable are FABRICATED by the harness — not a shell the
// firmware actually stood up. The honest question for a real lifecycle bridge
// is: does the emulator's own booted APPS/BREW ever construct a genuine
// AEECShell/IShell that we could pass as pIShell?
//
// This test drives the REAL orchestrator (pure Unicorn interpreter, no Dynarmic)
// booting the REAL proprietary NAND to AppMgr, and records the Core0 (ARM11
// APPS) PC trajectory. It asserts, as a RED blocker-witness, that Core0 NEVER
// reaches the BREW/APPS user-space where a shell would be created:
//   - ISHELL_CreateInstance @ 0x105c7fb4  (BrewSymbols::ishell_create_va)
//   - AEECShell dispatch     @ 0x10c874f4  (BrewSymbols::aeecshell_dispatch_va)
//   - APPS segment-11 code base 0x1013a000 (BREW app/asset region)
// Instead Core0 derails to PC=0x14 (UC_ERR_INSN_INVALID) and never leaves the
// early bring-up / 0xb000_xxxx trampoline region. Therefore a live pIShell
// context CANNOT yet be accessed: the upstream boot blockers (Core1 REX
// scheduling, Core0 PC=0x14 derail) gate it. This test makes that a measured,
// falsifiable fact instead of an assumption — and provides the negative control
// the project requires: it is NOT a "boot succeeded" gate; it is a "boot has
// NOT yet reached the shell" gate that would go GREEN→RED the moment the real
// boot advances, at which point the synthetic stub must be replaced by the live
// shell.
//
// Detector POWER control (the assertion CAN fail): the same parser proves it can
// positively recognize a VA in the target set by feeding it a synthetic line
// carrying 0x105c7fb4 — if the parser could never match, the RED witness would
// be vacuous. Mutant (argv "buggy") claims the live boot DOES reach the shell
// and must fail here.
//
// Gate tier: requires the real NAND -> Tier B. Without it, exit 77 (SKIP),
// never SKIP-as-PASS. Absolute proprietary paths; the orchestrator resolves the
// NAND via its own default/relative paths from tools/cpp.
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include <sys/stat.h>

using u32 = uint32_t;

// The three APPS/BREW user-space anchors where a real AEECShell/IShell would be
// constructed or dispatched (from zeebo_brew_loader.h::BrewSymbols + FINDINGS).
static constexpr u32 SHELL_ANCHORS[] = {
    0x105c7fb4u,  // ISHELL_CreateInstance
    0x10c874f4u,  // AEECShell dispatch vector
    0x1013a000u,  // APPS segment-11 BREW code base (VA base)
};
// The measured derail signature of the current live boot.
static constexpr u32 PC14 = 0x00000014u;

// Does a Core0 PC fall inside the BREW/APPS user-space shell region? We treat
// the segment-11 base 0x1013a000 as the start of a broad APPS code window; any
// PC at/above it (and below the b0xxxxxx AEE trampoline band) counts as
// "reached user-space where a shell lives". The two exact dispatch VAs are
// matched precisely.
static bool is_shell_region(u32 pc) {
    for (u32 a : SHELL_ANCHORS)
        if (pc == a) return true;
    // Broad APPS user code window: [0x1013a000, 0x14000000).
    if (pc >= 0x1013a000u && pc < 0x14000000u) return true;
    return false;
}

// Extract every "Core0(ARM11): pc=0x........" value from a log buffer.
static std::vector<u32> parse_core0_pcs(const std::string& log) {
    std::vector<u32> out;
    const char* key = "Core0(ARM11): pc=0x";
    size_t pos = 0;
    while ((pos = log.find(key, pos)) != std::string::npos) {
        pos += std::strlen(key);
        u32 v = 0; int n = 0;
        while (pos < log.size() && n < 8) {
            char c = log[pos];
            int d;
            if (c >= '0' && c <= '9') d = c - '0';
            else if (c >= 'a' && c <= 'f') d = c - 'a' + 10;
            else if (c >= 'A' && c <= 'F') d = c - 'A' + 10;
            else break;
            v = (v << 4) | (u32)d; ++pos; ++n;
        }
        if (n > 0) out.push_back(v);
    }
    return out;
}

static bool file_exists(const char* p) {
    struct stat st{};
    return ::stat(p, &st) == 0 && st.st_size > 0;
}

int main(int argc, char** argv) {
    const bool BUGGY = (argc > 1 && std::string(argv[1]) == "buggy");
    std::printf("=== Test Zeetris LIVE shell reachability%s ===\n",
                BUGGY ? " [MUTANT]" : "");

    // ── Detector POWER control: the parser + region test MUST be able to match a
    //    shell anchor. If it cannot, the RED witness below proves nothing. ──────
    {
        std::string probe = "  [Cycle 42] Core0(ARM11): pc=0x105c7fb4 insns=1 (ok)\n";
        auto pcs = parse_core0_pcs(probe);
        if (pcs.size() != 1 || pcs[0] != 0x105c7fb4u || !is_shell_region(pcs[0])) {
            std::printf("[power] FAIL: parser/region cannot recognize a shell VA "
                        "-> witness would be vacuous.\n");
            return 1;
        }
        // And a non-shell early-bringup PC must NOT be counted as shell.
        if (is_shell_region(PC14) || is_shell_region(0xb000afe8u)) {
            std::printf("[power] FAIL: region test false-positives on non-shell PC.\n");
            return 1;
        }
        std::printf("[power] parser recognizes shell VA 0x105c7fb4 and rejects "
                    "PC=0x14/0xb000afe8 (detector has power).\n");
    }

    // ── Require the real NAND to drive a real boot; else SKIP. ────────────────
    const char* NAND = "../../nand/1.1.2.bin";
    const char* APPS = "../../nand/1.1.2_APPS.bin";
    const char* AMSS = "../../nand/1.1.2_AMSS.bin";
    const char* ORCH = "./zeebo_lle_main";
    if (!file_exists(NAND) || !file_exists(APPS) || !file_exists(AMSS) ||
        !file_exists(ORCH)) {
        std::printf("[SKIP] NAND real ou orquestrador ausente "
                    "(nand=%d apps=%d amss=%d orch=%d).\n",
                    file_exists(NAND), file_exists(APPS), file_exists(AMSS),
                    file_exists(ORCH));
        std::printf("=== Test Zeetris LIVE shell: SKIP (exit 77) ===\n");
        return 77;
    }

    // ── Drive the REAL boot (pure Unicorn interpreter; no --jit) to AppMgr and
    //    capture Core0's PC trajectory. Absolute proprietary paths resolved by
    //    the orchestrator's own defaults from tools/cpp. ───────────────────────
    const char* LOG = "/tmp/zeetris_live_shell_boot.log";
    std::string cmd = std::string(ORCH) +
        " --boot-appmgr --headless --seconds=8 > " + LOG + " 2>&1";
    std::printf("[boot] %s\n", cmd.c_str());
    int rc = std::system(cmd.c_str());
    (void)rc; // orchestrator returns 0 even when the boot derails; we judge by PCs.

    // Read the log.
    std::string log;
    {
        FILE* f = std::fopen(LOG, "rb");
        if (!f) { std::printf("[FAIL] could not read boot log.\n"); return 1; }
        std::fseek(f, 0, SEEK_END); long n = std::ftell(f); std::fseek(f, 0, SEEK_SET);
        if (n > 0) { log.resize((size_t)n); size_t got = std::fread(&log[0], 1, (size_t)n, f); log.resize(got); }
        std::fclose(f);
    }

    auto pcs = parse_core0_pcs(log);
    if (pcs.empty()) {
        std::printf("[FAIL] no Core0 PC samples parsed — boot did not run as expected.\n");
        return 1;
    }

    // Measure: did Core0 EVER reach the shell region? Count PC=0x14 derail.
    size_t shell_hits = 0, pc14_hits = 0, first_shell_idx = pcs.size();
    u32 max_pc_below_b0 = 0; // highest APPS-region PC seen (context)
    for (size_t i = 0; i < pcs.size(); ++i) {
        u32 p = pcs[i];
        if (p == PC14) ++pc14_hits;
        if (is_shell_region(p)) {
            if (first_shell_idx == pcs.size()) first_shell_idx = i;
            ++shell_hits;
        }
        if (p >= 0x10000000u && p < 0x1013a000u && p > max_pc_below_b0)
            max_pc_below_b0 = p;
    }

    std::printf("[measure] Core0 PC samples=%zu  shell_region_hits=%zu  "
                "pc14_derail_hits=%zu  max_apps_pc_below_shell=0x%08x\n",
                pcs.size(), shell_hits, pc14_hits, max_pc_below_b0);
    std::printf("[measure] first Core0 sample=0x%08x  last=0x%08x\n",
                pcs.front(), pcs.back());

    const bool live_reached_shell = (shell_hits > 0);

    if (BUGGY) {
        // Mutant claims the live boot reaches the shell (so a live pIShell exists).
        if (!live_reached_shell) {
            std::printf("[MUTANT] expected live boot to reach shell region but it "
                        "did NOT — mutant correctly fails.\n");
            return 1;
        }
        std::printf("[MUTANT] (unexpectedly) saw shell region.\n");
        return 0;
    }

    // RED blocker-witness: the live boot must NOT reach the shell region, and it
    // must show the measured PC=0x14 derail — proving the live pIShell context
    // is inaccessible until the upstream boot blockers are cleared.
    if (live_reached_shell) {
        std::printf("[GREEN-UNEXPECTED] Core0 REACHED the shell region "
                    "(first at sample %zu). A LIVE pIShell context is now "
                    "derivable — replace the synthetic 0x40000000 stub with the "
                    "real shell and update this gate.\n", first_shell_idx);
        // This is NOT a failure of the emulator — it is progress. But the test's
        // stated invariant (blocker present) no longer holds, so it must be
        // revisited. Fail loudly so nobody ships a stale claim.
        return 2;
    }

    if (pc14_hits == 0) {
        std::printf("[FAIL] did not observe the measured PC=0x14 derail; boot "
                    "behavior changed — re-triage before trusting this witness.\n");
        return 1;
    }

    std::printf("=== Test Zeetris LIVE shell: PASS ===\n");
    std::printf("  MEASURED: over a real Unicorn AppMgr boot, Core0 never reached "
                "ISHELL_CreateInstance/AEECShell/APPS-user-space; it derailed to "
                "PC=0x14 (%zu samples). A live pIShell context is NOT yet "
                "accessible, so the Zeetris lifecycle bridge cannot use a real "
                "booted shell — the synthetic IShell in test_zeetris_ishell.cpp "
                "is an OBSERVATION instrument, not a live-context bridge.\n",
                pc14_hits);
    return 0;
}
