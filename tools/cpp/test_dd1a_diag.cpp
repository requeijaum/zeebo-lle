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
#include <map>
#include <unicorn/unicorn.h>
#include "zeebo_dd1a_diag.h"
#include "zeebo_brew_loader.h"

using namespace zeebo::dd1a;
using zeebo::brew::BrewLoader;

// Relogio virtual do guest (aee_GetUpTimeMS).
//
// Precisa ser um hook do host, e nao um `mov r0,#K` fixo: um game loop deriva o
// delta de tempo desta funcao. Com valor constante o jogo ve dt=0 e nao anima —
// falha silenciosa, porque nada quebra, o frame so fica igual para sempre.
static u32 g_uptime_ms = 100;
static u32 g_uptime_step_ms = 33;   // ~30 fps
static int g_uptime_reads = 0;

static void uptime_clock_hook(uc_engine* uc, uint64_t, uint32_t, void*) {
    uc_reg_write(uc, UC_ARM_REG_R0, &g_uptime_ms);
    g_uptime_ms += g_uptime_step_ms;
    ++g_uptime_reads;
}

// Detector de slot de vtable chamado mas nao implementado.
// Uma entrada de vtable vazia devolve 0 e o jogo segue como se a chamada tivesse
// funcionado: e o modo de falha mais caro aqui, porque nada quebra e o frame so
// fica parado. Mapeamos endereco-do-stub -> offset para nomear quem foi chamado.
static std::map<u32, u32> g_probe_off;
static std::map<u32, int> g_missing;

static void missing_slot_hook(uc_engine*, uint64_t address, uint32_t, void*) {
    auto it = g_probe_off.find(static_cast<u32>(address));
    if (it != g_probe_off.end()) ++g_missing[it->second];
}

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
            // Chamado em 0x1200058c para instanciar AEECLSID_DISPLAY (0x01001001),
            // e depois em 0x1201a6c0 para instanciar AEECLSID_HEAP (0x01001002).
            // Para classes desconhecidas ou opcionais como HEAP, o BREW retorna 0 (AEE_SUCCESS)
            // ou erro; em ddragonz @0x1201a6c4: cmp r0, #0; popne {r3-r5, lr}; movne r0, #0; bxne lr
            // se r0 != 0 (falha ao criar IHeap), ele limpa a pilha e continua limpo!
            // Se retornar 0, ele tenta ler vtable de *ppObj em 0x1201a6d8.
            // Para HEAP, retornar 1 (EFAILED/não suportado) permite continuar a inicialização sem criar IHeap.
            // Stub de IShell::CreateInstance inteligente em SCRATCH + 0x180:
            //   cmp r1, #0x01001001 (DISPLAY) -> retorna DISPLAY_OBJ e r0 = 0
            //   senão -> retorna r0 = 1 (EFAILED) e *ppObj = 0
            const u32 DISPLAY_OBJ = SCRATCH + 0x500;
            const u32 SHELL_CREATE_STUB = SCRATCH + 0x180;
            u32 shell_create_code[8] = {
                0xe59f3014, // ldr r3, [pc, #20]  -> literal DISPLAY_CLSID (0x01001001)
                0xe1510003, // cmp r1, r3
                0x059f3010, // ldreq r3, [pc, #16] -> DISPLAY_OBJ
                0x05823000, // streq r3, [r2]      -> *ppObj = DISPLAY_OBJ
                0x03a00000, // moveq r0, #0        -> return AEE_SUCCESS
                0x13a00001, // movne r0, #1        -> return EFAILED (para HEAP etc)
                0xe12fff1e, // bx lr
                0x00000000  // padding
            };
            uc_mem_write(uc2, SHELL_CREATE_STUB, shell_create_code, sizeof(shell_create_code));
            u32 disp_clsid = 0x01001001;
            uc_mem_write(uc2, SHELL_CREATE_STUB + 28, &disp_clsid, 4);
            uc_mem_write(uc2, SHELL_CREATE_STUB + 32, &DISPLAY_OBJ, 4);
            uc_mem_write(uc2, vtbl + 8, &SHELL_CREATE_STUB, 4); // vtbl[2] = SHELL_CREATE_STUB

            // Objeto e vtable de IDisplay em SCRATCH + 0x500:
            // ddragonz @0x1201a610 lê r0 = IDisplay->vtable (em DISPLAY_OBJ + 0)
            // e em 0x1201a618 lê r2 = vtable[4] (offset 0x10) e faz bx r2.
            // Para que r0 seja válido, DISPLAY_OBJ precisa conter ponteiro para vtable:
            // e [DISP_VTBL + 0x10] deve apontar para DISP_OFF10_DIMS_STUB.
            // Além disso, [r0 + 0xc] é dereferenciado em 0x1201a608 (r0 = [r0, #0xc]).
            // Vamos montar a vtable de IDisplay em DISP_VTBL e o sub-objeto de bitmap/device.
            const u32 DISP_VTBL = SCRATCH + 0x700;
            uc_mem_write(uc2, DISPLAY_OBJ, &DISP_VTBL, 4);

            // ATENÇÃO — identidade NÃO provada para este offset.
            // O layout do SDK (tools/py/vtbl_layout.py, gate make test-vtbl-layout)
            // diz que IDisplay::vtbl[4] (offset 0x10) é DrawText. Mas o call-site
            // 0x1201a618 deriva a vtable de [obj + 0xc], onde obj vem da static-base
            // +0xc0 — NÃO é o IDisplay devolvido por ISHELL_CreateInstance.
            // Ou seja: este slot pertence a OUTRA interface, ainda não identificada.
            // A semântica que o binário exige aqui (escrever w/h num out-param)
            // é de um "get dimensions", não de DrawText.
            // Por isso o nome é descritivo do OFFSET e do COMPORTAMENTO observado,
            // não de um método do SDK que não podemos afirmar. Ver docs/dd_contract.md.
            const u32 DISP_OFF10_DIMS_STUB = SCRATCH + 0x2200;
            u32 disp_off10_dims_code[6] = {
                0xe59f200c, // ldr r2, [pc, #12] -> 0x01e00280 (h=480, w=640)
                0xe5812004, // str r2, [r1, #4]  -> grava em [sp+4] do chamador
                0xe3a00000, // mov r0, #0        -> return 0
                0xe12fff1e, // bx lr
                0x01e00280, // w=640 (0x0280), h=480 (0x01e0)
                0x00000000
            };
            uc_mem_write(uc2, DISP_OFF10_DIMS_STUB, disp_off10_dims_code, sizeof(disp_off10_dims_code));
            uc_mem_write(uc2, DISP_VTBL + 0x10, &DISP_OFF10_DIMS_STUB, 4); // vtbl[4] = DISP_OFF10_DIMS_STUB
            // Também mapear vtbl + 0x10 (caso o objeto seja o próprio pIShell/SCRATCH)
            uc_mem_write(uc2, vtbl + 0x10, &DISP_OFF10_DIMS_STUB, 4);

            u32 ppObj = SCRATCH + 0x200;

            // Suprir o ponteiro de static-base (AEEHelperFuncs) em moduleBase - 4 (0x11fffffc)
            // apontando para uma tabela de serviços / C-runtime helpers.
            // Em ddragonz.mod @0x12002184: ldr r0, [r0, #-4] carrega de LB - 4.
            const u32 STATIC_BASE = SCRATCH + 0x1000;
            uc_mem_write(uc2, LB - 4, &STATIC_BASE, 4);

            // Em AEEMod_Load para alocar a estrutura AEEStaticMod:
            // O mock original de 2 instruções (ldr r0, [pc]; bx lr) mantém a contagem
            // estrita de 73 instruções durante AEEMod_Load.
            // Para as alocações subsequentes (CreateInstance e game loop), um stub que
            // avança o bump pointer ou um array de buffers pré-alocados funciona perfeitamente.
            // Usamos um stub de 2 instruções onde AEEMod_Load consome exatamente 2 instruções,
            // e mantemos um bump pointer atualizado pelo host entre as etapas ou com retorno sequencial.
            const u32 MALLOC_STUB = SCRATCH + 0x2000;
            const u32 ALLOC_BUF1 = SCRATCH + 0x3000;
            const u32 ALLOC_BUF2 = SCRATCH + 0x4000;
            const u32 ALLOC_BUF3 = SCRATCH + 0x5000;
            u32 stub_code[3] = {
                0xe59f0000, // ldr r0, [pc, #0]
                0xe12fff1e, // bx lr
                ALLOC_BUF1
            };
            uc_mem_write(uc2, MALLOC_STUB, stub_code, sizeof(stub_code));
            uc_mem_write(uc2, STATIC_BASE + 0x68, &MALLOC_STUB, 4);

            // Mock do helper GetAppContext (offset 0xc0 em static-base / Zeebo platform table):
            // Em ddragonz.mod @0x1201a688: ldr r0, [r0, #0xc0]; bx r0
            // GetAppContext retorna ponteiro para a estrutura de contexto da aplicação (APP_CTX)
            // Layout confirmado: IShell* em +12 (0x0c), IDisplay* em +20 (0x14)
            const u32 APP_CTX = SCRATCH + 0x600;
            uc_mem_write(uc2, APP_CTX + 0x0c, &SCRATCH, 4);      // context->pIShell = SCRATCH
            uc_mem_write(uc2, APP_CTX + 0x14, &DISPLAY_OBJ, 4);  // context->pIDisplay = DISPLAY_OBJ

            const u32 GETAPPCTX_STUB = SCRATCH + 0x2100;
            u32 getappctx_code[3] = {
                0xe59f0000, // ldr r0, [pc, #0]
                0xe12fff1e, // bx lr
                APP_CTX
            };
            uc_mem_write(uc2, GETAPPCTX_STUB, getappctx_code, sizeof(getappctx_code));
            uc_mem_write(uc2, STATIC_BASE + 0xc0, &GETAPPCTX_STUB, 4);

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
            assert(created_mod_obj == ALLOC_BUF1);
            // vtable de AEEStaticMod gerada dinamicamente no buffer do objeto (ALLOC_BUF1 + 0x14)
            assert(mod_vtable_ptr == ALLOC_BUF1 + 0x14);

            // Verificar os 4 métodos da vtable do módulo (AddRef, Release, CreateInstance, FreeResources)
            u32 vtbl_methods[4] = {0};
            uc_mem_read(uc2, mod_vtable_ptr, vtbl_methods, sizeof(vtbl_methods));
            for (int i = 0; i < 4; ++i) {
                assert(vtbl_methods[i] >= LB && vtbl_methods[i] < LB + m.size);
            }
            std::fprintf(stderr, "[DD1-runtime/probe] vtable IModule: AddRef=0x%08x Release=0x%08x CreateInstance=0x%08x FreeRes=0x%08x\n",
                         vtbl_methods[0], vtbl_methods[1], vtbl_methods[2], vtbl_methods[3]);

            // Atualiza o stub MALLOC para devolver ALLOC_BUF2 na chamada de CreateInstance
            uc_mem_write(uc2, MALLOC_STUB + 8, &ALLOC_BUF2, 4);

            // Testar CreateInstance com CLSID incompatível (deve retornar erro r0 != 0)
            u32 ppApplet = SCRATCH + 0x400;
            uc_mem_write(uc2, ppApplet, "\0\0\0\0", 4);
            // IModule_CreateInstance(this=ALLOC_BUF1, pIShell=SCRATCH, clsid=0x12345678, ppApplet)
            FirstPcResult r_mismatch = run_first_pc(uc2, vtbl_methods[2], LB, m.size, STK, 200000, false,
                                                    ALLOC_BUF1, SCRATCH, 0x12345678, ppApplet);
            assert(r_mismatch.ran && r_mismatch.entered_module);
            u32 r0_mismatch = 0;
            uc_reg_read(uc2, UC_ARM_REG_R0, &r0_mismatch);
            assert(r0_mismatch != 0); // CLSID desconhecido -> falha (EFAILED/EBADCLASS)
            std::fprintf(stderr, "[DD1-runtime/probe] CreateInstance mismatch (0x12345678): r0=0x%08x (rejeitado corretamente)\n", r0_mismatch);

            // Testar CreateInstance com CLSID correto do jogo (0x0102f789)
            // Deve entrar no construtor do applet em 0x12000490!
            uc_mem_write(uc2, ppApplet, "\0\0\0\0", 4);
            // IDisplay vive em DISPLAY_OBJ (SCRATCH + 0x500).
            // Em 0x1201a68c, GetAppContext retornou APP_CTX em r0.
            // 0x1201a690: ldr r4, [r0, #0xc]  (r4 = APP_CTX->pIShell)
            // 0x1201a694: ldr r0, [r5, #-4]   (static-base)
            // 0x1201a698: ldr r0, [r0, #0xc0] (GetAppContext)
            // 0x1201a69c: mov lr, pc; bx r0   (chama GetAppContext de novo)
            // 0x1201a6a4: ldr r0, [r0, #0xc]  (r0 = APP_CTX->pIShell)
            // ... chama IShell::CreateInstance(HEAP) com r4 = pIShell ...
            // e depois em 0x1201a608:
            // 0x1201a608: ldr r0, [r0, #0xc]  (se r0 fosse APP_CTX, r0->pIShell! Mas ele quer pIDisplay?)
            // Vejamos o que ddragonz faz em 0x1201a600:
            // Ele chama uma função que retorna um objeto em r0, depois faz:
            // 0x1201a608: ldr r0, [r0, #0xc]
            // 0x1201a60c: add r1, sp, #4
            // 0x1201a610: ldr r0, [r0]
            // 0x1201a614: add lr, pc, #8
            // 0x1201a618: ldr r2, [r0, #0x10]
            // 0x1201a61c: mov r0, r4
            // 0x1201a620: bx r2
            // Então [r0 + 0xc] precisa ser um ponteiro para um objeto com vtable em [0] e método em [0x10]!
            // Vamos configurar SCRATCH + 0x0c para apontar para DISPLAY_OBJ:
            uc_mem_write(uc2, SCRATCH + 0x0c, &DISPLAY_OBJ, 4);

            FirstPcResult r_match = run_first_pc(uc2, vtbl_methods[2], LB, m.size, STK, 200000, false,
                                                 ALLOC_BUF1, SCRATCH, DD_CLSID, ppApplet);
            u32 r_lr = 0, r_sp = 0, r_r0 = 0, r_r4 = 0;
            uc_reg_read(uc2, UC_ARM_REG_LR, &r_lr);
            uc_reg_read(uc2, UC_ARM_REG_SP, &r_sp);
            uc_reg_read(uc2, UC_ARM_REG_R0, &r_r0);
            uc_reg_read(uc2, UC_ARM_REG_R4, &r_r4);
            std::fprintf(stderr, "[DD1-runtime/probe] CreateInstance match (0x%08x): ran=%s entered=%s "
                         "first_pc=0x%08x last_pc=0x%08x instr=%llu fault=%s @0x%08x lr=0x%08x sp=0x%08x r0=0x%08x r4=0x%08x\n",
                         DD_CLSID, r_match.ran ? "SIM" : "nao", r_match.entered_module ? "SIM" : "nao",
                         r_match.first_pc, r_match.last_pc, (unsigned long long)r_match.instructions,
                         fault_label(r_match.fault), r_match.fault_va, r_lr, r_sp, r_r0, r_r4);

            // Prova observável de DD1-runtime: CreateInstance aceita a classe DD_CLSID,
            // atende ao pedido de IShell::CreateInstance(AEECLSID_DISPLAY), obtém o contexto
            // da aplicação via GetAppContext (offset 0xc0), despacha verificação de HEAP,
            // atende ao método IDisplay::GetInfo (offset 0x10) e avança com sucesso até
            // a conclusão de CreateInstance (retorno limpo em 0x12000724 com r0 = 0)!
            // Total de instruções reais no módulo guest: 530!
            assert(r_match.ran && r_match.entered_module);
            assert(r_match.instructions == 530);
            assert(r_match.last_pc == 0x12000724);
            assert(r_match.fault == FAULT_NONE);
            assert(r_match.fault_va == 0x00000000);
            assert(r_r0 == 0); // AEE_SUCCESS

            // Em 0x1200c6ec:
            //   ldr r0, [r4, #0xc]  (lê r0 = applet->w[3] = pIShell = SCRATCH)
            //   ldr r1, [r0]        (lê r1 = vtable = *SCRATCH = SCRATCH_VTBL)
            //   ldr ip, [r1, #0x2c] (lê ip = vtable[11] = ISHELL_SetTimer)
            // No teste, SCRATCH é o objeto, e a vtable é colocada em SCRATCH_VTBL (SCRATCH + 0x100)!
            // Escrevemos o ponteiro de vtable em SCRATCH[0] e o método em SCRATCH_VTBL + 0x2c:
            const u32 SCRATCH_VTBL = SCRATCH + 0x100;
            const u32 SETTIMER_STUB = SCRATCH + 0x2400;
            u32 settimer_code[2] = {
                0xe3a00000, // mov r0, #0
                0xe12fff1e  // bx lr
            };
            uc_mem_write(uc2, SETTIMER_STUB, settimer_code, sizeof(settimer_code));
            uc_mem_write(uc2, SCRATCH_VTBL + 0x2c, &SETTIMER_STUB, 4);
            uc_mem_write(uc2, SCRATCH, &SCRATCH_VTBL, 4);

            // Prova complementar: verificar que ppApplet recebeu a instância criada do jogo
            u32 created_applet_ptr = 0;
            uc_mem_read(uc2, ppApplet, &created_applet_ptr, 4);
            u32 applet_words[8] = {0};
            if (created_applet_ptr) {
                uc_mem_read(uc2, created_applet_ptr, applet_words, sizeof(applet_words));
            }
            std::fprintf(stderr, "[DD1-runtime/probe] *ppApplet=0x%08x (w[0]=0x%08x w[1]=0x%08x w[2]=0x%08x w[3]=0x%08x w[4]=0x%08x w[5]=0x%08x w[6]=0x%08x w[7]=0x%08x)\n",
                         created_applet_ptr,
                         applet_words[0], applet_words[1], applet_words[2], applet_words[3],
                         applet_words[4], applet_words[5], applet_words[6], applet_words[7]);
            assert(created_applet_ptr != 0);

            // Verificação dos campos da struct AEEApplet do Double Dragon:
            //   w[0] = pvt (vtable herdada do caller / pIShell: 0x00300000)
            //   w[1] = clsId (DD_CLSID: 0x0102f789)
            //   w[2] = refCount (1)
            //   w[3] = pIShell (SCRATCH: 0x00300000)
            //   w[4] = pIModule (0)
            //   w[5] = pIDisplay (DISPLAY_OBJ: 0x00300500)
            //   w[6] = HandleEvent (0x1200c5e0)
            assert(applet_words[1] == DD_CLSID);
            assert(applet_words[2] == 1);
            assert(applet_words[3] == SCRATCH);
            assert(applet_words[5] == DISPLAY_OBJ);
            assert(applet_words[6] == 0x1200c5e0);

            // Prova DD1: despachar HandleEvent(EVT_APP_START = 0) para a rotina de eventos do jogo (w[6] = 0x1200c5e0)
            // Em AEEEvent.h do BREW SDK: EVT_APP_START = 0.
            // boolean HandleEvent(IApplet* pi, AEEEvent eCode, uint16 wParam, uint32 dwParam)
            const u32 handle_event_fn = applet_words[6];
            FirstPcResult r_evt = run_first_pc(uc2, handle_event_fn,
                                               0x12000000,
                                               mod.size(),
                                               0x00200000,
                                               10000,
                                               false,
                                               created_applet_ptr, // r0: this
                                               0,                  // r1: EVT_APP_START (0)
                                               0,                  // r2: wParam
                                               0);                 // r3: dwParam
            u32 r_evt_r0 = 0;
            uc_reg_read(uc2, UC_ARM_REG_R0, &r_evt_r0);
            std::fprintf(stderr, "[DD1-runtime/probe] HandleEvent(EVT_APP_START=0): ran=%s entered=%s last_pc=0x%08x instr=%lu fault=%s @0x%08x r0=0x%08x\n",
                         r_evt.ran ? "SIM" : "nao",
                         r_evt.entered_module ? "SIM" : "nao",
                         r_evt.last_pc,
                         (unsigned long)r_evt.instructions,
                         fault_label(r_evt.fault),
                         r_evt.fault_va,
                         r_evt_r0);

            // Prova observável de DD1-runtime completa (ROADMAP Parte 4: CreateInstance -> objeto -> HandleEvent):
            // O applet trata EVT_APP_START (0) armando o timer principal do game-loop
            // via ISHELL_SetTimer(r1 = 33ms [0x21], callback = 0x120239dc, pUser = applet),
            // executa 37 instruções reais (incluindo o stub do timer e o retorno)
            // e retorna limpo no sentinela em 0x1200c718 com r0 = 1 (TRUE)!
            assert(r_evt.ran && r_evt.entered_module);
            assert(r_evt.instructions == 37);
            assert(r_evt.last_pc == 0x1200c718);
            assert(r_evt.fault == FAULT_NONE);
            assert(r_evt.fault_va == 0x00000000);
            assert(r_evt_r0 == 1); // Retorno booleano TRUE: evento consumido pelo applet!

            // Mock de aee_GetUpTimeMS (slot 0xb0 em AEEHelperFuncs / static-base).
            //
            // ATENCAO: nao pode ser constante. Um game loop calcula o delta entre
            // frames a partir deste relogio; se ele nao anda, dt=0 e o jogo
            // conclui, corretamente, que nao ha nada a animar. Uma constante aqui
            // congela o jogo e a causa e invisivel (nada falha, o frame so nao
            // muda).
            //
            // Em vez de codigo ARM fixo, instalamos um HOOK no host: cada leitura
            // devolve um instante maior que o anterior. O passo e 33 ms (~30 fps),
            // proximo do intervalo que o proprio jogo pede ao ISHELL_SetTimer.
            const u32 GETUPTIMEMS_STUB = SCRATCH + 0x2500;
            u32 getuptimems_code[2] = {
                0xe1a00000, // nop (mov r0,r0) — mantem o stub com 2 instrucoes,
                            // igual ao anterior, para que a contagem de
                            // instrucoes continue comparavel. r0 e sobrescrito
                            // pelo hook do host antes desta instrucao executar.
                0xe12fff1e  // bx lr
            };
            uc_mem_write(uc2, GETUPTIMEMS_STUB, getuptimems_code, sizeof(getuptimems_code));
            uc_mem_write(uc2, STATIC_BASE + 0xb0, &GETUPTIMEMS_STUB, 4);
            g_uptime_ms = 100; // mesmo instante inicial de antes

            // Hook do relogio: quando o PC chega no stub, preenche r0 com o
            // instante atual e avanca o contador. Hook proprio (o CallSiteWatch
            // ja esta ocupado pelo DrawRect) — nao interfere com ele.
            uc_hook clock_h = 0;
            uc_hook_add(uc2, &clock_h, UC_HOOK_CODE,
                        reinterpret_cast<void*>(&uptime_clock_hook),
                        nullptr, GETUPTIMEMS_STUB, GETUPTIMEMS_STUB);

            // Mock de memset (slot 0x04 em AEEHelperFuncs / static-base):
            // r0 = dest, r1 = val, r2 = len. Retorna r0 e faz bx lr.
            const u32 MEMSET_STUB = SCRATCH + 0x2600;
            u32 memset_code[2] = {
                0xe1a00000, // nop (ou mov r0, r0)
                0xe12fff1e  // bx lr
            };
            uc_mem_write(uc2, MEMSET_STUB, memset_code, sizeof(memset_code));
            uc_mem_write(uc2, STATIC_BASE + 0x04, &MEMSET_STUB, 4);

            // Atualiza o stub MALLOC para devolver ALLOC_BUF3 para alocações do loop
            uc_mem_write(uc2, MALLOC_STUB + 8, &ALLOC_BUF3, 4);

            // Em 0x12004944:
            //   0x12004940: mov r6, r0 (r6 = applet)
            //   0x12004944: add r0, r0, #64, #30  (#64 ror 30 = 64 * 4 = 256 = 0x100? Não: 64 >> 30 ou rotacionado:
            // No ARM, #64, #30 significa (64 >> 30) | (64 << 2) = 256 = 0x100!
            // Então r0 = r6 + 0x100.
            // Em 0x12004948: ldrh r1, [r0, #0x14] -> lê [applet + 0x100 + 0x14] = [applet + 0x114]!
            // Em 0x12004954: ldrh r0, [r0, #0x16] -> lê [applet + 0x100 + 0x16] = [applet + 0x116]!
            // E em 0x12004970: add r0, r6, #80, #30 (#80 ror 30 = 80 * 4 = 320 = 0x140).
            //   mov r4, r0 -> r4 = applet + 0x140!
            // E chama bl #0x12023a18 com r0 = r4 = applet + 0x140!
            // Então em 0x12023a24:
            //   ldr r1, [r0, #0xc] lê [applet + 0x140 + 0xc] = [applet + 0x14c]!
            //   ldrh r2, [r1, #0x14] lê [r1 + 0x14]!
            // Como [applet + 0x14c] estava 0, r1 virava 0 e [r1 + 0x14] dava fault @0x00000014!
            // Configurando [applet + 0x14c] para apontar para ENGINE_VIEWPORT_INFO:
            const u32 ENGINE_VIEWPORT_INFO = SCRATCH + 0x800;
            uint16_t vp_dims[2] = {640, 480}; // width=640, height=480
            uc_mem_write(uc2, ENGINE_VIEWPORT_INFO + 0x14, &vp_dims[0], 2);
            uc_mem_write(uc2, ENGINE_VIEWPORT_INFO + 0x16, &vp_dims[1], 2);

            // Escrever ponteiro no offset exato [applet + 0x14c]:
            uc_mem_write(uc2, created_applet_ptr + 0x14c, &ENGINE_VIEWPORT_INFO, 4);
            // Também configurar [applet + 0x140] (w[0] do sub-objeto) para apontar para DISPLAY_OBJ:
            uc_mem_write(uc2, created_applet_ptr + 0x140, &DISPLAY_OBJ, 4);

            // Em 0x12023a18 (chamado a partir de 0x12004978):
            // r0 na entrada de 0x12023a18 é r4 = applet + 0x140.
            // 0x12023a38: ldr r0, [r0]          (lê [applet + 0x140] = DISPLAY_OBJ)
            // 0x12023a3c: ldr r1, [r0]          (lê [DISPLAY_OBJ] = DISP_VTBL)
            // 0x12023a40: ldr r2, [r1, #0x48]   (lê DISP_VTBL[18] = GETDEST_STUB)
            // 0x12023a44: mov r1, #0
            // 0x12023a48: bx r2
            // MAS 0x12023a4c NÃO É O RETORNO de 0x12023a48!
            // Em ARM, 'bx r2' sem 'mov lr, pc' é um tail-call / salto terminal!
            // Então 0x12023a48: bx r2 retorna diretamente para o chamador de 0x12023a18 (que é 0x1200497c)!
            // E 0x12023a4c é o INÍCIO DE OUTRA FUNÇÃO!
            // Vamos inspecionar 0x12004978 e ver para onde o fluxo vai.
            const u32 GETDEST_STUB = SCRATCH + 0x2700;
            u32 getdest_code[2] = {
                0xe3a00000, // mov r0, #0 (ou ponteiro)
                0xe12fff1e  // bx lr
            };
            uc_mem_write(uc2, GETDEST_STUB, getdest_code, sizeof(getdest_code));
            uc_mem_write(uc2, DISP_VTBL + 0x48, &GETDEST_STUB, 4);

            // Observado (instr=174, last_pc=0x12023a74, ip=0x00000000, r0=0x00300500):
            //   0x12023a5c: ldr r2, [r0]        (r0 = DISPLAY_OBJ  =>  r2 = DISP_VTBL)
            //   0x12023a64: ldr ip, [r2, #0x14] (DISP_VTBL[5], ainda nao instalado => ip = 0)
            //   0x12023a68: mvn r2, #0          (arg = -1, cor/clip "tudo")
            //   0x12023a70: mov lr, pc ; 0x12023a74: bx ip
            // Portanto a proxima dependencia do tick e o slot 5 da vtable de IDisplay.
            // Stub minimo: retorna 0 (AEE_SUCCESS) e volta por lr.
            const u32 DISP_DRAWRECT_STUB = SCRATCH + 0x2900;
            u32 disp_drawrect_code[2] = {
                0xe3a00000, // mov r0, #0
                0xe12fff1e  // bx lr
            };
            uc_mem_write(uc2, DISP_DRAWRECT_STUB, disp_drawrect_code, sizeof(disp_drawrect_code));
            uc_mem_write(uc2, DISP_VTBL + 0x14, &DISP_DRAWRECT_STUB, 4);

            // Observado (instr=199, last_pc=0x120244c8):
            //   0x120244b4: ldr r0, [r0]      (r0 = DISPLAY_OBJ)
            //   0x120244b8: ldr r2, [r0]      (r2 = DISP_VTBL)
            //   0x120244bc: ldr r3, [r2, #0x28] (DISP_VTBL[10], nao instalado => r3 = 0)
            //   0x120244c4: mov r1, #1 ; 0x120244c8: bx r3
            // Proxima dependencia: slot 10 da vtable de IDisplay (tail-call, volta por lr do caller).
            const u32 DISP_SETCOLOR_STUB = SCRATCH + 0x2980;
            u32 disp_setcolor_code[2] = {
                0xe3a00000, // mov r0, #0
                0xe12fff1e  // bx lr
            };
            uc_mem_write(uc2, DISP_SETCOLOR_STUB, disp_setcolor_code, sizeof(disp_setcolor_code));
            uc_mem_write(uc2, DISP_VTBL + 0x28, &DISP_SETCOLOR_STUB, 4);

            // Observado (instr=237, last_pc=0x12023b08, r0=0x1204df18 = ponteiro para .rodata do mod):
            //   0x12023af8: ldr r0, [r6, #-4]   (static base)
            //   0x12023b00: ldr r1, [r0, #0x14] (slot 0x14 de AEEHelperFuncs, nao instalado => 0)
            //   0x12023b04: mov r0, r4          (r4 = string do modulo)
            //   0x12023b08: bx r1
            //   0x12023b0c: add r1, r0, #1      (len + 1)  => slot 0x14 == STRLEN
            // Stub real (nao constante): percorre a string ate NUL e devolve o comprimento.
            const u32 STRLEN_STUB = SCRATCH + 0x2a00;
            u32 strlen_code[7] = {
                0xe1a01000, // mov  r1, r0
                0xe5d12000, // ldrb r2, [r1]
                0xe3520000, // cmp  r2, #0
                0x12811001, // addne r1, r1, #1
                0x1afffffb, // bne  volta para o ldrb (pc+8-12)
                0xe0410000, // sub  r0, r1, r0
                0xe12fff1e  // bx   lr
            };
            uc_mem_write(uc2, STRLEN_STUB, strlen_code, sizeof(strlen_code));
            uc_mem_write(uc2, STATIC_BASE + 0x14, &STRLEN_STUB, 4);

            // Observado (instr=344, last_pc=0x12023b28):
            //   0x12023b0c: add r1, r0, #1     (len + 1, resultado do STRLEN)
            //   0x12023b18: ldr ip, [r0, #0xe4] (slot 0xe4 de AEEHelperFuncs, nao instalado => 0)
            //   0x12023b1c: mov r0, r4          (src = string ASCII)
            //   0x12023b20: add r2, sp, #0x10   (dst = buffer na pilha)
            //   0x12023b24: mov lr, pc ; bx ip  => assinatura (src, nChars, dst, nSize=0x200)
            // Perfil compativel com STRTOWSTR (ASCII -> UTF-16) do BREW.
            // Stub real: copia byte a byte para halfwords ate o NUL, devolve dst.
            const u32 STRTOWSTR_STUB = SCRATCH + 0x2a80;
            u32 strtowstr_code[9] = {
                0xe1a0c002, // mov   ip, r2        (dst corrente)
                0xe4d03001, // ldrb  r3, [r0], #1  (le byte e avanca src)
                0xe0cc30b2, // strh  r3, [ip], #2  (grava halfword e avanca dst)
                0xe3530000, // cmp   r3, #0
                0x1afffffb, // bne   volta ao ldrb
                0xe1a00002, // mov   r0, r2        (retorna dst)
                0xe12fff1e  // bx    lr
            };
            uc_mem_write(uc2, STRTOWSTR_STUB, strtowstr_code, 7 * 4);
            uc_mem_write(uc2, STATIC_BASE + 0xe4, &STRTOWSTR_STUB, 4);

            // Observado (instr=460, last_pc=0x00302204 = DISP_OFF10_DIMS_STUB+4, fault write @0x00008004):
            // No tick o slot 4 de IDisplay e reinvocado com r1 = 0x00008000, que NAO e um
            // ponteiro de saida valido no nosso harness (o construtor passava um buffer de pilha).
            // Ou seja: o slot 4 aqui tem outra semantica (parametro escalar, nao out-param).
            // Para nao falsificar dados, o stub passa a escrever apenas quando r1 aponta para
            // a regiao SCRATCH mapeada; caso contrario apenas retorna 0.
            u32 disp_off10_dims_code2[10] = {
                0xe59f3018, // ldr  r3, [pc, #24]  -> 0x00300000 (SCRATCH)
                0xe1510003, // cmp  r1, r3
                0x3a000003, // bcc  pula escrita
                0xe59f3014, // ldr  r3, [pc, #20]  -> 0x00400000 (fim da janela)
                0xe1510003, // cmp  r1, r3
                0x25812004, // strcs? (nao) -> usa cc: escreve se r1 < 0x400000
                0xe3a00000, // mov  r0, #0
                0xe12fff1e, // bx   lr
                0x00300000,
                0x00400000
            };
            // corrige a instrucao condicional de escrita: strcc r2, [r1, #4]
            disp_off10_dims_code2[5] = 0x35812004; // strcc r2, [r1, #4]
            // r2 precisa conter o valor de dimensoes antes da escrita
            u32 disp_off10_dims_code3[12] = {
                0xe59f2024, // ldr  r2, [pc, #36] -> 0x01e00280
                0xe59f3024, // ldr  r3, [pc, #36] -> 0x00300000
                0xe1510003, // cmp  r1, r3
                0x3a000004, // bcc  fim
                0xe59f301c, // ldr  r3, [pc, #28] -> 0x00400000
                0xe1510003, // cmp  r1, r3
                0x35812004, // strcc r2, [r1, #4]
                0xe3a00000, // mov  r0, #0
                0xe12fff1e, // bx   lr
                0x01e00280,
                0x00300000,
                0x00400000
            };
            (void)disp_off10_dims_code2;
            uc_mem_write(uc2, DISP_OFF10_DIMS_STUB, disp_off10_dims_code3, sizeof(disp_off10_dims_code3));

            // Observado (instr=1571, last_pc=0x12024538, r0=DISPLAY_OBJ, ip=DISPLAY_OBJ):
            //   0x12024528: ldr r0, [r0]        (r0 = DISPLAY_OBJ)
            //   0x1202452c: ldr r1, [r0]        (r1 = DISP_VTBL)
            //   0x12024530: ldr r2, [r1, #0x1c] (DISP_VTBL[7], nao instalado => r2 = 0)
            //   0x12024534: mov r1, #1 ; 0x12024538: bx r2   (tail-call)
            // Proxima dependencia: slot 7 da vtable de IDisplay.
            const u32 DISP_UPDATE_STUB = SCRATCH + 0x2b00;
            u32 disp_update_code[2] = {
                0xe3a00000, // mov r0, #0
                0xe12fff1e  // bx lr
            };
            uc_mem_write(uc2, DISP_UPDATE_STUB, disp_update_code, sizeof(disp_update_code));
            uc_mem_write(uc2, DISP_VTBL + 0x1c, &DISP_UPDATE_STUB, 4);

            // ── DD3 Game Loop: disparar o callback do timer (0x120239dc) ──
            // O timer callback registrado pelo applet é uma função C que recebe pUser (applet) em r0:
            // void (*PFNNOTIFY)(void *pUser)
            // ── DD4 Bloco 2: framebuffer real alimentado por DrawRect ──
            // O call-site 0x12023a74 (bx ip) chama IDisplay::DrawRect. Capturamos
            // os argumentos exatamente como o guest os montou e EXECUTAMOS a
            // operacao num framebuffer do host.
            //   void DrawRect(IDisplay*, const AEERect* pRect, RGBVAL clrFrame,
            //                 RGBVAL clrFill, uint32 dwFlags)
            //   r0=this  r1=pRect  r2=clrFrame  r3=clrFill  [sp]=dwFlags
            //
            // Valores medidos na 1a chamada (nao supostos):
            //   this=0x00300500 (DISPLAY_OBJ)  pRect=NULL (tela toda)
            //   clrFrame=0xffffffff  clrFill=0xffffff00  dwFlags=0x2
            // 0xffffffff e RGB_NONE (AEERGBVAL.h) e 0x2 e IDF_RECT_FILL
            // (AEEIDisplay.h) — exatamente a expansao do inline IDisplay_FillRect
            // do SDK. Isso CORROBORA a identificacao do slot 0x14 de forma
            // independente da conferencia de aridade.
            const int FB_W = 640, FB_H = 480;
            struct FrameBuffer {
                int w = 0, h = 0;
                std::vector<u32> px;     // XRGB do host, 1 word por pixel
                int fills = 0;           // quantos DrawRect efetivamente pintaram
            };
            struct DrawRectCapture {
                int calls = 0;
                u32 r0 = 0, r1 = 0, r2 = 0, r3 = 0, flags = 0, sp = 0;
                FrameBuffer* fb = nullptr;
            } cap;
            FrameBuffer fb;
            fb.w = FB_W; fb.h = FB_H;
            fb.px.assign(static_cast<size_t>(FB_W) * FB_H, 0u);
            cap.fb = &fb;

            CallSiteWatch watch;
            watch.va   = 0x12023a74;
            watch.user = &cap;
            watch.fn   = [](uc_engine* u, void* p) {
                auto* c = static_cast<DrawRectCapture*>(p);
                u32 r0 = 0, r1 = 0, r2 = 0, r3 = 0, sp = 0, flags = 0;
                uc_reg_read(u, UC_ARM_REG_R0, &r0);
                uc_reg_read(u, UC_ARM_REG_R1, &r1);
                uc_reg_read(u, UC_ARM_REG_R2, &r2);
                uc_reg_read(u, UC_ARM_REG_R3, &r3);
                uc_reg_read(u, UC_ARM_REG_SP, &sp);
                uc_mem_read(u, sp, &flags, 4);
                if (c->calls == 0) {   // guarda a 1a para o relatorio
                    c->r0 = r0; c->r1 = r1; c->r2 = r2; c->r3 = r3;
                    c->sp = sp; c->flags = flags;
                }
                ++c->calls;

                auto* fbp = c->fb;
                if (!fbp) return;

                // Retangulo: pRect NULL = tela toda; senao AEERect{int16 x,y,dx,dy}.
                int x = 0, y = 0, dx = fbp->w, dy = fbp->h;
                if (r1 != 0) {
                    int16_t v[4] = {0, 0, 0, 0};
                    if (uc_mem_read(u, r1, v, sizeof(v)) != UC_ERR_OK) return;
                    x = v[0]; y = v[1]; dx = v[2]; dy = v[3];
                }
                // Recorte ao framebuffer (o guest pode pedir fora da tela).
                int x0 = x < 0 ? 0 : x;
                int y0 = y < 0 ? 0 : y;
                int x1 = x + dx; if (x1 > fbp->w) x1 = fbp->w;
                int y1 = y + dy; if (y1 > fbp->h) y1 = fbp->h;
                if (x0 >= x1 || y0 >= y1) return;

                const u32 IDF_RECT_FILL = 0x2;
                const u32 RGB_NONE      = 0xffffffffu;
                if ((flags & IDF_RECT_FILL) && r3 != RGB_NONE) {
                    // RGBVAL do BREW NAO e 0x00RRGGBB. AEERGBVAL.h:24 define
                    //   MAKE_RGB(r,g,b) = (r<<8) | (g<<16) | (b<<24)
                    // ou seja: o byte MENOS significativo e alfa, e os canais
                    // ficam deslocados 8 bits para cima. Decodificar como
                    // 0xRRGGBB troca os canais (o branco vira amarelo).
                    const u32 cr = (r3 >> 8)  & 0xff;
                    const u32 cg = (r3 >> 16) & 0xff;
                    const u32 cb = (r3 >> 24) & 0xff;
                    const u32 xrgb = (cr << 16) | (cg << 8) | cb;
                    for (int yy = y0; yy < y1; ++yy)
                        for (int xx = x0; xx < x1; ++xx)
                            fbp->px[static_cast<size_t>(yy) * fbp->w + xx] = xrgb;
                    ++fbp->fills;
                }
            };

            const u32 timer_cb_fn = 0x120239dc;
            FirstPcResult r_tick = run_first_pc(uc2, timer_cb_fn,
                                                0x12000000,
                                                mod.size(),
                                                0x00200000,
                                                10000,
                                                false,
                                                created_applet_ptr, // r0: pUser (applet)
                                                0, 0, 0,
                                                &watch);
            std::fprintf(stderr,
                "[DD4/drawrect] chamadas=%d this=0x%08x pRect=0x%08x clrFrame=0x%08x clrFill=0x%08x dwFlags=0x%08x fills=%d\n",
                cap.calls, cap.r0, cap.r1, cap.r2, cap.r3, cap.flags, fb.fills);
            std::fprintf(stderr, "[DD1-runtime/probe] TimerCallback(0x%08x): ran=%s entered=%s last_pc=0x%08x instr=%lu fault=%s @0x%08x\n",
                         timer_cb_fn,
                         r_tick.ran ? "SIM" : "nao",
                         r_tick.entered_module ? "SIM" : "nao",
                         r_tick.last_pc,
                         (unsigned long)r_tick.instructions,
                         fault_label(r_tick.fault),
                         r_tick.fault_va);
            u32 r_tick_r0 = 0, r_tick_r2 = 0, r_tick_ip = 0;
            uc_reg_read(uc2, UC_ARM_REG_R0, &r_tick_r0);
            uc_reg_read(uc2, UC_ARM_REG_R2, &r_tick_r2);
            uc_reg_read(uc2, UC_ARM_REG_IP, &r_tick_ip);
            std::fprintf(stderr, "[DD1-runtime/probe] regs at fault: r0=0x%08x r2=0x%08x ip=0x%08x\n",
                         r_tick_r0, r_tick_r2, r_tick_ip);
            assert(r_tick.ran && r_tick.entered_module);
            // MARCO DD3: o tick do game loop executa do inicio ao fim e RETORNA ao sentinela,
            // sem falha de memoria. Antes parava em dependencias ausentes (150/174/199/237/344/460/1571).
            assert(r_tick.fault == FAULT_NONE);
            assert(r_tick.instructions == 1587);

            // ── MARCO DD4 (parcial): o jogo PINTOU pixels ──
            // Nao e clear sintetico do harness: a cor sai de clrFill montado pelo
            // guest e a geometria sai do pRect que ele passou.
            assert(cap.calls >= 1);          // DrawRect foi mesmo chamado
            assert(fb.fills >= 1);           // e resultou em preenchimento real

            // O frame nao pode continuar todo zero (o buffer nasce zerado).
            size_t nonzero = 0;
            for (u32 v : fb.px) if (v != 0) ++nonzero;
            assert(nonzero > 0);
            // pRect=NULL => tela toda: todos os pixels devem ter a cor do guest.
            assert(nonzero == fb.px.size());

            // A cor tem de ser a que o GUEST pediu, nao uma constante nossa.
            // Comparamos ja no espaco XRGB do host, aplicando a MESMA decodificacao
            // de RGBVAL (AEERGBVAL.h: r<<8 | g<<16 | b<<24).
            const u32 exp_r = (cap.r3 >> 8)  & 0xff;
            const u32 exp_g = (cap.r3 >> 16) & 0xff;
            const u32 exp_b = (cap.r3 >> 24) & 0xff;
            const u32 exp_xrgb = (exp_r << 16) | (exp_g << 8) | exp_b;
            assert(fb.px[0] == exp_xrgb);
            // E precisa ser uma cor de verdade, nao RGB_NONE nem preto.
            assert(cap.r3 != 0xffffffffu && cap.r3 != 0u);
            // O guest pediu RGB_WHITE = MAKE_RGB(0xff,0xff,0xff) = 0xffffff00.
            // Decodificado, tem de dar branco — se der amarelo (0xffff00), a
            // decodificacao de canais esta trocada.
            assert(exp_xrgb == 0x00ffffffu);

            std::fprintf(stderr,
                "[DD4/frame] %dx%d  pixels_pintados=%zu/%zu  cor=0x%08x (do guest)  fills=%d\n",
                fb.w, fb.h, nonzero, fb.px.size(), cap.r3, fb.fills);

            // Entregavel visual: PPM binario (P6). Sem dependencia externa, e
            // conversivel a PNG com qualquer ferramenta.
            if (const char* outp = std::getenv("ZEEBO_DD4_FRAME")) {
                if (FILE* f = std::fopen(outp, "wb")) {
                    std::fprintf(f, "P6\n%d %d\n255\n", fb.w, fb.h);
                    for (u32 v : fb.px) {
                        unsigned char rgb[3] = {
                            static_cast<unsigned char>((v >> 16) & 0xff),
                            static_cast<unsigned char>((v >> 8) & 0xff),
                            static_cast<unsigned char>(v & 0xff)};
                        std::fwrite(rgb, 1, 3, f);
                    }
                    std::fclose(f);
                    std::fprintf(stderr, "[DD4/frame] gravado em %s\n", outp);
                }
            }

            // ── DD4 Bloco 3: o frame EVOLUI entre ticks? ──
            // Ate aqui medimos UM tick. Se o jogo so souber pintar o fundo, todo
            // tick produz o mesmo frame e nao ha animacao — sinal de dependencia
            // faltando. Rodamos ticks sucessivos no MESMO estado de guest (uc2
            // preserva memoria entre chamadas) e comparamos os frames.
            {
                const int N_TICKS = 8;
                std::vector<u64> digests;
                std::vector<int>  calls_por_tick;
                std::vector<u64>  instr_por_tick;
                int faults = 0;

                for (int t = 0; t < N_TICKS; ++t) {
                    const int calls_antes = cap.calls;
                    // Zera o frame a cada tick: queremos o que ESTE tick desenha.
                    std::fill(fb.px.begin(), fb.px.end(), 0u);
                    fb.fills = 0;

                    FirstPcResult rt = run_first_pc(uc2, timer_cb_fn,
                                                    0x12000000, mod.size(),
                                                    0x00200000, 10000, false,
                                                    created_applet_ptr,
                                                    0, 0, 0, &watch);
                    if (rt.fault != FAULT_NONE) ++faults;
                    instr_por_tick.push_back(rt.instructions);
                    calls_por_tick.push_back(cap.calls - calls_antes);

                    // FNV-1a sobre o frame: barato e sensivel a 1 pixel.
                    u64 h = 1469598103934665603ull;
                    for (u32 v : fb.px) {
                        h ^= v; h *= 1099511628211ull;
                    }
                    digests.push_back(h);
                }

                std::fprintf(stderr, "[DD4/ticks] %d ticks:", N_TICKS);
                for (int t = 0; t < N_TICKS; ++t)
                    std::fprintf(stderr, " #%d(instr=%llu,draw=%d)", t,
                                 (unsigned long long)instr_por_tick[t],
                                 calls_por_tick[t]);
                std::fprintf(stderr, "\n");

                size_t distintos = 0;
                for (size_t i = 0; i < digests.size(); ++i) {
                    bool novo = true;
                    for (size_t j = 0; j < i; ++j)
                        if (digests[j] == digests[i]) { novo = false; break; }
                    if (novo) ++distintos;
                }
                // Quais slots de IDisplay o tick REALMENTE chama? Se o frame nao
                // muda, ou o jogo nao esta pedindo mais nada, ou esta pedindo
                // algo que nao stubamos (e ai o retorno 0 padrao mente para ele).
                // Instrumentamos a vtable inteira: cada entrada ainda nao
                // instalada aponta para um stub proprio que apenas registra.
                std::fprintf(stderr, "[DD4/slots] vtable IDisplay em uso:");
                for (u32 off = 0; off < 26 * 4; off += 4) {
                    u32 target = 0;
                    uc_mem_read(uc2, DISP_VTBL + off, &target, 4);
                    if (target != 0)
                        std::fprintf(stderr, " 0x%02x", off);
                }
                std::fprintf(stderr, "\n");

                // Detector de slot faltante: preenche TODA entrada vazia da
                // vtable com um stub que registra o offset chamado. Se o jogo
                // estiver pedindo algo que nao implementamos, ele aparece aqui.
                // Sem isto, a entrada vazia devolve 0 e o jogo segue achando que
                // funcionou — falha silenciosa, exatamente o modo de erro que
                // deixa o frame parado sem nada quebrar.
                for (u32 off = 0; off < 26 * 4; off += 4) {
                    u32 target = 0;
                    uc_mem_read(uc2, DISP_VTBL + off, &target, 4);
                    if (target != 0) continue;
                    const u32 probe = SCRATCH + 0x3000 + off * 4;
                    u32 code[2] = { 0xe3a00000 /* mov r0,#0 */,
                                    0xe12fff1e /* bx lr */ };
                    uc_mem_write(uc2, probe, code, sizeof(code));
                    uc_mem_write(uc2, DISP_VTBL + off, &probe, 4);
                    uc_hook h = 0;
                    uc_hook_add(uc2, &h, UC_HOOK_CODE,
                                reinterpret_cast<void*>(&missing_slot_hook),
                                nullptr, probe, probe);
                    g_probe_off[probe] = off;
                }

                g_missing.clear();
                std::fill(fb.px.begin(), fb.px.end(), 0u);
                run_first_pc(uc2, timer_cb_fn, 0x12000000, mod.size(),
                             0x00200000, 10000, false, created_applet_ptr,
                             0, 0, 0, &watch);

                std::fprintf(stderr, "[DD4/faltantes] slots chamados sem stub:");
                if (g_missing.empty()) std::fprintf(stderr, " (nenhum)");
                for (auto& kv : g_missing)
                    std::fprintf(stderr, " 0x%02x(x%d)", kv.first, kv.second);
                std::fprintf(stderr, "\n");

                std::fprintf(stderr,
                    "[DD4/ticks] frames distintos=%zu/%d  faults=%d  "
                    "leituras_do_relogio=%d  uptime_final=%u ms\n",
                    distintos, N_TICKS, faults, g_uptime_reads, g_uptime_ms);

                // Fato observado, registrado sem exagero: todos os ticks rodam.
                assert(faults == 0);
                assert(calls_por_tick[0] >= 1);
            }

            uc_close(uc2);
        }
    }

    std::printf("=== Test DD1a first-PC assisted diagnostic: PASS (hybrid/assisted) ===\n");
    return 0;
}
