// test_zeetris_full_lifecycle.cpp — Executa o ciclo de vida real completo do Zeetris:
//   (1) AEEMod_Load (0x12000048) -> inicializa IModule, aloca 36B via malloc, monta vtable IModule.
//   (2) IModule::CreateInstance (0x1200e114) -> cria applet do Zeetris (CLSID 0x12345678), aloca 88B.
//   (3) IApplet::HandleEvent (0x1200aa88) -> despacha EVT_APP_START (1) e eventos de controle.
//
// TDD com controles positivos, negativos e mutantes. Sem Dynarmic.
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

static constexpr u32 ENTRY_VA        = 0x12000048u; // AEEMod_Load
static constexpr u32 LOAD_VA         = 0x12000000u;
static constexpr u32 STK             = 0x00200000u;
static constexpr u32 MOD_SIZE        = 3939404u;
static constexpr u32 ZEETRIS_CLSID   = 0x12345678u;
static constexpr u32 CREATE_CLSID    = 0x0106e415u;

// Região de heap / static_base
static constexpr u32 STATIC_BASE_VA  = 0x30000000u;
static constexpr u32 MALLOC_STUB_VA  = 0x30002000u;
static constexpr u32 HEAP_START_VA   = 0x30005000u;

// Objeto criado por IShell::CreateInstance(0x0106e415)
static constexpr u32 EXTRA_OBJ_VA    = 0x60000000u;
static constexpr u32 EXTRA_VTBL_VA   = 0x60001000u;

// Objeto IDisplay criado por IShell::CreateInstance(0x01001001)
static constexpr u32 DISPLAY_OBJ_VA  = 0x60002000u;
static constexpr u32 DISPLAY_VTBL_VA = 0x60003000u;

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

struct ZeetrisCtx {
    ish::IShellEnv env{};
    u32 next_alloc = HEAP_START_VA;
    u32 pModule    = 0;
    u32 pApplet    = 0;
    u32 handle_event_va = 0;
    bool evt_app_start_handled = false;
    bool ishell_slot11_settimer = false;
    bool ishell_slot12_canceltimer = false;
    u32 timer_ms = 0;
    u32 timer_cb = 0;
    u32 timer_user = 0;
    int  malloc_calls = 0;
    bool force_fail_malloc = false;
};

static bool mem_hook(uc_engine* uc, uc_mem_type type, uint64_t address, int size, int64_t value, void* user) {
    (void)uc; (void)size; (void)value; (void)user;
    std::printf("[UNMAPPED ACCESS] type=%d addr=0x%08llx\n", type, (unsigned long long)address);
    std::fflush(stdout);
    return false;
}

static void malloc_handler(uc_engine* uc, uint64_t, uint32_t, void* user) {
    auto* ctx = static_cast<ZeetrisCtx*>(user);
    ctx->malloc_calls++;
    if (ctx->force_fail_malloc) {
        u32 zero = 0, lr = 0;
        uc_reg_read(uc, UC_ARM_REG_LR, &lr);
        uc_reg_write(uc, UC_ARM_REG_R0, &zero);
        uc_reg_write(uc, UC_ARM_REG_PC, &lr);
        return;
    }
    u32 size = 0, lr = 0;
    uc_reg_read(uc, UC_ARM_REG_R0, &size);
    uc_reg_read(uc, UC_ARM_REG_LR, &lr);
    u32 ret = ctx->next_alloc;
    ctx->next_alloc = (ctx->next_alloc + size + 15u) & ~15u;
    uc_reg_write(uc, UC_ARM_REG_R0, &ret);
    uc_reg_write(uc, UC_ARM_REG_PC, &lr);
}

static void code_hook(uc_engine* uc, uint64_t address, uint32_t, void* user) {
    auto* ctx = static_cast<ZeetrisCtx*>(user);
    u32 pc = static_cast<u32>(address);
    if (pc < 0x1000u) {
        u32 lr = 0, r0 = 0, r1 = 0, r2 = 0;
        uc_reg_read(uc, UC_ARM_REG_LR, &lr);
        uc_reg_read(uc, UC_ARM_REG_R0, &r0);
        uc_reg_read(uc, UC_ARM_REG_R1, &r1);
        uc_reg_read(uc, UC_ARM_REG_R2, &r2);
        std::printf("[LOW PC REACHED] pc=0x%08x lr=0x%08x r0=0x%08x r1=0x%08x r2=0x%08x\n",
                    pc, lr, r0, r1, r2);
        std::fflush(stdout);
    }
    int s = ish::slot_of(ctx->env, pc);
    if (s >= 0) {
        u32 lr = 0;
        uc_reg_read(uc, UC_ARM_REG_LR, &lr);
        if (s == 2) { // ISHELL_CreateInstance(pIShell, clsid, ppObj)
            u32 clsid = 0, ppObj = 0;
            uc_reg_read(uc, UC_ARM_REG_R1, &clsid);
            uc_reg_read(uc, UC_ARM_REG_R2, &ppObj);
            std::printf("ISHELL_CreateInstance called: clsid=0x%08x ppObj=0x%08x\n", clsid, ppObj);
            u32 obj = 0;
            if (clsid == 0x01001001) {
                obj = DISPLAY_OBJ_VA;
            } else {
                obj = EXTRA_OBJ_VA;
            }
            uc_mem_write(uc, ppObj, &obj, 4);
            u32 zero = 0;
            uc_reg_write(uc, UC_ARM_REG_R0, &zero);
            uc_reg_write(uc, UC_ARM_REG_PC, &lr);
            return;
        } else if (s == 11) { // ISHELL_SetTimer(IShell*, ms, pfnNotify, pUser)
            ctx->ishell_slot11_settimer = true;
            uc_reg_read(uc, UC_ARM_REG_R1, &ctx->timer_ms);
            uc_reg_read(uc, UC_ARM_REG_R2, &ctx->timer_cb);
            uc_reg_read(uc, UC_ARM_REG_R3, &ctx->timer_user);
            u32 zero = 0;
            uc_reg_write(uc, UC_ARM_REG_R0, &zero);
            uc_reg_write(uc, UC_ARM_REG_PC, &lr);
            return;
        } else if (s == 12) { // ISHELL_CancelTimer
            ctx->ishell_slot12_canceltimer = true;
            u32 r1 = 0, r2 = 0;
            uc_reg_read(uc, UC_ARM_REG_R1, &r1);
            uc_reg_read(uc, UC_ARM_REG_R2, &r2);
            std::printf("ISHELL_CancelTimer called: r1=0x%08x r2=0x%08x\n", r1, r2);
            u32 zero = 0;
            uc_reg_write(uc, UC_ARM_REG_R0, &zero);
            uc_reg_write(uc, UC_ARM_REG_PC, &lr);
            return;
        } else {
            u32 zero = 0;
            uc_reg_write(uc, UC_ARM_REG_R0, &zero);
            uc_reg_write(uc, UC_ARM_REG_PC, &lr);
            return;
        }
    } else if (pc >= 0x50000100u && pc < 0x50000300u) {
        // Objetos EXTRA (0x0106e415) e DISPLAY (0x01001001)
        u32 lr = 0, zero = 0;
        uc_reg_read(uc, UC_ARM_REG_LR, &lr);
        uc_reg_write(uc, UC_ARM_REG_R0, &zero);
        uc_reg_write(uc, UC_ARM_REG_PC, &lr);
        return;
    }
}

int main(int argc, char** argv) {
    const bool BUGGY = (argc > 1 && std::string(argv[1]) == "buggy");
    std::printf("=== Test Zeetris Full Lifecycle (Mod_Load -> CreateInstance -> HandleEvent)%s ===\n",
                BUGGY ? " [MUTANT]" : "");

    const char* menv = std::getenv("ZEETRIS_MOD");
    const std::string mod_path =
        menv ? menv : "/home/rafaelfrequiao/Downloads/mod/zeetris/zeetris.mod";
    std::vector<u8> mod = read_file(mod_path);
    if (mod.empty()) {
        std::printf("[SKIP] zeetris.mod ausente ('%s').\n", mod_path.c_str());
        return 77;
    }
    assert(mod.size() == MOD_SIZE);

    // ── (1) Execução Positiva: ciclo de vida completo ────────────────────────
    {
        ZeetrisCtx ctx{};
        uc_engine* uc = nullptr;
        assert(uc_open(UC_ARCH_ARM, UC_MODE_ARM, &uc) == UC_ERR_OK);

        // Mapeia regiões necessárias
        uc_mem_map(uc, 0x001F0000, 0x10000, UC_PROT_ALL);
        uc_mem_map(uc, 0x11FFF000, 0x1000, UC_PROT_ALL);
        uc_mem_map(uc, STATIC_BASE_VA, 0x100000, UC_PROT_ALL);
        uc_mem_map(uc, EXTRA_OBJ_VA, 0x10000, UC_PROT_ALL);
        // Região para variáveis globais do módulo zeetris (.bss / .data @ 0x003c0000 ou 0x123c0000)
        uc_mem_map(uc, 0x003C0000, 0x20000, UC_PROT_ALL);
        uc_mem_map(uc, 0x123C0000, 0x20000, UC_PROT_ALL);

        // Instala IShell sintético em 0x30000000
        u32 obj = ish::install(uc, ctx.env);
        assert(obj == ctx.env.obj_va);

        // Objeto EXTRA (0x0106e415) vtable
        u32 extra_vt = EXTRA_VTBL_VA;
        uc_mem_write(uc, EXTRA_OBJ_VA, &extra_vt, 4);
        for (u32 i = 0; i < 64; ++i) {
            u32 s = 0x50000100u + i * 4u;
            uc_mem_write(uc, EXTRA_VTBL_VA + i * 4u, &s, 4);
        }

        // Objeto DISPLAY (0x01001001) vtable
        u32 disp_vt = DISPLAY_VTBL_VA;
        uc_mem_write(uc, DISPLAY_OBJ_VA, &disp_vt, 4);
        for (u32 i = 0; i < 64; ++i) {
            u32 s = 0x50000200u + i * 4u;
            uc_mem_write(uc, DISPLAY_VTBL_VA + i * 4u, &s, 4);
        }

        // Slot 0xb0 de STATIC_BASE: GetUpTimeMs
        // mov r0, #100; bx lr
        const u32 GETUPTIMEMS_STUB_VA = 0x30002100u;
        u32 getuptime_code[2] = { 0xe3a00064, 0xe12fff1e };
        uc_mem_write(uc, GETUPTIMEMS_STUB_VA, getuptime_code, sizeof(getuptime_code));
        u32 getuptime_ptr = GETUPTIMEMS_STUB_VA;
        uc_mem_write(uc, STATIC_BASE_VA + 0xb0, &getuptime_ptr, 4);

        // Malloc stub em STATIC_BASE_VA + 0x68
        u32 bx_lr = 0xe12fff1e;
        uc_mem_write(uc, MALLOC_STUB_VA, &bx_lr, 4);
        u32 malloc_stub_ptr = MALLOC_STUB_VA;
        uc_mem_write(uc, STATIC_BASE_VA + 0x68, &malloc_stub_ptr, 4);

        // Free stub em STATIC_BASE_VA + 0x6c
        const u32 FREE_STUB_VA = 0x30002080u;
        uc_mem_write(uc, FREE_STUB_VA, &bx_lr, 4);
        u32 free_stub_ptr = FREE_STUB_VA;
        uc_mem_write(uc, STATIC_BASE_VA + 0x6c, &free_stub_ptr, 4);

        // Apontadores para static_base no prefixo de carga
        // ATENÇÃO: inject_bytes escreve 0 em module_base-4 e -8 se prepare_elf2mod_prefix
        // for chamado antes ou depois. Portanto, colocamos após a injeção do módulo!
        BrewLoader ld(uc);
        assert(ld.inject_bytes(mod, LOAD_VA, ZEETRIS_CLSID, "zeetris.mod"));

        u32 sb = STATIC_BASE_VA;
        uc_mem_write(uc, LOAD_VA - 8, &sb, 4);
        uc_mem_write(uc, LOAD_VA - 4, &sb, 4);

        // Hooks
        uc_hook h_malloc = 0, h_code = 0, h_mem = 0;
        uc_hook_add(uc, &h_malloc, UC_HOOK_CODE, (void*)malloc_handler, &ctx,
                    MALLOC_STUB_VA, MALLOC_STUB_VA);
        uc_hook_add(uc, &h_code, UC_HOOK_CODE, (void*)code_hook, &ctx, 1, 0);
        uc_hook_add(uc, &h_mem, UC_HOOK_MEM_UNMAPPED, (void*)mem_hook, &ctx, 1, 0);

        // ETAPA 1: AEEMod_Load
        u32 ppMod = 0x001fffd0;
        u32 z = 0, sp = STK, lr = 0xF0F0F0F0u;
        uc_reg_write(uc, UC_ARM_REG_R0, &obj);
        uc_reg_write(uc, UC_ARM_REG_R1, &z);
        uc_reg_write(uc, UC_ARM_REG_R2, &ppMod);
        uc_reg_write(uc, UC_ARM_REG_SP, &sp);
        uc_reg_write(uc, UC_ARM_REG_LR, &lr);

        uc_err err = uc_emu_start(uc, ENTRY_VA, 0xF0F0F0F0u, 0, 500000);
        if (err != UC_ERR_OK) {
            u32 pc = 0, sp_val = 0;
            uc_reg_read(uc, UC_ARM_REG_PC, &pc);
            uc_reg_read(uc, UC_ARM_REG_SP, &sp_val);
            std::printf("uc_emu_start failed: err=%d (%s) pc=0x%08x sp=0x%08x\n",
                        err, uc_strerror(err), pc, sp_val);
            std::fflush(stdout);
        }
        assert(err == UC_ERR_OK);
        u32 r0_ret = 0;
        uc_reg_read(uc, UC_ARM_REG_R0, &r0_ret);
        assert(r0_ret == 0); // AEE_SUCCESS
        uc_mem_read(uc, ppMod, &ctx.pModule, 4);
        assert(ctx.pModule != 0);

        // Verifica vtable de IModule
        u32 mod_vt = 0;
        uc_mem_read(uc, ctx.pModule, &mod_vt, 4);
        assert(mod_vt != 0);
        u32 mod_methods[4] = {0};
        uc_mem_read(uc, mod_vt, mod_methods, sizeof(mod_methods));
        assert(mod_methods[0] == 0x1200e0fc); // AddRef
        assert(mod_methods[1] == 0x1200e144); // Release
        assert(mod_methods[2] == 0x1200e114); // CreateInstance
        assert(mod_methods[3] == 0x1200e110); // FreeResources

        // ETAPA 2: IModule::CreateInstance (CLSID 0x12345678)
        u32 ppApplet = 0x001fffe0;
        u32 clsid = ZEETRIS_CLSID;
        uc_reg_write(uc, UC_ARM_REG_R0, &ctx.pModule);
        uc_reg_write(uc, UC_ARM_REG_R1, &obj);
        uc_reg_write(uc, UC_ARM_REG_R2, &clsid);
        uc_reg_write(uc, UC_ARM_REG_R3, &ppApplet);
        uc_reg_write(uc, UC_ARM_REG_SP, &sp);
        uc_reg_write(uc, UC_ARM_REG_LR, &lr);

        err = uc_emu_start(uc, mod_methods[2], 0xF0F0F0F0u, 0, 500000);
        if (err != UC_ERR_OK) {
            u32 cur_pc = 0, cur_sp = 0;
            uc_reg_read(uc, UC_ARM_REG_PC, &cur_pc);
            uc_reg_read(uc, UC_ARM_REG_SP, &cur_sp);
            std::printf("CreateInstance failed: err=%d (%s) pc=0x%08x sp=0x%08x\n",
                        err, uc_strerror(err), cur_pc, cur_sp);
            std::fflush(stdout);
        }
        assert(err == UC_ERR_OK);
        uc_reg_read(uc, UC_ARM_REG_R0, &r0_ret);
        assert(r0_ret == 0); // AEE_SUCCESS
        uc_mem_read(uc, ppApplet, &ctx.pApplet, 4);
        assert(ctx.pApplet != 0);

        // Verifica campos de AEEApplet
        u32 applet_clsid = 0, applet_pShell = 0, applet_pModule = 0;
        uc_mem_read(uc, ctx.pApplet + 0x04, &applet_clsid, 4);
        uc_mem_read(uc, ctx.pApplet + 0x0c, &applet_pShell, 4);
        uc_mem_read(uc, ctx.pApplet + 0x10, &applet_pModule, 4);
        uc_mem_read(uc, ctx.pApplet + 0x18, &ctx.handle_event_va, 4);

        assert(applet_clsid == ZEETRIS_CLSID);
        assert(applet_pShell == obj);
        assert(applet_pModule == ctx.pModule);
        assert(ctx.handle_event_va == 0x1200aa88); // HandleEvent do Zeetris

        // ETAPA 3: IApplet::HandleEvent(EVT_APP_START = 1)
        u32 evt = 1; // EVT_APP_START no Zeetris
        uc_reg_write(uc, UC_ARM_REG_R0, &ctx.pApplet);
        uc_reg_write(uc, UC_ARM_REG_R1, &evt);
        uc_reg_write(uc, UC_ARM_REG_R2, &z);
        uc_reg_write(uc, UC_ARM_REG_R3, &z);
        uc_reg_write(uc, UC_ARM_REG_SP, &sp);
        uc_reg_write(uc, UC_ARM_REG_LR, &lr);

        err = uc_emu_start(uc, ctx.handle_event_va, 0xF0F0F0F0u, 0, 500000);
        assert(err == UC_ERR_OK);
        uc_reg_read(uc, UC_ARM_REG_R0, &r0_ret);
        assert(r0_ret == 1); // Evento tratado com sucesso!
        assert(ctx.ishell_slot11_settimer || ctx.ishell_slot12_canceltimer);
        std::printf("[+] Positivo: EVT_APP_START disparou timer no IShell: ms=%u cb=0x%08x user=0x%08x\n",
                    ctx.timer_ms, ctx.timer_cb, ctx.timer_user);

        // ETAPA 4: Despacha eventos de entrada do Z-Pad (EVT_KEY_PRESS 0x100 e EVT_KEY_RELEASE 0x101)
        // Tecla AVK_SELECT (0x102) ou direcional
        u32 key_evt_press = 0x100;
        u32 key_code = 0x102; // AVK_SELECT
        uc_reg_write(uc, UC_ARM_REG_R0, &ctx.pApplet);
        uc_reg_write(uc, UC_ARM_REG_R1, &key_evt_press);
        uc_reg_write(uc, UC_ARM_REG_R2, &key_code);
        uc_reg_write(uc, UC_ARM_REG_R3, &z);
        uc_reg_write(uc, UC_ARM_REG_SP, &sp);
        uc_reg_write(uc, UC_ARM_REG_LR, &lr);

        err = uc_emu_start(uc, ctx.handle_event_va, 0xF0F0F0F0u, 0, 500000);
        assert(err == UC_ERR_OK);
        uc_reg_read(uc, UC_ARM_REG_R0, &r0_ret);
        std::printf("[+] Positivo: HandleEvent(EVT_KEY_PRESS, key=0x%x) retornou r0=%u\n", key_code, r0_ret);

        u32 key_evt_release = 0x101;
        uc_reg_write(uc, UC_ARM_REG_R0, &ctx.pApplet);
        uc_reg_write(uc, UC_ARM_REG_R1, &key_evt_release);
        uc_reg_write(uc, UC_ARM_REG_R2, &key_code);
        uc_reg_write(uc, UC_ARM_REG_R3, &z);
        uc_reg_write(uc, UC_ARM_REG_SP, &sp);
        uc_reg_write(uc, UC_ARM_REG_LR, &lr);

        err = uc_emu_start(uc, ctx.handle_event_va, 0xF0F0F0F0u, 0, 500000);
        assert(err == UC_ERR_OK);
        uc_reg_read(uc, UC_ARM_REG_R0, &r0_ret);
        std::printf("[+] Positivo: HandleEvent(EVT_KEY_RELEASE, key=0x%x) retornou r0=%u\n", key_code, r0_ret);

        // ETAPA 5: Invocação direta da rotina de frame / gameloop (0x1200ac5c)
        // O applet Zeetris precisa que [pApplet + 0x20] aponte para um contexto válido.
        // Alocamos um buffer de contexto (ex: 64KB) para o applet.
        u32 app_ctx_va = 0x30010000u;
        uc_mem_write(uc, ctx.pApplet + 0x20, &app_ctx_va, 4);

        // Zeetris registrou / cancelou o timer com cb = 0x1200ac5c e user = pApplet (0x30005030).
        u32 cb_va = 0x1200ac5c;
        uc_reg_write(uc, UC_ARM_REG_R0, &ctx.pApplet);
        uc_reg_write(uc, UC_ARM_REG_SP, &sp);
        uc_reg_write(uc, UC_ARM_REG_LR, &lr);

        err = uc_emu_start(uc, cb_va, 0xF0F0F0F0u, 0, 500000);
        if (err != UC_ERR_OK) {
            u32 pc = 0, sp_val = 0;
            uc_reg_read(uc, UC_ARM_REG_PC, &pc);
            uc_reg_read(uc, UC_ARM_REG_SP, &sp_val);
            std::printf("uc_emu_start on timer_cb failed: err=%d (%s) pc=0x%08x sp=0x%08x\n",
                        err, uc_strerror(err), pc, sp_val);
            std::fflush(stdout);
        }
        assert(err == UC_ERR_OK);
        std::printf("[+] Positivo: gameloop callback @0x%08x executou com sucesso (Frame 1)!\n", cb_va);

        // Executa múltiplos frames (ex: 10 quadros consecutivos) para garantir estabilidade contínua
        for (int frame = 2; frame <= 10; ++frame) {
            uc_reg_write(uc, UC_ARM_REG_R0, &ctx.pApplet);
            uc_reg_write(uc, UC_ARM_REG_SP, &sp);
            uc_reg_write(uc, UC_ARM_REG_LR, &lr);
            err = uc_emu_start(uc, cb_va, 0xF0F0F0F0u, 0, 500000);
            assert(err == UC_ERR_OK);
        }
        std::printf("[+] Positivo: gameloop executou 10 frames contínuos com sucesso!\n");

        uc_hook_del(uc, h_malloc);
        uc_hook_del(uc, h_code);
        uc_close(uc);
    }

    // ── (2) Controle Negativo: Malloc falha -> CreateInstance falha limpo ─────
    {
        ZeetrisCtx ctx{};
        ctx.force_fail_malloc = true;
        uc_engine* uc = nullptr;
        assert(uc_open(UC_ARCH_ARM, UC_MODE_ARM, &uc) == UC_ERR_OK);
        uc_mem_map(uc, 0x001F0000, 0x10000, UC_PROT_ALL);
        uc_mem_map(uc, 0x11FFF000, 0x1000, UC_PROT_ALL);
        uc_mem_map(uc, STATIC_BASE_VA, 0x10000, UC_PROT_ALL);
        uc_mem_map(uc, EXTRA_OBJ_VA, 0x10000, UC_PROT_ALL);

        u32 obj = ish::install(uc, ctx.env);
        u32 addref_insns[2] = { 0xe3a00001, 0xe12fff1e };
        uc_mem_write(uc, ctx.env.sentinel_va, addref_insns, sizeof(addref_insns));
        u32 bx_lr = 0xe12fff1e;
        uc_mem_write(uc, MALLOC_STUB_VA, &bx_lr, 4);
        u32 malloc_stub_ptr = MALLOC_STUB_VA;
        uc_mem_write(uc, STATIC_BASE_VA + 0x68, &malloc_stub_ptr, 4);
        BrewLoader ld(uc);
        assert(ld.inject_bytes(mod, LOAD_VA, ZEETRIS_CLSID, "zeetris.mod"));

        u32 sb = STATIC_BASE_VA;
        uc_mem_write(uc, LOAD_VA - 8, &sb, 4);
        uc_mem_write(uc, LOAD_VA - 4, &sb, 4);

        uc_hook h_malloc = 0;
        uc_hook_add(uc, &h_malloc, UC_HOOK_CODE, (void*)malloc_handler, &ctx,
                    MALLOC_STUB_VA, MALLOC_STUB_VA);

        u32 ppMod = 0x001fffd0;
        u32 z = 0, sp = STK, lr = 0xF0F0F0F0u;
        uc_reg_write(uc, UC_ARM_REG_R0, &obj);
        uc_reg_write(uc, UC_ARM_REG_R1, &z);
        uc_reg_write(uc, UC_ARM_REG_R2, &ppMod);
        uc_reg_write(uc, UC_ARM_REG_SP, &sp);
        uc_reg_write(uc, UC_ARM_REG_LR, &lr);

        uc_emu_start(uc, ENTRY_VA, 0xF0F0F0F0u, 0, 500000);
        u32 r0_ret = 0;
        uc_reg_read(uc, UC_ARM_REG_R0, &r0_ret);
        if (BUGGY) {
            assert(r0_ret == 0 && "MUTANT: deveria falhar quando malloc retorna NULL");
        } else {
            assert(r0_ret != 0); // Falhou honestamente (AEEMod_Load retornou erro sem estourar memória)
            std::printf("[-] Negativo: Malloc=NULL -> AEEMod_Load retorna erro %u (PASS)\n", r0_ret);
        }

        uc_hook_del(uc, h_malloc);
        uc_close(uc);
    }

    std::printf("=== Test Zeetris Full Lifecycle: PASS ===\n");
    return 0;
}
