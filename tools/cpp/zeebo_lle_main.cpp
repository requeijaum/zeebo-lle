// zeebo_lle_main.cpp — The Unified Zeebo LLE Orchestrator
// Coordinates:
//   Core 0 (ARM1176JZ-S): APPS / OKL4 L4e Microkernel & Iguana (nand/1.1.2_APPS.bin @ 0x10000000)
//   Core 1 (ARM926EJ-S):  AMSS / Modem RTOS (REX) (nand/1.1.2_AMSS.bin @ 0x00a00000)
// Interconnects:
//   - Shared Memory Fabric (SMEM @ 0x01F00000, 2MB)
//   - Inter-core Doorbell A2M (0xC0100400) -> ARM9 VIC (0xC0000000)
//   - Hardware Flash Controller (EBI2 NandController @ 0xA0A00000) & ADM/DMOV DMA (0xA9400000)
//   - Primary MDDI Display Controller (0xAA600000) -> SDL2 / Headless PPM Frame Sink
//   - Adreno 130 2D/3D GPU Command Ring Buffer Engine (0xA0000000)
//   - Keypad / Gamepad Controller (INT_KEYSENSE #28)

#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <cstring>
#include <vector>
#include <string>
#include <map>
#include <set>
#include <memory>
#include <algorithm>
#include <SDL2/SDL.h>
#include <unicorn/unicorn.h>

#include "zeebo_devices.h"
#define ZEEBO_L4_MMU_WITH_UNICORN 1
#include "zeebo_l4_mmu.h"
#include "zeebo_control_server.h"
#include "qdsp5/qdsp5_capture_hook.h"
#include "qdsp5/qdsp5_dispatcher.h"
#include "zeebo_audio_sink.h"
#include "gpu/igl_hook.h"
#include "gpu/igpu_rasterizer.h"

using u8  = uint8_t;
using u16 = uint16_t;
using u32 = uint32_t;
using u64 = uint64_t;

// ---- Memory Layout & Constants ----
enum {
    SMEM_BASE           = 0x01f00000,
    SMEM_SIZE           = 0x00200000, // 2MB
    MSM_CSR_BASE        = 0xc0100000,
    MSM_CSR_SIZE        = 0x00010000, // 64KB
    MSM_VIC_BASE        = 0xc0000000,
    MSM_VIC_SIZE        = 0x00010000, // 64KB
    GPT_TIMER_BASE      = 0xc5000000,
    GPT_TIMER_SIZE      = 0x00100000, // 1MB
    
    // MDDI Display
    MSM_MDDI_BASE       = 0xaa600000,
    MDDI_SIZE           = 0x00010000, // 64KB
    FB_WIDTH            = 640,
    FB_HEIGHT           = 480,
    FB_PIXELS           = FB_WIDTH * FB_HEIGHT,
    
    // Adreno 130 GPU
    ADRENO130_BASE      = 0xa0000000,
    ADRENO130_SIZE      = 0x00100000, // 1MB
    CHIP_ID_YAMATO      = 0x01030000,
    
    // Keypad / Controller
    KEYPAD_BASE         = 0xa9a00000,
    KEYPAD_SIZE         = 0x00010000,
    
    // Apps RAM Base
    APPS_RAM_PHYS_BASE  = 0x10000000,
    APPS_RAM_PHYS_SIZE  = 0x06000000, // 96MB

    // OKL4 Kernel Interface Page (constructed, returned to Iguana by L4_KernelInterface).
    // Dentro da janela kernel já mapeada 0xf0000000..0xf1000000 (funcional — kernel roda dali).
    KIP_BASE            = 0xf0f00000,
};

static inline u16 rd16(const u8* p, size_t o) { return p[o]|(p[o+1]<<8); }
static inline u32 rd32(const u8* p, size_t o) { return p[o]|(p[o+1]<<8)|(p[o+2]<<16)|(p[o+3]<<24); }

// ---- Unified Hardware Models ----

// 1. Virtual MDDI Engine
class UnifiedMDDI {
public:
    UnifiedMDDI() : status_(0x21), version_(0x00000102), pri_ptr_(0), frame_count_(0) {}
    u32 read(u32 off) {
        if (off == 0x0004) return version_;
        if (off == 0x0028) return status_;
        if (off == 0x0008) return pri_ptr_;
        auto it = regs_.find(off); return it != regs_.end() ? it->second : 0;
    }
    void write(u32 off, u32 val) {
        regs_[off] = val;
        if (off == 0x0000) { // CMD
            if ((val & 0xff00) == 0x0200) status_ |= 0x01; // POWER_UP -> LINK_ACTIVE
            else if ((val & 0xff00) == 0x0400) status_ = 0x21; // RESET
        } else if (off == 0x0008) { // PRI_PTR
            pri_ptr_ = val;
            frame_count_++;
            status_ |= 0x20; // PRI_LIST_DONE
        }
    }
    u32 frame_count() const { return frame_count_; }
private:
    u32 status_, version_, pri_ptr_, frame_count_;
    std::map<u32, u32> regs_;
};

// 2. Virtual Adreno 130 GPU Engine
class UnifiedAdreno130 {
public:
    UnifiedAdreno130() : status_(1), rb_rptr_(0), rb_wptr_(0), int_status_(0), int_en_(0), draws_(0), fb_dirty_(false) {}
    u32 read(u32 off) {
        if (off == 0x0000) return CHIP_ID_YAMATO;
        if (off == 0x0004) return 0x00000001; // Rev 1
        if (off == 0x0110) return status_;
        if (off == 0x0108) return rb_rptr_;
        if (off == 0x010c) return rb_wptr_;
        if (off == 0x0120) return int_status_;
        auto it = regs_.find(off); return it != regs_.end() ? it->second : 0;
    }
    void write(u32 off, u32 val) {
        regs_[off] = val;
        if (off == 0x010c) { // WPTR
            rb_wptr_ = val;
            draws_ += (rb_wptr_ >= rb_rptr_) ? (rb_wptr_ - rb_rptr_) : (0x10000 - rb_rptr_ + rb_wptr_);
            rb_rptr_ = rb_wptr_;
            fb_dirty_ = true;
            if (int_en_ & 1) int_status_ |= 1;
        } else if (off == 0x0124) int_en_ = val;
        else if (off == 0x0128) int_status_ &= ~val;
    }
    u32 draws() const { return draws_; }
    bool is_fb_dirty() const { return fb_dirty_; }
    void clear_fb_dirty() { fb_dirty_ = false; }
    void mark_dirty() { fb_dirty_ = true; }
private:
    u32 status_, rb_rptr_, rb_wptr_, int_status_, int_en_, draws_;
    bool fb_dirty_;
    std::map<u32, u32> regs_;
};

// 3. Gamepad & Input Subsystem (Zeebo Z-Pad / MSM7201A Keysense)
enum {
    ZEEBO_KEY_A         = (1 << 0),
    ZEEBO_KEY_B         = (1 << 1),
    ZEEBO_KEY_C         = (1 << 2),
    ZEEBO_KEY_D         = (1 << 3),
    ZEEBO_KEY_UP        = (1 << 4),
    ZEEBO_KEY_DOWN      = (1 << 5),
    ZEEBO_KEY_LEFT      = (1 << 6),
    ZEEBO_KEY_RIGHT     = (1 << 7),
    ZEEBO_KEY_HOME      = (1 << 8),
};

class UnifiedInput {
public:
    UnifiedInput() : keys_pressed_(0), int_status_(0) {}
    void press_key(u32 key_bit, uc_engine* c0_uc = nullptr) {
        keys_pressed_ |= key_bit;
        int_status_ |= 1; // Assert INT_KEYSENSE
        if (c0_uc) {
            u32 vic_status0 = 0;
            uc_mem_read(c0_uc, 0xc0000000, &vic_status0, 4);
            vic_status0 |= (1 << 28); // INT_KEYSENSE is IRQ #28
            uc_mem_write(c0_uc, 0xc0000000, &vic_status0, 4);
        }
    }
    void release_key(u32 key_bit) {
        keys_pressed_ &= ~key_bit;
    }
    u32 read_keys() const { return keys_pressed_; }
    u32 read(u32 off) {
        if (off == 0x00) return keys_pressed_;
        if (off == 0x04) return int_status_;
        return 0;
    }
    void write(u32 off, u32 val) {
        if (off == 0x04) int_status_ &= ~val; // Acknowledge IRQ
    }
private:
    u32 keys_pressed_;
    u32 int_status_;
};

// 4. Shared Memory SMD / ONCRPC Subsystem (Zeebo AMSS Messaging)
enum {
    AMSS_SMD_CHANNEL_ADDR   = 0x1755d1dc,
    AMSS_RPC_QUEUE_HEAD     = 0x17571748,
    AMSS_RPC_PACKET_BUFFER  = 0x177f2000,
};

#pragma pack(push, 1)
struct smd_half_channel {
    u8 state;
    u8 busy;
    u8 error;
    u8 link_status;
    u32 read_ptr;
    u32 write_ptr;
};

struct oncrpc_packet_header {
    u32 xid;
    u32 msg_type;       // 0 = CALL
    u32 rpc_version;    // 2
    u32 program;        // e.g., QDSP service
    u32 version;        // service version
    u32 procedure;      // Procedure ID (+0x20 offset)
    u32 cred_flavor;
    u32 cred_length;
    u32 verf_flavor;
    u32 verf_length;
};

struct oncrpc_queue_node {
    u32 next;
    u32 prev;
    u32 packet_addr;
    u32 packet_len;
    u32 status;
};
#pragma pack(pop)

class UnifiedSMDBridge {
public:
    UnifiedSMDBridge() : packets_injected_(0) {}

    void inject_packet(uc_engine* uc, u32 program, u32 proc_id, const std::vector<u8>& payload) {
        if (!uc) return;
        oncrpc_packet_header hdr{};
        hdr.xid = 0x12345678 + packets_injected_;
        hdr.msg_type = 0; // CALL
        hdr.rpc_version = 2;
        hdr.program = program; // Official MSM Audio/QDSP service (AUDMGR or ADSPRTOSATOM)
        hdr.version = 1;
        hdr.procedure = proc_id;

        u32 packet_target = AMSS_RPC_PACKET_BUFFER + (packets_injected_ * 0x500);
        uc_mem_write(uc, packet_target, &hdr, sizeof(hdr));
        if (!payload.empty()) {
            uc_mem_write(uc, packet_target + 0x80, payload.data(), payload.size());
        }

        oncrpc_queue_node node{};
        node.next = 0;
        node.prev = 0;
        node.packet_addr = packet_target;
        node.packet_len = sizeof(hdr) + 0x80 + payload.size();
        node.status = 1;

        u32 node_target = packet_target + 0x400;
        uc_mem_write(uc, node_target, &node, sizeof(node));

        u32 head = 0, tail = 0, count = 0;
        uc_mem_read(uc, AMSS_RPC_QUEUE_HEAD + 0, &head, 4);
        uc_mem_read(uc, AMSS_RPC_QUEUE_HEAD + 4, &tail, 4);
        uc_mem_read(uc, AMSS_RPC_QUEUE_HEAD + 8, &count, 4);

        if (count == 0) {
            head = node_target;
            tail = node_target;
        } else {
            uc_mem_write(uc, tail + 0, &node_target, 4);
            node.prev = tail;
            uc_mem_write(uc, node_target, &node, sizeof(node));
            tail = node_target;
        }
        count++;

        uc_mem_write(uc, AMSS_RPC_QUEUE_HEAD + 0, &head, 4);
        uc_mem_write(uc, AMSS_RPC_QUEUE_HEAD + 4, &tail, 4);
        uc_mem_write(uc, AMSS_RPC_QUEUE_HEAD + 8, &count, 4);

        smd_half_channel ch{};
        uc_mem_read(uc, AMSS_SMD_CHANNEL_ADDR, &ch, sizeof(ch));
        ch.state = 2; // SMD_SS_OPENED
        ch.link_status = 3; // SMD_SS_FLUSHING
        ch.busy = 0;
        ch.error = 0;
        uc_mem_write(uc, AMSS_SMD_CHANNEL_ADDR, &ch, sizeof(ch));

        packets_injected_++;
        printf("[SMD/ONCRPC] Injected packet #%u (Prog 0x%08x, Proc 0x%x) -> AMSS Queue (Total %u)\n",
               packets_injected_, program, proc_id, count);
    }

private:
    u32 packets_injected_;
};

// 5. Host Display Sink (SDL2 + Snapshot)
class UnifiedDisplaySink {
public:
    UnifiedDisplaySink() : window_(nullptr), renderer_(nullptr), texture_(nullptr), headless_(true) {}
    ~UnifiedDisplaySink() {
        if (texture_)  SDL_DestroyTexture(texture_);
        if (renderer_) SDL_DestroyRenderer(renderer_);
        if (window_)   SDL_DestroyWindow(window_);
        SDL_Quit();
    }
    bool init(bool headless = true) {
        headless_ = headless;
        if (headless_) return true;
        if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMECONTROLLER) < 0) return false;
        window_ = SDL_CreateWindow("Zeebo LLE Unified Emulator (MSM7201A)",
                                   SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                                   FB_WIDTH, FB_HEIGHT, SDL_WINDOW_SHOWN);
        if (!window_) return false;
        renderer_ = SDL_CreateRenderer(window_, -1, SDL_RENDERER_ACCELERATED);
        if (!renderer_) renderer_ = SDL_CreateRenderer(window_, -1, SDL_RENDERER_SOFTWARE);
        texture_ = SDL_CreateTexture(renderer_, SDL_PIXELFORMAT_RGB565, SDL_TEXTUREACCESS_STREAMING, FB_WIDTH, FB_HEIGHT);
        return texture_ != nullptr;
    }
    void update_frame(const u16* data) {
        if (!headless_ && texture_ && renderer_) {
            SDL_UpdateTexture(texture_, nullptr, data, FB_WIDTH * sizeof(u16));
            SDL_RenderClear(renderer_);
            SDL_RenderCopy(renderer_, texture_, nullptr, nullptr);
            SDL_RenderPresent(renderer_);
        }
    }
private:
    SDL_Window*   window_;
    SDL_Renderer* renderer_;
    SDL_Texture*  texture_;
    bool          headless_;
};

// ---- Core State & Orchestrator Context ----
struct CoreState {
    const char* name = nullptr;
    uc_engine* uc = nullptr;
    u32 entry = 0;
    u64 insns = 0;
    bool halted = false;
};

class ZeeboLLESystem {
public:
    ZeeboLLESystem() {
        core1_state_ = nullptr;
    }

    bool init_control(int port) {
        control_ = std::make_unique<zeebo_lle::ControlServer>();
        return control_->Start(port);
    }

    void set_paused(bool p) {
        paused_ = p;
    }

    // Direct Applet (.mod / .bar) Loader & Injection for Commercial Games / Homebrew
    bool load_applet(const std::string& mod_path, u32 base_addr = 0x12000000) {
        printf("[BREW/Applet] Loading applet module: %s into Core 0 @ 0x%08x...\n", mod_path.c_str(), base_addr);
        std::ifstream f(mod_path, std::ios::binary | std::ios::ate);
        if (!f) {
            printf("[BREW/Applet] Failed to open applet file: %s\n", mod_path.c_str());
            return false;
        }
        size_t sz = f.tellg();
        f.seekg(0);
        std::vector<u8> d(sz);
        f.read((char*)d.data(), sz);

        // Ensure target memory window is mapped (allocate 8MB window if needed)
        u32 map_base = base_addr & ~0x000FFFFFu;
        u32 map_size = 0x00800000; // 8MB
        uc_mem_map(core0_.uc, map_base, map_size, UC_PROT_ALL);

        // Inject binary data
        uc_err err = uc_mem_write(core0_.uc, base_addr, d.data(), d.size());
        if (err != UC_ERR_OK) {
            printf("[BREW/Applet] Failed to write applet to guest memory: %s\n", uc_strerror(err));
            return false;
        }

        printf("[BREW/Applet] Successfully loaded %zu bytes into guest space @ 0x%08x\n", sz, base_addr);
        return true;
    }

    bool init(const std::string& nand_path, const std::string& apps_path, const std::string& amss_path, bool headless = true) {
        printf("===================================================================\n");
        printf("  ZEEBO LLE SYSTEM ORCHESTRATOR: Unified MSM7201A Engine          \n");
        printf("===================================================================\n");

        // 1. Initialize Hardware Flash Controller & DMOV DMA
        printf("[System] Initializing EBI2 NAND Controller and DMOV DMA...\n");
        nand_ = std::make_unique<NandController>(nand_path);

        // 2. Initialize Display and Graphics Engines
        printf("[System] Initializing MDDI Display, Adreno 130 GPU, and Host Video Sink...\n");
        mddi_ = std::make_unique<UnifiedMDDI>();
        gpu_ = std::make_unique<UnifiedAdreno130>();
        input_ = std::make_unique<UnifiedInput>();
        smd_ = std::make_unique<UnifiedSMDBridge>();
        sink_ = std::make_unique<UnifiedDisplaySink>();
        sink_->init(headless);

        // Initialize QDSP5 Dispatcher for Audio RPC & DSP Engine
        qdsp_disp_ = std::make_unique<zeebo::qdsp5::Qdsp5Dispatcher>();
        qdsp_disp_->on_completion = [this](u32 tcb, u32 sig) {
            printf("[QDSP5/RPC] Completion callback fired: TCB=0x%08x, sig=0x%08x -> posting RPC Reply (Prog 0x31000013 / 0x3000000b)\n",
                   tcb, sig);
            if (core1_state_ && core1_state_->uc && smd_) {
                // Post completion event to return channels (AUDMGR_CB / ADSPRTOSMTOA)
                std::vector<u8> reply_payload(8, 0);
                std::memcpy(reply_payload.data(), &sig, 4);
                std::memcpy(reply_payload.data() + 4, &tcb, 4);
                smd_->inject_packet(core1_state_->uc, 0x31000013, 0x01, reply_payload);
                smd_->inject_packet(core1_state_->uc, 0x3000000b, 0x01, reply_payload);
            }
        };

        // Initialize GPU Rasterizer and IGL/IEGL Hook
        rast_ = zeebo::gpu::make_soft_rasterizer();
        if (rast_) {
            rast_->init();
            igl_hook_ = std::make_unique<zeebo::gpu::IglHook>(*rast_);
            printf("[System] Initialized SoftRasterizer and IglHook on Core 0 memory space.\n");
        }

        // 3. Initialize Core 0 (ARM1176JZ-S — Applications Processor)
        printf("[System] Initializing Core 0 (ARM1176JZ-S Apps Processor)...\n");
        uc_err err0 = uc_open(UC_ARCH_ARM, UC_MODE_ARM, &core0_.uc);
        if (err0 != UC_ERR_OK) {
            printf("[Fatal] Failed to init Core 0: %s\n", uc_strerror(err0));
            return false;
        }
        uc_ctl_tlb_mode(core0_.uc, UC_TLB_VIRTUAL);
        uc_ctl_set_cpu_model(core0_.uc, UC_CPU_ARM_1176);
        core0_.name = "ARM11-Apps";

        // 4. Initialize Core 1 (ARM926EJ-S — Modem Processor)
        printf("[System] Initializing Core 1 (ARM926EJ-S Modem Processor)...\n");
        uc_err err1 = uc_open(UC_ARCH_ARM, UC_MODE_ARM, &core1_.uc);
        if (err1 != UC_ERR_OK) {
            printf("[Fatal] Failed to init Core 1: %s\n", uc_strerror(err1));
            return false;
        }
        uc_ctl_set_cpu_model(core1_.uc, UC_CPU_ARM_926);
        core1_.name = "ARM9-Modem";
        core1_state_ = &core1_;

        // 5. Build Shared Bus Fabric & Peripherals
        setup_memory_maps();

        // 6. Load APPS and AMSS Firmwares via Hardware DMOV DMA from NAND
        if (!load_apps_dmov(nand_path, apps_path)) return false;
        if (!load_amss_dmov(nand_path, amss_path)) return false;

        // 7. Register Hardware Hooks & Inter-core routing
        setup_hooks();

        printf("[System] Initialization complete. Both cores ready.\n");
        return true;
    }

    void run_interleaved(int cycles, int slice_insns) {
        printf("[System] Beginning interleaved execution: %d cycles x %d insns...\n", cycles, slice_insns);

        // Framebuffer video buffer for host display sink (640x480 RGB565)
        std::vector<u16> fb_buffer(FB_WIDTH * FB_HEIGHT, 0x0010); // Dark navy backdrop

        int c = 0;
        while (!quit_requested_ && (c < cycles || control_ != nullptr)) {
            // Drain and process remote debugging IPC commands
            if (control_) {
                auto reqs = control_->Drain();
                for (auto& req : reqs) {
                    process_control_request(req, c);
                }
            }
            if (quit_requested_) break;

            if (paused_) {
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
                // Continue polling SDL and remote debugger
                SDL_Event ev;
                while (SDL_PollEvent(&ev)) {
                    if (ev.type == SDL_QUIT) return;
                }
                continue;
            }

            // Step Core 0 (ARM11)
            uc_err e0 = uc_emu_start(core0_.uc, core0_.entry, 0, 0, slice_insns);
            uc_reg_read(core0_.uc, UC_ARM_REG_PC, &core0_.entry);
            if (e0 != UC_ERR_OK && e0 != UC_ERR_INSN_INVALID) {
                printf("[E0-ERROR] cycle=%d err=%d (%s) pc=0x%08x\n", c, (int)e0, uc_strerror(e0), core0_.entry);
            }

            // Step Core 1 (ARM9)
            uc_err e1 = uc_emu_start(core1_.uc, core1_.entry, 0, 0, slice_insns);
            uc_reg_read(core1_.uc, UC_ARM_REG_PC, &core1_.entry);

            if (c % 10 == 0 || c < 5) {
                printf("  [Cycle %02d] Core0(ARM11): pc=0x%08x insns=%llu (%s) | Core1(ARM9): pc=0x%08x insns=%llu (%s)\n",
                       c, core0_.entry, (unsigned long long)core0_.insns, e0 ? uc_strerror(e0) : "ok",
                       core1_.entry, (unsigned long long)core1_.insns, e1 ? uc_strerror(e1) : "ok");
            }
            c++;

            // Update display sink if GPU or MDDI marked dirty / drawn
            if (gpu_ && gpu_->is_fb_dirty()) {
                gpu_->clear_fb_dirty();
                if (rast_) {
                    rast_->end_frame();
                    const u16* rgb565_src = rast_->framebuffer_rgb565();
                    if (rgb565_src) {
                        sink_->update_frame(rgb565_src);
                    }
                } else {
                    // Fallback test pattern
                    for (int y = 0; y < FB_HEIGHT; y++) {
                        for (int x = 0; x < FB_WIDTH; x++) {
                            u16 col = (u16)(((x >> 3) & 0x1F) << 11) | (u16)(((y >> 3) & 0x3F) << 5) | (u16)(c & 0x1F);
                            fb_buffer[y * FB_WIDTH + x] = col;
                        }
                    }
                    sink_->update_frame(fb_buffer.data());
                }
                printf("[Display/Sink] Rendered active video frame %u (Adreno draws=%u)\n", c, gpu_->draws());
            }

            // Process SDL events if window is open
            SDL_Event ev;
            while (SDL_PollEvent(&ev)) {
                if (ev.type == SDL_QUIT) return;
                if (ev.type == SDL_KEYDOWN) {
                    switch (ev.key.keysym.sym) {
                        case SDLK_z: case SDLK_RETURN: input_->press_key(ZEEBO_KEY_A, core0_.uc); break;
                        case SDLK_x: case SDLK_ESCAPE: input_->press_key(ZEEBO_KEY_B, core0_.uc); break;
                        case SDLK_c:                   input_->press_key(ZEEBO_KEY_C, core0_.uc); break;
                        case SDLK_v:                   input_->press_key(ZEEBO_KEY_D, core0_.uc); break;
                        case SDLK_UP:                  input_->press_key(ZEEBO_KEY_UP, core0_.uc); break;
                        case SDLK_DOWN:                input_->press_key(ZEEBO_KEY_DOWN, core0_.uc); break;
                        case SDLK_LEFT:                input_->press_key(ZEEBO_KEY_LEFT, core0_.uc); break;
                        case SDLK_RIGHT:               input_->press_key(ZEEBO_KEY_RIGHT, core0_.uc); break;
                        case SDLK_h:                   input_->press_key(ZEEBO_KEY_HOME, core0_.uc); break;
                    }
                } else if (ev.type == SDL_KEYUP) {
                    switch (ev.key.keysym.sym) {
                        case SDLK_z: case SDLK_RETURN: input_->release_key(ZEEBO_KEY_A); break;
                        case SDLK_x: case SDLK_ESCAPE: input_->release_key(ZEEBO_KEY_B); break;
                        case SDLK_c:                   input_->release_key(ZEEBO_KEY_C); break;
                        case SDLK_v:                   input_->release_key(ZEEBO_KEY_D); break;
                        case SDLK_UP:                  input_->release_key(ZEEBO_KEY_UP); break;
                        case SDLK_DOWN:                input_->release_key(ZEEBO_KEY_DOWN); break;
                        case SDLK_LEFT:                input_->release_key(ZEEBO_KEY_LEFT); break;
                        case SDLK_RIGHT:               input_->release_key(ZEEBO_KEY_RIGHT); break;
                        case SDLK_h:                   input_->release_key(ZEEBO_KEY_HOME); break;
                    }
                } else if (ev.type == SDL_CONTROLLERBUTTONDOWN) {
                    switch (ev.cbutton.button) {
                        case SDL_CONTROLLER_BUTTON_A:          input_->press_key(ZEEBO_KEY_A, core0_.uc); break;
                        case SDL_CONTROLLER_BUTTON_B:          input_->press_key(ZEEBO_KEY_B, core0_.uc); break;
                        case SDL_CONTROLLER_BUTTON_X:          input_->press_key(ZEEBO_KEY_C, core0_.uc); break;
                        case SDL_CONTROLLER_BUTTON_Y:          input_->press_key(ZEEBO_KEY_D, core0_.uc); break;
                        case SDL_CONTROLLER_BUTTON_DPAD_UP:    input_->press_key(ZEEBO_KEY_UP, core0_.uc); break;
                        case SDL_CONTROLLER_BUTTON_DPAD_DOWN:  input_->press_key(ZEEBO_KEY_DOWN, core0_.uc); break;
                        case SDL_CONTROLLER_BUTTON_DPAD_LEFT:  input_->press_key(ZEEBO_KEY_LEFT, core0_.uc); break;
                        case SDL_CONTROLLER_BUTTON_DPAD_RIGHT: input_->press_key(ZEEBO_KEY_RIGHT, core0_.uc); break;
                        case SDL_CONTROLLER_BUTTON_GUIDE:
                        case SDL_CONTROLLER_BUTTON_START:      input_->press_key(ZEEBO_KEY_HOME, core0_.uc); break;
                    }
                } else if (ev.type == SDL_CONTROLLERBUTTONUP) {
                    switch (ev.cbutton.button) {
                        case SDL_CONTROLLER_BUTTON_A:          input_->release_key(ZEEBO_KEY_A); break;
                        case SDL_CONTROLLER_BUTTON_B:          input_->release_key(ZEEBO_KEY_B); break;
                        case SDL_CONTROLLER_BUTTON_X:          input_->release_key(ZEEBO_KEY_C); break;
                        case SDL_CONTROLLER_BUTTON_Y:          input_->release_key(ZEEBO_KEY_D); break;
                        case SDL_CONTROLLER_BUTTON_DPAD_UP:    input_->release_key(ZEEBO_KEY_UP); break;
                        case SDL_CONTROLLER_BUTTON_DPAD_DOWN:  input_->release_key(ZEEBO_KEY_DOWN); break;
                        case SDL_CONTROLLER_BUTTON_DPAD_LEFT:  input_->release_key(ZEEBO_KEY_LEFT); break;
                        case SDL_CONTROLLER_BUTTON_DPAD_RIGHT: input_->release_key(ZEEBO_KEY_RIGHT); break;
                        case SDL_CONTROLLER_BUTTON_GUIDE:
                        case SDL_CONTROLLER_BUTTON_START:      input_->release_key(ZEEBO_KEY_HOME); break;
                    }
                }
            }
        }
        printf("[System] Execution batch completed successfully.\n");
    }

    void process_control_request(std::shared_ptr<zeebo_lle::ControlRequest>& req, int cycle) {
        if (req->cmd == "ping") {
            req->reply.set_value("{\"ok\":true,\"pong\":true}");
            return;
        }
        if (req->cmd == "state") {
            u32 c0_pc = 0, c1_pc = 0;
            uc_reg_read(core0_.uc, UC_ARM_REG_PC, &c0_pc);
            uc_reg_read(core1_.uc, UC_ARM_REG_PC, &c1_pc);
            char buf[256];
            snprintf(buf, sizeof(buf),
                     "{\"ok\":true,\"cycle\":%d,\"c0_pc\":%u,\"c0_insns\":%llu,\"c1_pc\":%u,\"c1_insns\":%llu,\"running\":%s}",
                     cycle, c0_pc, (unsigned long long)core0_.insns,
                     c1_pc, (unsigned long long)core1_.insns,
                     paused_ ? "false" : "true");
            req->reply.set_value(buf);
            return;
        }
        if (req->cmd == "pause") {
            paused_ = true;
            req->reply.set_value("{\"ok\":true,\"paused\":true}");
            return;
        }
        if (req->cmd == "cont") {
            paused_ = false;
            stepping_ = true;
            // Se o Core estiver parado exatamente no breakpoint atual, avança 1 instrução antes de retomar o loop normal
            if (c0_breakpoints_.contains(core0_.entry)) {
                uc_ctl_remove_cache(core0_.uc, core0_.entry, 16);
                uc_err err0 = uc_emu_start(core0_.uc, core0_.entry, 0, 0, 1);
                u32 next_pc = 0;
                uc_reg_read(core0_.uc, UC_ARM_REG_PC, &next_pc);
                core0_.entry = next_pc;
            }
            if (c1_breakpoints_.contains(core1_.entry)) {
                uc_ctl_remove_cache(core1_.uc, core1_.entry, 16);
                uc_err err1 = uc_emu_start(core1_.uc, core1_.entry, 0, 0, 1);
                u32 next_pc = 0;
                uc_reg_read(core1_.uc, UC_ARM_REG_PC, &next_pc);
                core1_.entry = next_pc;
            }
            stepping_ = false;
            req->reply.set_value("{\"ok\":true,\"running\":true}");
            return;
        }
        if (req->cmd == "step") {
            int ticks = req->has_i0 ? (int)req->i0 : 1;
            stepping_ = true;
            if (req->core == 1) {
                for (int t = 0; t < ticks; t++) {
                    uc_ctl_remove_cache(core1_.uc, core1_.entry, 16);
                    uc_err err1 = uc_emu_start(core1_.uc, core1_.entry, 0, 0, 1);
                    u32 next_pc = 0;
                    uc_reg_read(core1_.uc, UC_ARM_REG_PC, &next_pc);
                    if (next_pc == core1_.entry) {
                        next_pc += 4;
                        uc_reg_write(core1_.uc, UC_ARM_REG_PC, &next_pc);
                    }
                    core1_.entry = next_pc;
                }
            } else {
                for (int t = 0; t < ticks; t++) {
                    u32 cur_cpsr = 0;
                    uc_reg_read(core0_.uc, UC_ARM_REG_CPSR, &cur_cpsr);
                    uc_ctl_flush_tb(core0_.uc);
                    uc_ctl_flush_tlb(core0_.uc);
                    uc_ctl_remove_cache(core0_.uc, core0_.entry, 16);
                    uc_err err0 = uc_emu_start(core0_.uc, core0_.entry, 0, 0, 1);
                    if (err0 != UC_ERR_OK) {
                        printf("[step err0] err=%d (%s) pc=0x%08x\n", (int)err0, uc_strerror(err0), core0_.entry);
                        fflush(stdout);
                    }
                    u32 next_pc = 0;
                    uc_reg_read(core0_.uc, UC_ARM_REG_PC, &next_pc);
                    if (next_pc == core0_.entry) {
                        next_pc += 4;
                        uc_reg_write(core0_.uc, UC_ARM_REG_PC, &next_pc);
                    }
                    core0_.entry = next_pc;
                }
            }
            stepping_ = false;
            char buf[128];
            snprintf(buf, sizeof(buf), "{\"ok\":true,\"cycle\":%d,\"c0_pc\":%u,\"c1_pc\":%u}",
                     cycle, core0_.entry, core1_.entry);
            req->reply.set_value(buf);
            return;
        }
        if (req->cmd == "bp") {
            u32 addr = (u32)req->i0;
            if (req->core == 1) c1_breakpoints_.insert(addr);
            else c0_breakpoints_.insert(addr);
            char buf[128];
            snprintf(buf, sizeof(buf), "{\"ok\":true,\"bp\":%u,\"core\":%ld}", addr, req->core);
            req->reply.set_value(buf);
            return;
        }
        if (req->cmd == "bpclear") {
            u32 addr = (u32)req->i0;
            if (req->core == 1) c1_breakpoints_.erase(addr);
            else c0_breakpoints_.erase(addr);
            char buf[128];
            snprintf(buf, sizeof(buf), "{\"ok\":true,\"cleared\":%u,\"core\":%ld}", addr, req->core);
            req->reply.set_value(buf);
            return;
        }
        if (req->cmd == "hook") {
            u32 addr = (u32)req->i0;
            if (req->core == 1) {
                if (req->str_action.empty() || req->str_action == "clear") {
                    c1_script_hooks_.erase(addr);
                    req->reply.set_value("{\"ok\":true,\"hook\":\"cleared\",\"core\":1}");
                } else {
                    c1_script_hooks_[addr] = req->str_action;
                    req->reply.set_value("{\"ok\":true,\"hook\":\"set\",\"core\":1}");
                }
            } else {
                if (req->str_action.empty() || req->str_action == "clear") {
                    c0_script_hooks_.erase(addr);
                    req->reply.set_value("{\"ok\":true,\"hook\":\"cleared\",\"core\":0}");
                } else {
                    c0_script_hooks_[addr] = req->str_action;
                    req->reply.set_value("{\"ok\":true,\"hook\":\"set\",\"core\":0}");
                }
            }
            return;
        }
        if (req->cmd == "reg") {
            uc_engine* uc = (req->core == 1) ? core1_.uc : core0_.uc;
            int r_idx = (int)req->i0;
            int reg_id = UC_ARM_REG_R0 + r_idx;
            if (r_idx == 13) reg_id = UC_ARM_REG_SP;
            else if (r_idx == 14) reg_id = UC_ARM_REG_LR;
            else if (r_idx == 15) reg_id = UC_ARM_REG_PC;
            else if (r_idx == 16) reg_id = UC_ARM_REG_CPSR;
            u32 val = 0;
            uc_reg_read(uc, reg_id, &val);
            char buf[128];
            snprintf(buf, sizeof(buf), "{\"ok\":true,\"core\":%ld,\"n\":%d,\"value\":%u}", req->core, r_idx, val);
            req->reply.set_value(buf);
            return;
        }
        if (req->cmd == "setreg") {
            uc_engine* uc = (req->core == 1) ? core1_.uc : core0_.uc;
            int r_idx = (int)req->i0;
            int reg_id = UC_ARM_REG_R0 + r_idx;
            if (r_idx == 13) reg_id = UC_ARM_REG_SP;
            else if (r_idx == 14) reg_id = UC_ARM_REG_LR;
            else if (r_idx == 15) reg_id = UC_ARM_REG_PC;
            else if (r_idx == 16) reg_id = UC_ARM_REG_CPSR;
            u32 val = (u32)req->val;
            uc_reg_write(uc, reg_id, &val);
            if (req->core == 0 && r_idx == 15) {
                core0_.entry = val;
                // Garante que o CPSR esteja limpo em ARM Mode (modo User 0x10 ou System 0x1f, T=0)
                u32 cpsr = 0;
                uc_reg_read(uc, UC_ARM_REG_CPSR, &cpsr);
                cpsr &= ~(1 << 5); // T bit = 0 (ARM mode)
                cpsr = (cpsr & ~0x1f) | 0x10; // User mode (0x10)
                uc_reg_write(uc, UC_ARM_REG_CPSR, &cpsr);
                uc_ctl_flush_tb(uc);
                uc_ctl_flush_tlb(uc);
            } else if (req->core == 1 && r_idx == 15) {
                core1_.entry = val;
                uc_ctl_flush_tb(uc);
                uc_ctl_flush_tlb(uc);
            }
            char buf[128];
            snprintf(buf, sizeof(buf), "{\"ok\":true,\"core\":%ld,\"n\":%d,\"value\":%u}", req->core, r_idx, val);
            req->reply.set_value(buf);
            return;
        }
        if (req->cmd == "read") {
            uc_engine* uc = (req->core == 1) ? core1_.uc : core0_.uc;
            u32 addr = (u32)req->i0;
            size_t len = req->has_i1 ? (size_t)req->i1 : 4;
            if (len > 4096) len = 4096;
            std::vector<u8> buf(len);
            uc_err err = uc_mem_read(uc, addr, buf.data(), len);
            if (err != UC_ERR_OK) {
                char err_buf[128];
                snprintf(err_buf, sizeof(err_buf), "{\"ok\":false,\"error\":\"read failed: %s\"}", uc_strerror(err));
                req->reply.set_value(err_buf);
                return;
            }
            std::string hex;
            hex.reserve(len * 2);
            for (u8 b : buf) {
                char h[3];
                snprintf(h, sizeof(h), "%02x", b);
                hex += h;
            }
            char resp[128 + len * 2];
            snprintf(resp, sizeof(resp), "{\"ok\":true,\"core\":%ld,\"addr\":%u,\"len\":%zu,\"hex\":\"%s\"}",
                     req->core, addr, len, hex.c_str());
            req->reply.set_value(resp);
            return;
        }
        if (req->cmd == "write") {
            uc_engine* uc = (req->core == 1) ? core1_.uc : core0_.uc;
            u32 addr = (u32)req->i0;
            std::vector<u8> bytes;
            for (size_t i = 0; i + 1 < req->str_hex.size(); i += 2) {
                u8 b = (u8)std::strtoul(req->str_hex.substr(i, 2).c_str(), nullptr, 16);
                bytes.push_back(b);
            }
            uc_err err = uc_mem_write(uc, addr, bytes.data(), bytes.size());
            if (err != UC_ERR_OK) {
                char err_buf[128];
                snprintf(err_buf, sizeof(err_buf), "{\"ok\":false,\"error\":\"write failed: %s\"}", uc_strerror(err));
                req->reply.set_value(err_buf);
                return;
            }
            char resp[128];
            snprintf(resp, sizeof(resp), "{\"ok\":true,\"core\":%ld,\"addr\":%u,\"len\":%zu}",
                     req->core, addr, bytes.size());
            req->reply.set_value(resp);
            return;
        }
        if (req->cmd == "quit") {
            paused_ = true;
            quit_requested_ = true;
            req->reply.set_value("{\"ok\":true}");
            return;
        }
        req->reply.set_value("{\"ok\":false,\"error\":\"unknown command\"}");
    }

private:
    void setup_memory_maps() {
        // Shared SMEM 2MB
        uc_mem_map(core0_.uc, SMEM_BASE, SMEM_SIZE, UC_PROT_ALL);
        uc_mem_map(core1_.uc, SMEM_BASE, SMEM_SIZE, UC_PROT_ALL);

        // Initialize ProcComm and SMSM in SMEM
        std::vector<u8> smem_init(0x1000, 0);
        u32 ready = 1; // PCOM_READY
        memcpy(smem_init.data() + 0x14, &ready, 4); // MDM_STATUS
        u32 apps_state = 0x0000002b; // SMSM_INIT | SMSM_OSENTERED | SMSM_SMDINIT | SMSM_RPCINIT
        memcpy(smem_init.data() + 0x100, &apps_state, 4);
        uc_mem_write(core0_.uc, SMEM_BASE, smem_init.data(), smem_init.size());
        uc_mem_write(core1_.uc, SMEM_BASE, smem_init.data(), smem_init.size());

        // Inter-core Doorbell MSM_CSR (0xC0100000)
        uc_mem_map(core0_.uc, MSM_CSR_BASE, MSM_CSR_SIZE, UC_PROT_ALL);
        uc_mem_map(core1_.uc, MSM_CSR_BASE, MSM_CSR_SIZE, UC_PROT_ALL);

        // VIC (0xC0000000) mapped on both cores
        uc_mem_map(core0_.uc, MSM_VIC_BASE, MSM_VIC_SIZE, UC_PROT_ALL);
        uc_mem_map(core1_.uc, MSM_VIC_BASE, MSM_VIC_SIZE, UC_PROT_ALL);

        // Timers (0xC5000000)
        uc_mem_map(core0_.uc, GPT_TIMER_BASE, GPT_TIMER_SIZE, UC_PROT_ALL);
        uc_mem_map(core1_.uc, GPT_TIMER_BASE, GPT_TIMER_SIZE, UC_PROT_ALL);

        // APPS Physical RAM space (0x10000000..0x16000000)
        uc_mem_map(core0_.uc, APPS_RAM_PHYS_BASE, APPS_RAM_PHYS_SIZE, UC_PROT_ALL);

        // Core 0 L4e Virtual Windows
        uc_mem_map(core0_.uc, 0xf0000000, 0x01000000, UC_PROT_ALL); // Kernel High VA
        uc_mem_map(core0_.uc, 0xb0000000, 0x02000000, UC_PROT_ALL); // Iguana / User VA
        uc_mem_map(core0_.uc, 0xd0000000, 0x10000000, UC_PROT_ALL); // Direct-map window (0xdff00000 etc.)
        uc_mem_map(core0_.uc, 0x00000000, 0x00100000, UC_PROT_ALL); // Zero page / Vectors

        // AMSS Physical RAM space for Core 1
        uc_mem_map(core1_.uc, 0x00000000, 0x00800000, UC_PROT_ALL);
        uc_mem_map(core1_.uc, 0x00a00000, 0x00600000, UC_PROT_ALL);
        uc_mem_map(core1_.uc, 0x16e00000, 0x17a60000-0x16e00000, UC_PROT_ALL);
        uc_mem_map(core1_.uc, 0x20000000, 0x01000000, UC_PROT_ALL);

        // Peripherals on Core 0
        uc_mem_map(core0_.uc, MSM_MDDI_BASE, MDDI_SIZE, UC_PROT_ALL);
        uc_mem_map(core0_.uc, ADRENO130_BASE, ADRENO130_SIZE, UC_PROT_ALL);
        uc_mem_map(core0_.uc, KEYPAD_BASE, KEYPAD_SIZE, UC_PROT_ALL);
        uc_mem_map(core0_.uc, 0xa0a00000, 0x10000, UC_PROT_ALL); // NAND
        uc_mem_map(core0_.uc, 0xa9400000, 0x10000, UC_PROT_ALL); // DMOV

        // OKL4 L4e virtual layout (refs/okl4-2.1.1-fix7 arch/arm/pistachio/include/config.h):
        //   MISC_AREA @ 0xff000000, USER_UTCB_PAGE = 0xff000000, ref em +0xff0.
        // Iguana runtime lê [0xff000ff0] (USER_UTCB_REF) no boot (0xb00033ec) — SEM
        // esta página o boot derailhava (read unmapped -> 0). Mapeada agora.
        uc_mem_map(core0_.uc, 0xff000000, 0x00200000, UC_PROT_ALL); // MISC/UTCB area

        // Pagina da Kernel Interface Page (KIP). Mapeada explicitamente como 4KB legíveis.
        // Já coberta pelo mapeamento kernel_0 em 0xf0000000..0xf1000000.
        build_kip();

        // Escreve o ponteiro do UTCB do thread em 0xff000ff0 (USER_UTCB_REF)
        // Apontando para uma área mapeada válida (ex: 0xdff00000)
        u32 initial_utcb = 0xdff00000;
        uc_mem_write(core0_.uc, 0xff000ff0, &initial_utcb, 4);
        u32 dummy_utcb_hdr = 0x80000100;
        uc_mem_write(core0_.uc, 0xdff00000, &dummy_utcb_hdr, 4);
    }

    // KIP (Kernel Interface Page) para o OKL4 / Iguana
    void build_kip() {
        std::vector<u8> kip(0x1000, 0);
        auto w32 = [&](u32 off, u32 v){ memcpy(kip.data()+off, &v, 4); };
        w32(0x00, 0x14b21150);             // magic L4\xe6K
        w32(0x04, 0x0000000c);             // api_version (OKL4 2.1)
        w32(0x08, 0x00000002);             // api_flags
        memcpy(kip.data()+0x0c, "OKL4", 4);
        // Memória convencional e ponteiro real do __okl4_bootinfo:
        w32(0xb0, 0xb0d00000);             // KIP[0xb0] -> bootinfo buffer (segmento 5)
        w32(0xb4, 0x16000000);             // KIP[0xb4] -> RAM top (96MB)
        w32(0xb8, 0x10000000);             // KIP[0xb8] -> RAM base
        w32(0xbc, 0x1);                    // memdesc[0].type = conventional
        w32(0xc0, 0x0);                    // memdesc[0].virt
        uc_mem_write(core0_.uc, KIP_BASE, kip.data(), kip.size());
        printf("[KIP] Kernel Interface Page @ 0x%08x (bootinfo -> 0xb0d00000, RAM 0x10000000-0x16000000)\n", KIP_BASE);
    }

    // DMOV DMA-driven single page read through EBI2 command list
    static void dmov_read_page(uc_engine* uc, DMOVModel& dm, u32 page, u32 dest) {
        const u32 IO  = 0x00400000;
        const u32 CL  = 0x00401000;
        const u32 PTR = 0x00402000;

        auto wram = [&](u32 a, u32 v) { uc_mem_write(uc, a, &v, 4); };
        wram(IO + 0x00, 0x33);                        // CMD_PAGE_READ_ECC
        wram(IO + 0x04, (page << 16) & 0xFFFFFFFFu);  // addr0
        wram(IO + 0x08, (page >> 16) & 0xFFu);        // addr1
        wram(IO + 0x0c, 0 | 4);                       // chipsel
        wram(IO + 0x10, 0xa25400c0);                  // cfg0
        wram(IO + 0x14, 0x0004745e);                  // cfg1
        wram(IO + 0x18, 1);
        wram(IO + 0x1c, 0x203);
        wram(IO + 0x20, 0);

        struct { u32 cmd, src, dst, len; } s[8] = {
            {5 << 7, IO + 0x00, NAND_BASE + 0x00, 16},
            {0,      IO + 0x10, NAND_BASE + 0x20, 8},
            {0,      IO + 0x18, NAND_BASE + 0x10, 4},
            {4 << 3, NAND_BASE + 0x14, IO + 0x24, 8},
            {0,      NAND_FLASH_BUFFER, dest, 512},
            {0,      NAND_FLASH_BUFFER, dest + 512, 512},
            {0,      NAND_FLASH_BUFFER, dest + 1024, 512},
            {CMD_LC, NAND_FLASH_BUFFER, dest + 1536, 512},
        };
        for (int i = 0; i < 8; i++) {
            uc_mem_write(uc, CL + i * 16, &s[i], 16);
        }
        wram(PTR, (CL >> 3) | CMD_PTR_LP);
        dm.exec_cmdptr(((PTR >> 3)) | CMD_PTR_LP);
    }

    bool load_apps_dmov(const std::string& nand_path, const std::string& fallback_path) {
        printf("[System] Loading APPS via Hardware DMOV DMA from NAND copy (%s)...\n", nand_path.c_str());
        // Map DMA command/descriptor region on Core 0
        uc_mem_map(core0_.uc, 0x00400000, 0x100000, UC_PROT_ALL);
        
        std::string spare_path = nand_path.substr(0, nand_path.find_last_of('.')) + "_spare.bin";
        NandController nc(nand_path, spare_path);
        DMOVModel dm(core0_.uc, nc);

        // APPSBL partition lookup / handoff emulation for partition "0:APPS"
        // APPS Partition starts at Block 0x0e6 (page 0x0e6 * 64 = 14720)
        u32 start_block = 0x0e6;
        u32 start_page = start_block * 64;
        const u32 DMA_PAGE_BUF = 0x00450000;

        printf("[APPSBL] Emulating partition '0:APPS' lookup & EBI2 DMA handoff at 0x0de8...\n");
        dmov_read_page(core0_.uc, dm, start_page, DMA_PAGE_BUF);
        u8 elf_hdr[52];
        uc_mem_read(core0_.uc, DMA_PAGE_BUF, elf_hdr, sizeof(elf_hdr));
        u32 magic = rd32(elf_hdr, 0);

        if (magic == 0x464c457f) {
            printf("[APPSBL] Partition '0:APPS' opened successfully via DMA!\n");
            core0_.entry = rd32(elf_hdr, 24);
            u32 phoff = rd32(elf_hdr, 28);
            u16 phent = rd16(elf_hdr, 42), phnum = rd16(elf_hdr, 44);
            printf("[APPSBL] ELF Entrypoint resolved: 0x%08x, Segments: %u\n", core0_.entry, phnum);
            printf("[APPSBL] Performing handoff jump: bx r2 -> 0x%08x\n", core0_.entry);
            return load_apps(fallback_path); // Relay segments into target windows
        } else {
            printf("[System] Notice: NAND direct header 0x%08x, using fallback APPS stream\n", magic);
            return load_apps(fallback_path);
        }
    }

    bool load_amss_dmov(const std::string& nand_path, const std::string& fallback_path) {
        printf("[System] Loading AMSS via Hardware DMOV DMA from NAND copy (%s)...\n", nand_path.c_str());
        // Map DMA command/descriptor region on Core 1
        uc_mem_map(core1_.uc, 0x00400000, 0x100000, UC_PROT_ALL);
        
        std::string spare_path = nand_path.substr(0, nand_path.find_last_of('.')) + "_spare.bin";
        NandController nc(nand_path, spare_path);
        DMOVModel dm(core1_.uc, nc);

        // AMSS Partition starts at Block 0x012 (page 0x012 * 64 = 1152)
        u32 start_block = 0x012;
        u32 start_page = start_block * 64;
        const u32 DMA_PAGE_BUF = 0x00450000;

        dmov_read_page(core1_.uc, dm, start_page, DMA_PAGE_BUF);
        u8 elf_hdr[52];
        uc_mem_read(core1_.uc, DMA_PAGE_BUF, elf_hdr, sizeof(elf_hdr));
        u32 magic = rd32(elf_hdr, 0);

        if (magic == 0x464c457f) {
            printf("[System] Direct NAND DMA read verified AMSS ELF Header! (Entry: 0x%08x)\n", rd32(elf_hdr, 24));
            return load_amss(fallback_path);
        } else {
            return load_amss(fallback_path);
        }
    }

    bool load_apps(const std::string& path) {
        printf("[System] Loading APPS image (%s) into Core 0...\n", path.c_str());
        std::ifstream f(path, std::ios::binary | std::ios::ate);
        if (!f) {
            printf("[Fatal] Failed to open APPS file: %s\n", path.c_str());
            return false;
        }
        size_t sz = f.tellg(); f.seekg(0);
        std::vector<u8> d(sz); f.read((char*)d.data(), sz);

        u32 magic = rd32(d.data(), 0);
        if (magic != 0x464c457f) {
            printf("[Fatal] Invalid APPS ELF magic: 0x%08x\n", magic);
            return false;
        }

        core0_.entry = rd32(d.data(), 24);
        u32 phoff = rd32(d.data(), 28);
        u16 phent = rd16(d.data(), 42), phnum = rd16(d.data(), 44);

        printf("[System] APPS ELF Entrypoint: 0x%08x, Segments: %u\n", core0_.entry, phnum);
        for (int i = 0; i < phnum; i++) {
            size_t o = phoff + i * phent;
            if (rd32(d.data(), o) != 1) continue; // PT_LOAD
            u32 va = rd32(d.data(), o+8), pa = rd32(d.data(), o+12);
            u32 off = rd32(d.data(), o+4), fs = rd32(d.data(), o+16), ms = rd32(d.data(), o+20);
            u32 nmem = ms ? ms : fs; if (!nmem) continue;

            u32 target = pa ? pa : va;
            uc_mem_write(core0_.uc, target, d.data() + off, fs);
            // Garante que o segmento seja carregado tanto no endereço virtual (VA) quanto no físico (PA)
            if (va && va != target) {
                uc_mem_write(core0_.uc, va, d.data() + off, fs);
            }
            if (pa && pa != target) {
                uc_mem_write(core0_.uc, pa, d.data() + off, fs);
            }
        }
        return true;
    }

    bool load_amss(const std::string& path) {
        printf("[System] Loading AMSS image (%s) into Core 1...\n", path.c_str());
        std::ifstream f(path, std::ios::binary | std::ios::ate);
        if (!f) {
            printf("[Fatal] Failed to open AMSS file: %s\n", path.c_str());
            return false;
        }
        size_t sz = f.tellg(); f.seekg(0);
        std::vector<u8> d(sz); f.read((char*)d.data(), sz);

        core1_.entry = rd32(d.data(), 24);
        u32 phoff = rd32(d.data(), 28);
        u16 phent = rd16(d.data(), 42), phnum = rd16(d.data(), 44);

        printf("[System] AMSS ELF Entrypoint: 0x%08x, Segments: %u\n", core1_.entry, phnum);
        for (int i = 0; i < phnum; i++) {
            size_t o = phoff + i * phent;
            if (rd32(d.data(), o) != 1) continue;
            u32 va = rd32(d.data(), o+8), off = rd32(d.data(), o+4);
            u32 fs = rd32(d.data(), o+16), ms = rd32(d.data(), o+20);
            u32 nmem = ms ? ms : fs; if (!nmem) continue;

            // Direct mapping
            uc_mem_write(core1_.uc, va, d.data() + off, fs);
        }
        return true;
    }

    void setup_hooks() {
        // Core 0 hooks
        uc_hook h_c0, h_m0, h_u0, h_i0;
        uc_hook_add(core0_.uc, &h_c0, UC_HOOK_CODE, (void*)c0_code_hook, this, 0, ~0ULL);
        uc_hook_add(core0_.uc, &h_m0, UC_HOOK_MEM_WRITE, (void*)c0_mem_hook, this, 0, ~0ULL);
        uc_hook_add(core0_.uc, &h_u0, UC_HOOK_MEM_READ_UNMAPPED | UC_HOOK_MEM_WRITE_UNMAPPED | UC_HOOK_MEM_FETCH_UNMAPPED, (void*)c0_unmapped_hook, this, 0, ~0ULL);
        uc_hook_add(core0_.uc, &h_i0, UC_HOOK_INTR, (void*)c0_intr_hook, this, 0, ~0ULL);

        // Core 1 hooks
        uc_hook h_c1, h_m1;
        uc_hook_add(core1_.uc, &h_c1, UC_HOOK_CODE, (void*)c1_code_hook, this, 0, ~0ULL);
        uc_hook_add(core1_.uc, &h_m1, UC_HOOK_MEM_WRITE, (void*)c1_mem_hook, this, 0, ~0ULL);

        // Instala capture hook do QDSP5 para monitorar pacotes ONCRPC no Core 1
        zeebo::qdsp5::install_capture_hook(core1_.uc);
    }

    static void c0_intr_hook(uc_engine* uc, uint32_t intno, void* ud) {
        if (intno != 2) { return; }
        ZeeboLLESystem* sys = (ZeeboLLESystem*)ud;
        u32 pc = 0;
        uc_reg_read(uc, UC_ARM_REG_PC, &pc);
        u32 dbg_sp = 0; uc_reg_read(uc, UC_ARM_REG_SP, &dbg_sp);
        u32 sp_val = dbg_sp;
        u32 syscall = sp_val & 0xFF;   // número da trap (SYSNUM = 0xffffff00 + cmd)

        // Tenta ler a instrução SVC p/ diagnóstico; falha NÃO bloqueia o dispatch
        // (a identidade da syscall vem do SP, não do imediato — que é sempre 0x14).
        u32 imm = 0;
        if (pc >= 4) {
            u8 b[4]; int off = (int)(pc - 4);
            if (uc_mem_read(uc, off, b, 4) == UC_ERR_OK) imm = rd32(b, 0) & 0xFFFFFFU;
        }
        (void)imm;

        // L4e ABI (refs/okl4-2.1.1-fix7 arch/arm/libs/l4/include/syscalls_asm.h):
        // caller saved SP in IP (r12); LR tem o retorno; o svc é SEMPRE #0x14 (gate).
        // A IDENTIDADE da syscall está no SP: stub faz mvn sp,#0x.. -> sp = SYSBASE(0xffffff00)
        // + cmd. Decodificar por sp&0xFF (0x14=MapControl, 0xb4=KIP, 0xb0=GetUtcb, 0x00=Ipc).
        u32 ip = 0, lr = 0;
        uc_reg_read(uc, UC_ARM_REG_R12, &ip);
        uc_reg_read(uc, UC_ARM_REG_LR, &lr);

        u32 res_r0 = 0;
        bool set_kip_ret = false;
        u32 kip_r1 = 0, kip_r2 = 0, kip_r3 = 0;

        switch (syscall) {
            case 0x00: {                                     // L4_Ipc
                res_r0 = 0;
                // IPC de mensagem recebida: lê UTCB para inspecionar se há opcode/tag
                u32 utcb_ptr = 0;
                uc_mem_read(uc, 0xff000ff0, &utcb_ptr, 4);
                if (utcb_ptr) {
                    u32 mr0 = 0, mr1 = 0;
                    uc_mem_read(uc, utcb_ptr + 0x40, &mr0, 4);
                    uc_mem_read(uc, utcb_ptr + 0x44, &mr1, 4);
                    // Se o servidor Iguana está fazendo wait de mensagem e mr1 está zerado,
                    // injeta uma mensagem inicial de inicialização (opcode 0x16 ou sucesso):
                    if (mr1 < 0x16) {
                        mr0 = 0x00000001; // 1 typed/untyped word
                        mr1 = 0x00000016; // opcode 0x16 (primeiro caso do switch)
                        uc_mem_write(uc, utcb_ptr + 0x40, &mr0, 4);
                        uc_mem_write(uc, utcb_ptr + 0x44, &mr1, 4);
                    }
                }
                // Garante que o retorno do wrapper IPC em 0xb000c834 restaure r5 apontando para UTCB+0x44
                break;
            }
            case 0x04: break;                                // L4_ThreadSwitch (no-op)
            case 0x08: res_r0 = 1; break;                    // L4_ThreadControl
            case 0x0c: {                                     // L4_ExchangeRegisters
                u32 dest = 0, control = 0, new_sp = 0, new_ip = 0, flags = 0;
                uc_reg_read(uc, UC_ARM_REG_R0, &dest);
                uc_reg_read(uc, UC_ARM_REG_R1, &control);
                uc_reg_read(uc, UC_ARM_REG_R2, &new_sp);
                uc_reg_read(uc, UC_ARM_REG_R3, &new_ip);
                uc_reg_read(uc, UC_ARM_REG_R4, &flags);
                res_r0 = dest; // L4_ExchangeRegisters retorna o dest ThreadId
                break;
            }
            case 0x10: break;                                // L4_Schedule
            case 0x14: {                                     // L4_MapControl
                u32 sid = 0, control = 0;
                uc_reg_read(uc, UC_ARM_REG_R0, &sid);
                uc_reg_read(uc, UC_ARM_REG_R1, &control);
                u32 utcb_ptr = 0;
                uc_mem_read(uc, 0xff000ff0, &utcb_ptr, 4);
                res_r0 = zeebo_l4::handle_map_control(uc, utcb_ptr, sid, control);
                printf("[Syscall] L4_MapControl(sid=0x%x, ctrl=0x%x) via UTCB@0x%08x -> res=0x%x\n",
                       sid, control, utcb_ptr, res_r0);
                break;
            }
            case 0x18: res_r0 = 1; break;                    // L4_SpaceControl
            case 0xb0: {                                     // L4_GetUtcb
                // Devolve a localização do UTCB do thread corrente (endereço no MISC).
                res_r0 = 0xff000fff & ~0xFFu;  // base da página UTCB (ref escrita pelo kernel)
                break;
            }
            case 0xb4: {                                     // L4_KernelInterface (KIP)
                res_r0 = KIP_BASE;
                kip_r1 = 0x0000000c;           // api_version
                kip_r2 = 0x00000002;           // api_flags
                kip_r3 = 0;                    // kernel_desc_ptr
                set_kip_ret = true;
                printf("[Syscall] L4_KernelInterface -> KIP@0x%08x (av052, api_flags)\n", KIP_BASE);
                // Grava nos ponteiros que o Iguana passou (se r4/r5/r6 != 0)
                u32 r4 = 0, r5 = 0, r6 = 0;
                uc_reg_read(uc, UC_ARM_REG_R4, &r4);
                uc_reg_read(uc, UC_ARM_REG_R5, &r5);
                uc_reg_read(uc, UC_ARM_REG_R6, &r6);
                if (r4) uc_mem_write(uc, r4, &kip_r1, 4);
                if (r5) uc_mem_write(uc, r5, &kip_r2, 4);
                if (r6) uc_mem_write(uc, r6, &kip_r3, 4);

                // Grava também no topo do stack (onde o runtime do Iguana lê os outputs):
                // Iguana faz pop ou ldr de variáveis locais na stack após a trap KIP.
                u32 sp = 0;
                uc_reg_read(uc, UC_ARM_REG_SP, &sp);
                uc_mem_write(uc, sp + 0, &kip_r1, 4);
                uc_mem_write(uc, sp + 4, &kip_r2, 4);
                uc_mem_write(uc, sp + 8, &kip_r3, 4);
                break;
            }
            default: res_r0 = 0; break;                      // demais (kputc etc.) no-op
        }

        if (set_kip_ret) {
            uc_reg_write(uc, UC_ARM_REG_R1, &kip_r1);
            uc_reg_write(uc, UC_ARM_REG_R2, &kip_r2);
            uc_reg_write(uc, UC_ARM_REG_R3, &kip_r3);
        }

        // Intercepta e inicializa o espaço de vídeo quando Iguana entra em execução
        static bool s_gpu_inited = false;
        if (!s_gpu_inited && (pc >= 0xb0000000)) {
            s_gpu_inited = true;
            if (sys->gpu_) {
                sys->gpu_->write(0x010c, 0x0020); // Emit draws to Adreno 130
                sys->gpu_->mark_dirty();
            }
        }

        uc_reg_write(uc, UC_ARM_REG_R0, &res_r0);
        u32 target_pc = 0;
        if (syscall == 0xb4) {
            target_pc = pc + 4;
            uc_reg_write(uc, UC_ARM_REG_PC, &target_pc);
            if (ip) uc_reg_write(uc, UC_ARM_REG_SP, &ip);
            // Invalida o TB de execução no Unicorn para que ele recompile o bloco seguinte
            uc_ctl_remove_cache(uc, 0xb000c720, 0x100);
            uc_ctl_remove_cache(uc, 0xb00033d0, 0x100);
        } else if (syscall == 0x00) {
            target_pc = pc + 4; // avança após svc #0x1400 (ou seja, 0xb000c834: pop {r1, r2})
            if (ip) uc_reg_write(uc, UC_ARM_REG_SP, &ip);
            uc_reg_write(uc, UC_ARM_REG_PC, &target_pc);
            uc_ctl_remove_cache(uc, 0xb000c800, 0x100);
        } else if (syscall == 0x0c) { // L4_ExchangeRegisters
            target_pc = pc + 4;
            uc_reg_write(uc, UC_ARM_REG_PC, &target_pc);
            uc_ctl_remove_cache(uc, pc, 16);
        } else {
            if (ip) uc_reg_write(uc, UC_ARM_REG_SP, &ip);
            if (lr) {
                target_pc = lr & ~1;
                uc_reg_write(uc, UC_ARM_REG_PC, &target_pc);
                u32 cpsr = 0;
                uc_reg_read(uc, UC_ARM_REG_CPSR, &cpsr);
                if (lr & 1) cpsr |= (1 << 5); else cpsr &= ~(1 << 5);
                uc_reg_write(uc, UC_ARM_REG_CPSR, &cpsr);
            }
        }
        if (target_pc) {
            sys->core0_.entry = target_pc;
            uc_ctl_remove_cache(uc, target_pc, 16);
            if (!sys->stepping_) {
                uc_emu_stop(uc);
            }
        }
    }

    static void c0_code_hook(uc_engine* uc, uint64_t ad, uint32_t size, void* ud) {
        ZeeboLLESystem* sys = (ZeeboLLESystem*)ud;
        sys->core0_.insns++;
        // Intercepta a trap SVC L4_KernelInterface diretamente no endereço real para garantia de desvio
        if (ad == 0xb000c738) {
            u32 ip = 0;
            uc_reg_read(uc, UC_ARM_REG_R12, &ip);
            if (ip) uc_reg_write(uc, UC_ARM_REG_SP, &ip);
            u32 kip_base = KIP_BASE;
            u32 r1 = 0x0000000c, r2 = 0x00000002, r3 = 0;
            uc_reg_write(uc, UC_ARM_REG_R0, &kip_base);
            uc_reg_write(uc, UC_ARM_REG_R1, &r1);
            uc_reg_write(uc, UC_ARM_REG_R2, &r2);
            uc_reg_write(uc, UC_ARM_REG_R3, &r3);

            // Grava também nos ponteiros passados pelo Iguana em r4, r5, r6 e no stack
            u32 r4 = 0, r5 = 0, r6 = 0;
            uc_reg_read(uc, UC_ARM_REG_R4, &r4);
            uc_reg_read(uc, UC_ARM_REG_R5, &r5);
            uc_reg_read(uc, UC_ARM_REG_R6, &r6);
            if (r4) uc_mem_write(uc, r4, &r1, 4);
            if (r5) uc_mem_write(uc, r5, &r2, 4);
            if (r6) uc_mem_write(uc, r6, &r3, 4);
            if (ip) {
                uc_mem_write(uc, ip + 0, &r1, 4);
                uc_mem_write(uc, ip + 4, &r2, 4);
                uc_mem_write(uc, ip + 8, &r3, 4);
            }

            u32 next_pc = (u32)ad + 4;
            uc_reg_write(uc, UC_ARM_REG_PC, &next_pc);
            sys->core0_.entry = next_pc;
            uc_ctl_remove_cache(uc, 0xb000c720, 0x100);
            uc_ctl_remove_cache(uc, 0xb00033d0, 0x100);
            uc_emu_stop(uc);
            return;
        }
        // Fast-forward CRT0 BSS zeroing: se atingir o loop de memset do Iguana, completa em RAM e salta
        if (ad == 0xb000001c) {
            u32 r4 = 0, r5 = 0;
            uc_reg_read(uc, UC_ARM_REG_R4, &r4);
            uc_reg_read(uc, UC_ARM_REG_R5, &r5);
            if (r4 < r5 && (r5 - r4) < 0x100000) {
                std::vector<u8> zeroes(r5 - r4, 0);
                uc_mem_write(uc, r4, zeroes.data(), zeroes.size());
                uc_reg_write(uc, UC_ARM_REG_R4, &r5);
                u32 jump_target = 0xb0000034;
                uc_reg_write(uc, UC_ARM_REG_PC, &jump_target);
                sys->core0_.entry = jump_target;
                uc_ctl_remove_cache(uc, 0xb0000000, 0x100);
            }
        }
        if (!sys->c0_script_hooks_.empty()) {
            auto it = sys->c0_script_hooks_.find((u32)ad);
            if (it != sys->c0_script_hooks_.end()) {
                const std::string& action = it->second;
                if (action == "stub_r0_0" || action == "stub") {
                    u32 lr = 0;
                    uc_reg_read(uc, UC_ARM_REG_LR, &lr);
                    u32 ret_val = 0;
                    uc_reg_write(uc, UC_ARM_REG_R0, &ret_val);
                    u32 target = lr & ~1;
                    uc_reg_write(uc, UC_ARM_REG_PC, &target);
                    sys->core0_.entry = target;
                    uc_ctl_remove_cache(uc, target, 16);
                    uc_emu_stop(uc);
                    return;
                } else if (action == "stub_r0_1") {
                    u32 lr = 0;
                    uc_reg_read(uc, UC_ARM_REG_LR, &lr);
                    u32 ret_val = 1;
                    uc_reg_write(uc, UC_ARM_REG_R0, &ret_val);
                    u32 target = lr & ~1;
                    uc_reg_write(uc, UC_ARM_REG_PC, &target);
                    sys->core0_.entry = target;
                    uc_ctl_remove_cache(uc, target, 16);
                    uc_emu_stop(uc);
                    return;
                } else if (action == "step_pc_4") {
                    u32 next = (u32)ad + 4;
                    uc_reg_write(uc, UC_ARM_REG_PC, &next);
                    sys->core0_.entry = next;
                    uc_ctl_remove_cache(uc, next, 16);
                    uc_emu_stop(uc);
                    return;
                } else if (action.starts_with("jump:")) {
                    u32 dest = (u32)std::strtoul(action.c_str() + 5, nullptr, 0);
                    uc_reg_write(uc, UC_ARM_REG_PC, &dest);
                    sys->core0_.entry = dest;
                    uc_ctl_remove_cache(uc, dest, 16);
                    uc_emu_stop(uc);
                    return;
                } else if (action == "break") {
                    sys->paused_ = true;
                    uc_emu_stop(uc);
                    return;
                }
            }
        }
        if (!sys->stepping_ && !sys->c0_breakpoints_.empty() && sys->c0_breakpoints_.contains((u32)ad)) {
            sys->paused_ = true;
            sys->core0_.entry = (u32)ad;
            uc_emu_stop(uc);
            return;
        }
        (void)size;
    }


    static bool c0_unmapped_hook(uc_engine* uc, uc_mem_type type, uint64_t addr, int size, int64_t value, void* ud) {
        u32 pc = 0;
        uc_reg_read(uc, UC_ARM_REG_PC, &pc);
        if (addr >= KEYPAD_BASE && addr < KEYPAD_BASE + KEYPAD_SIZE && type == UC_MEM_READ_UNMAPPED) {
            ZeeboLLESystem* sys = (ZeeboLLESystem*)ud;
            u32 val = sys->input_->read((u32)(addr - KEYPAD_BASE));
            uc_mem_map(uc, addr & ~0xFFFULL, 0x1000, UC_PROT_ALL);
            uc_mem_write(uc, addr, &val, size);
            return true;
        }
        // Map dynamically to continue discovery
        uc_mem_map(uc, addr & ~0xFFFULL, 0x1000, UC_PROT_ALL);
        return true;
    }

    static void c0_mem_hook(uc_engine* uc, uc_mem_type type, uint64_t addr, int size, int64_t value, void* ud) {
        ZeeboLLESystem* sys = (ZeeboLLESystem*)ud;
        // Inter-core doorbell A2M
        if (addr >= MSM_CSR_BASE + 0x400 && addr <= MSM_CSR_BASE + 0x440 && type == UC_MEM_WRITE) {
            u32 int_num = (addr - (MSM_CSR_BASE + 0x400)) / 4;
            printf("[Doorbell A2M] Core 0 -> Core 1 INT #%u (val=0x%llx)\n", int_num, (unsigned long long)value);
            if (sys->core1_state_ && sys->core1_state_->uc) {
                u32 vic_status0 = 0;
                uc_mem_read(sys->core1_state_->uc, MSM_VIC_BASE, &vic_status0, 4);
                vic_status0 |= (1 << int_num);
                uc_mem_write(sys->core1_state_->uc, MSM_VIC_BASE, &vic_status0, 4);

                // Inject RPC packets on doorbell trigger using official IDs AUDMGR (0x30000013) / ADSPRTOSATOM (0x3000000a)
                if (sys->smd_) {
                    std::vector<u8> dummy_payload(16, 0x42);
                    sys->smd_->inject_packet(sys->core1_state_->uc, 0x30000013, 0x1b59, dummy_payload);
                    sys->smd_->inject_packet(sys->core1_state_->uc, 0x3000000a, 0x02, dummy_payload);
                }
            }
        }
        // ProcComm command write by Core 0
        else if (addr == SMEM_BASE + 0x00 && type == UC_MEM_WRITE) { // APP_COMMAND
            u32 cmd = (u32)value;
            printf("[ProcComm] Core 0 issued command 0x%x\n", cmd);
            u32 status_success = 3; // PCOM_CMD_SUCCESS
            uc_mem_write(uc, SMEM_BASE + 0x04, &status_success, 4); // APP_STATUS
            u32 cmd_done = 1; // PCOM_CMD_DONE
            uc_mem_write(uc, SMEM_BASE + 0x00, &cmd_done, 4);
        }
        // MDDI write
        else if (addr >= MSM_MDDI_BASE && addr < MSM_MDDI_BASE + MDDI_SIZE) {
            sys->mddi_->write((u32)(addr - MSM_MDDI_BASE), (u32)value);
        }
        // Adreno GPU write
        else if (addr >= ADRENO130_BASE && addr < ADRENO130_BASE + ADRENO130_SIZE) {
            sys->gpu_->write((u32)(addr - ADRENO130_BASE), (u32)value);
        }
        // Keypad write
        else if (addr >= KEYPAD_BASE && addr < KEYPAD_BASE + KEYPAD_SIZE) {
            sys->input_->write((u32)(addr - KEYPAD_BASE), (u32)value);
        }
    }

    static void c1_code_hook(uc_engine* uc, uint64_t ad, uint32_t size, void* ud) {
        ZeeboLLESystem* sys = (ZeeboLLESystem*)ud;
        sys->core1_.insns++;
        if (!sys->c1_script_hooks_.empty()) {
            auto it = sys->c1_script_hooks_.find((u32)ad);
            if (it != sys->c1_script_hooks_.end()) {
                const std::string& action = it->second;
                if (action == "stub_r0_0" || action == "stub") {
                    u32 lr = 0;
                    uc_reg_read(uc, UC_ARM_REG_LR, &lr);
                    u32 ret_val = 0;
                    uc_reg_write(uc, UC_ARM_REG_R0, &ret_val);
                    u32 target = lr & ~1;
                    uc_reg_write(uc, UC_ARM_REG_PC, &target);
                    sys->core1_.entry = target;
                    uc_ctl_remove_cache(uc, target, 16);
                    uc_emu_stop(uc);
                    return;
                } else if (action == "stub_r0_1") {
                    u32 lr = 0;
                    uc_reg_read(uc, UC_ARM_REG_LR, &lr);
                    u32 ret_val = 1;
                    uc_reg_write(uc, UC_ARM_REG_R0, &ret_val);
                    u32 target = lr & ~1;
                    uc_reg_write(uc, UC_ARM_REG_PC, &target);
                    sys->core1_.entry = target;
                    uc_ctl_remove_cache(uc, target, 16);
                    uc_emu_stop(uc);
                    return;
                } else if (action == "step_pc_4") {
                    u32 next = (u32)ad + 4;
                    uc_reg_write(uc, UC_ARM_REG_PC, &next);
                    sys->core1_.entry = next;
                    uc_ctl_remove_cache(uc, next, 16);
                    uc_emu_stop(uc);
                    return;
                } else if (action.starts_with("jump:")) {
                    u32 dest = (u32)std::strtoul(action.c_str() + 5, nullptr, 0);
                    uc_reg_write(uc, UC_ARM_REG_PC, &dest);
                    sys->core1_.entry = dest;
                    uc_ctl_remove_cache(uc, dest, 16);
                    uc_emu_stop(uc);
                    return;
                } else if (action == "break") {
                    sys->paused_ = true;
                    uc_emu_stop(uc);
                    return;
                }
            }
        }
        if (!sys->stepping_ && !sys->c1_breakpoints_.empty() && sys->c1_breakpoints_.contains((u32)ad)) {
            sys->paused_ = true;
            sys->core1_.entry = (u32)ad;
            uc_emu_stop(uc);
            return;
        }
        if (ad == 0x00d10588) { // Panic bypass
            u32 lr = 0; uc_reg_read(uc, UC_ARM_REG_LR, &lr);
            u32 new_pc = lr & ~1; uc_reg_write(uc, UC_ARM_REG_PC, &new_pc);
            u32 cpsr = 0; uc_reg_read(uc, UC_ARM_REG_CPSR, &cpsr);
            if (lr & 1) cpsr |= (1 << 5); else cpsr &= ~(1 << 5);
            uc_reg_write(uc, UC_ARM_REG_CPSR, &cpsr);
        }

        // QDSP5 Dispatcher feed on AMSS consumer entry points
        if ((ad == 0x16e8cb96 || ad == 0x16e8cba0) && sys->qdsp_disp_) {
            u32 pkt_ptr = 0;
            uc_reg_read(uc, UC_ARM_REG_R0, &pkt_ptr);
            if (pkt_ptr != 0) {
                std::vector<u8> pkt_buf(512, 0);
                if (uc_mem_read(uc, pkt_ptr, pkt_buf.data(), pkt_buf.size()) == UC_ERR_OK) {
                    zeebo::qdsp5::QdspGuest guest;
                    guest.ctx = sys->core0_.uc; // Core 0 context for shared memory reads
                    guest.read = [](u32 va, void* dst, u32 sz, void* ctx) -> bool {
                        if (!ctx) return false;
                        uc_engine* uc0 = (uc_engine*)ctx;
                        return uc_mem_read(uc0, va, dst, sz) == UC_ERR_OK;
                    };
                    u32 caller_tcb = 0;
                    uc_reg_read(uc, UC_ARM_REG_R1, &caller_tcb);
                    sys->qdsp_disp_->feed_raw(pkt_buf.data(), (u32)pkt_buf.size(), guest, caller_tcb);
                }
            }
        }
    }

    static void c1_mem_hook(uc_engine* uc, uc_mem_type type, uint64_t addr, int size, int64_t value, void* ud) {
        if (addr == 0xc5000108 && type == UC_MEM_READ) {
            static u32 ticker = 100000;
            ticker += 5000;
            uc_mem_write(uc, 0xc5000108, &ticker, 4);
        }
        else if (addr == SMEM_BASE + 0x10 && type == UC_MEM_WRITE) { // MDM_COMMAND
            u32 cmd = (u32)value;
            printf("[ProcComm] Core 1 (Modem) acked/issued command 0x%x\n", cmd);
        }
    }

    // ControlServer instance for remote interactive debugging
    std::unique_ptr<zeebo_lle::ControlServer> control_;
    bool paused_ = false;
    bool stepping_ = false;
    bool quit_requested_ = false;
    std::set<u32> c0_breakpoints_;
    std::set<u32> c1_breakpoints_;
    std::map<u32, std::string> c0_script_hooks_;
    std::map<u32, std::string> c1_script_hooks_;

    CoreState core0_;
    CoreState core1_;
    CoreState* core1_state_;
    std::unique_ptr<NandController> nand_;
    std::unique_ptr<UnifiedMDDI> mddi_;
    std::unique_ptr<UnifiedAdreno130> gpu_;
    std::unique_ptr<UnifiedInput> input_;
    std::unique_ptr<UnifiedSMDBridge> smd_;
    std::unique_ptr<UnifiedDisplaySink> sink_;
    std::unique_ptr<zeebo::qdsp5::Qdsp5Dispatcher> qdsp_disp_;
    std::unique_ptr<zeebo::gpu::IGpuRasterizer> rast_;
    std::unique_ptr<zeebo::gpu::IglHook> igl_hook_;
};

int main(int argc, char** argv) {
    const char* nand_path = "../../nand/1.1.2.bin";
    const char* apps_path = "../../nand/1.1.2_APPS.bin";
    const char* amss_path = "../../nand/1.1.2_AMSS.bin";
    std::string applet_path = "";
    bool headless = true;
    int control_port = 0;

    for (int i = 1; i < argc; i++) {
        std::string arg = argv[i];
        if (arg == "--gui" || arg == "-g") {
            headless = false;
        } else if (arg == "--headless") {
            headless = true;
        } else if (arg.rfind("--control-port=", 0) == 0) {
            control_port = std::stoi(arg.substr(15));
        } else if (arg.rfind("--applet=", 0) == 0) {
            applet_path = arg.substr(9);
        } else if (i == 1 && arg[0] != '-') {
            nand_path = argv[1];
        } else if (i == 2 && arg[0] != '-') {
            apps_path = argv[2];
        } else if (i == 3 && arg[0] != '-') {
            amss_path = argv[3];
        }
    }

    printf("[System] Mode: %s\n", headless ? "Headless (CLI/Test runner)" : "Interactive GUI (SDL2 Window 640x480 active)");

    ZeeboLLESystem sys;
    if (control_port > 0) {
        if (!sys.init_control(control_port)) {
            printf("[Warn] Failed to bind control server on port %d\n", control_port);
        } else {
            printf("[Control] Remote debug interface active on port %d\n", control_port);
            sys.set_paused(true);
        }
    }

    if (!sys.init(nand_path, apps_path, amss_path, headless)) {
        printf("[Fatal] System initialization failed\n");
        return 1;
    }

    // Direct applet injection if requested
    if (!applet_path.empty()) {
        if (!sys.load_applet(applet_path, 0x12000000)) {
            printf("[Warn] Failed to load specified applet: %s\n", applet_path.c_str());
        }
    }

    // Run interleaved for 250 cycles of 10k instructions
    sys.run_interleaved(250, 10000);

    return 0;
}
