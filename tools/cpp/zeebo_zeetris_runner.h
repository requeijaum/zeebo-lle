// zeebo_zeetris_runner.h — Runner do ciclo de vida completo do Zeetris (AEEMod_Load -> IModule::CreateInstance -> HandleEvent -> Gameloop)
#pragma once
#include <cstdint>
#include <cstdio>
#include <vector>
#include <chrono>
#include <unicorn/unicorn.h>

namespace zeebo::zeetris {

using u8  = uint8_t;
using u16 = uint16_t;
using u32 = uint32_t;

struct ZeetrisContext {
    u32 pModule = 0;
    u32 pApplet = 0;
    u32 handle_event_va = 0;
    u32 gameloop_cb_va = 0x1200ac5c;
    u32 app_ctx_va = 0x30010000;
    u32 heap_base = 0x30005000;
    u32 heap_ptr = 0x30005000;
    bool is_running = false;
};

class ZeetrisRunner {
public:
    static constexpr u32 ENTRY_VA        = 0x12000048u; // AEEMod_Load
    static constexpr u32 LOAD_VA         = 0x12000000u;
    static constexpr u32 STK             = 0x00200000u;
    static constexpr u32 ZEETRIS_CLSID   = 0x12345678u;
    static constexpr u32 CREATE_CLSID    = 0x0106e415u;

    static constexpr u32 STATIC_BASE_VA  = 0x30000000u;
    static constexpr u32 GETUPTIME_VA    = 0x30002100u;
    static constexpr u32 EXTRA_OBJ_VA    = 0x60000000u;
    static constexpr u32 EXTRA_VTBL_VA   = 0x60001000u;
    static constexpr u32 DISPLAY_OBJ_VA  = 0x60002000u;
    static constexpr u32 DISPLAY_VTBL_VA = 0x60003000u;

    static void hook_malloc_stub(uc_engine* uc, uint64_t addr, uint32_t size, void* user_data) {
        (void)addr;
        (void)size;
        auto* ctx = reinterpret_cast<ZeetrisContext*>(user_data);
        u32 req_sz = 0, lr = 0;
        uc_reg_read(uc, UC_ARM_REG_R0, &req_sz);
        uc_reg_read(uc, UC_ARM_REG_LR, &lr);
        u32 allocated = ctx->heap_ptr;
        u32 aligned_sz = (req_sz + 7u) & ~7u;
        ctx->heap_ptr += aligned_sz;
        uc_reg_write(uc, UC_ARM_REG_R0, &allocated);
        uc_reg_write(uc, UC_ARM_REG_PC, &lr);
    }

    static void hook_code(uc_engine* uc, uint64_t addr, uint32_t size, void* user_data) {
        (void)size;
        (void)user_data;
        u32 pc = static_cast<u32>(addr);

        if (pc >= 0x50000000u && pc < 0x50000100u) {
            u32 lr = 0;
            uc_reg_read(uc, UC_ARM_REG_LR, &lr);
            u32 s = (pc - 0x50000000u) / 4u;
            if (s == 2) { // CreateInstance
                u32 clsid = 0, ppObj = 0;
                uc_reg_read(uc, UC_ARM_REG_R1, &clsid);
                uc_reg_read(uc, UC_ARM_REG_R2, &ppObj);
                u32 obj_ptr = 0;
                if (clsid == CREATE_CLSID) {
                    obj_ptr = EXTRA_OBJ_VA;
                } else if (clsid == 0x01001001u) {
                    obj_ptr = DISPLAY_OBJ_VA;
                }
                if (obj_ptr != 0 && ppObj != 0) {
                    uc_mem_write(uc, ppObj, &obj_ptr, 4);
                }
                u32 zero = 0;
                uc_reg_write(uc, UC_ARM_REG_R0, &zero);
                uc_reg_write(uc, UC_ARM_REG_PC, &lr);
            } else if (s == 12) { // CancelTimer
                u32 zero = 0;
                uc_reg_write(uc, UC_ARM_REG_R0, &zero);
                uc_reg_write(uc, UC_ARM_REG_PC, &lr);
            } else {
                u32 zero = 0;
                uc_reg_write(uc, UC_ARM_REG_R0, &zero);
                uc_reg_write(uc, UC_ARM_REG_PC, &lr);
            }
        } else if (pc >= 0x50000100u && pc < 0x50000300u) {
            u32 lr = 0, zero = 0;
            uc_reg_read(uc, UC_ARM_REG_LR, &lr);
            uc_reg_write(uc, UC_ARM_REG_R0, &zero);
            uc_reg_write(uc, UC_ARM_REG_PC, &lr);
        }
    }

    static bool mem_hook(uc_engine* uc, uc_mem_type type, uint64_t address, int size, int64_t value, void* user) {
        (void)uc; (void)size; (void)value; (void)user;
        std::printf("[UNMAPPED ACCESS] type=%d addr=0x%08llx\n", type, (unsigned long long)address);
        std::fflush(stdout);
        return false;
    }

    static bool setup_and_start(uc_engine* uc, ZeetrisContext& ctx) {
        if (!uc) return false;

        uc_mem_map(uc, 0x001F0000, 0x10000, UC_PROT_ALL);
        uc_mem_map(uc, 0x11FFF000, 0x1000, UC_PROT_ALL);
        uc_mem_map(uc, STATIC_BASE_VA, 0x100000, UC_PROT_ALL);
        uc_mem_map(uc, 0x40000000, 0x10000, UC_PROT_ALL);
        uc_mem_map(uc, 0x50000000, 0x10000, UC_PROT_ALL);
        uc_mem_map(uc, 0x60000000, 0x10000, UC_PROT_ALL);
        uc_mem_map(uc, 0x003C0000, 0x20000, UC_PROT_ALL);
        uc_mem_map(uc, 0x123C0000, 0x20000, UC_PROT_ALL);

        // static_base no load_va - 8 e load_va - 4
        u32 sb = STATIC_BASE_VA;
        uc_mem_write(uc, LOAD_VA - 8, &sb, 4);
        uc_mem_write(uc, LOAD_VA - 4, &sb, 4);

        // Stub bx lr no malloc/free
        const u32 MALLOC_STUB_VA = 0x30002000u;
        const u32 FREE_STUB_VA   = 0x30002080u;
        u32 bx_lr = 0xe12fff1e;
        uc_mem_write(uc, MALLOC_STUB_VA, &bx_lr, 4);
        uc_mem_write(uc, FREE_STUB_VA, &bx_lr, 4);

        u32 malloc_ptr = MALLOC_STUB_VA;
        u32 free_ptr   = FREE_STUB_VA;
        uc_mem_write(uc, STATIC_BASE_VA + 0x68, &malloc_ptr, 4);
        uc_mem_write(uc, STATIC_BASE_VA + 0x6c, &free_ptr, 4);

        // GetUpTimeMS no slot 0xb0
        u32 getuptime_code[2] = {
            0xe3a00064, // mov r0, #100
            0xe12fff1e  // bx lr
        };
        uc_mem_write(uc, GETUPTIME_VA, getuptime_code, sizeof(getuptime_code));
        u32 getuptime_ptr = GETUPTIME_VA;
        uc_mem_write(uc, STATIC_BASE_VA + 0xb0, &getuptime_ptr, 4);

        // Objeto IShell
        u32 ishell_obj = 0x40000000u;
        u32 ishell_vt  = 0x40001000u;
        uc_mem_write(uc, ishell_obj, &ishell_vt, 4);
        for (u32 i = 0; i < 64; ++i) {
            u32 s = 0x50000000u + i * 4u;
            uc_mem_write(uc, ishell_vt + i * 4u, &s, 4);
        }

        // Objeto EXTRA vtable
        u32 extra_vt = EXTRA_VTBL_VA;
        uc_mem_write(uc, EXTRA_OBJ_VA, &extra_vt, 4);
        for (u32 i = 0; i < 64; ++i) {
            u32 s = 0x50000100u + i * 4u;
            uc_mem_write(uc, EXTRA_VTBL_VA + i * 4u, &s, 4);
        }

        // Objeto DISPLAY vtable
        u32 disp_vt = DISPLAY_VTBL_VA;
        uc_mem_write(uc, DISPLAY_OBJ_VA, &disp_vt, 4);
        for (u32 i = 0; i < 64; ++i) {
            u32 s = 0x50000200u + i * 4u;
            uc_mem_write(uc, DISPLAY_VTBL_VA + i * 4u, &s, 4);
        }

        uc_hook h_malloc = 0, h_code = 0, h_mem = 0;
        uc_hook_add(uc, &h_malloc, UC_HOOK_CODE, (void*)hook_malloc_stub, &ctx,
                    MALLOC_STUB_VA, MALLOC_STUB_VA);
        uc_hook_add(uc, &h_code, UC_HOOK_CODE, (void*)hook_code, &ctx,
                    0x50000000u, 0x50000300u);
        uc_hook_add(uc, &h_mem, UC_HOOK_MEM_UNMAPPED, (void*)mem_hook, &ctx, 1, 0);

        // ETAPA 1: AEEMod_Load
        u32 ppMod = 0x001FFFD0u;
        u32 sp = STK, lr = 0xF0F0F0F0u;
        uc_reg_write(uc, UC_ARM_REG_R0, &ishell_obj);
        uc_reg_write(uc, UC_ARM_REG_R1, &ishell_obj);
        uc_reg_write(uc, UC_ARM_REG_R2, &ppMod);
        uc_reg_write(uc, UC_ARM_REG_SP, &sp);
        uc_reg_write(uc, UC_ARM_REG_LR, &lr);

        uc_err err = uc_emu_start(uc, ENTRY_VA, 0xF0F0F0F0u, 0, 500000);
        if (err != UC_ERR_OK) {
            u32 pc = 0, sp_val = 0;
            uc_reg_read(uc, UC_ARM_REG_PC, &pc);
            uc_reg_read(uc, UC_ARM_REG_SP, &sp_val);
            std::printf("[ZeetrisRunner] AEEMod_Load falhou: %s (pc=0x%08x sp=0x%08x)\n",
                        uc_strerror(err), pc, sp_val);
            return false;
        }
        uc_mem_read(uc, ppMod, &ctx.pModule, 4);

        // ETAPA 2: IModule::CreateInstance
        u32 vtbl_va = 0;
        uc_mem_read(uc, ctx.pModule, &vtbl_va, 4);
        u32 create_instance_va = 0;
        uc_mem_read(uc, vtbl_va + 8, &create_instance_va, 4);

        u32 ppApplet = 0x001FFFD4u;
        u32 clsid = ZEETRIS_CLSID;
        uc_reg_write(uc, UC_ARM_REG_R0, &ctx.pModule);
        uc_reg_write(uc, UC_ARM_REG_R1, &ishell_obj);
        uc_reg_write(uc, UC_ARM_REG_R2, &clsid);
        uc_reg_write(uc, UC_ARM_REG_R3, &ppApplet);
        uc_reg_write(uc, UC_ARM_REG_SP, &sp);
        uc_reg_write(uc, UC_ARM_REG_LR, &lr);

        err = uc_emu_start(uc, create_instance_va, 0xF0F0F0F0u, 0, 500000);
        if (err != UC_ERR_OK) {
            std::printf("[ZeetrisRunner] IModule::CreateInstance falhou: %s\n", uc_strerror(err));
            return false;
        }
        uc_mem_read(uc, ppApplet, &ctx.pApplet, 4);
        uc_mem_read(uc, ctx.pApplet + 0x18, &ctx.handle_event_va, 4);

        // Configura AppContext
        uc_mem_write(uc, ctx.pApplet + 0x20, &ctx.app_ctx_va, 4);

        // ETAPA 3: IApplet::HandleEvent(EVT_APP_START = 1)
        u32 evt = 1;
        u32 r2 = 0, r3 = 0;
        uc_reg_write(uc, UC_ARM_REG_R0, &ctx.pApplet);
        uc_reg_write(uc, UC_ARM_REG_R1, &evt);
        uc_reg_write(uc, UC_ARM_REG_R2, &r2);
        uc_reg_write(uc, UC_ARM_REG_R3, &r3);
        uc_reg_write(uc, UC_ARM_REG_SP, &sp);
        uc_reg_write(uc, UC_ARM_REG_LR, &lr);

        err = uc_emu_start(uc, ctx.handle_event_va, 0xF0F0F0F0u, 0, 500000);
        if (err != UC_ERR_OK) {
            std::printf("[ZeetrisRunner] EVT_APP_START falhou: %s\n", uc_strerror(err));
            return false;
        }

        // Contexto do Applet [pApplet + 0x20]
        u32 app_ctx_va = 0x30010000u;
        uc_mem_write(uc, ctx.pApplet + 0x20, &app_ctx_va, 4);

        ctx.is_running = true;
        std::printf("[ZeetrisRunner] Ciclo de vida inicializado com sucesso (Applet @ 0x%08x, Gameloop @ 0x%08x)\n",
                    ctx.pApplet, ctx.gameloop_cb_va);
        return true;
    }

    static bool step_frame(uc_engine* uc, ZeetrisContext& ctx) {
        if (!ctx.is_running || !uc) return false;
        u32 sp = STK, lr = 0xF0F0F0F0u;
        uc_reg_write(uc, UC_ARM_REG_R0, &ctx.pApplet);
        uc_reg_write(uc, UC_ARM_REG_SP, &sp);
        uc_reg_write(uc, UC_ARM_REG_LR, &lr);

        uc_err err = uc_emu_start(uc, ctx.gameloop_cb_va, 0xF0F0F0F0u, 0, 500000);
        return err == UC_ERR_OK;
    }

    static bool dispatch_key(uc_engine* uc, ZeetrisContext& ctx, u32 key_code, bool pressed) {
        if (!ctx.is_running || !uc) return false;
        u32 evt = pressed ? 0x100u : 0x101u;
        u32 sp = STK, lr = 0xF0F0F0F0u;
        u32 r3 = 0;
        uc_reg_write(uc, UC_ARM_REG_R0, &ctx.pApplet);
        uc_reg_write(uc, UC_ARM_REG_R1, &evt);
        uc_reg_write(uc, UC_ARM_REG_R2, &key_code);
        uc_reg_write(uc, UC_ARM_REG_R3, &r3);
        uc_reg_write(uc, UC_ARM_REG_SP, &sp);
        uc_reg_write(uc, UC_ARM_REG_LR, &lr);

        uc_err err = uc_emu_start(uc, ctx.handle_event_va, 0xF0F0F0F0u, 0, 500000);
        if (err != UC_ERR_OK) return false;
        u32 r0 = 0;
        uc_reg_read(uc, UC_ARM_REG_R0, &r0);
        return r0 == 1;
    }
};

} // namespace zeebo::zeetris
