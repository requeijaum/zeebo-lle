// zeebo_zeetris_runner.h — Runner do ciclo de vida completo do Zeetris (AEEMod_Load -> IModule::CreateInstance -> HandleEvent -> Gameloop)
#pragma once
#include <cstdint>
#include <cstdio>
#include <functional>
#include <map>
#include <vector>
#include <chrono>
#include <cstdlib>
#include <unicorn/unicorn.h>
#include "gpu/igl_hook.h"   // zeebo::gpu::GuestMachine (ponte GL ES -> IglHook)

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
    uint32_t display_update_calls = 0;
    uint32_t display_drawrect_calls = 0;
    uint32_t display_bitblt_calls = 0;
    uint32_t uptime_ms = 100;

    // IMedia tracking
    uint32_t media_register_notify_calls = 0;
    uint32_t media_set_param_calls = 0;
    uint32_t media_play_calls = 0;
    uint32_t media_notify_fn = 0;
    uint32_t media_notify_user = 0;

    // IGL (GL ES) dispatch tracking
    // Estado de botões que a PLATAFORMA fornece ao poll do jogo (o driver de
    // input do firmware preenche isso; rodando só o .mod, o host faz esse papel).
    u16 platform_buttons = 0;

    // Contadores de código do caminho de input (env-gated, ver hooks).
    uint32_t input_poll_calls = 0;    // execucoes do poll da mascara (0x1200c3dc)
    uint32_t input_block_calls = 0;   // execucoes do bloco de input do gameloop
    uint32_t input_store_calls = 0;   // execucoes da rotina que escreve no struct de input
    uint32_t tex_loader_calls = 0;    // execucoes da rotina de carga de textura (0x120056fc)
    // Quantas vezes cada handler de bit da mascara executou (8 bits do gameloop).
    uint32_t btn_handler_calls[8] = {0,0,0,0,0,0,0,0};
    uint32_t igl_calls = 0;
    uint32_t igl_handled = 0;
    std::map<uint32_t, uint32_t> igl_slot_calls;
    std::map<uint32_t, uint32_t> igl_slot_handled; // slots aceitos pelo IglHook
    // Ponte real para o IglHook (gpu/). Se vazio, os slots gl* caem no stub
    // honesto que apenas conta a chamada e devolve void.
    std::function<bool(int, zeebo::gpu::GuestMachine&)> igl_dispatcher;

    // Framebuffer 640x480 RGB565 (inicializado em branco 0xFFFF)
    std::vector<uint16_t> framebuffer = std::vector<uint16_t>(640 * 480, 0xFFFF);
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
    static constexpr u32 IGL_OBJ_VA      = 0x60007000u;
    static constexpr u32 IGL_VTBL_VA     = 0x60008000u;
    static constexpr u32 IGL_SENTINEL_VA = 0x50001000u;
    static constexpr u32 IGL_SLOTS       = 80u;
    // PROVADO: no Zeebo o objeto devolvido por ISHELL_CreateInstance(0x01001001)
    // É a interface de 80 slots (0..2 IBase + 3..79 gl*). Evidência:
    //  (a) os 108 thunks do módulo cobrem offsets 12..316 (slots 3..79) e todos
    //      leem o MESMO global 0x003c14c4;
    //  (b) o jogo grava nesse global o ponteiro que recebeu de CreateInstance;
    //  (c) o slot 7 é chamado com r0=0x4000 = GL_COLOR_BUFFER_BIT (bit de glClear).
    static constexpr u32 DISPLAY_OBJ_VA  = IGL_OBJ_VA;
    static constexpr u32 DISPLAY_VTBL_VA = IGL_VTBL_VA;
    static constexpr u32 BITMAP_OBJ_VA   = 0x60004000u;
    static constexpr u32 BITMAP_VTBL_VA  = 0x60005000u;

    // Ponte uc -> GuestMachine: args 0..3 em R0..R3, >=4 em SP+((n-4)*4);
    // `read` lê memória guest (usada pelo IglHook para arrays de vértices/cor no
    // momento do draw, conforme igl_hook.h).
    static zeebo::gpu::GuestMachine make_guest_machine(uc_engine* uc) {
        zeebo::gpu::GuestMachine gm;
        gm.arg = [uc](int n) -> u32 {
            u32 v = 0;
            if (n >= 0 && n < 4) {
                const int regs[4] = {UC_ARM_REG_R0, UC_ARM_REG_R1,
                                     UC_ARM_REG_R2, UC_ARM_REG_R3};
                uc_reg_read(uc, regs[n], &v);
            } else if (n >= 4) {
                u32 sp = 0;
                uc_reg_read(uc, UC_ARM_REG_SP, &sp);
                uc_mem_read(uc, sp + static_cast<u32>(n - 4) * 4u, &v, 4);
            }
            return v;
        };
        gm.read = [uc](u32 va, void* dst, u32 size) {
            return uc_mem_read(uc, va, dst, size) == UC_ERR_OK;
        };
        gm.set_ret = [uc](u32 v) { uc_reg_write(uc, UC_ARM_REG_R0, &v); };
        return gm;
    }

    static void hook_getuptime(uc_engine* uc, uint64_t addr, uint32_t size, void* user_data) {
        (void)addr; (void)size;
        auto* ctx = reinterpret_cast<ZeetrisContext*>(user_data);
        if (ctx) {
            ctx->uptime_ms += 16;
            uc_reg_write(uc, UC_ARM_REG_R0, &ctx->uptime_ms);
        }
    }

    static void hook_poll_count(uc_engine* uc, uint64_t addr, uint32_t size, void* user_data) {
        (void)uc; (void)addr; (void)size;
        if (auto* ctx = reinterpret_cast<ZeetrisContext*>(user_data)) ctx->input_poll_calls++;
    }
    static void hook_input_block(uc_engine* uc, uint64_t addr, uint32_t size, void* user_data) {
        (void)uc; (void)addr; (void)size;
        if (auto* ctx = reinterpret_cast<ZeetrisContext*>(user_data)) ctx->input_block_calls++;
    }

    // Handler de bit do gameloop -> indice. Ordem dos bits 1,2,4,8,0x10,0x20,0x80,0x200.
    static void hook_btn_handler(uc_engine* uc, uint64_t addr, uint32_t size, void* user_data) {
        (void)uc; (void)size;
        auto* ctx = reinterpret_cast<ZeetrisContext*>(user_data);
        if (!ctx) return;
        switch (static_cast<u32>(addr)) {
            case 0x1200b0e8u: ctx->btn_handler_calls[0]++; break;
            case 0x1200b0d4u: ctx->btn_handler_calls[1]++; break;
            case 0x1200b0c0u: ctx->btn_handler_calls[2]++; break;
            case 0x1200b04cu: ctx->btn_handler_calls[3]++; break;
            case 0x1200b064u: ctx->btn_handler_calls[4]++; break;
            case 0x1200b07cu: ctx->btn_handler_calls[5]++; break;
            case 0x1200b094u: ctx->btn_handler_calls[6]++; break;
            case 0x1200b0acu: ctx->btn_handler_calls[7]++; break;
            default: break;
        }
    }

    // Fornece o estado de botões da plataforma ao poll do jogo (R0 no retorno).
    static void hook_poll_return(uc_engine* uc, uint64_t addr, uint32_t size, void* user_data) {
        (void)addr; (void)size;
        auto* ctx = reinterpret_cast<ZeetrisContext*>(user_data);
        if (!ctx) return;
        u32 v = ctx->platform_buttons;
        uc_reg_write(uc, UC_ARM_REG_R0, &v);
    }

    // Mostra o endereço exato de onde o poll lê a máscara (R3 no ldrh).
    static void hook_poll_addr(uc_engine* uc, uint64_t addr, uint32_t size, void* user_data) {
        (void)size; (void)user_data;
        static int n = 0;
        if (n++ >= 3) return;
        u32 r3 = 0;
        uc_reg_read(uc, UC_ARM_REG_R3, &r3);
        u32 v = 0;
        uc_mem_read(uc, r3 + 0x10, &v, 4);
        printf("[poll-fonte@0x%x] R3=0x%08x -> ldrh [R3+0x10]=0x%04x\n",
               (u32)addr, r3, (u32)(v & 0xffff));
    }

    // Mostra, uma vez, o que o gameloop realmente recebe: R0 = máscara (logo após
    // o BL do poll) e R7 = ponteiro do struct onde guarda a máscara anterior.
    static void hook_after_poll(uc_engine* uc, uint64_t addr, uint32_t size, void* user_data) {
        (void)size; (void)user_data;
        static int n = 0;
        if (n++ >= 4) return;
        u32 r0 = 0, r7 = 0, sb = 0;
        uc_reg_read(uc, UC_ARM_REG_R0, &r0);
        uc_reg_read(uc, UC_ARM_REG_R7, &r7);
        uc_reg_read(uc, UC_ARM_REG_SB, &sb);
        printf("[poll@0x%x] mask(R0)=0x%08x prev_ptr(R7)=0x%08x cur(R11/sb)=0x%08x\n",
               (u32)addr, r0, r7, sb);
    }

    // A rotina que carrega texturas (0x120056fc: chama glGenTextures/glTexImage2D
    // 17x) nao tem BL caller nem referencia literal. Instrumento de CÓDIGO para
    // responder se ela executa alguma vez (env-gated).
    static void hook_tex_loader(uc_engine* uc, uint64_t addr, uint32_t size, void* user_data) {
        (void)uc; (void)addr; (void)size;
        if (auto* ctx = reinterpret_cast<ZeetrisContext*>(user_data)) ctx->tex_loader_calls++;
    }

    static void hook_input_store(uc_engine* uc, uint64_t addr, uint32_t size, void* user_data) {
        (void)uc; (void)addr; (void)size;
        if (auto* ctx = reinterpret_cast<ZeetrisContext*>(user_data)) ctx->input_store_calls++;
    }
    static void hook_input_watch(uc_engine* uc, uc_mem_type type, uint64_t addr,
                                 int size, int64_t value, void* user_data) {
        (void)user_data;
        // Só os primeiros eventos interessam: provam se o jogo chega a LER a
        // máscara (poll roda) e se algo a ESCREVE.
        static int n = 0;
        if (n >= 24) return;
        n++;
        u32 pc = 0;
        uc_reg_read(uc, UC_ARM_REG_PC, &pc);
        const char* kind = (type == UC_MEM_WRITE) ? "W" : "R";
        printf("[INPUT-%s] pc=0x%08x addr=0x%llx size=%d val=0x%llx\n",
               kind, pc, (unsigned long long)addr, size, (unsigned long long)value);
    }

    static void hook_trace_gameloop(uc_engine* uc, uint64_t addr, uint32_t size, void* user_data) {
        (void)size; (void)user_data;
        // Instrumento de diagnóstico: imprime CADA instrução da rotina de desenho,
        // o que enche o log e derruba o FPS. Fica atrás de ZEEBO_ZEETRIS_TRACE.
        static const bool trace_on = std::getenv("ZEEBO_ZEETRIS_TRACE") != nullptr;
        if (!trace_on) return;
        u32 pc = (u32)addr;
        static int trace_steps = 0;
        if (trace_steps < 30) {
            u32 r0 = 0, r1 = 0, r2 = 0, r3 = 0, r4 = 0, r6 = 0, sl = 0, fp = 0;
            uc_reg_read(uc, UC_ARM_REG_R0, &r0);
            uc_reg_read(uc, UC_ARM_REG_R1, &r1);
            uc_reg_read(uc, UC_ARM_REG_R2, &r2);
            uc_reg_read(uc, UC_ARM_REG_R3, &r3);
            uc_reg_read(uc, UC_ARM_REG_R4, &r4);
            uc_reg_read(uc, UC_ARM_REG_R6, &r6);
            uc_reg_read(uc, UC_ARM_REG_SL, &sl);
            uc_reg_read(uc, UC_ARM_REG_FP, &fp);
            u32 fp_val = 0;
            if (fp != 0) uc_mem_read(uc, fp + 0x54, &fp_val, 4);
            std::printf("[TRACE PC=0x%08x] r0=0x%x r1=0x%x r2=0x%x r3=0x%x r4=0x%x r6=0x%x sl=0x%x fp=0x%x [fp+0x54]=0x%x\n",
                        pc, r0, r1, r2, r3, r4, r6, sl, fp, fp_val);
            if (pc == 0x12008648) {
                u32 val_3c14c4 = 0, val_123c14c4 = 0, pool_word = 0;
                uc_mem_read(uc, 0x003c14c4, &val_3c14c4, 4);
                uc_mem_read(uc, 0x123c14c4, &val_123c14c4, 4);
                uc_mem_read(uc, 0x1200e568, &pool_word, 4);
                std::printf("  --> [0x003c14c4]=0x%08x, [0x123c14c4]=0x%08x, [0x1200e568]=0x%08x\n",
                            val_3c14c4, val_123c14c4, pool_word);
            }
            trace_steps++;
        }
    }
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
        auto* ctx = reinterpret_cast<ZeetrisContext*>(user_data);
        u32 pc = static_cast<u32>(addr);

        if (pc >= 0x50000000u && pc < 0x50000100u) {
            u32 lr = 0;
            uc_reg_read(uc, UC_ARM_REG_LR, &lr);
            u32 s = (pc - 0x50000000u) / 4u;
            if (s == 2) { // CreateInstance
                u32 clsid = 0, ppObj = 0;
                uc_reg_read(uc, UC_ARM_REG_R1, &clsid);
                uc_reg_read(uc, UC_ARM_REG_R2, &ppObj);
                std::printf("[Zeetris IShell] CreateInstance clsid=0x%08x ppObj=0x%08x\n", clsid, ppObj);
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
            } else if (s == 4) { // GetDeviceInfo
                u32 pdi = 0;
                uc_reg_read(uc, UC_ARM_REG_R1, &pdi);
                std::printf("[Zeetris IShell] GetDeviceInfo pdi=0x%08x\n", pdi);
                if (pdi != 0) {
                    u16 w = 640, h = 480;
                    uc_mem_write(uc, pdi + 0, &w, 2);
                    uc_mem_write(uc, pdi + 2, &h, 2);
                }
                u32 zero = 0;
                uc_reg_write(uc, UC_ARM_REG_R0, &zero);
                uc_reg_write(uc, UC_ARM_REG_PC, &lr);
            } else if (s == 12) { // CancelTimer
                u32 r1 = 0, r2 = 0;
                uc_reg_read(uc, UC_ARM_REG_R1, &r1);
                uc_reg_read(uc, UC_ARM_REG_R2, &r2);
                u32 zero = 0;
                uc_reg_write(uc, UC_ARM_REG_R0, &zero);
                uc_reg_write(uc, UC_ARM_REG_PC, &lr);
            } else if (s == 11) { // SetTimer
                u32 r1 = 0, r2 = 0, r3 = 0;
                uc_reg_read(uc, UC_ARM_REG_R1, &r1);
                uc_reg_read(uc, UC_ARM_REG_R2, &r2);
                uc_reg_read(uc, UC_ARM_REG_R3, &r3);
                u32 zero = 0;
                uc_reg_write(uc, UC_ARM_REG_R0, &zero);
                uc_reg_write(uc, UC_ARM_REG_PC, &lr);
            } else {
                std::printf("[Zeetris IShell] Chamada ao slot %u (lr=0x%08x)\n", s, lr);
                u32 zero = 0;
                uc_reg_write(uc, UC_ARM_REG_R0, &zero);
                uc_reg_write(uc, UC_ARM_REG_PC, &lr);
            }
        } else if (pc >= 0x50000100u && pc < 0x50000200u) {
            u32 lr = 0, zero = 0;
            uc_reg_read(uc, UC_ARM_REG_LR, &lr);
            u32 slot = (pc - 0x50000100u) / 4u;
            if (slot == 3 && ctx) { // RegisterNotify(IMedia *po, PFNMEDIANOTIFY pfnNotify, void *pUser)
                u32 pfnNotify = 0, pUser = 0;
                uc_reg_read(uc, UC_ARM_REG_R1, &pfnNotify);
                uc_reg_read(uc, UC_ARM_REG_R2, &pUser);
                ctx->media_notify_fn = pfnNotify;
                ctx->media_notify_user = pUser;
                ctx->media_register_notify_calls++;
                std::printf("[IMedia] RegisterNotify pfnNotify=0x%08x pUser=0x%08x (calls=%u)\n",
                            pfnNotify, pUser, ctx->media_register_notify_calls);
            } else if (slot == 4 && ctx) { // SetMediaParm(IMedia *po, int nParamID, int32 p1, int32 p2)
                u32 nParamID = 0, p1 = 0, p2 = 0;
                uc_reg_read(uc, UC_ARM_REG_R1, &nParamID);
                uc_reg_read(uc, UC_ARM_REG_R2, &p1);
                uc_reg_read(uc, UC_ARM_REG_R3, &p2);
                ctx->media_set_param_calls++;
                std::printf("[IMedia] SetMediaParm param=%u p1=0x%08x p2=0x%08x\n", nParamID, p1, p2);
            } else if (slot == 6 && ctx) { // Play(IMedia *po)
                ctx->media_play_calls++;
                std::printf("[IMedia] Play called! (total=%u)\n", ctx->media_play_calls);
            } else {
                std::printf("[IMedia] Chamada ao slot %u (lr=0x%08x)\n", slot, lr);
            }
            uc_reg_write(uc, UC_ARM_REG_R0, &zero);
            uc_reg_write(uc, UC_ARM_REG_PC, &lr);
        } else if (pc >= 0x50000200u && pc < 0x50000500u) {
            u32 lr = 0, zero = 0;
            uc_reg_read(uc, UC_ARM_REG_LR, &lr);
            u32 slot = (pc - 0x50000200u) / 4u;
            std::printf("[DISPLAY OBJ] Chamada ao slot %u (lr=0x%08x)\n", slot, lr);
            if (ctx) {
                if (slot == 7) ctx->display_update_calls++;
                if (slot == 5) ctx->display_drawrect_calls++;
                if (slot == 6) ctx->display_bitblt_calls++;
            }
            if (slot == 5 && ctx) { // DrawRect(po, pRect, clrFrame, clrFill, dwFlags)
                u32 pRect = 0, clrFill = 0;
                uc_reg_read(uc, UC_ARM_REG_R1, &pRect);
                uc_reg_read(uc, UC_ARM_REG_R3, &clrFill);
                int x0 = 0, y0 = 0, x1 = 640, y1 = 480;
                if (pRect != 0) {
                    int16_t rx = 0, ry = 0, rdx = 0, rdy = 0;
                    uc_mem_read(uc, pRect + 0, &rx, 2);
                    uc_mem_read(uc, pRect + 2, &ry, 2);
                    uc_mem_read(uc, pRect + 4, &rdx, 2);
                    uc_mem_read(uc, pRect + 6, &rdy, 2);
                    x0 = rx;
                    y0 = ry;
                    x1 = x0 + rdx;
                    y1 = y0 + rdy;
                }
                uint32_t r = (clrFill >> 16) & 0xFF;
                uint32_t g = (clrFill >> 8) & 0xFF;
                uint32_t b = clrFill & 0xFF;
                uint16_t c565 = static_cast<uint16_t>(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
                for (int y = std::max(y0, 0); y < std::min(y1, 480); ++y) {
                    for (int x = std::max(x0, 0); x < std::min(x1, 640); ++x) {
                        ctx->framebuffer[y * 640 + x] = c565;
                    }
                }
            }
            if (slot == 16) { // GetDeviceBitmap(IDisplay *pIDisplay, IBitmap **ppBitmap)
                u32 ppBitmap = 0;
                uc_reg_read(uc, UC_ARM_REG_R1, &ppBitmap);
                std::printf("[DISPLAY OBJ] GetDeviceBitmap ppBitmap=0x%08x\n", ppBitmap);
                if (ppBitmap != 0) {
                    u32 b_obj = BITMAP_OBJ_VA;
                    uc_mem_write(uc, ppBitmap, &b_obj, 4);
                }
            }
            uc_reg_write(uc, UC_ARM_REG_R0, &zero);
            uc_reg_write(uc, UC_ARM_REG_PC, &lr);
        } else if (pc >= IGL_SENTINEL_VA && pc < IGL_SENTINEL_VA + IGL_SLOTS * 4u) {
            // Slots 0..2 (IBase) recebem `po` em R0; slots 3..79 (gl*) NÃO —
            // R0 é o PRIMEIRO ARGUMENTO REAL (ABI do macro IGL_glXxx).
            u32 lr = 0, a0 = 0, a1 = 0, a2 = 0, a3 = 0, zero = 0;
            uc_reg_read(uc, UC_ARM_REG_LR, &lr);
            uc_reg_read(uc, UC_ARM_REG_R0, &a0);
            uc_reg_read(uc, UC_ARM_REG_R1, &a1);
            uc_reg_read(uc, UC_ARM_REG_R2, &a2);
            uc_reg_read(uc, UC_ARM_REG_R3, &a3);
            u32 slot = (pc - IGL_SENTINEL_VA) / 4u;
            if (ctx) {
                ctx->igl_calls++;
                ctx->igl_slot_calls[slot]++;
                if (ctx->igl_slot_calls[slot] <= 3) {
                    std::printf("[IGL] slot %u gl* (lr=0x%08x) a0=0x%08x a1=0x%08x "
                                "a2=0x%08x a3=0x%08x\n", slot, lr, a0, a1, a2, a3);
                }
                if (ctx->igl_dispatcher) {
                    // R0..R3 foram lidos acima mas o GuestMachine relê via uc para
                    // que o IglHook possa pedir args >=4 (pilha) e ler arrays do
                    // guest no draw. Slots gl* NÃO recebem `po` (ver igl_hook.h).
                    auto gm = make_guest_machine(uc);
                    if (ctx->igl_dispatcher(static_cast<int>(slot), gm)) {
                        ctx->igl_handled++;
                        ctx->igl_slot_handled[slot]++;
                    }
                    uc_reg_write(uc, UC_ARM_REG_PC, &lr);
                    return;
                }
            }
            // Sem dispatcher: stub honesto (slots gl* devolvem void).
            uc_reg_write(uc, UC_ARM_REG_R0, &zero);
            uc_reg_write(uc, UC_ARM_REG_PC, &lr);
        } else if (pc >= 0x50000500u && pc < 0x50000600u) {
            u32 lr = 0, zero = 0;
            uc_reg_read(uc, UC_ARM_REG_LR, &lr);
            u32 slot = (pc - 0x50000500u) / 4u;
            std::printf("[BITMAP OBJ] Chamada ao slot %u (lr=0x%08x)\n", slot, lr);
            uc_reg_write(uc, UC_ARM_REG_R0, &zero);
            uc_reg_write(uc, UC_ARM_REG_PC, &lr);
        }
    }

    static bool mem_hook(uc_engine* uc, uc_mem_type type, uint64_t address, int size, int64_t value, void* user) {
        (void)size; (void)value; (void)user;
        u32 pc = 0, lr = 0, r1 = 0, r2 = 0, r3 = 0;
        uc_reg_read(uc, UC_ARM_REG_PC, &pc);
        uc_reg_read(uc, UC_ARM_REG_LR, &lr);
        uc_reg_read(uc, UC_ARM_REG_R1, &r1);
        uc_reg_read(uc, UC_ARM_REG_R2, &r2);
        uc_reg_read(uc, UC_ARM_REG_R3, &r3);
        std::printf("[UNMAPPED ACCESS] type=%d addr=0x%08llx at PC=0x%08x LR=0x%08x (r1=0x%x, r2=0x%x, r3=0x%x)\n",
                    type, (unsigned long long)address, pc, lr, r1, r2, r3);
        std::fflush(stdout);
        return false;
    }

    static std::vector<u8> load_file(const std::string& path) {
        std::ifstream f(path, std::ios::binary | std::ios::ate);
        if (!f) return {};
        std::streamoff n = f.tellg();
        if (n <= 0) return {};
        std::vector<u8> b(static_cast<size_t>(n));
        f.seekg(0);
        f.read(reinterpret_cast<char*>(b.data()), n);
        return b;
    }

    static bool is_zeetris_mod(const std::vector<u8>& bytes) {
        if (bytes.size() < 0x20) return false;
        // Zeetris possui branch inicial para 0x12000048 (AEEMod_Load)
        u32 first_word = *reinterpret_cast<const u32*>(bytes.data());
        return (first_word == 0xEA000010u); // b 0x4c (PC+8+0x40 = 0x48)
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

        // Global do módulo em 0x003c14c4 aponta para o objeto IGL (ver
        // constantes IGL_*): os 108 thunks do módulo provam que é a interface
        // GL ES (slots 3..79), não IDisplay.
        u32 disp_ptr = IGL_OBJ_VA;
        uc_mem_write(uc, 0x003c14c4, &disp_ptr, 4);
        uc_mem_write(uc, 0x123c14c4, &disp_ptr, 4);

        // Habilita renderizador ativo no Zeetris (0x3c12e8 + 0x54 e 0x3c14e8 + 0x54)
        u32 render_active = 1;
        uc_mem_write(uc, 0x003c12e8 + 0x54, &render_active, 4);
        uc_mem_write(uc, 0x123c12e8 + 0x54, &render_active, 4);
        uc_mem_write(uc, 0x003c14e8 + 0x54, &render_active, 4);
        uc_mem_write(uc, 0x123c14e8 + 0x54, &render_active, 4);

        // Inicializa estruturas de áudio/mídia globais apontadas por 0x3c142c
        u32 one = 1;
        uc_mem_write(uc, 0x003c142c + 8, &one, 4);
        uc_mem_write(uc, 0x123c142c + 8, &one, 4);
        uc_mem_write(uc, 0x003c142c + 4, &one, 4);
        uc_mem_write(uc, 0x123c142c + 4, &one, 4);
        u32 extra_ptr = EXTRA_OBJ_VA;
        uc_mem_write(uc, 0x003c142c + 0x74, &extra_ptr, 4);
        uc_mem_write(uc, 0x123c142c + 0x74, &extra_ptr, 4);

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
            0xe3a00064, // placeholder (substituído pelo hook)
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

        // Objeto IGL (80 slots: 0 AddRef / 1 Release / 2 QueryInterface / 3..79 gl*).
        // É também o objeto devolvido para AEECLSID_DISPLAY (0x01001001): no Zeebo
        // a interface de display exposta ao jogo é a de 80 slots (ver IGL_*).
        u32 igl_vt = IGL_VTBL_VA;
        uc_mem_write(uc, IGL_OBJ_VA, &igl_vt, 4);
        for (u32 i = 0; i < IGL_SLOTS; ++i) {
            u32 s = IGL_SENTINEL_VA + i * 4u;
            uc_mem_write(uc, IGL_VTBL_VA + i * 4u, &s, 4);
        }

        // Objeto BITMAP vtable
        u32 bmp_vt = BITMAP_VTBL_VA;
        uc_mem_write(uc, BITMAP_OBJ_VA, &bmp_vt, 4);
        for (u32 i = 0; i < 64; ++i) {
            u32 s = 0x50000500u + i * 4u;
            uc_mem_write(uc, BITMAP_VTBL_VA + i * 4u, &s, 4);
        }

        // Seam de PLATAFORMA: o poll de botões do jogo recebe o estado do host.
        // Sem isso a máscara é sempre 0 (nada no .mod a escreve) e o jogo nunca
        // vê botão pressionado -- medido: rotina que escreve a máscara roda 0x.
        uc_hook h_btn = 0;
        uc_hook_add(uc, &h_btn, UC_HOOK_CODE, (void*)hook_poll_return, &ctx,
                    0x1200c3e4u, 0x1200c3e4u);
        uc_hook h_malloc = 0, h_code = 0, h_mem = 0, h_uptime = 0;
        uc_hook_add(uc, &h_malloc, UC_HOOK_CODE, (void*)hook_malloc_stub, &ctx,
                    MALLOC_STUB_VA, MALLOC_STUB_VA);
        uc_hook_add(uc, &h_uptime, UC_HOOK_CODE, (void*)hook_getuptime, &ctx,
                    GETUPTIME_VA, GETUPTIME_VA);
        uc_hook_add(uc, &h_code, UC_HOOK_CODE, (void*)hook_code, &ctx,
                    0x50000000u, 0x50001200u);
        uc_hook h_trace = 0;
        uc_hook_add(uc, &h_trace, UC_HOOK_CODE, (void*)hook_trace_gameloop, &ctx,
                    0x12008614u, 0x12008658u);
        uc_hook_add(uc, &h_mem, UC_HOOK_MEM_UNMAPPED, (void*)mem_hook, &ctx, 1, 0);
        // Instrumento (env-gated): quem escreve no struct de input do jogo?
        // O gameloop lê a máscara de botões via poll em [0x123c14ac+0x10]
        // (0x123c14bc) e nenhum store do módulo a escreve -> tem de vir de um
        // callback. Este hook mostra o PC escritor, ou a ausência dele.
        // CONTROLE POSITIVO: a janela inclui 0x123c14c4 (ponteiro IGL, lido a
        // cada comando GL). Se o hook não disparar nem nele, o instrumento está
        // morto e nenhum "negativo" pode ser reportado a partir dele.
        if (std::getenv("ZEEBO_ZEETRIS_WATCH_INPUT")) {
            // MEM hooks (READ/WRITE) não disparam neste build nem para o ponteiro
            // IGL (lido a cada comando GL) -> instrumento inválido, descartado.
            // Usamos hook de CÓDIGO, que comprovadamente funciona (h_code/h_trace):
            // conta quantas vezes o jogo executa o "poll" da máscara (0x1200c3dc).
            uc_hook h_poll = 0;
            uc_err e5 = uc_hook_add(uc, &h_poll, UC_HOOK_CODE, (void*)hook_poll_count, &ctx,
                                    0x1200c3dcu, 0x1200c3e4u);
            uc_hook h_upd = 0;
            uc_err e6 = uc_hook_add(uc, &h_upd, UC_HOOK_CODE, (void*)hook_input_block, &ctx,
                                    0x1200ad08u, 0x1200ad5cu);
            uc_hook h_st = 0;
            uc_err e7 = uc_hook_add(uc, &h_st, UC_HOOK_CODE, (void*)hook_input_store, &ctx,
                                    0x1200c0a0u, 0x1200c1a0u);
            uc_hook h_bh = 0;
            uc_hook_add(uc, &h_bh, UC_HOOK_CODE, (void*)hook_btn_handler, &ctx,
                        0x1200b040u, 0x1200b0f0u);
            uc_hook h_ap = 0;
            uc_hook_add(uc, &h_ap, UC_HOOK_CODE, (void*)hook_after_poll, &ctx,
                        0x1200ad0cu, 0x1200ad10u);
            uc_hook h_tl = 0;
            uc_err e8 = uc_hook_add(uc, &h_tl, UC_HOOK_CODE, (void*)hook_tex_loader, &ctx,
                                    0x120056fcu, 0x12005a98u);
            (void)e8;
            uc_hook h_pa = 0;
            uc_hook_add(uc, &h_pa, UC_HOOK_CODE, (void*)hook_poll_addr, &ctx,
                        0x1200c3deu, 0x1200c3e2u);
            printf("[ZeetrisRunner] hooks de input: poll err=%d, bloco err=%d, stores err=%d\n",
                   (int)e5, (int)e6, (int)e7);
        }

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
        u32 ishell_ptr = 0x40000000u;
        u32 idisplay_ptr = DISPLAY_OBJ_VA;
        uc_mem_write(uc, ctx.app_ctx_va + 12, &ishell_ptr, 4);
        uc_mem_write(uc, ctx.app_ctx_va + 20, &idisplay_ptr, 4);
        uc_mem_write(uc, ctx.pApplet + 0x14, &idisplay_ptr, 4);

        // Atualiza a estrutura referenciada por 0x120095fc (fp em 0x12008614)
        u32 fp_target = 0;
        if (uc_mem_read(uc, 0x120095fc, &fp_target, 4) == UC_ERR_OK && fp_target != 0) {
            u32 one = 1;
            uc_mem_write(uc, fp_target + 0x54, &one, 4);
            if (fp_target < 0x12000000) {
                uc_mem_write(uc, 0x12000000 + fp_target + 0x54, &one, 4);
            }
        }

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

        // Preenche campos do AppContext: IShell (+12) e IDisplay (+20)
        uc_mem_write(uc, app_ctx_va + 12, &ishell_ptr, 4);
        uc_mem_write(uc, app_ctx_va + 20, &idisplay_ptr, 4);

        ctx.is_running = true;
        std::printf("[ZeetrisRunner] Ciclo de vida inicializado com sucesso (Applet @ 0x%08x, Gameloop @ 0x%08x)\n",
                    ctx.pApplet, ctx.gameloop_cb_va);
        return true;
    }

    static bool step_frame(uc_engine* uc, ZeetrisContext& ctx) {
        if (!ctx.is_running || !uc) return false;
        // Força flag de renderização em 0x123c14e8 + 0x54 antes do frame
        u32 one = 1;
        uc_mem_write(uc, 0x123c14e8 + 0x54, &one, 4);
        uc_mem_write(uc, 0x003c14e8 + 0x54, &one, 4);
        u32 disp_ptr = DISPLAY_OBJ_VA;
        uc_mem_write(uc, 0x003c14c4, &disp_ptr, 4);
        uc_mem_write(uc, 0x123c14c4, &disp_ptr, 4);
        // Após o boot/relocação, 0x123c16c4 guarda o ponteiro ativo de IDisplay
        uc_mem_write(uc, 0x123c16c4, &disp_ptr, 4);
        uc_mem_write(uc, 0x123c16c4 + 4, &disp_ptr, 4);
        u32 sp = STK, lr = 0xF0F0F0F0u;
        uc_reg_write(uc, UC_ARM_REG_R0, &ctx.pApplet);
        uc_reg_write(uc, UC_ARM_REG_SP, &sp);
        uc_reg_write(uc, UC_ARM_REG_LR, &lr);

        uc_err err = uc_emu_start(uc, ctx.gameloop_cb_va, 0xF0F0F0F0u, 0, 500000);
        u32 pc_end = 0;
        uc_reg_read(uc, UC_ARM_REG_PC, &pc_end);
        static int f_count = 0;
        if (f_count++ < 3) {
            std::printf("[ZeetrisRunner] step_frame finished: err=%s pc_end=0x%08x\n",
                        uc_strerror(err), pc_end);
        }
        return err == UC_ERR_OK;
    }

    // Seam de input da PLATAFORMA. O gameloop do jogo faz poll de uma máscara
    // de botões de 16 bits em 0x003c14bc (espelho 0x123c14bc) e despacha por
    // bits {1,2,4,8,0x10,0x20,0x80,0x200}. Medido: o poll roda 180x em 60
    // frames, mas a rotina que ESCREVE a máscara roda 0x -- quem a preenche é o
    // driver de input da plataforma, que não existe quando rodamos só o .mod.
    // Aqui o host faz esse papel (mesma classe de seam do ponteiro IGL).
    // Endereço MEDIDO com hook no ldrh do poll (0x1200c3e0): R3=0x123c16ac, logo
    // a máscara é o halfword em R3+0x10 = 0x123c16bc. (Já errei uma vez supondo
    // 0x3c14bc a partir do literal do pool: o literal certo é outro.)
    // Injeção no RETORNO do poll (medido: 0x1200c3dc lê [0x123c16ac+0x10] e
    // devolve em R0; escrever a memória não persiste porque o jogo a limpa).
    // O hook em 0x1200c3e4 (bx lr) sobrescreve R0 com o estado do host, que é
    // exatamente o que o driver de input da plataforma entregaria.
    static void set_platform_buttons(uc_engine* uc, ZeetrisContext& ctx, u16 mask) {
        (void)uc;
        ctx.platform_buttons = mask;
    }

    // Endereço da máscara, para instrumentos/testes.
    static constexpr u32 BUTTON_MASK_VA = 0x123c16bcu;

    static bool dispatch_key(uc_engine* uc, ZeetrisContext& ctx, u32 key_code, bool pressed) {
        if (!ctx.is_running || !uc) return false;
        // Padrão Qualcomm BREW: EVT_KEY_PRESS = 0x101, EVT_KEY_RELEASE = 0x102
        u32 evt = pressed ? 0x101u : 0x102u;
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
