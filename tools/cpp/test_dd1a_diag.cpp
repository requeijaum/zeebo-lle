// test_dd1a_diag.cpp — QW99 Parte 4, DD1a: primeiro PC ASSISTIDO do Double Dragon.
//
// Prova, sob Unicorn INTÉRPRETE (sem Dynarmic), que o diagnóstico DD1a:
//   (1) valida a identidade do pacote (App ID 274754, CLSID 0x0102F789, tamanho
//       do ddragonz.mod) e rejeita pacotes falsos;
//   (2) executa o entry ARM cru de um módulo injetado e reporta HONESTAMENTE se
//       o PC entrou na faixa do módulo e qual foi a primeira dependência ausente;
//   (3) NUNCA usa o handler fixo 0x10532344 nem força sucesso.
//
// Controles:
//   * FIXTURE POSITIVO (instrumento): módulo sintético cru cujo entry executa e
//     depois lê de VA não mapeada → entered_module==true + FAULT_READ_UNMAPPED.
//     Prova que o instrumento SABE detectar entrada no módulo E a 1ª dependência.
//   * FIXTURE NEGATIVO (instrumento): entry cujo fetch é imediatamente não
//     mapeado → entered_module==false. Prova que o instrumento PODE reportar
//     "não entrou".
//   * MUTAÇÃO (argv "buggy"): reintroduz force_success (fabrica entered/sem
//     falha). O fixture negativo então passa a alegar entrada → RED reproduzido.
//
// Gate de ROM real: se o ddragonz.mod real (DD1A_MOD) não existir, sai 77 (SKIP)
// — nunca fabrica execução. Se existir, injeta e executa o entry cru real.
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>
#include <unicorn/unicorn.h>
#include "zeebo_dd1a_diag.h"
#include "zeebo_brew_loader.h"

using namespace zeebo::dd1a;
using zeebo::brew::BrewLoader;

static void put32(std::vector<u8>& b, size_t off, u32 v) {
    if (off + 4 > b.size()) b.resize(off + 4, 0);
    std::memcpy(b.data() + off, &v, 4);
}

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

// Constrói um MIF ESTRUTURALMENTE válido cujo único registro de applet (20 bytes,
// f4==0 e fc==0) carrega `clsid` — exatamente a forma que zeebo::brew::MifParser
// reconhece como registro de applet. Espelha a estrutura do 274754.mif real
// (magic 0x0011, tabela de bounds em 0x10/0x14).
static std::vector<u8> make_structural_mif(u32 clsid) {
    std::vector<u8> b(0x64, 0);
    auto p16 = [&](size_t off, uint16_t v){ std::memcpy(b.data()+off, &v, 2); };
    p16(0x00, 0x0011);          // magic
    put32(b, 0x10, 0x20);       // table_offset
    put32(b, 0x14, 2);          // section_count => 3 bounds
    put32(b, 0x20, 0x40);       // bound[0]
    put32(b, 0x24, 0x24);       // bound[1] -> sec0 tamanho não-applet (negativo p/ diff)
    // corrige ordenação: bounds devem ser crescentes.
    put32(b, 0x20, 0x40);
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
    std::printf("=== Test DD1a first-PC assisted diagnostic (hybrid/assisted)%s ===\n",
                BUGGY ? " [MUTANT: force_success]" : "");

    // Proibição explícita: constante do handler Z-Wheel jamais é o entry real.
    assert(FORBIDDEN_ZWHEEL_HANDLER == 0x10532344u);

    // ── (1) Identidade do pacote — PROVENIÊNCIA ESTRUTURAL ───────────────────
    //
    // App ID: DERIVADO do caminho real (diretório mod/<id>/, arquivo mif/<id>.mif),
    // com parsing ESTRITO. NÃO é mais o constante DD_APP_ID passado como argumento
    // (isso tornava app_id_ok tautológico). CLSID: validado na POSIÇÃO ESTRUTURAL
    // do registro de applet do MIF (zeebo::brew::MifParser), NÃO por varredura cega
    // de 4 bytes. A varredura crua é apenas CORROBORAÇÃO diagnóstica, nunca
    // autoridade de identidade.
    {
        // (1a) Derivação estrita do App ID a partir do caminho.
        AppIdParse p = parse_app_id_from_path(
            "/x/.Tuxality/Infuse/brew/mod/274754/ddragonz.mod");
        assert(p.ok && p.app_id == DD_APP_ID);
        AppIdParse pm = parse_app_id_from_path("/x/brew/mif/274754.mif");
        assert(pm.ok && pm.app_id == DD_APP_ID);

        // Controle: diretório ERRADO → derivação continua, mas app_id_ok falha.
        AppIdParse wrong = parse_app_id_from_path("/x/brew/mod/999999/ddragonz.mod");
        assert(wrong.ok && wrong.app_id == 999999u);
        // Controle: caminho MALFORMADO (não-numérico) → parsing ESTRITO rejeita.
        assert(!parse_app_id_from_path("/x/brew/mod/27a4b/x.mod").ok);
        assert(!parse_app_id_from_path("/x/brew/mod/2747540000000/x.mod").ok); // overflow u32
        assert(!parse_app_id_from_path("/x/brew/mod//x.mod").ok);              // dir vazio
        assert(!parse_app_id_from_path("/x/brew/mif/274a.mif").ok);            // stem não-numérico

        // (1b) MIF ESTRUTURAL positivo: registro de applet de 20 bytes com o CLSID.
        std::vector<u8> mif_ok = make_structural_mif(DD_CLSID);
        PackageIdentity ok = validate_package(
            "/x/brew/mod/274754/ddragonz.mod", "/x/brew/mif/274754.mif",
            mif_ok, DD_MOD_SIZE);
        assert(ok.app_id_ok && ok.app_id == DD_APP_ID);
        assert(ok.clsid_ok && ok.clsid == 0x0102f789u);
        assert(ok.clsid_structural);          // veio do registro estrutural
        assert(ok.mod_size_ok && ok.valid());

        // (1c) Controle CLSID: bytes 0x0102F789 presentes SOMENTE fora de um
        //      registro estrutural (varredura crua os acha, estrutura NÃO).
        //      clsid_ok DEVE ser falso; a varredura corrobora mas não decide.
        std::vector<u8> mif_decoy = make_structural_mif(0x11223344u); // applet real ≠ DD
        // injeta os 4 bytes do CLSID num ponto arbitrário não-estrutural.
        mif_decoy.resize(mif_decoy.size() + 8, 0);
        put32(mif_decoy, mif_decoy.size() - 6, DD_CLSID);
        PackageIdentity decoy = validate_package(
            "/x/brew/mod/274754/ddragonz.mod", "/x/brew/mif/274754.mif",
            mif_decoy, DD_MOD_SIZE);
        assert(!decoy.clsid_ok);              // estrutura não confirma DD
        assert(decoy.clsid_scan_corroborates);// varredura crua achou (diagnóstico)
        assert(!decoy.valid());               // identidade REJEITADA apesar do scan

        // (1d) Controle negativo pleno: App ID errado, CLSID ausente, tamanho errado.
        std::vector<u8> mif_bad(64, 0x55);
        PackageIdentity bad = validate_package(
            "/x/brew/mod/999999/other.mod", "/x/brew/mif/999999.mif",
            mif_bad, 1234);
        assert(!bad.app_id_ok && !bad.clsid_ok && !bad.mod_size_ok && !bad.valid());
    }

    // ── (2) Fixture POSITIVO do instrumento: entra no módulo e bate na 1ª
    //        dependência ausente (leitura em VA não mapeada). ────────────────
    {
        uc_engine* uc = nullptr;
        assert(uc_open(UC_ARCH_ARM, UC_MODE_ARM, &uc) == UC_ERR_OK);
        const u32 LB = 0x12000000, STK = 0x00100000;
        uc_mem_map(uc, STK & ~0xfffu, 0x1000, UC_PROT_ALL);

        BrewLoader ld(uc);
        // Código cru: mov r0,#imm ; mov r4,#0 (dep VA base) ; ldr r1,[r4] → 0.
        // Simplificamos com endereço absoluto não mapeado via literal pool.
        std::vector<u8> mod(0x100, 0);
        put32(mod, 0x00, 0xE3A00001); // mov r0, #1        (entra no módulo)
        put32(mod, 0x04, 0xE59F4004); // ldr r4, [pc, #4]  (carrega 0xDEAD0000)
        put32(mod, 0x08, 0xE5941000); // ldr r1, [r4]      → READ_UNMAPPED
        put32(mod, 0x0C, 0xE12FFF1E); // bx lr
        put32(mod, 0x10, 0xDEAD0000); // literal: VA não mapeada (pc+8+4 de 0x04)
        assert(ld.inject_bytes(mod, LB, 0, "fixture+.mod"));
        const auto& m = ld.module();
        assert(m.entry_va == LB); // ENTRY_RAW_START
        assert(m.entry_kind == zeebo::brew::ENTRY_RAW_START);

        FirstPcResult r = run_first_pc(uc, m.entry_va, LB, m.size, STK, 10000, BUGGY);
        assert(r.ran);
        assert(std::string(r.label()) == "hybrid/assisted");
        assert(r.entered_module);          // executou dentro do módulo
        assert(r.first_pc == LB);          // primeiro PC == entry
        assert(r.instructions >= 2);       // mov + ldr r4 antes da falha
        assert(r.fault == FAULT_READ_UNMAPPED); // 1ª dependência ausente
        assert(r.fault_va == 0xDEAD0000u);
        std::printf("[+] positivo: entrou no módulo, 1ª dep ausente @0x%08x (%s)\n",
                    r.fault_va, fault_label(r.fault));
        uc_close(uc);
    }

    // ── (3) Fixture NEGATIVO do instrumento: entry NÃO mapeado → não entra.
    //        A mutação force_success fabrica entered → RED reproduzido. ──────
    {
        uc_engine* uc = nullptr;
        assert(uc_open(UC_ARCH_ARM, UC_MODE_ARM, &uc) == UC_ERR_OK);
        const u32 STK = 0x00100000;
        uc_mem_map(uc, STK & ~0xfffu, 0x1000, UC_PROT_ALL);
        // Entry aponta para uma VA que NUNCA foi mapeada (nada injetado ali).
        const u32 UNMAPPED_ENTRY = 0x20000000, FAKE_BEGIN = 0x20000000, FAKE_SZ = 0x1000;

        FirstPcResult r = run_first_pc(uc, UNMAPPED_ENTRY, FAKE_BEGIN, FAKE_SZ,
                                       STK, 10000, BUGGY);
        assert(r.ran);
        assert(r.instructions == 0);       // nenhuma instrução executou
        // Controle: sem mutação, o instrumento reporta honestamente "não entrou".
        // Com force_success (buggy), entered vira true → esta asserção FALHA.
        assert(!r.entered_module);
        assert(r.fault == FAULT_FETCH_UNMAPPED);
        std::printf("[-] negativo: fetch não mapeado @0x%08x, não entrou (%s)\n",
                    r.fault_va, fault_label(r.fault));
        uc_close(uc);
    }

    // ── (4) ROM REAL: injeta e executa o entry cru de ddragonz.mod. ──────────
    // Gate honesto: sem o mod real → SKIP (exit 77), nunca fabrica execução.
    {
        const char* env = std::getenv("DD1A_MOD");
        const std::string mod_path =
            env ? env : "/home/rafaelfrequiao/.Tuxality/Infuse/brew/mod/274754/ddragonz.mod";
        const char* menv = std::getenv("DD1A_MIF");
        const std::string mif_path =
            menv ? menv : "/home/rafaelfrequiao/.Tuxality/Infuse/brew/mif/274754.mif";

        std::vector<u8> mod = read_file(mod_path);
        std::vector<u8> mif = read_file(mif_path);
        if (mod.empty() || mif.empty()) {
            std::printf("[SKIP] ROM real ausente (mod='%s' mif='%s'); DD1a real não executado.\n",
                        mod_path.c_str(), mif_path.c_str());
            std::printf("=== Test DD1a: SKIP (exit 77) — sem fabricação de execução ===\n");
            return 77;
        }

        // Identidade real do pacote: App ID DERIVADO do caminho real (não constante),
        // CLSID na posição estrutural do MIF, tamanho do mod.
        PackageIdentity id = validate_package(mod_path, mif_path, mif, mod.size());
        std::printf("[ID] app_id=%u(%s) clsid=0x%08x(%s,%s) mod=%u(%s) scan_corrob=%s\n",
                    id.app_id, id.app_id_ok ? "ok" : "X",
                    id.clsid, id.clsid_ok ? "ok" : "X",
                    id.clsid_structural ? "estrutural" : "NAO-estrutural",
                    id.mod_size, id.mod_size_ok ? "ok" : "X",
                    id.clsid_scan_corroborates ? "sim" : "nao");
        assert(id.app_id == DD_APP_ID);       // derivado do caminho, não do constante
        assert(id.clsid_structural);          // CLSID veio do registro estrutural do MIF
        assert(id.valid());                   // pacote real DEVE ter identidade correta

        // Injeção + resolução de entry via infraestrutura existente.
        uc_engine* uc = nullptr;
        assert(uc_open(UC_ARCH_ARM, UC_MODE_ARM, &uc) == UC_ERR_OK);
        const u32 LB = 0x12000000, STK = 0x00200000;
        // Pilha com headroom ABAIXO do topo (o prólogo `str lr,[sp,#-4]!`
        // decrementa SP): mapeia [0x1F0000, 0x200000) para não confundir
        // underflow de pilha do instrumento com dependência real do módulo.
        uc_mem_map(uc, 0x001F0000, 0x10000, UC_PROT_ALL);
        BrewLoader ld(uc);
        assert(ld.inject_bytes(mod, LB, DD_CLSID, "ddragonz.mod"));
        const auto& m = ld.module();
        // ddragonz.mod começa com `str lr,[sp,#-4]!` (04 e0 2d e5) → RAW_START.
        assert(m.entry_va == LB);
        assert(m.entry_kind == zeebo::brew::ENTRY_RAW_START);
        // NUNCA o handler fixo da Z-Wheel.
        assert(m.entry_va != FORBIDDEN_ZWHEEL_HANDLER);

        FirstPcResult r = run_first_pc(uc, m.entry_va, LB, m.size, STK, 200000, false);
        std::printf("[DD1a/hybrid-assisted] entry=0x%08x kind=%s\n",
                    m.entry_va, zeebo::brew::entry_kind_label(m.entry_kind));
        std::printf("[DD1a/hybrid-assisted] entrou_no_módulo=%s first_pc=0x%08x "
                    "last_pc=0x%08x instr=%llu\n",
                    r.entered_module ? "SIM" : "não", r.first_pc, r.last_pc,
                    (unsigned long long)r.instructions);
        std::printf("[DD1a/hybrid-assisted] 1ª falha: %s @0x%08x (uc=%s)\n",
                    fault_label(r.fault), r.fault_va, uc_strerror(r.uc_status));
        std::printf("[DD1a/hybrid-assisted] RÓTULO=%s — NÃO é boot orgânico, "
                    "NÃO é PASS de jogo, NÃO fecha marco B.\n", r.label());

        // Marco DD1a: o PC executou DENTRO da faixa do módulo (primeiro PC real).
        assert(r.ran);
        assert(r.entered_module);
        assert(r.first_pc >= LB && r.first_pc < LB + m.size);
        // DD1a espera NÃO sobreviver ao primeiro import: alguma dependência
        // ausente ou orçamento — não exigimos sucesso (regra de ouro).
        assert(r.fault != FAULT_NONE || r.instructions > 0);
        uc_close(uc);

        // DD1-runtime: teste exploratório assistido passando contexto de chamada AEEMod_Load
        // (pIShell != 0, ppObj != 0) e verificando que a execução avança além do branch
        // de validação inicial (23 instruções) até a primeira dependência de import/GOT real.
        {
            uc_engine* uc2 = nullptr;
            assert(uc_open(UC_ARCH_ARM, UC_MODE_ARM, &uc2) == UC_ERR_OK);
            uc_mem_map(uc2, 0x001F0000, 0x10000, UC_PROT_ALL);
            BrewLoader ld2(uc2);
            assert(ld2.inject_bytes(mod, LB, DD_CLSID, "ddragonz.mod"));

            // Scratch para instâncias simuladas: pIShell e ppObj
            const u32 SCRATCH = 0x00300000;
            uc_mem_map(uc2, SCRATCH, 0x10000, UC_PROT_ALL);
            u32 vtbl = SCRATCH + 0x100;
            uc_mem_write(uc2, SCRATCH, &vtbl, 4); // pIShell->vtable
            u32 ppObj = SCRATCH + 0x200;

            // Suprir o ponteiro de static-base (AEEHelperFuncs) em moduleBase - 4 (0x11fffffc)
            // apontando para uma tabela de serviços / C-runtime helpers.
            // Em ddragonz.mod @0x12002184: ldr r0, [r0, #-4] carrega de LB - 4.
            const u32 STATIC_BASE = SCRATCH + 0x1000;
            uc_mem_write(uc2, LB - 4, &STATIC_BASE, 4);

            // Invocação com r0=pIShell, r1=pIModule(0), r2=ppObj
            FirstPcResult r2 = run_first_pc(uc2, m.entry_va, LB, m.size, STK, 200000, false,
                                            SCRATCH, 0, ppObj, 0);
            std::fflush(stdout);
            std::fprintf(stderr, "[DD1-runtime/probe] com args pIShell/ppObj: ran=%s entered=%s "
                         "first_pc=0x%08x last_pc=0x%08x instr=%llu fault=%s @0x%08x\n",
                         r2.ran ? "SIM" : "nao", r2.entered_module ? "SIM" : "nao",
                         r2.first_pc, r2.last_pc, (unsigned long long)r2.instructions,
                         fault_label(r2.fault), r2.fault_va);

            // Prova observável: avança além das 23 instruções iniciais e além das 35 instruções
            // (com static_base em LB-4, alcança 37 instruções e executa bx r1 em 0x12002194,
            // onde r1 é carregado de [STATIC_BASE + 0x68], que representa MALLOC em AEEHelperFuncs).
            assert(r2.ran && r2.entered_module);
            assert(r2.instructions == 37);
            assert(r2.last_pc == 0x12002194);
            assert(r2.fault == FAULT_FETCH_UNMAPPED);
            assert(r2.fault_va == 0x00000000);

            uc_close(uc2);
        }
    }

    std::printf("=== Test DD1a first-PC assisted diagnostic: PASS (hybrid/assisted) ===\n");
    return 0;
}
