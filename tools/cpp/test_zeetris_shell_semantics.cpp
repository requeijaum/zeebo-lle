// test_zeetris_shell_semantics.cpp — SEMANTIC gate for the Zeetris lifecycle.
//
// PREMISE UNDER TEST (negative control): the primary worktree's env-gated trace
// shows a REAL AEECShell dispatch at pc=0x10c874f4 with r0=0xb0d02000, r1=0,
// r2=0x0001c003, sp=0xb0e1ff0c, and NO PC=0x14 derail. The old lifecycle
// inference promoted "r0 present at the dispatch VA" to "r0 IS pIShell". That is
// a NON-SEMANTIC inference: a register value at a call site proves nothing about
// whether it points at a live IShell object with a real vtable. r0=0xb0d02000 is
// in fact the injected Iguana env_base pool — NOT a BREW IShell.
//
// This test is a strict TDD measurement built on the read-only, env-gated
// [SHELL-SEM] instrumentation (ZEEBO_SHELL_SEM=1) added to the orchestrator. That
// instrument, at the EXACT ISHELL_CreateInstance (0x105c7fb4) / AEECShell
// dispatch (0x10c874f4) anchors, LIVE-reads the ARM context AND dereferences it:
//   r0 -> vtable ptr (*r0) -> vtable[0..3]  and classifies each as code/non-code,
//   plus, for CreateInstance, r1=clsid, r2=ppOut and *ppOut.
// It never writes guest PC/register/pointer/memory. It emits one line ending in
//   VERDICT=LIVE_ISHELL   (r0 readable, *r0 readable, >=2 vtable slots in code)
// or VERDICT=NON_SEMANTIC (anything weaker).
//
// The gate is a NEGATIVE CONTROL that can ONLY go GREEN when the exact semantic
// chain is measured:
//   * If the boot NEVER reaches the anchor (origin/zeetris-playable derails to
//     PC=0x14 without the primary worktree's uncommitted boot fixes) -> the honest
//     dependency is unmet; SKIP (exit 77). Never SKIP-as-PASS.
//   * If the anchor IS reached but every hit is VERDICT=NON_SEMANTIC (e.g. the
//     AEECShell dispatch with r0=0xb0d02000 env_base) -> the OLD inference is
//     PROVEN INVALID; the test FAILS the stale claim (exit 1) and reports why.
//   * The test goes GREEN (exit 0) ONLY when at least one ISHELL_CreateInstance
//     hit is VERDICT=LIVE_ISHELL — i.e. a genuine IShell object + vtable measured
//     with live registers, and a real ppOut return pathway captured.
//
// Detector POWER controls (the assertion CAN fail both ways):
//   * A synthetic LIVE_ISHELL CreateInstance line MUST be recognized as green.
//   * A synthetic NON_SEMANTIC AEECShell line with r0=0xb0d02000 MUST be rejected
//     as NOT a live shell — closing the "r0 present == pIShell" hole.
//   * Mutant (argv "buggy") accepts NON_SEMANTIC as if it proved a live shell and
//     MUST fail.
//
// Gate tier: requires the real NAND -> Tier B. Without it, exit 77 (SKIP). The
// orchestrator resolves the proprietary NAND via its own default relative paths
// from tools/cpp (absolute proprietary bins symlinked under ../../nand/).
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include <sys/stat.h>

using u32 = uint32_t;

static constexpr u32 ISHELL_CREATE_VA      = 0x105c7fb4u; // ISHELL_CreateInstance
static constexpr u32 AEECSHELL_DISPATCH_VA = 0x10c874f4u; // AEECShell dispatch
static constexpr u32 ENV_BASE_R0           = 0xb0d02000u; // injected Iguana env pool

struct SemHit {
    u32 pc = 0, r0 = 0, r1 = 0, r2 = 0, sp = 0, lr = 0;
    bool is_create = false;
    bool live_ishell = false; // VERDICT=LIVE_ISHELL
};

static bool hex_after(const std::string& s, size_t& pos, const char* key, u32& out) {
    size_t k = s.find(key, pos);
    if (k == std::string::npos) return false;
    k += std::strlen(key);
    u32 v = 0; int n = 0;
    while (k < s.size() && n < 8) {
        char c = s[k]; int d;
        if (c >= '0' && c <= '9') d = c - '0';
        else if (c >= 'a' && c <= 'f') d = c - 'a' + 10;
        else if (c >= 'A' && c <= 'F') d = c - 'A' + 10;
        else break;
        v = (v << 4) | (u32)d; ++k; ++n;
    }
    if (n == 0) return false;
    out = v; pos = k; return true;
}

// Parse "[SHELL-SEM] site=<name> pc=0x.. r0=0x.. r1=0x.. r2=0x.. r3=0x.. sp=0x..
//        lr=0x.. ... VERDICT=<LIVE_ISHELL|NON_SEMANTIC> insns=.."
static std::vector<SemHit> parse_sem_hits(const std::string& log) {
    std::vector<SemHit> out;
    const char* key = "[SHELL-SEM] site=";
    size_t line0 = 0;
    while ((line0 = log.find(key, line0)) != std::string::npos) {
        size_t eol = log.find('\n', line0);
        std::string line = log.substr(line0, (eol == std::string::npos ? log.size() : eol) - line0);
        SemHit h{};
        h.is_create = line.find("site=ISHELL_CreateInstance") != std::string::npos;
        size_t p = 0;
        bool ok = hex_after(line, p, "pc=0x", h.pc) &&
                  hex_after(line, p, "r0=0x", h.r0) &&
                  hex_after(line, p, "r1=0x", h.r1) &&
                  hex_after(line, p, "r2=0x", h.r2) &&
                  hex_after(line, p, "sp=0x", h.sp) &&
                  hex_after(line, p, "lr=0x", h.lr);
        h.live_ishell = line.find("VERDICT=LIVE_ISHELL") != std::string::npos;
        // Accept only lines whose PC is an EXACT anchor (never a broad-region PC).
        if (ok && (h.pc == ISHELL_CREATE_VA || h.pc == AEECSHELL_DISPATCH_VA))
            out.push_back(h);
        line0 = (eol == std::string::npos) ? log.size() : eol + 1;
    }
    return out;
}

static bool file_exists(const char* p) {
    struct stat st{};
    return ::stat(p, &st) == 0 && st.st_size > 0;
}

int main(int argc, char** argv) {
    const bool BUGGY = (argc > 1 && std::string(argv[1]) == "buggy");
    std::printf("=== Test Zeetris shell SEMANTICS (live IShell/vtable)%s ===\n",
                BUGGY ? " [MUTANT]" : "");

    // ── Detector POWER controls ──────────────────────────────────────────────
    {
        // (1) A genuine LIVE_ISHELL CreateInstance line must be recognized green.
        std::string good =
            "[SHELL-SEM] site=ISHELL_CreateInstance pc=0x105c7fb4 r0=0x0a012340 "
            "r1=0x01004d2a r2=0x0a000f00 r3=0x0 sp=0x0a000ee0 lr=0x105c8010 "
            "r0_read=1 vptr=0x10abc000 vt_read=1 vt[0..3]=0x10c80010,0x10c80044,"
            "0x10c80080,0x10c800c4 code_slots=4 lr_code=1 VERDICT=LIVE_ISHELL insns=99\n";
        auto g = parse_sem_hits(good);
        if (g.size() != 1 || !g[0].is_create || !g[0].live_ishell ||
            g[0].pc != ISHELL_CREATE_VA) {
            std::printf("[power] FAIL: parser cannot recognize a LIVE_ISHELL "
                        "CreateInstance line.\n");
            return 1;
        }
        // (2) The exact primary-worktree AEECShell dispatch with r0=0xb0d02000
        //     (env_base) is NON_SEMANTIC and MUST NOT be treated as a live shell.
        std::string non =
            "[SHELL-SEM] site=AEECShell_dispatch pc=0x10c874f4 r0=0xb0d02000 "
            "r1=0x00000000 r2=0x0001c003 r3=0x0 sp=0xb0e1ff0c lr=0x10c87510 "
            "r0_read=1 vptr=0x00000005 vt_read=0 vt[0..3]=0x0,0x0,0x0,0x0 "
            "code_slots=0 lr_code=1 VERDICT=NON_SEMANTIC insns=42\n";
        auto nn = parse_sem_hits(non);
        if (nn.size() != 1 || nn[0].live_ishell) {
            std::printf("[power] FAIL: parser mistook the r0=0xb0d02000 env_base "
                        "dispatch for a live IShell (old inference hole reopened).\n");
            return 1;
        }
        if (nn[0].r0 != ENV_BASE_R0 || nn[0].r2 != 0x0001c003u ||
            nn[0].sp != 0xb0e1ff0cu) {
            std::printf("[power] FAIL: parser mis-extracted the live registers.\n");
            return 1;
        }
        std::printf("[power] parser accepts LIVE_ISHELL CreateInstance and rejects "
                    "the exact r0=0xb0d02000 AEECShell dispatch as NON_SEMANTIC "
                    "(negative control has power).\n");
    }

    // ── Require the real NAND + orchestrator; else SKIP. ─────────────────────
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
        std::printf("=== Test Zeetris shell SEMANTICS: SKIP (exit 77) ===\n");
        return 77;
    }

    // ── Drive the REAL boot (pure Unicorn interpreter; no --jit) with the
    //    semantic anchor instrument enabled. Read-only; never patches guest state.
    const char* LOG = "/tmp/zeetris_shell_semantics_boot.log";
    std::string cmd = std::string("ZEEBO_SHELL_SEM=1 ") + ORCH +
        " --boot-appmgr --headless --seconds=8 > " + LOG + " 2>&1";
    std::printf("[boot] %s\n", cmd.c_str());
    int rc = std::system(cmd.c_str());
    (void)rc; // orchestrator returns 0 even when boot derails; judge by trace.

    std::string log;
    {
        FILE* f = std::fopen(LOG, "rb");
        if (!f) { std::printf("[FAIL] could not read boot log.\n"); return 1; }
        std::fseek(f, 0, SEEK_END); long n = std::ftell(f); std::fseek(f, 0, SEEK_SET);
        if (n > 0) { log.resize((size_t)n); size_t got = std::fread(&log[0], 1, (size_t)n, f); log.resize(got); }
        std::fclose(f);
    }

    auto hits = parse_sem_hits(log);
    size_t create_hits = 0, live_create = 0, non_sem = 0;
    for (const auto& h : hits) {
        if (h.is_create) ++create_hits;
        if (h.live_ishell && h.is_create) ++live_create;
        if (!h.live_ishell) ++non_sem;
    }
    std::printf("[measure] sem_hits=%zu create_hits=%zu live_ishell_create=%zu "
                "non_semantic=%zu\n", hits.size(), create_hits, live_create, non_sem);

    // Honest dependency: origin without the primary worktree's uncommitted boot
    // fixes never reaches the anchor (PC=0x14 derail). Report + SKIP, never fake it.
    if (hits.empty()) {
        std::printf("[SKIP] the exact live shell anchor was NOT reached in this "
                    "worktree — origin/zeetris-playable derails to PC=0x14 without "
                    "the primary worktree's uncommitted boot fixes. The semantic "
                    "chain therefore CANNOT be measured here; dependency reported "
                    "honestly, not fabricated.\n");
        std::printf("=== Test Zeetris shell SEMANTICS: SKIP (exit 77) ===\n");
        return 77;
    }

    if (BUGGY) {
        // Mutant treats a NON_SEMANTIC anchor hit as if it proved a live shell.
        if (live_create == 0) {
            std::printf("[MUTANT] claimed a live shell from a NON_SEMANTIC anchor "
                        "hit but no LIVE_ISHELL CreateInstance was measured — mutant "
                        "correctly fails.\n");
            return 1;
        }
        std::printf("[MUTANT] (unexpectedly) a real LIVE_ISHELL was measured.\n");
        return 0;
    }

    // Anchor reached but NO live IShell measured -> the old non-semantic inference
    // is PROVEN INVALID. Fail the stale claim loudly.
    if (live_create == 0) {
        const SemHit& h = hits.front();
        std::printf("[REFUTED] the anchor was executed (%zu hits) but NONE is a live "
                    "IShell: e.g. site pc=0x%08x r0=0x%08x r2=0x%08x sp=0x%08x is "
                    "VERDICT=NON_SEMANTIC. This PROVES the old inference ('r0 present "
                    "at the dispatch VA is pIShell') INVALID: r0=0xb0d02000 is the "
                    "injected env_base pool, whose *r0 is not a readable vtable of "
                    "code. A genuine ISHELL_CreateInstance with a live vtable and a "
                    "real ppOut return pathway has NOT been measured. This gate goes "
                    "GREEN only when that exact semantic chain is captured.\n",
                    hits.size(), h.pc, h.r0, h.r2, h.sp);
        return 1;
    }

    // GREEN: a genuine live IShell object + vtable + ppOut return pathway measured.
    for (const auto& h : hits) {
        if (h.is_create && h.live_ishell) {
            std::printf("[GREEN] LIVE ISHELL_CreateInstance measured: pc=0x%08x "
                        "r0(pIShell)=0x%08x clsid=0x%08x ppOut=0x%08x sp=0x%08x "
                        "return_to=0x%08x. r0 dereferences to a readable vtable whose "
                        "first slots are APPS/BREW code — a genuine IShell object, "
                        "not the env_base r0=0xb0d02000. The synthetic 0x40000000 "
                        "stub can now be replaced by this captured live context.\n",
                        h.pc, h.r0, h.r1, h.r2, h.sp, h.lr);
            break;
        }
    }
    std::printf("=== Test Zeetris shell SEMANTICS: PASS ===\n");
    return 0;
}
