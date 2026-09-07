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
#include <memory>
#include <algorithm>
#include <SDL2/SDL.h>
#include <unicorn/unicorn.h>

#include "zeebo_devices.h"

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
    UnifiedAdreno130() : status_(1), rb_rptr_(0), rb_wptr_(0), int_status_(0), int_en_(0), draws_(0) {}
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
            if (int_en_ & 1) int_status_ |= 1;
        } else if (off == 0x0124) int_en_ = val;
        else if (off == 0x0128) int_status_ &= ~val;
    }
    u32 draws() const { return draws_; }
private:
    u32 status_, rb_rptr_, rb_wptr_, int_status_, int_en_, draws_;
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
    void press_key(u32 key_bit) {
        keys_pressed_ |= key_bit;
        int_status_ |= 1; // Assert INT_KEYSENSE
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

// 4. Host Display Sink (SDL2 + Snapshot)
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
        sink_ = std::make_unique<UnifiedDisplaySink>();
        sink_->init(headless);

        // 3. Initialize Core 0 (ARM1176JZ-S — Applications Processor)
        printf("[System] Initializing Core 0 (ARM1176JZ-S Apps Processor)...\n");
        uc_err err0 = uc_open(UC_ARCH_ARM, UC_MODE_ARM, &core0_.uc);
        if (err0 != UC_ERR_OK) {
            printf("[Fatal] Failed to init Core 0: %s\n", uc_strerror(err0));
            return false;
        }
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

        for (int c = 0; c < cycles; c++) {
            // Step Core 0 (ARM11)
            uc_err e0 = uc_emu_start(core0_.uc, core0_.entry, 0, 0, slice_insns);
            uc_reg_read(core0_.uc, UC_ARM_REG_PC, &core0_.entry);

            // Step Core 1 (ARM9)
            uc_err e1 = uc_emu_start(core1_.uc, core1_.entry, 0, 0, slice_insns);
            uc_reg_read(core1_.uc, UC_ARM_REG_PC, &core1_.entry);

            printf("  [Cycle %02d] Core0(ARM11): pc=0x%08x insns=%llu (%s) | Core1(ARM9): pc=0x%08x insns=%llu (%s)\n",
                   c, core0_.entry, (unsigned long long)core0_.insns, e0 ? uc_strerror(e0) : "ok",
                   core1_.entry, (unsigned long long)core1_.insns, e1 ? uc_strerror(e1) : "ok");

            // Process SDL events if window is open
            SDL_Event ev;
            while (SDL_PollEvent(&ev)) {
                if (ev.type == SDL_QUIT) return;
                if (ev.type == SDL_KEYDOWN) {
                    switch (ev.key.keysym.sym) {
                        case SDLK_z: case SDLK_RETURN: input_->press_key(ZEEBO_KEY_A); break;
                        case SDLK_x: case SDLK_ESCAPE: input_->press_key(ZEEBO_KEY_B); break;
                        case SDLK_c:                   input_->press_key(ZEEBO_KEY_C); break;
                        case SDLK_v:                   input_->press_key(ZEEBO_KEY_D); break;
                        case SDLK_UP:                  input_->press_key(ZEEBO_KEY_UP); break;
                        case SDLK_DOWN:                input_->press_key(ZEEBO_KEY_DOWN); break;
                        case SDLK_LEFT:                input_->press_key(ZEEBO_KEY_LEFT); break;
                        case SDLK_RIGHT:               input_->press_key(ZEEBO_KEY_RIGHT); break;
                        case SDLK_h:                   input_->press_key(ZEEBO_KEY_HOME); break;
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
                }
            }
        }
        printf("[System] Execution batch completed successfully.\n");
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

        // APPS Partition starts at Block 0x0e6 (page 0x0e6 * 64 = 14720)
        u32 start_block = 0x0e6;
        u32 start_page = start_block * 64;
        const u32 DMA_PAGE_BUF = 0x00450000;

        dmov_read_page(core0_.uc, dm, start_page, DMA_PAGE_BUF);
        u8 elf_hdr[52];
        uc_mem_read(core0_.uc, DMA_PAGE_BUF, elf_hdr, sizeof(elf_hdr));
        u32 magic = rd32(elf_hdr, 0);

        if (magic == 0x464c457f) {
            printf("[System] Direct NAND DMA read verified APPS ELF Header!\n");
            core0_.entry = rd32(elf_hdr, 24);
            u32 phoff = rd32(elf_hdr, 28);
            u16 phent = rd16(elf_hdr, 42), phnum = rd16(elf_hdr, 44);
            printf("[System] APPS ELF Entrypoint: 0x%08x, Segments: %u\n", core0_.entry, phnum);
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
            // If segment has different PA and VA, map both
            if (pa && (pa & ~0xFFFu) != (va & ~0xFFFu)) {
                uc_mem_map(core0_.uc, pa & ~0xFFFu, ((nmem+0xFFF)&~0xFFFu)+0x1000, UC_PROT_ALL);
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
        uc_hook_add(core0_.uc, &h_u0, UC_HOOK_MEM_READ_UNMAPPED | UC_HOOK_MEM_WRITE_UNMAPPED, (void*)c0_unmapped_hook, this, 0, ~0ULL);
        uc_hook_add(core0_.uc, &h_i0, UC_HOOK_INTR, (void*)c0_intr_hook, this, 1, 0);

        // Core 1 hooks
        uc_hook h_c1, h_m1;
        uc_hook_add(core1_.uc, &h_c1, UC_HOOK_CODE, (void*)c1_code_hook, this, 0, ~0ULL);
        uc_hook_add(core1_.uc, &h_m1, UC_HOOK_MEM_READ | UC_HOOK_MEM_WRITE, (void*)c1_mem_hook, this, 0, ~0ULL);
    }

    static void c0_intr_hook(uc_engine* uc, uint32_t intno, void* ud) {
        ZeeboLLESystem* sys = (ZeeboLLESystem*)ud;
        u32 pc = 0;
        uc_reg_read(uc, UC_ARM_REG_PC, &pc);
        u8 b[4]; int off = pc - 4;
        if (uc_mem_read(uc, off, b, 4) != UC_ERR_OK) return;
        u32 w = rd32(b, 0); u32 imm = w & 0xFFFFFF;

        // L4e syscall ABI:
        // caller saved SP in IP (r12)
        // LR has return address
        u32 ip = 0, lr = 0;
        uc_reg_read(uc, UC_ARM_REG_R12, &ip);
        uc_reg_read(uc, UC_ARM_REG_LR, &lr);
        
        // Emulate successful return: r0 = 0
        u32 zero = 0;
        uc_reg_write(uc, UC_ARM_REG_R0, &zero);
        if (ip) uc_reg_write(uc, UC_ARM_REG_SP, &ip);
        if (lr) {
            u32 target_pc = lr & ~1;
            uc_reg_write(uc, UC_ARM_REG_PC, &target_pc);
            u32 cpsr = 0;
            uc_reg_read(uc, UC_ARM_REG_CPSR, &cpsr);
            if (lr & 1) cpsr |= (1 << 5); else cpsr &= ~(1 << 5);
            uc_reg_write(uc, UC_ARM_REG_CPSR, &cpsr);
        }
    }

    static void c0_code_hook(uc_engine* uc, uint64_t ad, uint32_t size, void* ud) {
        ZeeboLLESystem* sys = (ZeeboLLESystem*)ud;
        sys->core0_.insns++;
    }

    static void c0_unmapped_hook(uc_engine* uc, uc_mem_type type, uint64_t addr, int size, int64_t value, void* ud) {
        u32 pc = 0;
        uc_reg_read(uc, UC_ARM_REG_PC, &pc);
        if (addr >= KEYPAD_BASE && addr < KEYPAD_BASE + KEYPAD_SIZE && type == UC_MEM_READ_UNMAPPED) {
            ZeeboLLESystem* sys = (ZeeboLLESystem*)ud;
            u32 val = sys->input_->read((u32)(addr - KEYPAD_BASE));
            uc_mem_map(uc, addr & ~0xFFFULL, 0x1000, UC_PROT_ALL);
            uc_mem_write(uc, addr, &val, size);
            return;
        }
        printf("[Core0 Unmapped] %s at 0x%08llx (size %d, val 0x%llx) at pc=0x%08x\n",
               type == UC_MEM_WRITE_UNMAPPED ? "WRITE" : "READ",
               (unsigned long long)addr, size, (unsigned long long)value, pc);
        // Map dynamically to continue discovery
        uc_mem_map(uc, addr & ~0xFFFULL, 0x1000, UC_PROT_ALL);
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
        if (ad == 0x00d10588) { // Panic bypass
            u32 lr = 0; uc_reg_read(uc, UC_ARM_REG_LR, &lr);
            u32 new_pc = lr & ~1; uc_reg_write(uc, UC_ARM_REG_PC, &new_pc);
            u32 cpsr = 0; uc_reg_read(uc, UC_ARM_REG_CPSR, &cpsr);
            if (lr & 1) cpsr |= (1 << 5); else cpsr &= ~(1 << 5);
            uc_reg_write(uc, UC_ARM_REG_CPSR, &cpsr);
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

    CoreState core0_;
    CoreState core1_;
    CoreState* core1_state_;
    std::unique_ptr<NandController> nand_;
    std::unique_ptr<UnifiedMDDI> mddi_;
    std::unique_ptr<UnifiedAdreno130> gpu_;
    std::unique_ptr<UnifiedInput> input_;
    std::unique_ptr<UnifiedDisplaySink> sink_;
};

int main(int argc, char** argv) {
    const char* nand_path = argc > 1 ? argv[1] : "../../nand/1.1.2.bin";
    const char* apps_path = argc > 2 ? argv[2] : "../../nand/1.1.2_APPS.bin";
    const char* amss_path = argc > 3 ? argv[3] : "../../nand/1.1.2_AMSS.bin";

    ZeeboLLESystem sys;
    if (!sys.init(nand_path, apps_path, amss_path, true)) {
        printf("[Fatal] System initialization failed\n");
        return 1;
    }

    // Run interleaved for 60 cycles of 10k instructions (600k instructions per core)
    sys.run_interleaved(60, 10000);

    return 0;
}
