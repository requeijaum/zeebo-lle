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

            // vtable de IShell: slot 0 é AddRef (ou QueryInterface). Em 0x120021e8:
            //   ldr r0, [r6]      (r6 = pIShell -> r0 = vtable)
            //   ldr r1, [r0]      (slot 0)
            //   mov r0, r6        (r0 = pIShell / this)
            //   bx r1             (chama AddRef)
            // Stub trivial para AddRef em SCRATCH + 0x150: mov r0, #1; bx lr
            const u32 ADDREF_STUB = SCRATCH + 0x150;
            u32 addref_code[2] = {
                0xe3a00001, // mov r0, #1
                0xe12fff1e  // bx lr
            };
            uc_mem_write(uc2, ADDREF_STUB, addref_code, sizeof(addref_code));
            uc_mem_write(uc2, vtbl, &ADDREF_STUB, 4); // vtbl[0] = ADDREF_STUB

            // Slot 2 de IShell: CreateInstance(this=r0, clsid=r1, ppObj=r2)
            // Chamado em 0x1200058c para instanciar AEECLSID_DISPLAY (0x01001001).
            // Stub de IShell::CreateInstance em SCRATCH + 0x180:
            // Grava objeto de display simulado (SCRATCH + 0x500) em *ppObj (*r2) e retorna 0 (AEE_SUCCESS)
            const u32 DISPLAY_OBJ = SCRATCH + 0x500;
            const u32 SHELL_CREATE_STUB = SCRATCH + 0x180;
            u32 shell_create_code[4] = {
                0xe59f3004, // ldr r3, [pc, #4]  -> DISPLAY_OBJ
                0xe5823000, // str r3, [r2]      -> *ppObj = DISPLAY_OBJ
                0xe3a00000, // mov r0, #0        -> return AEE_SUCCESS
                0xe12fff1e  // bx lr
            };
            uc_mem_write(uc2, SHELL_CREATE_STUB, shell_create_code, sizeof(shell_create_code));
            uc_mem_write(uc2, SHELL_CREATE_STUB + 16, &DISPLAY_OBJ, 4);
            uc_mem_write(uc2, vtbl + 8, &SHELL_CREATE_STUB, 4); // vtbl[2] = SHELL_CREATE_STUB

            u32 ppObj = SCRATCH + 0x200;

            // Suprir o ponteiro de static-base (AEEHelperFuncs) em moduleBase - 4 (0x11fffffc)
            // apontando para uma tabela de serviços / C-runtime helpers.
            // Em ddragonz.mod @0x12002184: ldr r0, [r0, #-4] carrega de LB - 4.
            const u32 STATIC_BASE = SCRATCH + 0x1000;
            uc_mem_write(uc2, LB - 4, &STATIC_BASE, 4);

            // Mock do helper MALLOC (offset 0x68 em AEEHelperFuncs):
            // 0x12002190: add r0, r5, #0x10 -> r0 é o tamanho a alocar
            // 0x12002194: bx r1 -> salta para malloc(r0)
            // Função stub ARM em SCRATCH + 0x2000:
            //   ldr r0, [pc, #4]  (retorna buffer pré-alocado)
            //   bx lr
            //   .word ALLOC_BUF
            // Stub MALLOC deve retornar ALLOC_BUF no registrador r0
            // Stub ARM em SCRATCH + 0x2000:
            //   ldr r0, [pc, #0]
            //   bx lr
            //   .word ALLOC_BUF
            const u32 MALLOC_STUB = SCRATCH + 0x2000;
            const u32 ALLOC_BUF = SCRATCH + 0x3000;
            u32 stub_code[3] = {
                0xe59f0000, // ldr r0, [pc, #0]
                0xe12fff1e, // bx lr
                ALLOC_BUF
            };
            uc_mem_write(uc2, MALLOC_STUB, stub_code, sizeof(stub_code));
            uc_mem_write(uc2, STATIC_BASE + 0x68, &MALLOC_STUB, 4);

            // Invocação com r0=pIShell, r1=pIModule(0), r2=ppObj
            // (ABI do entry de ddragonz.mod @0x12000014: mov r3, r2; mov r2, r1; mov r1, r0; mov r0, #0x14; bl 0x1200212c)
            // Para que 0x1200212c receba r3 != 0, o chamador precisa passar ppObj em r2 (que vira r3) E r1 != 0 (que vira r2)!
            FirstPcResult r2 = run_first_pc(uc2, m.entry_va, LB, m.size, STK, 200000, false,
                                            SCRATCH, 1, ppObj, 0);
            std::fflush(stdout);
            std::fprintf(stderr, "[DD1-runtime/probe] com args pIShell/ppObj: ran=%s entered=%s "
                         "first_pc=0x%08x last_pc=0x%08x instr=%llu fault=%s @0x%08x\n",
                         r2.ran ? "SIM" : "nao", r2.entered_module ? "SIM" : "nao",
                         r2.first_pc, r2.last_pc, (unsigned long long)r2.instructions,
                         fault_label(r2.fault), r2.fault_va);

            // Prova observável: com static_base em LB-4, MALLOC mockado e pIShell->AddRef,
            // o AEEMod_Load inicializa com sucesso a estrutura do módulo (IModule + vtable),
            // grava o ponteiro de saída em *ppObj, chama AddRef em pIShell, e retorna limpo
            // (0x12000030: bx lr com r0 = 0)!
            // Total de instruções executadas no módulo: 73 (completa AEEMod_Load com sucesso pleno).
            assert(r2.ran && r2.entered_module);
            assert(r2.instructions == 73);
            assert(r2.last_pc == 0x12000030);
            assert(r2.fault == FAULT_NONE);

            // Prova complementar: verificar que ppObj recebeu o ponteiro do objeto alocado (ALLOC_BUF)
            // e que r0 retornou 0 (AEE_SUCCESS)
            u32 created_mod_obj = 0;
            uc_mem_read(uc2, ppObj, &created_mod_obj, 4);
            u32 r0_val = 0;
            uc_reg_read(uc2, UC_ARM_REG_R0, &r0_val);
            u32 mod_vtable_ptr = 0;
            uc_mem_read(uc2, created_mod_obj, &mod_vtable_ptr, 4);
            std::fprintf(stderr, "[DD1-runtime/probe] retorno r0=0x%08x *ppObj=0x%08x (obj[0]=0x%08x)\n",
                         r0_val, created_mod_obj, mod_vtable_ptr);
            assert(r0_val == 0);
            assert(created_mod_obj == ALLOC_BUF);
            // vtable de AEEStaticMod gerada dinamicamente no buffer do objeto (ALLOC_BUF + 0x14)
            assert(mod_vtable_ptr == ALLOC_BUF + 0x14);

            // Verificar os 4 métodos da vtable do módulo (AddRef, Release, CreateInstance, FreeResources)
            u32 vtbl_methods[4] = {0};
            uc_mem_read(uc2, mod_vtable_ptr, vtbl_methods, sizeof(vtbl_methods));
            for (int i = 0; i < 4; ++i) {
                assert(vtbl_methods[i] >= LB && vtbl_methods[i] < LB + m.size);
            }
            std::fprintf(stderr, "[DD1-runtime/probe] vtable IModule: AddRef=0x%08x Release=0x%08x CreateInstance=0x%08x FreeRes=0x%08x\n",
                         vtbl_methods[0], vtbl_methods[1], vtbl_methods[2], vtbl_methods[3]);

            // Testar CreateInstance com CLSID incompatível (deve retornar erro r0 != 0)
            u32 ppApplet = SCRATCH + 0x400;
            uc_mem_write(uc2, ppApplet, "\0\0\0\0", 4);
            // IModule_CreateInstance(this=ALLOC_BUF, pIShell=SCRATCH, clsid=0x12345678, ppApplet)
            FirstPcResult r_mismatch = run_first_pc(uc2, vtbl_methods[2], LB, m.size, STK, 200000, false,
                                                    ALLOC_BUF, SCRATCH, 0x12345678, ppApplet);
            assert(r_mismatch.ran && r_mismatch.entered_module);
            u32 r0_mismatch = 0;
            uc_reg_read(uc2, UC_ARM_REG_R0, &r0_mismatch);
            assert(r0_mismatch != 0); // CLSID desconhecido -> falha (EFAILED/EBADCLASS)
            std::fprintf(stderr, "[DD1-runtime/probe] CreateInstance mismatch (0x12345678): r0=0x%08x (rejeitado corretamente)\n", r0_mismatch);

            // Testar CreateInstance com CLSID correto do jogo (0x0102f789)
            // Deve entrar no construtor do applet em 0x12000490!
            uc_mem_write(uc2, ppApplet, "\0\0\0\0", 4);
            FirstPcResult r_match = run_first_pc(uc2, vtbl_methods[2], LB, m.size, STK, 200000, false,
                                                 ALLOC_BUF, SCRATCH, DD_CLSID, ppApplet);
            std::fprintf(stderr, "[DD1-runtime/probe] CreateInstance match (0x%08x): ran=%s entered=%s "
                         "first_pc=0x%08x last_pc=0x%08x instr=%llu fault=%s @0x%08x\n",
                         DD_CLSID, r_match.ran ? "SIM" : "nao", r_match.entered_module ? "SIM" : "nao",
                         r_match.first_pc, r_match.last_pc, (unsigned long long)r_match.instructions,
                         fault_label(r_match.fault), r_match.fault_va);

            // Prova observável de DD1-runtime: CreateInstance aceita a classe DD_CLSID,
            // atende ao pedido de IShell::CreateInstance(AEECLSID_DISPLAY), e avança
            // até a instrução 174 no módulo guest!
            assert(r_match.ran && r_match.entered_module);
            assert(r_match.instructions == 174);
            assert(r_match.last_pc == 0x1201a68c);
            assert(r_match.fault == FAULT_FETCH_UNMAPPED);
            assert(r_match.fault_va == 0x00000000);

            uc_close(uc2);
        }
    }

    std::printf("=== Test DD1a first-PC assisted diagnostic: PASS (hybrid/assisted) ===\n");
    return 0;
}
