// test_zeetris_live_shell.cpp — Reachability witness for two historical symbol
// labels, now explicitly refuted as BREW shell entry points.
//
// The trace is still useful: it proves the real AppMgr boot reaches exactly
// 0x10c874f4 and records its live registers. The provenance and semantic gates
// establish that this site is an APPS/AMSS bootstrap env-installer, r0 points to
// env_base (0xb0d02000), and 0x105c7fb4 is rodata. Therefore an anchor hit is
// boot-progress evidence only; it must never create a pIShell, mark a module
// loaded, or bind applet services.
//
// The detector has positive and negative controls and requires the real NAND.
// Missing firmware exits 77. The "buggy" mode preserves the mutation property:
// it asserts the old reachability blocker (that neither exact address executes)
// and must fail once the bootstrap anchor is observed.
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include <sys/stat.h>

using u32 = uint32_t;

// The two EXACT APPS/BREW user-space anchors where a real AEECShell/IShell is
// constructed or dispatched (from zeebo_brew_loader.h::BrewSymbols).
static constexpr u32 ISHELL_CREATE_VA     = 0x105c7fb4u;  // ISHELL_CreateInstance
static constexpr u32 AEECSHELL_DISPATCH_VA = 0x10c874f4u; // AEECShell dispatch
// The measured derail signature of the current live boot.
static constexpr u32 PC14 = 0x00000014u;

// One captured anchor hit: the live ARM context at an exact shell anchor.
struct AnchorHit {
    u32 pc, r0, r1, r2, sp;
};

// Parse every orchestrator anchor-trace line of the form:
//   "[SHELL-ANCHOR] Core0 HIT <name> pc=0x........ r0=0x........ r1=0x........
//    r2=0x........ sp=0x........ insns=..."
// A hit is only counted when pc is EXACTLY one of the two shell anchors — a PC
// merely inside the broad APPS window does NOT produce this line and is never
// counted. This is the evidence upgrade over the broad-region heuristic.
static bool parse_hex_after(const std::string& s, size_t& pos, const char* key, u32& out) {
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

static std::vector<AnchorHit> parse_anchor_hits(const std::string& log) {
    std::vector<AnchorHit> out;
    const char* key = "[SHELL-ANCHOR]";
    size_t line0 = 0;
    while ((line0 = log.find(key, line0)) != std::string::npos) {
        size_t eol = log.find('\n', line0);
        std::string line = log.substr(line0, (eol == std::string::npos ? log.size() : eol) - line0);
        AnchorHit h{};
        size_t p = 0;
        bool ok =
            parse_hex_after(line, p, "pc=0x", h.pc) &&
            parse_hex_after(line, p, "r0=0x", h.r0) &&
            parse_hex_after(line, p, "r1=0x", h.r1) &&
            parse_hex_after(line, p, "r2=0x", h.r2) &&
            parse_hex_after(line, p, "sp=0x", h.sp);
        // Only accept the line if the PC is an EXACT anchor — never a broad-region PC.
        if (ok && (h.pc == ISHELL_CREATE_VA || h.pc == AEECSHELL_DISPATCH_VA))
            out.push_back(h);
        line0 = (eol == std::string::npos) ? log.size() : eol + 1;
    }
    return out;
}

// Extract every per-cycle "Core0(ARM11): pc=0x........" value (context: confirms
// the boot still derails to PC=0x14, i.e. behavior unchanged).
static std::vector<u32> parse_core0_pcs(const std::string& log) {
    std::vector<u32> out;
    const char* key = "Core0(ARM11): pc=0x";
    size_t pos = 0;
    while ((pos = log.find(key, pos)) != std::string::npos) {
        pos += std::strlen(key);
        u32 v = 0; int n = 0;
        while (pos < log.size() && n < 8) {
            char c = log[pos]; int d;
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
    std::printf("=== Test Zeetris LIVE shell anchor reachability%s ===\n",
                BUGGY ? " [MUTANT]" : "");

    // ── Detector POWER control: the parser MUST recognize an exact anchor line
    //    AND reject a broad-window (non-anchor) PC. If it cannot match, the RED
    //    witness is vacuous; if it over-matches a broad PC, it repeats the old
    //    heuristic hole. ─────────────────────────────────────────────────────
    {
        std::string good =
            "[SHELL-ANCHOR] Core0 HIT ISHELL_CreateInstance pc=0x105c7fb4 "
            "r0=0x40000000 r1=0x0106e415 r2=0x7ffdd000 sp=0x0a000f00 insns=12345\n";
        auto hits = parse_anchor_hits(good);
        if (hits.size() != 1 || hits[0].pc != ISHELL_CREATE_VA ||
            hits[0].r0 != 0x40000000u || hits[0].r1 != 0x0106e415u ||
            hits[0].r2 != 0x7ffdd000u || hits[0].sp != 0x0a000f00u) {
            std::printf("[power] FAIL: parser cannot recognize an exact anchor line "
                        "with its live context -> witness would be vacuous.\n");
            return 1;
        }
        // A broad-window PC (inside old [0x1013a000,0x14000000)) that is NOT one
        // of the two exact anchors MUST NOT be counted — closes the over-count.
        std::string broad =
            "[SHELL-ANCHOR] Core0 HIT ISHELL_CreateInstance pc=0x1013a100 "
            "r0=0x1 r1=0x2 r2=0x3 sp=0x4 insns=1\n";
        if (!parse_anchor_hits(broad).empty()) {
            std::printf("[power] FAIL: parser counted a broad-region PC as an exact "
                        "anchor (over-count hole reopened).\n");
            return 1;
        }
        std::printf("[power] parser accepts exact anchor 0x105c7fb4 (with live "
                    "r0/r1/r2/sp) and rejects broad-region PC 0x1013a100 "
                    "(detector has power, no over-count).\n");
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
        std::printf("=== Test Zeetris LIVE shell anchor: SKIP (exit 77) ===\n");
        return 77;
    }

    // ── Drive the REAL boot (pure Unicorn interpreter; no --jit) to AppMgr with
    //    the per-instruction anchor trace enabled (ZEEBO_SHELL_TRACE=1). The
    //    trace is read-only: it fabricates no PC, pointer, or guest state; it
    //    only emits the live ARM context AT the exact shell anchor if reached.
    //    Absolute proprietary paths resolved by the orchestrator from tools/cpp.
    const char* LOG = "/tmp/zeetris_live_shell_boot.log";
    std::string cmd = std::string("ZEEBO_SHELL_TRACE=1 ") + ORCH +
        " --boot-appmgr --headless --seconds=8 > " + LOG + " 2>&1";
    std::printf("[boot] %s\n", cmd.c_str());
    int rc = std::system(cmd.c_str());
    (void)rc; // orchestrator returns 0 even when the boot derails; we judge by trace.

    // Read the combined stdout+stderr log.
    std::string log;
    {
        FILE* f = std::fopen(LOG, "rb");
        if (!f) { std::printf("[FAIL] could not read boot log.\n"); return 1; }
        std::fseek(f, 0, SEEK_END); long n = std::ftell(f); std::fseek(f, 0, SEEK_SET);
        if (n > 0) { log.resize((size_t)n); size_t got = std::fread(&log[0], 1, (size_t)n, f); log.resize(got); }
        std::fclose(f);
    }

    // Measure: did Core0 EXECUTE either exact shell anchor? And is the PC=0x14
    // derail still present (boot behavior unchanged)?
    auto hits = parse_anchor_hits(log);
    auto pcs  = parse_core0_pcs(log);
    if (pcs.empty()) {
        std::printf("[FAIL] no Core0 PC samples parsed — boot did not run as expected.\n");
        return 1;
    }
    size_t pc14_hits = 0;
    for (u32 p : pcs) if (p == PC14) ++pc14_hits;

    std::printf("[measure] exact_anchor_hits=%zu  Core0 per-cycle samples=%zu  "
                "pc14_derail_hits=%zu\n",
                hits.size(), pcs.size(), pc14_hits);
    std::printf("[measure] first Core0 sample=0x%08x  last=0x%08x\n",
                pcs.front(), pcs.back());

    const bool live_reached_anchor = !hits.empty();

    if (BUGGY) {
        // O mutante agora representa a AFIRMAÇÃO ANTIGA deste gate: "o boot vivo
        // nunca executa o âncora do shell, logo não há pIShell vivo". Como a
        // medição mostra que ele É alcançado, essa afirmação tem de falhar.
        if (live_reached_anchor) {
            std::printf("[MUTANT] a afirmação antiga (bootstrap inalcançável) falha "
                        "como esperado: o boot vivo ALCANÇOU o instalador de env.\n");
            return 1;
        }
        std::printf("[MUTANT] (inesperadamente) o âncora não foi alcançado.\n");
        return 0;
    }

    // Measured truth: the real boot reaches the disproven bootstrap anchor.
    // This is a boot-progress regression witness, not shell-object evidence.
    if (!live_reached_anchor) {
        std::printf("[FAIL] o boot vivo NÃO alcançou o âncora do shell (0x%08x nem "
                    "0x%08x); o boot REGREDIU em relação à medição anterior.\n",
                    ISHELL_CREATE_VA, AEECSHELL_DISPATCH_VA);
        return 1;
    }

    const AnchorHit& h = hits.front();
    std::printf("=== Test Zeetris bootstrap anchor reachability: PASS ===\n");
    std::printf("  MEDIDO: o boot AppMgr real executa o instalador de env APPS/AMSS "
                "pc=0x%08x r0=0x%08x r1=0x%08x r2=0x%08x sp=0x%08x. "
                "r0 é env_base, NÃO pIShell.\n",
                h.pc, h.r0, h.r1, h.r2, h.sp);
    std::printf("  CONTRATO: estes VAs permanecem somente como âncoras diagnósticas; "
                "não podem marcar módulo loaded nem disparar binding BREW/IGL. "
                "A construção de um IShell vivo continua não provada. "
                "(pc14_derail_hits=%zu — informativo.)\n", pc14_hits);
    return 0;
}
