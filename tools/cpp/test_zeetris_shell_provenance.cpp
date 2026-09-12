// test_zeetris_shell_provenance.cpp — CALL-PROVENANCE gate for the Zeetris blocker.
//
// MEASURED ANCHOR (primary worktree, ZEEBO_SHELL_SEM=1):
//   [SHELL-SEM] AEECShell_dispatch pc=0x10c874f4 r0=0xb0d02000 r1=0 r2=r3=0x0001c003
//               sp=0xb0e1ff0c lr=0x10137598 ; vptr=5 code_slots=0 -> NON_SEMANTIC.
//
// The symbol table calls 0x10c874f4 "AEECShell_dispatch" and 0x105c7fb4
// "ISHELL_CreateInstance". This test proves, STATICALLY from the real APPS.bin
// ELF (Tier B), that BOTH labels are MISIDENTIFIED and that 0x10c874f4 is an
// APPS/AMSS bootstrap env-installer, NOT a BREW IShell dispatch. Read-only:
// it only reads bytes out of the firmware image; it never boots, patches, or
// mutates guest state.
//
// WHAT A REAL BREW IShell DISPATCH LOOKS LIKE (positive discriminator):
//   the caller loads the vtable FROM the object and calls INDIRECTLY through a
//   vtable slot:  ldr rX,[r0]      (vptr = *pIShell)
//                 ldr pc,[rX,#imm] / blx rX
//   i.e. an INDIRECT, register-based call whose target comes from the object.
//
// WHAT WE ACTUALLY MEASURE AT THE ANCHOR (negative discriminator = truth):
//   * 0x105c7fb4 is not code at all — it is the rodata C-string
//       "ISHELL_CreateInstance failed: %d"  (the loader grabbed a string-table
//       hit, so ishell_create_va is bogus).
//   * 0x10c874f4 is THUMB and begins  push {r4,lr}  (0xb510) — a leaf routine
//       that does  str r0,[r1]  (0x6008, storing the env block into a global),
//       bl 0x10c87320, then  movs r0,#4 ; pop {r4,pc}  — it RETURNS A CONSTANT 4.
//   * Its only caller (0x10137590) is:
//       ldr r0,[sp]            ; load the boot env pointer off the stack
//       blx 0x10c874f4         ; DIRECT absolute call (encoding 0xfa2d3fd6)
//       add sp,sp,r0           ; consume the returned size (r0=4) as a stack
//                               ; adjustment  -> runtime/bootstrap descriptor idiom
//     No vtable is ever loaded; the call is a direct BL to an absolute VA, and
//     r0=0xb0d02000 is the injected Iguana env/bootinfo page (APPS.bin seg
//     b0d00000-b0d02000), NOT a heap IShell object.
//
// VERDICT taxonomy per candidate site:
//   VTABLE_DISPATCH  — caller loads vptr from the object and calls indirectly
//                      through a vtable slot (a genuine BREW shell dispatch).
//   AMSS_BOOTSTRAP   — direct absolute call whose return value adjusts sp, callee
//                      is a leaf env-installer returning a constant (bootstrap).
//
// The gate is RED: it goes GREEN (exit 0) ONLY if the measured anchor is a real
// VTABLE_DISPATCH. Today it is AMSS_BOOTSTRAP, so the stale "0x10c874f4 is the
// AEECShell dispatch" claim FAILS (exit 1). Without the real NAND -> SKIP (77).
//
// Detector POWER controls (the classifier CAN fail both ways):
//   (+) a synthetic vtable-indirect caller MUST classify VTABLE_DISPATCH.
//   (-) the exact bootstrap encodings MUST classify AMSS_BOOTSTRAP.
//   mutant (argv "buggy") accepts AMSS_BOOTSTRAP as a shell and MUST fail.
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include <sys/stat.h>

using u32 = uint32_t;
using u16 = uint16_t;
using u8  = uint8_t;

static constexpr u32 ANCHOR_VA   = 0x10c874f4u; // labelled "AEECShell_dispatch"
static constexpr u32 CALLER_VA   = 0x10137590u; // start of the 3-insn call site
static constexpr u32 LR_VA       = 0x10137598u; // measured return address
static constexpr u32 ISHELL_VA   = 0x105c7fb4u; // labelled "ISHELL_CreateInstance"
static constexpr u32 ENV_BASE_R0 = 0xb0d02000u; // injected Iguana env pool

enum Verdict { V_UNKNOWN = 0, V_VTABLE_DISPATCH, V_AMSS_BOOTSTRAP };

static bool file_exists(const char* p) {
    struct stat st{}; return ::stat(p, &st) == 0 && st.st_size > 0;
}
static u32 rd32(const u8* d, size_t o) {
    return (u32)d[o] | ((u32)d[o+1]<<8) | ((u32)d[o+2]<<16) | ((u32)d[o+3]<<24);
}
static u16 rd16(const u8* d, size_t o) { return (u16)(d[o] | (d[o+1]<<8)); }

// Resolve an APPS/AMSS VA to a file offset via PT_LOAD headers, exactly like the
// orchestrator's loader (VA match preferred, then PA match), restricted to the
// APPS code region so a bootinfo/PA alias never shadows the real code segment.
static long va_to_off(const std::vector<u8>& d, u32 va) {
    if (d.size() < 64 || d[0]!=0x7f || d[1]!='E') return -1;
    u32 phoff = rd32(d.data(),28);
    u16 phent = rd16(d.data(),42), phnum = rd16(d.data(),44);
    // pass 1: VA match in code region
    for (int pass=0; pass<2; ++pass)
    for (int i=0;i<phnum;i++){
        size_t o=phoff+(size_t)i*phent; if (o+32>d.size()) break;
        if (rd32(d.data(),o)!=1) continue; // PT_LOAD
        u32 off=rd32(d.data(),o+4), pva=rd32(d.data(),o+8),
            ppa=rd32(d.data(),o+12), fsz=rd32(d.data(),o+16);
        u32 base = pass==0 ? pva : ppa;
        if (base < 0x10000000u) continue;
        if (va>=base && va<base+fsz) return (long)off + (long)(va-base);
    }
    return -1;
}

// Classify a candidate dispatch by reading the caller's call-site encodings and
// the callee prologue/epilogue out of the raw firmware bytes. No disassembler:
// we match the exact ARM/THUMB encodings that discriminate the two idioms.
static Verdict classify_site(const std::vector<u8>& d, u32 caller_va, u32 callee_va,
                             std::string& why) {
    long co = va_to_off(d, caller_va);
    long fo = va_to_off(d, callee_va);
    if (co < 0 || fo < 0) { why = "site not backed by APPS code segment"; return V_UNKNOWN; }

    // --- Caller idiom (ARM words at caller_va..caller_va+8) ---
    u32 w0 = rd32(d.data(), co);      // caller_va+0
    u32 w1 = rd32(d.data(), co+4);    // caller_va+4  (the call)
    u32 w2 = rd32(d.data(), co+8);    // caller_va+8  (post-call consumer)

    // Direct absolute BL/BLX (ARM): top byte 0xfa/0xfb => BLX_imm, 0xeb => BL.
    bool direct_call = ((w1>>24)&0xfe)==0xfa || ((w1>>24)&0xff)==0xeb;
    // A vtable dispatch instead calls INDIRECTLY through a register/slot:
    //   blx rX  (0x012fff3x) | bx  | ldr pc,[rX,..] (0xe59ff.. is pc<-[pc], not it;
    //   real form is 0xe590f00x / ldr pc,[rN,#imm]).
    bool indirect_reg_call =
        (w1 & 0x0ffffff0u) == 0x012fff30u ||           // blx rX
        (w1 & 0x0f70f000u) == 0x0510f000u;             // ldr pc,[rN,#imm]
    // A vtable dispatch first LOADS the vptr from the object: ldr rX,[r0{,#imm}].
    bool loads_vptr_from_obj = (w0 & 0x0ff00000u) == 0x05900000u &&  // ldr rX,[rN,#imm]
                               ((w0>>16)&0xf) == 0;                  // base Rn == r0
    // The bootstrap idiom consumes the RETURN value as a stack adjust: add sp,sp,rX.
    bool ret_adjusts_sp = (w2 & 0x0ffff000u) == 0x008dd000u;        // add sp,sp,rX

    // --- Callee idiom (THUMB halfwords at callee_va) ---
    u16 h0 = rd16(d.data(), fo);      // prologue
    bool thumb_leaf_push = (h0 & 0xff00u) == 0xb500u;               // push {..,lr}
    // Scan a small prologue window for: str r0,[rN] (0x600x env store) and a
    // constant-return epilogue movs r0,#imm (0x20xx) + pop {..,pc} (0xbdxx).
    bool stores_env = false, const_ret = false, pops_pc = false;
    for (int i=0;i<24;i+=2) {
        u16 h = rd16(d.data(), fo+i);
        if ((h & 0xffc0u) == 0x6000u) stores_env = true;            // str r0,[rN,#0]
        if ((h & 0xff00u) == 0x2000u) const_ret  = true;            // movs r0,#imm
        if ((h & 0xff00u) == 0xbd00u) pops_pc     = true;           // pop {..,pc}
    }

    bool is_bootstrap = direct_call && ret_adjusts_sp && thumb_leaf_push &&
                        stores_env && const_ret && pops_pc;
    bool is_vtable = indirect_reg_call && loads_vptr_from_obj;

    char buf[512];
    std::snprintf(buf, sizeof buf,
        "caller[%08x]=%08x %08x %08x  callee_h0=%04x | direct_call=%d "
        "ret_adjusts_sp=%d thumb_leaf=%d stores_env=%d const_ret=%d pops_pc=%d | "
        "indirect_reg=%d loads_vptr=%d",
        caller_va, w0, w1, w2, h0, direct_call, ret_adjusts_sp, thumb_leaf_push,
        stores_env, const_ret, pops_pc, indirect_reg_call, loads_vptr_from_obj);
    why = buf;

    if (is_vtable && !is_bootstrap) return V_VTABLE_DISPATCH;
    if (is_bootstrap) return V_AMSS_BOOTSTRAP;
    return V_UNKNOWN;
}

// Is the VA the start of a rodata C string (not code)? Used to prove the bogus
// ISHELL_CreateInstance label.
static bool is_cstring(const std::vector<u8>& d, u32 va, const char* expect) {
    long o = va_to_off(d, va);
    if (o < 0) return false;
    size_t n = std::strlen(expect);
    if ((size_t)o + n > d.size()) return false;
    return std::memcmp(d.data()+o, expect, n) == 0;
}

static const char* vname(Verdict v){
    return v==V_VTABLE_DISPATCH?"VTABLE_DISPATCH":v==V_AMSS_BOOTSTRAP?"AMSS_BOOTSTRAP":"UNKNOWN";
}

int main(int argc, char** argv) {
    const bool BUGGY = (argc>1 && std::string(argv[1])=="buggy");
    std::printf("=== Test Zeetris shell CALL-PROVENANCE (AMSS bootstrap vs BREW dispatch)%s ===\n",
                BUGGY?" [MUTANT]":"");

    // ── Detector POWER controls: classifier must separate the two idioms. ──
    {
        std::vector<u8> synth(0x40, 0);
        // Fake a minimal ELF header so va_to_off maps VA==off 1:1 for the controls.
        synth.assign(0x1000, 0);
        synth[0]=0x7f; synth[1]='E'; synth[2]='L'; synth[3]='F';
        auto put32=[&](size_t o,u32 v){ synth[o]=v; synth[o+1]=v>>8; synth[o+2]=v>>16; synth[o+3]=v>>24; };
        auto put16=[&](size_t o,u16 v){ synth[o]=v&0xff; synth[o+1]=v>>8; };
        put32(28,64); put16(42,32); put16(44,1);          // phoff=64,phent=32,phnum=1
        put32(64,1); put32(64+4,0); put32(64+8,0x10000000);// PT_LOAD off=0 va=0x10000000
        put32(64+12,0x10000000); put32(64+16,(u32)synth.size());
        auto W=[&](u32 va,u32 w){ size_t o=va-0x10000000; put32(o,w); };
        auto H=[&](u32 va,u16 h){ size_t o=va-0x10000000; put16(o,h); };

        // (+) POSITIVE control: a real vtable-indirect dispatch.
        u32 pc_caller=0x10000100, pc_callee=0x10000200;
        W(pc_caller,   0xe5901000);      // ldr r1,[r0]      (load vptr from object)
        W(pc_caller+4, 0xe12fff31);      // blx r1           (indirect through slot)
        W(pc_caller+8, 0xe1a00000);      // nop-ish (mov r0,r0)
        H(pc_callee,   0xb510);          // callee prologue (irrelevant for vtable)
        std::string why;
        Verdict vp = classify_site(synth, pc_caller, pc_callee, why);
        if (vp != V_VTABLE_DISPATCH) {
            std::printf("[power] FAIL: positive control not classified VTABLE_DISPATCH (got %s). %s\n",
                        vname(vp), why.c_str());
            return 1;
        }

        // (-) NEGATIVE control: the exact bootstrap encodings.
        u32 nb_caller=0x10000300, nb_callee=0x10000400;
        W(nb_caller,   0xe59d0000);      // ldr r0,[sp]
        W(nb_caller+4, 0xfa000001);      // blx <imm>        (direct absolute)
        W(nb_caller+8, 0xe08dd000);      // add sp,sp,r0     (return adjusts sp)
        H(nb_callee,   0xb510);          // push {r4,lr}
        H(nb_callee+2, 0x6008);          // str r0,[r1]      (store env to global)
        H(nb_callee+4, 0x2004);          // movs r0,#4       (constant return)
        H(nb_callee+6, 0xbd10);          // pop {r4,pc}
        Verdict vn = classify_site(synth, nb_caller, nb_callee, why);
        if (vn != V_AMSS_BOOTSTRAP) {
            std::printf("[power] FAIL: negative control not classified AMSS_BOOTSTRAP (got %s). %s\n",
                        vname(vn), why.c_str());
            return 1;
        }
        std::printf("[power] classifier separates VTABLE_DISPATCH from AMSS_BOOTSTRAP "
                    "(both controls have power).\n");
    }

    // ── Require the real APPS.bin (Tier B); else SKIP. ─────────────────────
    const char* APPS = "../../nand/1.1.2_APPS.bin";
    if (!file_exists(APPS)) {
        std::printf("[SKIP] real APPS.bin ausente (%s) — provenance nao mensuravel.\n", APPS);
        std::printf("=== Test Zeetris shell CALL-PROVENANCE: SKIP (exit 77) ===\n");
        return 77;
    }
    std::vector<u8> d;
    { FILE* f=std::fopen(APPS,"rb"); std::fseek(f,0,SEEK_END); long n=std::ftell(f);
      std::fseek(f,0,SEEK_SET); d.resize((size_t)n); size_t g=std::fread(d.data(),1,(size_t)n,f);
      d.resize(g); std::fclose(f); }

    // Fact 1: the "ISHELL_CreateInstance" symbol VA is a rodata STRING, not code.
    bool ishell_is_string = is_cstring(d, ISHELL_VA, "ISHELL_CreateInstance failed");
    std::printf("[measure] 0x%08x labelled ISHELL_CreateInstance -> is C-string \"%s\"? %s\n",
                ISHELL_VA, "ISHELL_CreateInstance failed: %d",
                ishell_is_string ? "YES (bogus symbol = rodata hit)" : "no");

    // Fact 2: classify the measured anchor's call provenance from real bytes.
    std::string why;
    Verdict v = classify_site(d, CALLER_VA, ANCHOR_VA, why);
    std::printf("[measure] anchor 0x%08x via caller 0x%08x (LR 0x%08x, r0=env 0x%08x):\n"
                "          %s\n          VERDICT=%s\n",
                ANCHOR_VA, CALLER_VA, LR_VA, ENV_BASE_R0, why.c_str(), vname(v));

    if (BUGGY) {
        // Mutant asserts the anchor IS a live BREW dispatch.
        if (v != V_VTABLE_DISPATCH) {
            std::printf("[MUTANT] claimed AEECShell/BREW dispatch but provenance is %s "
                        "— mutant correctly fails.\n", vname(v));
            return 1;
        }
        return 0;
    }

    if (v == V_VTABLE_DISPATCH) {
        std::printf("[GREEN] the anchor is a genuine vtable-indirect BREW shell dispatch.\n");
        std::printf("=== Test Zeetris shell CALL-PROVENANCE: PASS ===\n");
        return 0;
    }

    // RED: the stale "AEECShell_dispatch" label is proven wrong.
    std::printf("[REFUTED] 0x%08x is NOT a BREW IShell dispatch. Provenance = %s: the caller\n"
                "       loads the boot env off the stack (ldr r0,[sp]), makes a DIRECT\n"
                "       absolute call, and consumes the returned constant as a stack\n"
                "       adjustment (add sp,sp,r0). The THUMB callee is a leaf that stores\n"
                "       the env block into a global and returns a constant — an APPS/AMSS\n"
                "       bootstrap env-installer, running before any BREW shell exists.\n"
                "       r0=0x%08x is the injected Iguana env/bootinfo page, not an IShell.\n"
                "       The 'ISHELL_CreateInstance' symbol (0x%08x) is a rodata string.\n"
                "       No faithful direct shell-construction target is proven here; the\n"
                "       symbol table's aeecshell/ishell VAs are MISIDENTIFIED.\n",
                ANCHOR_VA, vname(v), ENV_BASE_R0, ISHELL_VA);
    return 1;
}
