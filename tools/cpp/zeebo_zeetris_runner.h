// zeebo_zeetris_runner.h — Runner do ciclo de vida completo do Zeetris (AEEMod_Load -> IModule::CreateInstance -> HandleEvent -> Gameloop)
#pragma once
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <fstream>
#include <optional>
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
    // CONTROLE POSITIVO DO INSTRUMENTO: hook identico instalado em 0x1200ac5c
    // (gameloop, executa ~50x/s COMPROVADAMENTE). Se este contador ficar 0,
    // o mecanismo de hook esta quebrado e tex_loader_calls==0 nao significa nada.
    uint32_t hook_selftest_calls = 0;
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

    // Sistema de arquivos virtual (IFileMgr / IFile) servindo data/*
    std::string data_dir = "/home/rafaelfrequiao/Downloads/mod/zeetris/data";
    struct OpenFileInfo {
        std::string name;
        std::vector<uint8_t> data;
        uint32_t position = 0;
    };
    std::map<uint32_t, OpenFileInfo> open_files;
    uint32_t file_open_calls = 0;
    uint32_t file_read_calls = 0;

    // Callback de áudio PCM emitido pelo IMedia::Play
    std::function<void(const int16_t* pcm, size_t count, int sample_rate, int channels)> on_audio_play;
    std::vector<int16_t> current_media_pcm;
    int current_media_rate = 0;
    int current_media_channels = 0;
};

class ZeetrisRunner {
public:
    static constexpr u32 ENTRY_VA        = 0x12000048u; // AEEMod_Load
    static constexpr u32 LOAD_VA         = 0x12000000u;
    static constexpr u32 STK             = 0x00200000u;
    static constexpr u32 ZEETRIS_CLSID   = 0x12345678u;
    static constexpr u32 CREATE_CLSID    = 0x0106e415u;
    static constexpr u32 RETURN_SENTINEL = 0xF0F0F0F0u;

    static constexpr u32 key_event(bool pressed) {
        // Qualcomm AEEEvent: press=0x0100, release=0x0101.
        return pressed ? 0x0100u : 0x0101u;
    }

    static bool completed_at_return_sentinel(uc_engine* uc, uc_err err) {
        if (!uc || err != UC_ERR_OK) return false;
        u32 pc = 0;
        if (uc_reg_read(uc, UC_ARM_REG_PC, &pc) != UC_ERR_OK) return false;
        return (pc & ~1u) == (RETURN_SENTINEL & ~1u);
    }

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
    static constexpr u32 FILEMGR_OBJ_VA  = 0x60002000u;
    static constexpr u32 FILEMGR_VTBL_VA = 0x60003000u;
    static constexpr u32 FILE_OBJ_BASE_VA= 0x60006000u;
    static constexpr u32 FILE_VTBL_VA    = 0x60009000u;
    static constexpr u32 NOTIFY_OBJ_VA   = 0x6000A000u;
    static constexpr u32 NOTIFY_VTBL_VA  = 0x6000B000u;

    struct WavAudio {
        int sample_rate = 0;
        int channels = 0;
        std::vector<int16_t> samples;
    };

    static inline uint16_t read_u16_le(const uint8_t* p) {
        return static_cast<uint16_t>(p[0]) | static_cast<uint16_t>(p[1] << 8);
    }
    static inline uint32_t read_u32_le(const uint8_t* p) {
        return static_cast<uint32_t>(p[0]) |
               (static_cast<uint32_t>(p[1]) << 8) |
               (static_cast<uint32_t>(p[2]) << 16) |
               (static_cast<uint32_t>(p[3]) << 24);
    }

    static std::optional<WavAudio> parse_wav(const uint8_t* data, size_t size) {
        if (size < 12 || std::memcmp(data, "RIFF", 4) != 0 || std::memcmp(data + 8, "WAVE", 4) != 0) {
            return std::nullopt;
        }
        bool have_fmt = false;
        uint16_t audio_format = 0, channels = 0, bits_per_sample = 0;
        uint32_t sample_rate = 0, data_size = 0;
        const uint8_t* data_chunk = nullptr;

        size_t pos = 12;
        while (pos + 8 <= size) {
            const uint8_t* chunk_id = data + pos;
            uint32_t chunk_size = read_u32_le(data + pos + 4);
            size_t chunk_data_offset = pos + 8;
            if (chunk_data_offset + chunk_size > size) break;

            if (std::memcmp(chunk_id, "fmt ", 4) == 0 && chunk_size >= 16) {
                const uint8_t* fmt = data + chunk_data_offset;
                audio_format = read_u16_le(fmt + 0);
                channels = read_u16_le(fmt + 2);
                sample_rate = read_u32_le(fmt + 4);
                bits_per_sample = read_u16_le(fmt + 14);
                have_fmt = true;
            } else if (std::memcmp(chunk_id, "data", 4) == 0) {
                data_chunk = data + chunk_data_offset;
                data_size = chunk_size;
            }
            pos = chunk_data_offset + chunk_size + (chunk_size & 1);
        }

        if (!have_fmt || data_chunk == nullptr) return std::nullopt;
        if (audio_format != 1) return std::nullopt; // PCM
        if (channels != 1 && channels != 2) return std::nullopt;
        if (bits_per_sample != 8 && bits_per_sample != 16) return std::nullopt;

        WavAudio out;
        out.sample_rate = static_cast<int>(sample_rate);
        out.channels = channels;

        if (bits_per_sample == 16) {
            size_t sample_count = data_size / 2;
            out.samples.resize(sample_count);
            for (size_t i = 0; i < sample_count; ++i) {
                out.samples[i] = static_cast<int16_t>(read_u16_le(data_chunk + i * 2));
            }
        } else {
            size_t sample_count = data_size;
            out.samples.resize(sample_count);
            for (size_t i = 0; i < sample_count; ++i) {
                int unsigned_sample = data_chunk[i];
                out.samples[i] = static_cast<int16_t>((unsigned_sample - 128) * 256);
            }
        }
        return out;
    }

    static std::string read_c_string(uc_engine* uc, u32 addr) {
        std::string s;
        if (!uc || addr == 0) return s;
        for (u32 a = addr; ; ++a) {
            char c = 0;
            if (uc_mem_read(uc, a, &c, 1) != UC_ERR_OK || c == '\0') break;
            s.push_back(c);
            if (s.size() > 512) break; // salvaguarda contra strings corrompidas
        }
        return s;
    }

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

    // Controle positivo: mesmo tipo de hook, endereco sabidamente executado.
    static void hook_selftest(uc_engine* uc, uint64_t addr, uint32_t size, void* user_data) {
        (void)uc; (void)addr; (void)size;
        if (auto* ctx = reinterpret_cast<ZeetrisContext*>(user_data)) ctx->hook_selftest_calls++;
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
                } else if (clsid == 0x01001001u) { // AEECLSID_DISPLAY
                    obj_ptr = DISPLAY_OBJ_VA;
                } else if (clsid == 0x01001003u) { // AEECLSID_FILEMGR
                    obj_ptr = FILEMGR_OBJ_VA;
                } else if (clsid == 0x0106c411u) { // AEECLSID_HID (driver de entrada Qualcomm)
                    obj_ptr = EXTRA_OBJ_VA;
                } else if (clsid == 0x01005511u) { // Notification/Progress service
                    obj_ptr = NOTIFY_OBJ_VA;
                } else if (clsid == 0x01014bc3u) { // AEECLSID_GL (OpenGL ES 1.1)
                    obj_ptr = IGL_OBJ_VA;
                } else if (clsid == 0x01014bc4u) { // AEECLSID_EGL (EGL 1.1)
                    obj_ptr = DISPLAY_OBJ_VA; // expõe vtable compatível
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

                if (nParamID == 1 /* MM_PARM_MEDIA_DATA */ && p1 != 0) {
                    u32 cls_data = 0, data_ptr = 0, data_size = 0;
                    uc_mem_read(uc, p1 + 0, &cls_data, 4);
                    uc_mem_read(uc, p1 + 4, &data_ptr, 4);
                    uc_mem_read(uc, p1 + 8, &data_size, 4);
                    if (cls_data == 0 /* kMmdFileName */ && data_ptr != 0) {
                        std::string audio_fname = read_c_string(uc, data_ptr);
                        std::string host_path = audio_fname;
                        if (host_path.rfind("data/", 0) == 0 || host_path.rfind("data\\", 0) == 0) {
                            host_path = ctx->data_dir + "/" + host_path.substr(5);
                        } else if (host_path.find('/') == std::string::npos && host_path.find('\\') == std::string::npos) {
                            host_path = ctx->data_dir + "/" + host_path;
                        }
                        auto audio_bytes = load_file(host_path);
                        if (!audio_bytes.empty()) {
                            auto parsed = parse_wav(audio_bytes.data(), audio_bytes.size());
                            if (parsed) {
                                ctx->current_media_pcm = std::move(parsed->samples);
                                ctx->current_media_rate = parsed->sample_rate;
                                ctx->current_media_channels = parsed->channels;
                                std::printf("[IMedia] WAV carregado: '%s' (%d Hz, %d canais, %zu amostras)\n",
                                            audio_fname.c_str(), parsed->sample_rate, parsed->channels, ctx->current_media_pcm.size());
                            } else {
                                std::printf("[IMedia] Arquivo de audio '%s' carregado mas nao e WAV PCM\n", audio_fname.c_str());
                            }
                        }
                    }
                }
            } else if (slot == 6 && ctx) { // Play(IMedia *po)
                ctx->media_play_calls++;
                std::printf("[IMedia] Play called! (total=%u)\n", ctx->media_play_calls);
                if (ctx->on_audio_play && !ctx->current_media_pcm.empty()) {
                    ctx->on_audio_play(ctx->current_media_pcm.data(), ctx->current_media_pcm.size(),
                                       ctx->current_media_rate, ctx->current_media_channels);
                }
            } else {
                std::printf("[IMedia] Chamada ao slot %u (lr=0x%08x)\n", slot, lr);
            }
            uc_reg_write(uc, UC_ARM_REG_R0, &zero);
            uc_reg_write(uc, UC_ARM_REG_PC, &lr);
        } else if (pc >= 0x50000200u && pc < 0x50000300u) {
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
        } else if (pc >= 0x50000300u && pc < 0x50000400u) {
            // IFileMgr vtable slots
            u32 lr = 0, zero = 0;
            uc_reg_read(uc, UC_ARM_REG_LR, &lr);
            u32 slot = (pc - 0x50000300u) / 4u;
            if (slot == 2 && ctx) { // OpenFile(IFileMgr* po, const char* pszFile, OpenFileMode mode)
                u32 name_addr = 0, mode = 0;
                uc_reg_read(uc, UC_ARM_REG_R1, &name_addr);
                uc_reg_read(uc, UC_ARM_REG_R2, &mode);
                std::string fname = read_c_string(uc, name_addr);
                ctx->file_open_calls++;

                // Resolve caminho no host
                std::string host_path = fname;
                // Normaliza prefixo "data/" ou "data\"
                if (host_path.rfind("data/", 0) == 0 || host_path.rfind("data\\", 0) == 0) {
                    host_path = ctx->data_dir + "/" + host_path.substr(5);
                } else if (host_path.find('/') == std::string::npos && host_path.find('\\') == std::string::npos) {
                    host_path = ctx->data_dir + "/" + host_path;
                }

                auto file_bytes = load_file(host_path);
                u32 file_handle = 0;
                if (!file_bytes.empty()) {
                    uint32_t handle_idx = ctx->open_files.size();
                    file_handle = FILE_OBJ_BASE_VA + handle_idx * 0x100u;
                    u32 f_vt = FILE_VTBL_VA;
                    uc_mem_write(uc, file_handle, &f_vt, 4);

                    ZeetrisContext::OpenFileInfo of;
                    of.name = fname;
                    of.data = std::move(file_bytes);
                    of.position = 0;
                    ctx->open_files[file_handle] = std::move(of);
                    std::printf("[IFileMgr] OpenFile('%s' -> '%s', size=%zu) => 0x%08x\n",
                                fname.c_str(), host_path.c_str(), ctx->open_files[file_handle].data.size(), file_handle);
                } else {
                    std::printf("[IFileMgr] OpenFile('%s' -> '%s') FALHOU (arquivo nao encontrado)\n",
                                fname.c_str(), host_path.c_str());
                }
                uc_reg_write(uc, UC_ARM_REG_R0, &file_handle);
                uc_reg_write(uc, UC_ARM_REG_PC, &lr);
            } else if (slot == 7 && ctx) { // Test(IFileMgr* po, const char* pszName) -> 0 se existe, 1 se não
                u32 name_addr = 0;
                uc_reg_read(uc, UC_ARM_REG_R1, &name_addr);
                std::string fname = read_c_string(uc, name_addr);
                std::string host_path = fname;
                if (host_path.rfind("data/", 0) == 0 || host_path.rfind("data\\", 0) == 0) {
                    host_path = ctx->data_dir + "/" + host_path.substr(5);
                } else if (host_path.find('/') == std::string::npos && host_path.find('\\') == std::string::npos) {
                    host_path = ctx->data_dir + "/" + host_path;
                }
                std::ifstream f(host_path, std::ios::binary);
                u32 ret = f.good() ? 0u : 1u;
                std::printf("[IFileMgr] Test('%s' -> '%s') => %u\n", fname.c_str(), host_path.c_str(), ret);
                uc_reg_write(uc, UC_ARM_REG_R0, &ret);
                uc_reg_write(uc, UC_ARM_REG_PC, &lr);
            } else if (slot == 3 && ctx) { // GetInfo(IFileMgr* po, const char* pszName, FileInfo* pInfo)
                u32 name_addr = 0, pInfo = 0;
                uc_reg_read(uc, UC_ARM_REG_R1, &name_addr);
                uc_reg_read(uc, UC_ARM_REG_R2, &pInfo);
                std::string fname = read_c_string(uc, name_addr);
                std::string host_path = fname;
                if (host_path.rfind("data/", 0) == 0 || host_path.rfind("data\\", 0) == 0) {
                    host_path = ctx->data_dir + "/" + host_path.substr(5);
                } else if (host_path.find('/') == std::string::npos && host_path.find('\\') == std::string::npos) {
                    host_path = ctx->data_dir + "/" + host_path;
                }
                std::ifstream f(host_path, std::ios::binary | std::ios::ate);
                u32 ret = 1;
                if (f.good() && pInfo != 0) {
                    ret = 0;
                    u32 size = static_cast<u32>(f.tellg());
                    u8 attrib = 0;
                    u32 date = 0;
                    uc_mem_write(uc, pInfo + 0, &attrib, 1);
                    uc_mem_write(uc, pInfo + 4, &date, 4);
                    uc_mem_write(uc, pInfo + 8, &size, 4);
                    char name_buf[64] = {0};
                    std::strncpy(name_buf, fname.c_str(), 63);
                    uc_mem_write(uc, pInfo + 12, name_buf, 64);
                }
                std::printf("[IFileMgr] GetInfo('%s' -> '%s') => ret=%u\n", fname.c_str(), host_path.c_str(), ret);
                uc_reg_write(uc, UC_ARM_REG_R0, &ret);
                uc_reg_write(uc, UC_ARM_REG_PC, &lr);
            } else if (slot == 8) { // GetFreeSpace(IFileMgr* po, uint32* pdwTotal)
                u32 pdwTotal = 0;
                uc_reg_read(uc, UC_ARM_REG_R1, &pdwTotal);
                if (pdwTotal != 0) {
                    u32 total = 1024 * 1024;
                    uc_mem_write(uc, pdwTotal, &total, 4);
                }
                u32 free_sp = 1024 * 1024;
                uc_reg_write(uc, UC_ARM_REG_R0, &free_sp);
                uc_reg_write(uc, UC_ARM_REG_PC, &lr);
            } else {
                std::printf("[IFileMgr] Chamada ao slot %u (lr=0x%08x)\n", slot, lr);
                uc_reg_write(uc, UC_ARM_REG_R0, &zero);
                uc_reg_write(uc, UC_ARM_REG_PC, &lr);
            }
        } else if (pc >= 0x50000600u && pc < 0x50000700u) {
            // IFile vtable slots
            u32 lr = 0, zero = 0;
            uc_reg_read(uc, UC_ARM_REG_LR, &lr);
            u32 slot = (pc - 0x50000600u) / 4u;
            u32 handle = 0;
            uc_reg_read(uc, UC_ARM_REG_R0, &handle);

            if (slot == 3 && ctx) { // Read(IFile* po, void* pDest, uint32 nWant)
                u32 pDest = 0, nWant = 0;
                uc_reg_read(uc, UC_ARM_REG_R1, &pDest);
                uc_reg_read(uc, UC_ARM_REG_R2, &nWant);
                ctx->file_read_calls++;

                auto it = ctx->open_files.find(handle);
                u32 bytes_read = 0;
                if (it != ctx->open_files.end() && pDest != 0) {
                    auto& of = it->second;
                    if (of.position < of.data.size()) {
                        size_t avail = of.data.size() - of.position;
                        bytes_read = static_cast<u32>(std::min(static_cast<size_t>(nWant), avail));
                        uc_mem_write(uc, pDest, of.data.data() + of.position, bytes_read);
                        of.position += bytes_read;
                    }
                    std::printf("[IFile] Read(handle=0x%08x, want=%u, pos_after=%u) => %u bytes\n",
                                handle, nWant, of.position, bytes_read);
                } else {
                    std::printf("[IFile] Read(handle=0x%08x) handle desconhecido!\n", handle);
                }
                uc_reg_write(uc, UC_ARM_REG_R0, &bytes_read);
                uc_reg_write(uc, UC_ARM_REG_PC, &lr);
            } else if (slot == 7 && ctx) { // Seek(IFile* po, FileSeekType seek, int32 moveDistance)
                u32 seek_type = 0;
                int32_t dist = 0;
                uc_reg_read(uc, UC_ARM_REG_R1, &seek_type);
                uc_reg_read(uc, UC_ARM_REG_R2, &dist);

                auto it = ctx->open_files.find(handle);
                u32 ret = 0;
                if (it != ctx->open_files.end()) {
                    auto& of = it->second;
                    int64_t new_pos = of.position;
                    if (seek_type == 0) new_pos = dist; // _SEEK_START
                    else if (seek_type == 1) new_pos = static_cast<int64_t>(of.data.size()) + dist; // _SEEK_END
                    else if (seek_type == 2) new_pos += dist; // _SEEK_CURRENT

                    if (new_pos < 0) new_pos = 0;
                    if (new_pos > static_cast<int64_t>(of.data.size())) new_pos = of.data.size();
                    of.position = static_cast<uint32_t>(new_pos);
                    std::printf("[IFile] Seek(handle=0x%08x, type=%u, dist=%d) => new_pos=%u\n",
                                handle, seek_type, dist, of.position);
                }
                uc_reg_write(uc, UC_ARM_REG_R0, &ret);
                uc_reg_write(uc, UC_ARM_REG_PC, &lr);
            } else if (slot == 6 && ctx) { // GetInfo(IFile* po, FileInfo* pInfo)
                u32 pInfo = 0;
                uc_reg_read(uc, UC_ARM_REG_R1, &pInfo);
                auto it = ctx->open_files.find(handle);
                if (it != ctx->open_files.end() && pInfo != 0) {
                    const auto& of = it->second;
                    // AEEFileInfo: { char attrib; uint32 dwCreationDate; uint32 dwSize; char szName[64]; }
                    u8 attrib = 0;
                    u32 date = 0;
                    u32 size = static_cast<u32>(of.data.size());
                    uc_mem_write(uc, pInfo + 0, &attrib, 1);
                    uc_mem_write(uc, pInfo + 4, &date, 4);
                    uc_mem_write(uc, pInfo + 8, &size, 4);
                    char name_buf[64] = {0};
                    std::strncpy(name_buf, of.name.c_str(), 63);
                    uc_mem_write(uc, pInfo + 12, name_buf, 64);
                }
                uc_reg_write(uc, UC_ARM_REG_R0, &zero);
                uc_reg_write(uc, UC_ARM_REG_PC, &lr);
            } else {
                std::printf("[IFile] Chamada ao slot %u (handle=0x%08x, lr=0x%08x)\n", slot, handle, lr);
                uc_reg_write(uc, UC_ARM_REG_R0, &zero);
                uc_reg_write(uc, UC_ARM_REG_PC, &lr);
            }
        } else if (pc >= 0x50000700u && pc < 0x50000800u) {
            u32 lr = 0, zero = 0;
            uc_reg_read(uc, UC_ARM_REG_LR, &lr);
            u32 slot = (pc - 0x50000700u) / 4u;
            u32 r1 = 0, r2 = 0, r3 = 0;
            uc_reg_read(uc, UC_ARM_REG_R1, &r1);
            uc_reg_read(uc, UC_ARM_REG_R2, &r2);
            uc_reg_read(uc, UC_ARM_REG_R3, &r3);
            std::printf("[NOTIFY 0x01005511] Chamada ao slot %u (lr=0x%08x) r1=0x%08x r2=0x%08x r3=0x%08x\n",
                        slot, lr, r1, r2, r3);
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

        // CONTROLE NEGATIVO (env-gated): ZEEBO_ZEETRIS_NO_GLOBAL_PATCHES=1 desliga
        // TODOS os remendos manuais de globais abaixo. Hipotese do RCA: os remendos
        // sao inertes e o defeito e estrutural (secao RW nao carregada + sem
        // relocacao). Se a saida for identica com e sem eles, a hipotese se confirma.
        const bool no_global_patches =
            std::getenv("ZEEBO_ZEETRIS_NO_GLOBAL_PATCHES") != nullptr;
        if (!no_global_patches) {
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
        }

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

        // Objeto IFileMgr vtable
        u32 fm_vt = FILEMGR_VTBL_VA;
        uc_mem_write(uc, FILEMGR_OBJ_VA, &fm_vt, 4);
        for (u32 i = 0; i < 64; ++i) {
            u32 s = 0x50000300u + i * 4u;
            uc_mem_write(uc, FILEMGR_VTBL_VA + i * 4u, &s, 4);
        }

        // Shared IFile vtable
        for (u32 i = 0; i < 64; ++i) {
            u32 s = 0x50000600u + i * 4u;
            uc_mem_write(uc, FILE_VTBL_VA + i * 4u, &s, 4);
        }

        // Objeto 0x01005511 (Notification/Progress) vtable
        u32 notif_vt = NOTIFY_VTBL_VA;
        uc_mem_write(uc, NOTIFY_OBJ_VA, &notif_vt, 4);
        for (u32 i = 0; i < 64; ++i) {
            u32 s = 0x50000700u + i * 4u;
            uc_mem_write(uc, NOTIFY_VTBL_VA + i * 4u, &s, 4);
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

        // --- Instrumento de carga de textura + seu CONTROLE POSITIVO ---
        // Antes, ambos viviam sob ZEEBO_ZEETRIS_WATCH_INPUT; com o gate desligado
        // tex_loader_calls era 0 por construcao (hook nunca instalado), o que foi
        // lido erradamente como "a rotina nunca executa". Agora sao incondicionais
        // e o par tex/selftest permite distinguir "nao executa" de "nao medido".
        uc_hook h_tl_always = 0;
        uc_err e_tl = uc_hook_add(uc, &h_tl_always, UC_HOOK_CODE, (void*)hook_tex_loader,
                                  &ctx, 0x120056fcu, 0x120056ffu);
        uc_hook h_sf = 0;
        uc_err e_sf = uc_hook_add(uc, &h_sf, UC_HOOK_CODE, (void*)hook_selftest,
                                  &ctx, 0x1200ac5cu, 0x1200ac5fu);
        if (e_tl != UC_ERR_OK || e_sf != UC_ERR_OK)
            printf("[ZeetrisRunner] AVISO: uc_hook_add falhou (tex=%d selftest=%d)\n",
                   (int)e_tl, (int)e_sf);
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
        u32 sp = STK, lr = RETURN_SENTINEL;
        uc_reg_write(uc, UC_ARM_REG_R0, &ishell_obj);
        uc_reg_write(uc, UC_ARM_REG_R1, &ishell_obj);
        uc_reg_write(uc, UC_ARM_REG_R2, &ppMod);
        uc_reg_write(uc, UC_ARM_REG_SP, &sp);
        uc_reg_write(uc, UC_ARM_REG_LR, &lr);

        uc_err err = uc_emu_start(uc, ENTRY_VA, RETURN_SENTINEL, 0, 500000);
        if (!completed_at_return_sentinel(uc, err)) {
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

        err = uc_emu_start(uc, create_instance_va, RETURN_SENTINEL, 0, 500000);
        if (!completed_at_return_sentinel(uc, err)) {
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

        err = uc_emu_start(uc, ctx.handle_event_va, RETURN_SENTINEL, 0, 500000);
        if (!completed_at_return_sentinel(uc, err)) {
            std::printf("[ZeetrisRunner] EVT_APP_START falhou: %s\n", uc_strerror(err));
            return false;
        }

        // Contexto do Applet [pApplet + 0x20].
        // ATENÇÃO: este AppContext é FABRICADO pelo runner (região zero em
        // 0x30010000), não é o contexto que o BREW/CRT do jogo criaria. Fica
        // env-gated para poder medir a hipótese de que é ele que mantém o jogo
        // num estado "vazio" — com o gate ligado, o jogo fica com o que ele
        // mesmo puser ali (ou com o que o .mod trouxer).
        // MEDIÇÃO CRÍTICA: o que o JOGO deixou em [pApplet+0x20] depois do
        // EVT_APP_START? Se for um ponteiro de heap, o jogo alocou/inicializou o
        // próprio contexto e a linha seguinte o CLOBA com uma região zerada.
        {
            u32 game_ctx = 0;
            if (uc_mem_read(uc, ctx.pApplet + 0x20, &game_ctx, 4) == UC_ERR_OK) {
                std::printf("[ZeetrisRunner] [pApplet+0x20] DEPOIS do EVT_APP_START "
                            "= 0x%08x%s\n", game_ctx,
                            (game_ctx >= 0x30005000u && game_ctx < 0x30020000u)
                                ? "  <== ponteiro do heap do jogo (o runner CLOBA!)"
                                : "  (nao parece ponteiro de heap do jogo)");
            }
        }
        // PADRÃO: o jogo constrói o próprio contexto (evt==0 -> malloc+init),
        // medido em 0x1200ab4c. ZEEBO_ZEETRIS_SYNTH_APPCTX=1 volta ao paliativo
        // antigo (região sintética zerada em 0x30010000) só para comparação.
        if (!std::getenv("ZEEBO_ZEETRIS_SYNTH_APPCTX")) {
            // MEDIDO: com evt==0 o jogo faz malloc(size) + init e grava o próprio
            // contexto em [pApplet+0x20] (0x1200ab4c..0x1200ab6c). Não fabricamos
            // nada: deixamos o jogo construir o contexto dele.
            ctx.is_running = true;
            bool ok_ev0 = dispatch_app_event(uc, ctx, 0u);
            u32 game_ctx = 0;
            uc_mem_read(uc, ctx.pApplet + 0x20, &game_ctx, 4);
            std::printf("[ZeetrisRunner] EV0: disp(evt=0)=%d -> [pApplet+0x20]=0x%08x%s\n",
                        (int)ok_ev0, game_ctx,
                        (game_ctx >= 0x30005000u && game_ctx < 0x30020000u)
                            ? "  (contexto do PROPRIO jogo, no heap)" : "");
        } else if (!std::getenv("ZEEBO_ZEETRIS_NO_APPCTX")) {
            u32 app_ctx_va = 0x30010000u;
            uc_mem_write(uc, ctx.pApplet + 0x20, &app_ctx_va, 4);
            uc_mem_write(uc, app_ctx_va + 12, &ishell_ptr, 4);
            uc_mem_write(uc, app_ctx_va + 20, &idisplay_ptr, 4);
        } else {
            std::printf("[ZeetrisRunner] ZEEBO_ZEETRIS_NO_APPCTX: AppContext NAO "
                        "injetado (medindo a hipotese)\n");
        }

        ctx.is_running = true;
        std::printf("[ZeetrisRunner] Ciclo de vida inicializado com sucesso (Applet @ 0x%08x, Gameloop @ 0x%08x)\n",
                    ctx.pApplet, ctx.gameloop_cb_va);
        return true;
    }

    static bool step_frame(uc_engine* uc, ZeetrisContext& ctx) {
        if (!ctx.is_running || !uc) return false;
        // Controle negativo: mesmo gate dos remendos de setup_and_start.
        static const bool no_global_patches =
            std::getenv("ZEEBO_ZEETRIS_NO_GLOBAL_PATCHES") != nullptr;
        if (!no_global_patches) {
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
        }
        u32 sp = STK, lr = RETURN_SENTINEL;
        uc_reg_write(uc, UC_ARM_REG_R0, &ctx.pApplet);
        uc_reg_write(uc, UC_ARM_REG_SP, &sp);
        uc_reg_write(uc, UC_ARM_REG_LR, &lr);

        uc_err err = uc_emu_start(uc, ctx.gameloop_cb_va, RETURN_SENTINEL, 0, 500000);
        u32 pc_end = 0;
        uc_reg_read(uc, UC_ARM_REG_PC, &pc_end);
        static int f_count = 0;
        if (f_count++ < 3) {
            std::printf("[ZeetrisRunner] step_frame finished: err=%s pc_end=0x%08x\n",
                        uc_strerror(err), pc_end);
        }
        return completed_at_return_sentinel(uc, err);
    }

    // Despacha um evento de aplicação arbitrário ao HandleEvent do applet.
    // Existe para MEDIR (não para assumir) se algum evento além do EVT_APP_START
    // faz o jogo inicializar o próprio contexto/carregar assets.
    static bool dispatch_app_event(uc_engine* uc, ZeetrisContext& ctx, u32 evt, u32 wparam = 0) {
        if (!ctx.is_running || !uc) return false;
        u32 sp = STK, lr = RETURN_SENTINEL, r3 = 0;
        uc_reg_write(uc, UC_ARM_REG_R0, &ctx.pApplet);
        uc_reg_write(uc, UC_ARM_REG_R1, &evt);
        uc_reg_write(uc, UC_ARM_REG_R2, &wparam);
        uc_reg_write(uc, UC_ARM_REG_R3, &r3);
        uc_reg_write(uc, UC_ARM_REG_SP, &sp);
        uc_reg_write(uc, UC_ARM_REG_LR, &lr);
        uc_err err = uc_emu_start(uc, ctx.handle_event_va, RETURN_SENTINEL, 0, 500000);
        if (!completed_at_return_sentinel(uc, err)) return false;
        u32 r0 = 0;
        uc_reg_read(uc, UC_ARM_REG_R0, &r0);
        return r0 == 1;
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
        const u32 evt = key_event(pressed);
        u32 sp = STK, lr = RETURN_SENTINEL;
        u32 r3 = 0;
        uc_reg_write(uc, UC_ARM_REG_R0, &ctx.pApplet);
        uc_reg_write(uc, UC_ARM_REG_R1, &evt);
        uc_reg_write(uc, UC_ARM_REG_R2, &key_code);
        uc_reg_write(uc, UC_ARM_REG_R3, &r3);
        uc_reg_write(uc, UC_ARM_REG_SP, &sp);
        uc_reg_write(uc, UC_ARM_REG_LR, &lr);

        uc_err err = uc_emu_start(uc, ctx.handle_event_va, RETURN_SENTINEL, 0, 500000);
        if (!completed_at_return_sentinel(uc, err)) return false;
        u32 r0 = 0;
        uc_reg_read(uc, UC_ARM_REG_R0, &r0);
        return r0 == 1;
    }
};

} // namespace zeebo::zeetris
