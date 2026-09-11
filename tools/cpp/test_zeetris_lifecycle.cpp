// test_zeetris_lifecycle.cpp — Zeetris lifecycle probe (evidence-only, TDD).
//
// Deriva, a partir dos BYTES REAIS do módulo/MIF do Zeetris, DUAS coisas
// estruturais e nada além disso:
//
//   (1) O CLASS ID do applet, na POSIÇÃO ESTRUTURAL do registro de applet do
//       MIF (zeebo::brew::MifParser), não por varredura cega de 4 bytes.
//   (2) A ABI de chamada do ENTRY do módulo, DECODIFICADA do prólogo real:
//       resolve o entry via BrewLoader::resolve_mod_entry (o `.mod` do Zeetris
//       começa com `b AEEMod_Load` → ENTRY_RAW_BRANCH em 0x12000048) e decodifica
//       a 1ª instrução do entry (STMFD sp!/push) para saber QUAIS registradores
//       de argumento (r0..r3) o entry preserva — a evidência da aridade AAPCS.
//
// Depois EXECUTA o entry cru real sob Unicorn INTÉRPRETE, registra que o PC
// entrou na faixa do módulo, e PARA HONESTAMENTE na primeira dependência ausente,
// nomeando o VA e o tipo de falha. NÃO alega boot de jogo, NÃO usa handler fixo,
// NÃO fabrica sucesso.
//
// LIÇÃO DA v1 (inválida): a v1 chamou AEEMod_Load (0x12000048) COMO SE FOSSE um
// HandleEvent Thumb e recebeu UC_ERR_INSN_INVALID. Aqui a ABI do entry é DERIVADA
// dos bytes (ARM, não Thumb; args em r0..r2), não presumida. Um controle prova
// que tratar o entry como Thumb produz exatamente a falha da v1.
//
// Gate de ROM real: sem o zeetris.mod/.mif reais → exit 77 (SKIP), jamais fabrica.
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
#include "zeebo_brew_mif.h"
#include "zeebo_dd1a_diag.h"        // run_first_pc / FirstPcResult (instrumento genérico)
#include "zeebo_zeetris_lifecycle.h" // clean-room: ABI do entry + clsid estrutural

using namespace zeebo::zeetris;
using zeebo::brew::BrewLoader;

using u8  = uint8_t;
using u32 = uint32_t;

// Identidade estrutural MEDIDA do Zeetris (dos bytes reais; ver docs).
static constexpr u32 ZEETRIS_CLSID     = 0x12345678u; // sec5 do MIF (registro applet 20B)
static constexpr u32 ZEETRIS_ENTRY_VA  = 0x12000048u; // alvo do `b` inicial (RAW_BRANCH)
static constexpr u32 ZEETRIS_MOD_SIZE  = 3939404u;    // zeetris.mod (bytes)
static constexpr u32 ZEETRIS_PROLOGUE  = 0xe92d4017u; // push {r0,r1,r2,r4,lr}
static constexpr u32 FORBIDDEN_ZWHEEL_HANDLER = 0x10532344u; // jamais o entry real

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

static void put32(std::vector<u8>& b, size_t off, u32 v) {
    if (off + 4 > b.size()) b.resize(off + 4, 0);
    std::memcpy(b.data() + off, &v, 4);
}

// MIF estruturalmente válido cujo único registro de applet (20 bytes, f4==0 e
// fc==0) carrega `clsid` — a forma que zeebo::brew::MifParser reconhece. Espelha
// a estrutura observada do zeetris.mif real (magic 0x0011, bounds em 0x10/0x14).
static std::vector<u8> make_structural_mif(u32 clsid) {
    std::vector<u8> b(0x64, 0);
    uint16_t magic = 0x0011;
    std::memcpy(b.data() + 0x00, &magic, 2);
    put32(b, 0x10, 0x20);       // table_offset
    put32(b, 0x14, 2);          // section_count => 3 bounds
    put32(b, 0x20, 0x40);       // bound[0]
    put32(b, 0x24, 0x50);       // bound[1]: sec0=[0x40,0x50) size 16 (não applet)
    put32(b, 0x28, 0x64);       // bound[2]: sec1=[0x50,0x64) size 20 (applet)
    put32(b, 0x50, clsid);      // f0 = clsid
    put32(b, 0x54, 0);          // f4 = 0 (estrutural)
    put32(b, 0x58, 7);          // f8 arbitrário
    put32(b, 0x5c, 0);          // fc = 0 (estrutural)
    put32(b, 0x60, 1);          // f10 arbitrário
    return b;
}

int main(int argc, char** argv) {
    const bool BUGGY = (argc > 1 && std::string(argv[1]) == "buggy");
    std::printf("=== Test Zeetris lifecycle probe (evidence-only)%s ===\n",
                BUGGY ? " [MUTANT: thumb-entry ABI]" : "");

    // ── (1) DECODER DE ABI — fixtures sintéticos (instrumento pode falhar) ────
    {
        // push {r0,r1,r2,r4,lr}: preserva r0,r1,r2 → 3 slots de argumento.
        u32 mask = 0;
        assert(decode_arm_push(0xe92d4017u, &mask));
        assert(mask == 0x4017u);
        assert(count_arg_regs(mask) == 3);   // r0,r1,r2 contíguos a partir de r0
        assert(mask_has_lr(mask));           // bit 14 (lr) presente

        // push {r0,lr}: só 1 slot de argumento.
        assert(decode_arm_push(0xe92d4001u, &mask));
        assert(count_arg_regs(mask) == 1);

        // push {r4,lr} (0xe92d4010): 0 slots de argumento preservados.
        assert(decode_arm_push(0xe92d4010u, &mask));
        assert(count_arg_regs(mask) == 0);

        // Controle: r0 e r2 mas NÃO r1 → não é bloco de argumento contíguo,
        // conta só r0 (para de contar no primeiro ausente).
        assert(decode_arm_push(0xe92d4005u, &mask)); // r0,r2,lr
        assert(count_arg_regs(mask) == 1);

        // Controle negativo: NÃO é um push (é o `b` inicial do módulo).
        assert(!decode_arm_push(0xea000010u, nullptr));
        // Controle negativo: instrução aleatória.
        assert(!decode_arm_push(0xe3a00001u, nullptr)); // mov r0,#1
    }

    // ── (2) ABI derivada de um módulo SINTÉTICO com o mesmo formato do Zeetris:
    //        1ª palavra `b entry`, e o entry começa com push {r0,r1,r2,r4,lr}. ─
    {
        const u32 LB = 0x12000000;
        std::vector<u8> mod(0x100, 0);
        // 1ª palavra: b 0x12000048 (offset 0x40 words = 0x10) → mesmo alvo do real.
        // imm24 = (0x12000048 - (LB+8)) >> 2 = (0x48 - 8) >> 2 = 0x10.
        put32(mod, 0x00, 0xea000000u | 0x10);
        put32(mod, 0x48, 0xe92d4017u); // push {r0,r1,r2,r4,lr}
        put32(mod, 0x4c, 0xe12fff1e);  // bx lr

        EntryAbi abi = analyze_entry_abi(mod, LB);
        assert(abi.resolved);
        assert(abi.entry_va == LB + 0x48);
        assert(abi.entry_kind == zeebo::brew::ENTRY_RAW_BRANCH);
        assert(abi.is_push);
        assert(abi.arg_regs == 3);
        assert(abi.preserves_lr);
        assert(abi.mode_arm);

        // Controle: entry cujo prólogo NÃO é push → ABI de arg não derivada,
        // mas o entry continua resolvido honestamente.
        std::vector<u8> mod2 = mod;
        put32(mod2, 0x48, 0xe3a00001u); // mov r0,#1 (não é push)
        EntryAbi abi2 = analyze_entry_abi(mod2, LB);
        assert(abi2.resolved && !abi2.is_push && abi2.arg_regs == 0);
    }

    // ── (3) CLSID estrutural — fixtures sintéticos ────────────────────────────
    {
        MifClsid c = derive_mif_clsid(make_structural_mif(ZEETRIS_CLSID));
        assert(c.ok && c.clsid == ZEETRIS_CLSID);

        // Decoy: os 4 bytes do clsid aparecem FORA de um registro estrutural.
        std::vector<u8> decoy = make_structural_mif(0x0BADF00Du);
        decoy.resize(decoy.size() + 8, 0);
        put32(decoy, decoy.size() - 6, ZEETRIS_CLSID);
        MifClsid cd = derive_mif_clsid(decoy);
        assert(cd.ok && cd.clsid == 0x0BADF00Du); // estrutura decide, não a varredura
        assert(cd.clsid != ZEETRIS_CLSID);

        // Lixo: sem magic válido → nada derivado.
        std::vector<u8> junk(64, 0x55);
        assert(!derive_mif_clsid(junk).ok);
    }

    // ── (4) ROM REAL: deriva ABI+clsid dos bytes reais e executa o entry cru. ─
    const char* menv = std::getenv("ZEETRIS_MOD");
    const std::string mod_path =
        menv ? menv : "/home/rafaelfrequiao/Downloads/mod/zeetris/zeetris.mod";
    const char* fenv = std::getenv("ZEETRIS_MIF");
    const std::string mif_path =
        fenv ? fenv : "/home/rafaelfrequiao/Downloads/mif/zeetris.mif";

    std::vector<u8> mod = read_file(mod_path);
    std::vector<u8> mif = read_file(mif_path);
    if (mod.empty() || mif.empty()) {
        std::printf("[SKIP] ROM real ausente (mod='%s' mif='%s'); probe não executado.\n",
                    mod_path.c_str(), mif_path.c_str());
        std::printf("=== Test Zeetris lifecycle: SKIP (exit 77) — sem fabricação ===\n");
        return 77;
    }

    // (4a) CLSID estrutural REAL do MIF.
    MifClsid rc = derive_mif_clsid(mif);
    std::printf("[Zeetris/id] MIF clsid=0x%08x (%s, estrutural)\n",
                rc.clsid, rc.ok ? "ok" : "X");
    assert(rc.ok);
    assert(rc.clsid == ZEETRIS_CLSID);

    // (4b) ABI do entry DERIVADA dos bytes reais.
    assert(mod.size() == ZEETRIS_MOD_SIZE);
    EntryAbi abi = analyze_entry_abi(mod, 0x12000000);
    std::printf("[Zeetris/abi] first_insn=0x%08x @0x%08x -> entry=0x%08x kind=%s\n",
                abi.first_insn, abi.first_va, abi.entry_va,
                zeebo::brew::entry_kind_label(abi.entry_kind));
    std::printf("[Zeetris/abi] prologue=0x%08x is_push=%s push_mask=0x%04x "
                "arg_regs=r0..r%d(%d) preserves_lr=%s mode=%s\n",
                abi.prologue_insn, abi.is_push ? "SIM" : "nao", abi.push_mask,
                abi.arg_regs - 1, abi.arg_regs, abi.preserves_lr ? "SIM" : "nao",
                abi.mode_arm ? "ARM" : "Thumb");
    assert(abi.resolved);
    assert(abi.entry_va == ZEETRIS_ENTRY_VA);
    assert(abi.entry_kind == zeebo::brew::ENTRY_RAW_BRANCH);
    assert(abi.prologue_insn == ZEETRIS_PROLOGUE);
    assert(abi.is_push);
    assert(abi.arg_regs == 3);   // AEEMod_Load recebe 3 args (r0=pIShell, r1, r2=ppMod)
    assert(abi.preserves_lr);
    assert(abi.mode_arm);        // ARM, NÃO Thumb — refuta a premissa da v1
    // Provas de honestidade: o entry NÃO é o handler fixo da Z-Wheel.
    assert(abi.entry_va != FORBIDDEN_ZWHEEL_HANDLER);

    // (4c) Executa o entry cru REAL sob Unicorn intérprete, com a ABI derivada:
    //      ARM (não Thumb), args em r0..r2. Registra entrada no módulo e a 1ª
    //      dependência ausente — NUNCA alega boot.
    {
        uc_engine* uc = nullptr;
        assert(uc_open(UC_ARCH_ARM, UC_MODE_ARM, &uc) == UC_ERR_OK);
        const u32 LB = 0x12000000, STK = 0x00200000;
        uc_mem_map(uc, 0x001F0000, 0x10000, UC_PROT_ALL); // pilha com headroom
        BrewLoader ld(uc);
        assert(ld.inject_bytes(mod, LB, ZEETRIS_CLSID, "zeetris.mod"));
        const auto& m = ld.module();
        assert(m.entry_va == ZEETRIS_ENTRY_VA);
        assert(m.entry_kind == zeebo::brew::ENTRY_RAW_BRANCH);

        // Chamada com args zerados (r0..r2 = 0), MODO ARM: é a forma correta
        // derivada da ABI. Sem Thumb, sem PC forjado.
        zeebo::dd1a::FirstPcResult r =
            zeebo::dd1a::run_first_pc(uc, m.entry_va, LB, m.size, STK, 500000,
                                      false, 0, 0, 0, 0);
        std::printf("[Zeetris/exec] entrou_no_módulo=%s first_pc=0x%08x "
                    "last_pc=0x%08x instr=%llu\n",
                    r.entered_module ? "SIM" : "não", r.first_pc, r.last_pc,
                    (unsigned long long)r.instructions);
        std::printf("[Zeetris/exec] 1ª dependência ausente: %s @0x%08x (uc=%s)\n",
                    zeebo::dd1a::fault_label(r.fault), r.fault_va,
                    uc_strerror(r.uc_status));
        std::printf("[Zeetris/exec] RÓTULO=hybrid/assisted — NÃO é boot de jogo, "
                    "NÃO é PASS.\n");

        // Marcos EVIDENCIAIS (não são boot):
        assert(r.ran);
        assert(r.entered_module);                 // o entry cru real executou
        assert(r.first_pc == ZEETRIS_ENTRY_VA);   // 1º PC == entry derivado
        assert(r.first_pc >= LB && r.first_pc < LB + m.size);
        // O entry NÃO sobrevive à 1ª dependência (import/GOT/OEM ausente):
        // isso é o esperado de um probe honesto — não exigimos sucesso.
        assert(r.fault == zeebo::dd1a::FAULT_READ_UNMAPPED);
        assert(r.instructions > 0);
        // Contagem MEDIDA (gate de regressão: qualquer mudança de comportamento
        // do intérprete/entry reprova). Valor observado dos bytes reais.
        assert(r.instructions == 5038);
        std::fflush(stdout);
        uc_close(uc);
    }

    // (4d) CONTROLE que reproduz a FALHA DA v1: tratar o entry ARM como Thumb.
    //      A v1 chamou AEEMod_Load como HandleEvent (Thumb) → UC_ERR_INSN_INVALID.
    //      Aqui provamos, dos bytes reais, que a premissa Thumb é a errada:
    //      forçamos bit0=1 (Thumb) e exigimos que NÃO seja execução limpa.
    //      O MUTANTE (argv "buggy") pretende que a ABI Thumb funciona → RED.
    {
        uc_engine* uc = nullptr;
        assert(uc_open(UC_ARCH_ARM, UC_MODE_ARM, &uc) == UC_ERR_OK);
        const u32 LB = 0x12000000, STK = 0x00200000;
        uc_mem_map(uc, 0x001F0000, 0x10000, UC_PROT_ALL);
        BrewLoader ld(uc);
        assert(ld.inject_bytes(mod, LB, ZEETRIS_CLSID, "zeetris.mod"));

        u32 r0 = 0, r1 = 0, r2 = 0, sp = STK, lr = 0xF0F0F0F0u;
        uc_reg_write(uc, UC_ARM_REG_R0, &r0); uc_reg_write(uc, UC_ARM_REG_R1, &r1);
        uc_reg_write(uc, UC_ARM_REG_R2, &r2); uc_reg_write(uc, UC_ARM_REG_SP, &sp);
        uc_reg_write(uc, UC_ARM_REG_LR, &lr);
        // Entry|1 força decodificação Thumb (a premissa da v1).
        uc_err e = uc_emu_start(uc, ZEETRIS_ENTRY_VA | 1u, 0xF0F0F0F0u, 0, 2000);
        const bool thumb_clean = (e == UC_ERR_OK);
        std::printf("[Zeetris/ctrl-v1] entry como THUMB → uc=%s (%s)\n",
                    uc_strerror(e),
                    thumb_clean ? "limpo (premissa v1 sustentada?)"
                                : "inválido/abortado (premissa v1 REFUTADA)");
        if (BUGGY) {
            // MUTANTE: finge que a ABI Thumb é boa. O controle abaixo então falha.
            assert(thumb_clean && "MUTANT: entry Thumb deveria rodar limpo");
        } else {
            // Real: a ABI Thumb NÃO produz execução limpa (refuta a v1).
            assert(!thumb_clean);
        }
        uc_close(uc);
    }

    std::printf("=== Test Zeetris lifecycle probe: PASS (evidence-only, no boot) ===\n");
    return 0;
}
