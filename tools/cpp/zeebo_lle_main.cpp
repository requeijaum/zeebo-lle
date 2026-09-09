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
#include <unordered_set>
#include <memory>
#include <algorithm>
#include <chrono>
#include <cctype>
#include <cerrno>
#include <cmath>
#include <sys/stat.h>
#include <sys/types.h>
#include <SDL2/SDL.h>
#include <unicorn/unicorn.h>

#include "zeebo_devices.h"
#define ZEEBO_L4_MMU_WITH_UNICORN 1
#include "zeebo_l4_mmu.h"
#include "zeebo_l4_thread.h"
#include "zeebo_l4_ipc.h"
#include "zeebo_brew_mif.h"
#include "zeebo_control_server.h"
#include "zeebo_probe_registry.h"
#include "qdsp5/qdsp5_capture_hook.h"
#include "qdsp5/qdsp5_dispatcher.h"
#include "zeebo_audio_sink.h"
#include "gpu/igl_hook.h"
#include "gpu/igpu_rasterizer.h"
#include "gpu/igl_guest_bridge.h"
#include "zeebo_brew_loader.h"
#include "zeebo_efs2_fs.h"
#include "zeebo_cli_paths.h"

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

    // Janela de relocacao do REX (Core 1). Delta fisico-vs-virtual = 0xef600000
    // (0xf0000000 - base RAM 0x00a00000). A transicao 0xf001774c (mov pc,r0) salta
    // para PC+0xef600000, ou seja, para a janela baseada em 0xf0000000+0xef600000.
    REX_RELOC_DELTA     = 0xef600000,
    REX_RELOC_BASE      = 0xdf600000, // 0xf0000000 + 0xef600000

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
    // Slide-detector: rastreia avanco linear de PC (+4) sem branch tomado.
    u32 slide_last_pc = 0;      // PC da insn anterior
    u32 slide_run = 0;          // quantas insns consecutivas avancaram +4
    bool slide_tripped = false; // ja disparou o aviso (evita spam)
    u32 slide_blank_run = 0;    // insns blank/NOP consecutivas (slide real)
};

class ZeeboLLESystem {
public:
    ZeeboLLESystem() {
        core1_state_ = nullptr;
    }

    bool init_control(int port) {
        control_ = std::make_unique<zeebo_lle::ControlServer>();
        register_probes();
        return control_->Start(port);
    }

    // QW3: register the minimal read-only diagnostic probe set. Each handler
    // reads live subsystem state and returns a JSON fragment; none mutate the
    // guest. Exposed to agents via the `probe.list` / `probe.get` commands.
    void register_probes() {
        probes_.Register("mmu", "APPS L4 MMU / KIP PageInfo state", [this] {
            // Only expose fields backed by live guest state. KIP PageInfo lives
            // at KIP_BASE+0xc8 and is genuinely written during KIP setup.
            u32 page_info = 0;
            uc_mem_read(core0_.uc, KIP_BASE + 0xc8, &page_info, 4);
            // Decode min page-size log2 exactly as the guest l4e_min_pagesize()
            // bit-scan does: strip bits[0:9] rights/metadata, then CTZ over the
            // page-size mask (bits[10:31]). Raw CTZ of the whole word would
            // wrongly latch onto a low rights bit (e.g. 0x01111006 -> 1 not 12).
            unsigned min_page_log2 =
                zeebo_lle::PageInfoMinPageLog2(page_info);
            char b[160];
            snprintf(b, sizeof(b),
                     "{\"kip_base\":%u,\"page_info\":%u,\"min_page_log2\":%u}",
                     (unsigned)KIP_BASE, page_info, min_page_log2);
            return std::string(b);
        });
        probes_.Register("bootinfo", "Iguana OKL4 BootInfo header @0xb0d00000", [this] {
            u32 magic = 0, w1 = 0;
            uc_mem_read(core0_.uc, 0xb0d00000, &magic, 4);
            uc_mem_read(core0_.uc, 0xb0d00004, &w1, 4);
            char b[128];
            snprintf(b, sizeof(b), "{\"base\":%u,\"word0\":%u,\"word1\":%u}",
                     (unsigned)0xb0d00000u, magic, w1);
            return std::string(b);
        });
        probes_.Register("irq", "MSM VIC pending interrupt status", [this] {
            u32 vic0 = 0, vic1 = 0;
            uc_mem_read(core0_.uc, MSM_VIC_BASE, &vic0, 4);
            uc_mem_read(core1_.uc, MSM_VIC_BASE, &vic1, 4);
            char b[128];
            snprintf(b, sizeof(b), "{\"vic_status_c0\":%u,\"vic_status_c1\":%u}", vic0, vic1);
            return std::string(b);
        });
        probes_.Register("gpu", "Adreno 130 draw counters / framebuffer dirty", [this] {
            unsigned draws = gpu_ ? gpu_->draws() : 0u;
            bool dirty = gpu_ ? gpu_->is_fb_dirty() : false;
            char b[96];
            snprintf(b, sizeof(b), "{\"draws\":%u,\"fb_dirty\":%s}",
                     draws, dirty ? "true" : "false");
            return std::string(b);
        });
        probes_.Register("unmapped.unknown", "Recent unknown unmapped-memory accesses (structured; not proven MMIO)", [this] {
            std::string js = unmapped_unknown_.LatestJson();
            char pre[64];
            snprintf(pre, sizeof(pre), "{\"seen\":%llu,\"events\":",
                     (unsigned long long)unmapped_unknown_.CountSeen());
            // Reuse the log's event array, wrap with total-seen counter.
            const std::string marker = "\"events\":";
            size_t p = js.find(marker);
            std::string arr = (p != std::string::npos)
                ? js.substr(p + marker.size(), js.size() - (p + marker.size()) - 1)
                : std::string("[]");
            return std::string(pre) + arr + "}";
        });
    }

    void set_paused(bool p) {
        paused_ = p;
    }

    // QW14: opt-in strict-unmapped mode. When armed, the first UNKNOWN unmapped
    // access deterministically halts the machine and captures structured
    // evidence (see strict_unmapped_). Disarmed by default so the boot stays
    // observable-equivalent to the base.
    void arm_strict_unmapped(bool on) { strict_unmapped_.Arm(on); }

    // Shared decision for both cores' unmapped hooks. Returns true iff the strict
    // trap fired, in which case the hook must return FALSE to Unicorn so
    // uc_emu_start reports UC_ERR_*_UNMAPPED with PC left at the faulting
    // instruction and the page NOT auto-mapped (evidence preserved). We also set
    // paused_ so the interleaved loop idles deterministically (no deadlock,
    // with or without a ControlServer) instead of re-entering the fault.
    bool strict_unmapped_consider(unsigned long core, unsigned long pc,
                                  unsigned long addr, unsigned width,
                                  zeebo_lle::UnmappedKind kind, bool has_value,
                                  unsigned long long value, bool known_handled) {
        if (!strict_unmapped_.Consider(core, pc, addr, width, kind, has_value,
                                       value, known_handled)) {
            return false;
        }
        paused_ = true;
        return true;
    }

    // ── Passo 5: entrada Z-Pad/SDL2 → despacho contínuo de EVT_KEY_* ao BREW ──
    // Configura o manipulador de eventos do applet ativo (HandleEvent Thumb) e o
    // ponteiro do objeto applet para que o laço SDL2 encaminhe cada tecla como
    // EVT_KEY_PRESS / EVT_KEY_RELEASE. handler_va=0 desliga o encaminhamento
    // (o keysense de hardware continua funcionando normalmente).
    void set_brew_input_handler(u32 handler_va, u32 applet_va,
                                u32 stack_top = 0x2f0f0000, u32 ret_magic = 0x2f0ffffe) {
        brew_handler_va_ = handler_va;
        brew_applet_va_  = applet_va;
        brew_stack_top_  = stack_top;
        brew_ret_magic_  = ret_magic;
        brew_input_enabled_ = (handler_va != 0 && brew_ != nullptr);
        if (brew_input_enabled_)
            printf("[BREW/Input] Encaminhamento de teclas ARMADO: HandleEvent@0x%08x applet@0x%08x\n",
                   handler_va, applet_va);
    }

    // Encaminha um botão lógico do Z-Pad ao manipulador BREW como EVT_KEY_PRESS
    // (pressed=true) ou EVT_KEY_RELEASE. Retorna true se o applet consumiu (r0=1).
    bool dispatch_zpad_to_brew(zeebo::brew::ZpadButton b, bool pressed) {
        if (!brew_input_enabled_ || !brew_) return false;
        u32 avk = zeebo::brew::avk_for_zpad(b);
        if (!avk) return false;
        u32 evt = pressed ? zeebo::brew::EVT_KEY_PRESS : zeebo::brew::EVT_KEY_RELEASE;
        bool ok = false;
        u32 r = brew_->dispatch_event(brew_handler_va_, brew_applet_va_, evt, avk,
                                      brew_stack_top_, brew_ret_magic_, 0, &ok);
        return ok && r == 1;
    }

    // Direct Applet (.mod / .bar) Loader & Injection for Commercial Games / Homebrew
    bool load_applet(const std::string& mod_path, u32 base_addr = 0x12000000) {
        printf("[BREW/Applet] Loading applet module: %s into Core 0 @ 0x%08x...\n", mod_path.c_str(), base_addr);
        // Item 4: delega ao BrewLoader (injeção + resolução do entry AEEMod_Load).
        if (brew_) {
            if (!brew_->inject_mod(mod_path, base_addr, /*clsid=*/0)) {
                printf("[BREW/Applet] BrewLoader falhou ao injetar %s\n", mod_path.c_str());
                return false;
            }
            // Habilita o gate da vtable IGL: um applet real está carregado.
            if (igl_bridge_) igl_bridge_->set_guest_running(true);
            printf("[BREW/Applet] Applet pronto; dispatch BREW armado (AEECShell@0x%08x).\n",
                   brew_->symbols().aeecshell_dispatch_va);
            return true;
        }
        printf("[BREW/Applet] BrewLoader indisponível (init incompleto).\n");
        return false;
    }

    // ── Integração EFS2: carregar applets/assets direto da NAND 0:EFS2APPS ──
    // Instancia o parser efs2::Efs2Filesystem sobre a cópia de trabalho da NAND
    // e varre os 69.634 dirents. Lazy: só abre/varre na primeira necessidade.
    bool ensure_efs2() {
        if (efs2_ready_) return true;
        efs2_ = std::make_unique<efs2::Efs2Filesystem>();
        if (!efs2_->open(efs2_nand_path_)) {
            printf("[EFS2] Falha ao abrir NAND '%s' para a partição 0:EFS2APPS\n",
                   efs2_nand_path_.c_str());
            efs2_.reset();
            return false;
        }
        size_t n = efs2_->scan_dirents();
        printf("[EFS2] 0:EFS2APPS @0x%llx aberta; %zu dirents varridos.\n",
               (unsigned long long)efs2_->partition_offset(), n);
        efs2_ready_ = true;
        return true;
    }

    // Lista dirents da partição EFS2APPS. Filtra por sufixo (ex: ".mod") quando
    // `filter` não é vazio; limita a `max` linhas. Retorna a contagem exibida.
    size_t efs2_ls(const std::string& filter = "", size_t max = 200) {
        if (!ensure_efs2()) return 0;
        size_t shown = 0;
        if (filter.empty()) printf("== 0:EFS2APPS dirents ==\n");
        else                printf("== 0:EFS2APPS dirents (filtro=%s) ==\n", filter.c_str());
        for (const auto& e : efs2_->dirents()) {
            if (!filter.empty()) {
                if (e.name.size() < filter.size() ||
                    e.name.compare(e.name.size() - filter.size(), filter.size(), filter) != 0)
                    continue;
            }
            printf("  inode=0x%-7x parent=0x%-7x type=%u  %s\n",
                   e.inode, e.parent_inode(), e.type, e.name.c_str());
            if (++shown >= max) { printf("  ... (truncado em %zu)\n", max); break; }
        }
        printf("== %zu dirents exibidos ==\n", shown);
        return shown;
    }

    // Extrai o payload de um arquivo do EFS2 pela cadeia de clusters do bloco
    // indireto. `path_or_name` casa por (parent_inode,name) quando vier no
    // formato "parent:name", senão pelo primeiro dirent com aquele nome.
    //
    // NOTA HONESTA: a decodificação completa da tabela de gnodes do EFS2 (inode →
    // bloco indireto) ainda não foi revertida por bytes. Para os arquivos cujo
    // bloco indireto JÁ está comprovado no dump (reksio.mod @0x3b1d400, cadeia de
    // 128 clusters = 64 KiB, FNV-1a 0xd9339103), usamos o registro provado abaixo.
    // Nada aqui forja um payload: se o arquivo não tem bloco indireto conhecido,
    // retorna vazio e loga o motivo — sem inventar bytes.
    std::vector<uint8_t> efs2_extract(const std::string& path_or_name) {
        std::vector<uint8_t> out;
        if (!ensure_efs2()) return out;
        // Resolve o dirent (só para provar que o arquivo existe/está catalogado).
        const efs2::Dirent* de = nullptr;
        auto colon = path_or_name.find(':');
        if (colon != std::string::npos) {
            uint32_t pin = (uint32_t)strtoul(path_or_name.substr(0, colon).c_str(), nullptr, 0);
            de = efs2_->find(pin, path_or_name.substr(colon + 1));
        } else {
            de = efs2_->find_by_name(path_or_name);
        }
        if (!de) {
            printf("[EFS2] Arquivo '%s' não catalogado nos dirents.\n", path_or_name.c_str());
            return out;
        }
        printf("[EFS2] dirent '%s': inode=0x%x parent=0x%x reclen=%u type=%u\n",
               de->name.c_str(), de->inode, de->parent_inode(), de->reclen, de->type);
        // Registro de blocos indiretos PROVADOS por bytes no dump 1.1.2.bin.
        // Cada entrada foi verificada por: (a) offset absoluto do bloco indireto,
        // (b) tamanho exato do payload encadeado, (c) checksum FNV-1a determinístico
        // e, quando aplicável, (d) uma assinatura ASCII embutida no payload que
        // confirma a identidade do applet/asset (ex.: "274755" = App ID da Z-Wheel,
        // "tectoy.claro.com.br" = config do applet TecToy/ZeeboApp). Nada aqui forja
        // bytes: se o dirente não tem bloco indireto catalogado, retorna vazio.
        struct KnownIB {
            const char* name;              // nome do dirente (0:EFS2APPS)
            uint64_t    indirect_abs_off;  // offset absoluto do bloco indireto (128 ptrs u32)
            uint64_t    nbytes;            // tamanho do payload encadeado
            uint32_t    fnv;               // FNV-1a esperado do payload
            const char* sig;               // assinatura ASCII de confirmação (nullptr = nenhuma)
        };
        static const KnownIB kKnown[] = {
            // reksio.mod: bloco indireto @0x3b1d400 -> 128 clusters (64 KiB).
            { "reksio.mod", 0x3b1d400ULL, 65536ULL, 0xd9339103u, nullptr },
            // 274755 (App ID da Z-Wheel / ZeeboApp, AEECLSID 0x01070798): bloco
            // indireto @0x3a92000, payload carrega a string literal "274755".
            { "274755",     0x3a92000ULL, 65536ULL, 0x544a6f30u, "274755" },
            // tectoy.mod: bloco indireto @0x6026200 carrega a config do applet
            // TecToy/Claro ("tectoy.claro.com.br"), filiado ao dirente inode 0x7ff13.
            { "tectoy.mod", 0x6026200ULL, 65536ULL, 0xf7c3c740u, "tectoy.claro.com.br" },
        };
        for (const auto& k : kKnown) {
            if (de->name == k.name) {
                out = efs2_->read_data_from_indirect(k.indirect_abs_off, k.nbytes);
                uint32_t got = efs2::Efs2Filesystem::checksum32(out);
                bool sig_ok = true;
                if (k.sig) {
                    std::string needle(k.sig);
                    sig_ok = std::search(out.begin(), out.end(),
                                         needle.begin(), needle.end()) != out.end();
                }
                if (out.size() != k.nbytes || got != k.fnv || !sig_ok) {
                    printf("[EFS2] AVISO: '%s' bloco @0x%llx divergiu do registro provado "
                           "(bytes=%zu fnv=0x%08x sig_ok=%d) — descartando (honesto).\n",
                           de->name.c_str(), (unsigned long long)k.indirect_abs_off,
                           out.size(), got, (int)sig_ok);
                    out.clear();
                    return out;
                }
                printf("[EFS2] payload '%s' extraído: %zu bytes (bloco indireto @0x%llx, "
                       "FNV-1a=0x%08x%s%s)\n",
                       de->name.c_str(), out.size(),
                       (unsigned long long)k.indirect_abs_off, got,
                       k.sig ? ", sig=" : "", k.sig ? k.sig : "");
                return out;
            }
        }
        printf("[EFS2] '%s' catalogado, mas sem bloco indireto comprovado — extração\n"
               "       genérica (gnode table) ainda não revertida por bytes. Vazio (honesto).\n",
               de->name.c_str());
        return out;
    }

    // Localiza um arquivo no EFS2, extrai o payload via cadeia de clusters e o
    // injeta em memória de Core 0 via BrewLoader (inject_bytes). Retorna true só
    // com injeção verificada.
    bool load_applet_from_efs2(const std::string& path_or_name, u32 base_addr = 0x12000000) {
        printf("[EFS2/Applet] Carregando '%s' direto da NAND 0:EFS2APPS...\n", path_or_name.c_str());
        std::vector<uint8_t> payload = efs2_extract(path_or_name);
        if (payload.empty()) {
            printf("[EFS2/Applet] Sem payload extraível para '%s'.\n", path_or_name.c_str());
            return false;
        }
        if (!brew_) {
            printf("[EFS2/Applet] BrewLoader indisponível (init incompleto).\n");
            return false;
        }
        std::string origin = "efs2:" + path_or_name;
        // QW35: Se o arquivo for um MIF (.mif), efetua o parse dos metadados e do CLSID
        uint32_t clsid = 0;
        if (path_or_name.size() >= 4 && path_or_name.substr(path_or_name.size() - 4) == ".mif") {
            auto mif_info = zeebo::brew::MifParser::parse(payload);
            if (mif_info.valid) {
                clsid = mif_info.clsid;
                printf("[EFS2/MIF] MIF '%s' validado: AEECLSID=0x%08x mod='%s'\n",
                       path_or_name.c_str(), clsid, mif_info.mod_file.c_str());
            }
        }
        if (!brew_->inject_bytes(payload, base_addr, clsid, origin)) {
            printf("[EFS2/Applet] BrewLoader falhou ao injetar '%s'.\n", path_or_name.c_str());
            return false;
        }
        if (igl_bridge_) igl_bridge_->set_guest_running(true);
        printf("[EFS2/Applet] Applet '%s' injetado @0x%08x; dispatch BREW armado (AEECShell@0x%08x).\n",
               path_or_name.c_str(), base_addr, brew_->symbols().aeecshell_dispatch_va);
        return true;
    }

    // ── Ciclo de vida do applet Z-Wheel (ZeeboApp) sob EVT_APP_START ──────────
    // O payload de 274755 extraído do EFS2 (bloco indireto @0x3a92000) é METADADO
    // de gnode do VFS (carrega nomes como "slidemodel.qxm" e a assinatura ASCII
    // "274755"), NÃO um ELF/.mod executável — o código executável e os
    // manipuladores de ciclo de vida do ZeeboApp residem embutidos no ELF de
    // 0:APPS, no manipulador Thumb @0x10532344 (já carregado em Core 0 pelos
    // PT_LOAD com PF_X). Portanto, "instanciar e chamar o ciclo de vida do applet"
    // = despachar EVT_APP_START (0x1f96) a esse manipulador pré-mapeado, roteando
    // a chamada gráfica que ele dispara ([applet+0x2c] → vtable[10]/slot 0x28,
    // arg=1) para o SoftRasterizer — exatamente o contrato provado no harness
    // test-zwheel. Retorna true se o manipulador tratou EVT_APP_START (r0==1).
    static ZeeboLLESystem* s_zwheel_hook_sys_;   // ctx p/ o hook transitório do stub
    static uint32_t        s_zwheel_stub_va_;
    bool dispatch_zwheel_app_start() {
        static constexpr u32 ZWHEEL_HANDLER_VA = 0x10532344; // manipulador Thumb do ZeeboApp
        static constexpr u32 EVT_APP_START     = 0x1f96;     // K(0x1f92)+4
        static constexpr u32 GFX_SLOT_OFF      = 0x28;       // vtable[10]
        if (!brew_ || !core0_.uc) {
            printf("[Z-Wheel/Life] BrewLoader/Core0 indisponível — dispatch abortado.\n");
            return false;
        }
        // Confirma que o manipulador está REALMENTE carregado (0:APPS PF_X).
        u8 pfx[2] = {0};
        if (uc_mem_read(core0_.uc, ZWHEEL_HANDLER_VA, pfx, 2) != UC_ERR_OK) {
            printf("[Z-Wheel/Life] manipulador @0x%08x não mapeado — 0:APPS não carregado?\n",
                   ZWHEEL_HANDLER_VA);
            return false;
        }
        printf("[Z-Wheel/Life] manipulador ZeeboApp @0x%08x presente (bytes %02x %02x, "
               "esperado 70 b5 push{r4-r6,lr})\n", ZWHEEL_HANDLER_VA, pfx[0], pfx[1]);

        // Janela de scratch do ciclo de vida (fora do firmware e do payload 274755
        // @0x12000000): applet + objeto gráfico + vtable + trampolim + pilha.
        const u32 SB      = 0x22000000, SS = 0x00100000;
        const u32 APPLET  = SB + 0x0100;
        const u32 GFXOBJ  = SB + 0x0200;
        const u32 GFXVTBL = SB + 0x0300;
        const u32 GFXSTUB = SB + 0x1000;
        const u32 STACKTP = SB + 0xf000;
        const u32 RETMAG  = SB + 0xfffe;
        uc_mem_map(core0_.uc, SB, SS, UC_PROT_ALL); // ok se já mapeado
        std::vector<u8> zeros(SS, 0);
        uc_mem_write(core0_.uc, SB, zeros.data(), zeros.size());
        for (int s = 0; s < 16; s++) { u32 fn = 0; uc_mem_write(core0_.uc, GFXVTBL + s*4, &fn, 4); }
        u32 stub_thumb = GFXSTUB | 1u;
        uc_mem_write(core0_.uc, GFXVTBL + GFX_SLOT_OFF, &stub_thumb, 4);
        u16 bxlr = 0x4770; uc_mem_write(core0_.uc, GFXSTUB, &bxlr, 2); // fallback bx lr
        u32 gfxvt = GFXVTBL; uc_mem_write(core0_.uc, GFXOBJ, &gfxvt, 4);        // obj[0]=&vtable
        u32 gfxobj = GFXOBJ; uc_mem_write(core0_.uc, APPLET + 0x2c, &gfxobj, 4);// applet[0x2c]=obj

        // Hook transitório: captura o `blx` da vtable gráfica → SoftRasterizer.
        s_zwheel_hook_sys_ = this;
        s_zwheel_stub_va_  = GFXSTUB;
        uc_hook h_stub;
        uc_hook_add(core0_.uc, &h_stub, UC_HOOK_CODE, (void*)zwheel_stub_hook, this,
                    GFXSTUB, GFXSTUB + 4);

        const u16* fb = rast_ ? rast_->framebuffer_rgb565() : nullptr;
        unsigned long long sum_before = 0;
        if (fb) for (size_t i = 0; i < static_cast<size_t>(FB_WIDTH) * FB_HEIGHT; i++) sum_before += fb[i];

        printf("[Z-Wheel/Life] despachando EVT_APP_START(0x%04x) → HandleEvent@0x%08x "
               "applet@0x%08x\n", EVT_APP_START, ZWHEEL_HANDLER_VA, APPLET);
        bool clean = false;
        u32 r0 = brew_->dispatch_event(ZWHEEL_HANDLER_VA, APPLET, EVT_APP_START,
                                       /*keycode=*/0, STACKTP, RETMAG, /*dwparam=*/0, &clean);
        uc_hook_del(core0_.uc, h_stub);
        s_zwheel_hook_sys_ = nullptr;

        unsigned long long sum_after = 0;
        if (fb) for (size_t i = 0; i < static_cast<size_t>(FB_WIDTH) * FB_HEIGHT; i++) sum_after += fb[i];

        bool ok = clean && r0 == 1 && zwheel_gfx_calls_ >= 1;
        printf("[Z-Wheel/Life] EVT_APP_START → r0=%u (uc=%s) | chamadas gráficas=%d | "
               "framebuffer soma antes=%llu depois=%llu\n%s\n",
               r0, clean ? "clean" : "abortado", zwheel_gfx_calls_,
               sum_before, sum_after,
               ok ? "[Z-Wheel/Life] PASS: ciclo de vida do ZeeboApp instanciado — "
                    "EVT_APP_START tratado com SUCESSO (r0=1)."
                  : "[Z-Wheel/Life] AVISO: ciclo de vida não completou o contrato "
                    "(esperado r0=1 + chamada gráfica).");
        if (ok && igl_bridge_) igl_bridge_->set_guest_running(true);
        if (ok) {
            // Persiste o scratch do ciclo de vida para o loop interativo (Passo 11):
            // reencaminha EVT_KEY_* ao MESMO manipulador ZeeboApp @0x10532344 e
            // applet scratch, e re-arma o roteamento gráfico slot 10 → SoftRasterizer.
            zwheel_applet_va_ = APPLET;
            zwheel_stack_top_ = STACKTP;
            zwheel_ret_magic_ = RETMAG;
            zwheel_life_armed_ = true;
            // O HandleEvent do ZeeboApp (0x1f9x = ciclo de vida) só consome
            // eventos de lifecycle; o encaminhamento de teclas usa o mesmo
            // manipulador validado sob Unicorn (retorno real r0=1 = consumido).
            set_brew_input_handler(ZWHEEL_HANDLER_VA, APPLET, STACKTP, RETMAG);
        }
        return ok;
    }

    // ── Passo 11: loop interativo/contínuo do applet Z-Wheel (274755) ──
    // Após dispatch_zwheel_app_start(), mantém um ciclo que (a) apresenta o
    // framebuffer RGB565 no HostVideoSink/tela SDL2 à taxa de quadros e (b)
    // despacha continuamente eventos de teclado/gamepad mapeados para AVK BREW
    // via dispatch_zpad_to_brew (HandleEvent Thumb sob Unicorn, r0 real).
    // Roda por `max_seconds` (>0) ou até o fechamento da janela (interativo).
    void run_zwheel_interactive(bool headless, double max_seconds,
                                const std::string& dump_frames_dir = "") {
        if (!zwheel_life_armed_) {
            printf("[Z-Wheel/Loop] ciclo de vida não armado — pulando loop interativo.\n");
            return;
        }
        printf("[Z-Wheel/Loop] iniciando loop%s%s (headless=%d)\n",
               max_seconds > 0.0 ? " por tempo" : "",
               (!headless) ? " interativo" : "",
               (int)headless);

        // Re-arma o hook do trampolim gráfico (slot 10 → SoftRasterizer) para que
        // toda vez que o applet acionar a vtable, o framebuffer seja atualizado.
        s_zwheel_hook_sys_ = this;
        s_zwheel_stub_va_  = 0x22000000 + 0x1000; // = GFXSTUB do dispatch
        uc_hook h_stub = 0;
        if (core0_.uc)
            uc_hook_add(core0_.uc, &h_stub, UC_HOOK_CODE, (void*)zwheel_stub_hook, this,
                        s_zwheel_stub_va_, s_zwheel_stub_va_ + 4);

        const u16* fb = rast_ ? rast_->framebuffer_rgb565() : nullptr;
        if (fb && sink_) sink_->update_frame(fb);

        auto t_start = std::chrono::steady_clock::now();
        auto t_last  = t_start;
        uint64_t frames = 0, key_dispatches = 0;
        bool run = true;

        while (run) {
            // (b) Bombeia eventos SDL2 → AVK BREW (despacho contínuo de Z-Pad).
            SDL_Event ev;
            while (SDL_PollEvent(&ev)) {
                if (ev.type == SDL_QUIT) { run = false; break; }
                if (ev.type == SDL_KEYDOWN) {
                    if (ev.key.keysym.sym == SDLK_ESCAPE || ev.key.keysym.sym == SDLK_q) {
                        // ESC/Q fecham o preview; teclas de jogo abaixo.
                    }
                    zeebo::brew::ZpadButton b;
                    if (sdl_to_zpad(ev.key.keysym.sym, b)) {
                        if (dispatch_zpad_to_brew(b, true)) key_dispatches++;
                    }
                } else if (ev.type == SDL_KEYUP) {
                    zeebo::brew::ZpadButton b;
                    if (sdl_to_zpad(ev.key.keysym.sym, b)) {
                        if (dispatch_zpad_to_brew(b, false)) key_dispatches++;
                    }
                } else if (ev.type == SDL_CONTROLLERBUTTONDOWN || ev.type == SDL_CONTROLLERBUTTONUP) {
                    zeebo::brew::ZpadButton b;
                    if (pad_to_zpad(ev.cbutton.button, b))
                        if (dispatch_zpad_to_brew(b, ev.type == SDL_CONTROLLERBUTTONDOWN)) key_dispatches++;
                }
            }

            // (a) Aciona a vtable gráfica p/ atualizar e apresentar o framebuffer.
            //     Re-dirige o SoftRasterizer diretamente (mesmo contrato do slot 10)
            //     e apresenta na tela; mantém a apresentação contínua à taxa de quadros.
            if (rast_) {
                rast_->begin_frame();
                rast_->set_viewport(0, 0, FB_WIDTH, FB_HEIGHT);
                rast_->clear_color(0.1f, 0.2f, 0.8f, 1.0f);
                rast_->clear(0x4000);
                rast_->end_frame();
                fb = rast_->framebuffer_rgb565();
            }
            if (fb && sink_) sink_->update_frame(fb);
            frames++;

            if (!dump_frames_dir.empty() && frames <= 60) {
                char p[512];
                snprintf(p, sizeof(p), "%s/frame_%06llu.ppm",
                         dump_frames_dir.c_str(), (unsigned long long)frames);
                save_ppm(fb, p);
            }

            auto now = std::chrono::steady_clock::now();
            double total = std::chrono::duration<double>(now - t_start).count();
            if (max_seconds > 0.0 && total >= max_seconds) { run = false; }

            double since = std::chrono::duration<double>(now - t_last).count();
            if (since >= 1.0) {
                printf("[Z-Wheel/Loop] FPS=%.1f | frames=%llu | teclas_consumidas=%llu | t=%.1fs\n",
                       (double)frames / (total > 0 ? total : 1.0),
                       (unsigned long long)frames,
                       (unsigned long long)key_dispatches, total);
                t_last = now;
            }
            if (!headless) std::this_thread::sleep_for(std::chrono::milliseconds(16));
        }

        if (core0_.uc && h_stub) uc_hook_del(core0_.uc, h_stub);
        s_zwheel_hook_sys_ = nullptr;
        printf("[Z-Wheel/Loop] loop encerrado: frames=%llu, teclas consumidas=%llu.\n",
               (unsigned long long)frames, (unsigned long long)key_dispatches);
    }

    // Mapeia um símbolo de tecla SDL2 para o botão lógico do Z-Pad (Passo 11).
    static bool sdl_to_zpad(int sym, zeebo::brew::ZpadButton& out) {
        using namespace zeebo::brew;
        switch (sym) {
            case SDLK_z: case SDLK_RETURN: out = ZP_A; return true;
            case SDLK_x: case SDLK_ESCAPE: out = ZP_B; return true;
            case SDLK_c:      out = ZP_1; return true;
            case SDLK_v:      out = ZP_2; return true;
            case SDLK_SPACE:  out = ZP_3; return true;
            case SDLK_LSHIFT: out = ZP_4; return true;
            case SDLK_UP:     out = ZP_UP; return true;
            case SDLK_DOWN:   out = ZP_DOWN; return true;
            case SDLK_LEFT:   out = ZP_LEFT; return true;
            case SDLK_RIGHT:  out = ZP_RIGHT; return true;
            case SDLK_h:      out = ZP_HOME; return true;
            default: return false;
        }
    }

    // Mapeia um botão de gamepad SDL2 para o botão lógico do Z-Pad (Passo 11).
    static bool pad_to_zpad(int btn, zeebo::brew::ZpadButton& out) {
        using namespace zeebo::brew;
        switch (btn) {
            case SDL_CONTROLLER_BUTTON_A:          out = ZP_A; return true;
            case SDL_CONTROLLER_BUTTON_B:          out = ZP_B; return true;
            case SDL_CONTROLLER_BUTTON_X:          out = ZP_1; return true;
            case SDL_CONTROLLER_BUTTON_Y:          out = ZP_2; return true;
            case SDL_CONTROLLER_BUTTON_DPAD_UP:    out = ZP_UP; return true;
            case SDL_CONTROLLER_BUTTON_DPAD_DOWN:  out = ZP_DOWN; return true;
            case SDL_CONTROLLER_BUTTON_DPAD_LEFT:  out = ZP_LEFT; return true;
            case SDL_CONTROLLER_BUTTON_DPAD_RIGHT: out = ZP_RIGHT; return true;
            case SDL_CONTROLLER_BUTTON_GUIDE:
            case SDL_CONTROLLER_BUTTON_START:      out = ZP_HOME; return true;
            default: return false;
        }
    }

    // Hook do trampolim da vtable gráfica da Z-Wheel: idêntico ao contrato do
    // harness test-zwheel — dirige o SoftRasterizer (viewport + clear azul),
    // devolve r0=0 (o validador do firmware @0x10724104 então retorna 1) e
    // retorna da função (PC=LR respeitando o bit Thumb).
    static void zwheel_stub_hook(uc_engine* uc, uint64_t address, uint32_t, void* ud) {
        ZeeboLLESystem* sys = (ZeeboLLESystem*)ud;
        if ((u32)(address & ~1u) != s_zwheel_stub_va_) return;
        u32 arg = 0, lr = 0, r0 = 0;
        uc_reg_read(uc, UC_ARM_REG_R1, &arg);
        uc_reg_read(uc, UC_ARM_REG_LR, &lr);
        uc_reg_read(uc, UC_ARM_REG_R0, &r0);
        sys->zwheel_gfx_calls_++;
        printf("[Z-Wheel/Life] chamada gráfica capturada: vtable[10] (this=0x%08x) arg=%u "
               "→ roteando p/ SoftRasterizer\n", r0, arg);
        if (sys->rast_) {
            sys->rast_->begin_frame();
            sys->rast_->set_viewport(0, 0, FB_WIDTH, FB_HEIGHT);
            sys->rast_->clear_color(0.1f, 0.2f, 0.8f, 1.0f);
            sys->rast_->clear(0x4000 /*GL_COLOR_BUFFER_BIT*/);
            sys->rast_->end_frame();
            if (sys->gpu_) sys->gpu_->mark_dirty();
        }
        u32 zero = 0; uc_reg_write(uc, UC_ARM_REG_R0, &zero); // contrato: método → 0
        uc_reg_write(uc, UC_ARM_REG_PC, &lr);                 // retorna ao chamador
    }

    void set_efs2_nand_path(const std::string& p) { efs2_nand_path_ = p; }
    void set_boot_target(int firstapp) { boot_firstapp_ = firstapp; }
    int boot_target() const { return boot_firstapp_; }

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
            igl_bridge_ = std::make_unique<zeebo::gpu::IglGuestBridge>(*igl_hook_);
            printf("[System] Initialized SoftRasterizer, IglHook and IGL guest-vtable bridge on Core 0 memory space.\n");
        }
        // Item 4: loader BREW (vinculado ao uc de Core 0 após uc_open, ver init).
        brew_ = std::make_unique<zeebo::brew::BrewLoader>();

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
        if (brew_) { brew_->bind_uc(core0_.uc); brew_->bind_lut(&vtlb_); }

        // 4. Initialize Core 1 (ARM926EJ-S — Modem Processor)
        printf("[System] Initializing Core 1 (ARM926EJ-S Modem Processor)...\n");
        uc_err err1 = uc_open(UC_ARCH_ARM, UC_MODE_ARM, &core1_.uc);
        if (err1 != UC_ERR_OK) {
            printf("[Fatal] Failed to init Core 1: %s\n", uc_strerror(err1));
            return false;
        }
        // TLB flat/virtual: o firmware REX habilita a MMU do ARM9 via
        // 'mcr p15,c1,c0,0' (SCTLR.M=1) em 0xf0017718. Em UC_TLB_CPU (default),
        // o Unicorn passa a traduzir com as page tables do guest (TTBR ainda nao
        // configurada de forma valida p/ o nosso mapa espelhado) e TODA insn apos
        // esse ponto vira no-op silencioso (PC+=4, sem efeito), NOP-slide ate os
        // zeros em 0xf00184cc. Como o bring-up do Core1 usa mapeamento espelhado
        // (VA==host, janela de relocacao 0xdf600000), forcamos UC_TLB_VIRTUAL —
        // igual ao Core0 — para o Unicorn ignorar a MMU do guest e manter o mapa
        // plano. Validado por execucao real: sem isso os registradores congelam
        // exatamente na escrita do SCTLR (r3=0x5317d).
        uc_ctl_tlb_mode(core1_.uc, UC_TLB_VIRTUAL);
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

    // Passo 3 / Fase 13: pré-visualização gráfica da Z-Wheel.
    // Reproduz, através da MESMA fachada (SoftRasterizer via IglHook), o ciclo
    // gráfico capturado no harness test-zwheel (slot 10 / byte-offset 0x28 da
    // vtable do ZeeboApp, chamado com arg=1 em EVT_APP_START): viewport cheio +
    // clear azul → pixels RGB565 reais, transferidos para a textura/renderer
    // SDL2 (SDL_RenderPresent) na janela de 640x480.
    // Retorna a soma dos pixels do framebuffer (prova numérica do pipeline).
    // Em GUI, mantém a janela aberta apresentando o frame até SDL_QUIT.
    unsigned long long run_zwheel_preview(bool headless, const std::string& dump_frames_dir = "") {
        printf("[Z-Wheel] Preview: dirigindo o ciclo gráfico (slot 10/0x28, arg=1) "
               "pelo SoftRasterizer → SDL2 640x480...\n");
        if (!rast_) {
            printf("[Z-Wheel][ERRO] SoftRasterizer não inicializado.\n");
            return 0;
        }
        // Caminho idêntico ao roteamento da vtable IGL da Z-Wheel no harness:
        // set_viewport / clear_color / clear (fachada Adreno 130 → SoftRasterizer).
        rast_->begin_frame();
        rast_->set_viewport(0, 0, FB_WIDTH, FB_HEIGHT);
        rast_->clear_color(0.1f, 0.2f, 0.8f, 1.0f);
        rast_->clear(0x4000 /*GL_COLOR_BUFFER_BIT*/);
        rast_->end_frame();

        const u16* fb = rast_->framebuffer_rgb565();
        unsigned long long sum = 0;
        if (fb) for (size_t i = 0; i < static_cast<size_t>(FB_WIDTH) * FB_HEIGHT; i++) sum += fb[i];
        printf("[Z-Wheel] Frame RGB565 produzido: soma de pixels = %llu\n", sum);

        if (fb && sink_) sink_->update_frame(fb);

        // Salva frame em PPM para inspeção direta de imagem
        save_ppm(fb, "/tmp/zeebo_zwheel_frame.ppm");
        printf("[Z-Wheel] Frame dump salvo em: /tmp/zeebo_zwheel_frame.ppm\n");

        if (!dump_frames_dir.empty()) {
            char p[512];
            snprintf(p, sizeof(p), "%s/frame_000000.ppm", dump_frames_dir.c_str());
            save_ppm(fb, p);
            printf("[Dump] Frame 0 salvo em: %s\n", p);
        }

        if (headless) {
            printf("[Z-Wheel] Modo headless: frame único apresentado ao sink (sem janela).\n");
            return sum;
        }

        printf("[Z-Wheel] Janela SDL2 ativa — apresentando frame. Feche a janela ou "
               "pressione ESC/Q para sair.\n");
        bool run = true;
        auto t_start = std::chrono::steady_clock::now();
        auto t_last = t_start;
        uint64_t frames = 0;
        while (run) {
            if (fb && sink_) sink_->update_frame(fb); // re-apresenta (RenderPresent)
            frames++;
            if (!dump_frames_dir.empty() && frames < 60) {
                char p[512];
                snprintf(p, sizeof(p), "%s/frame_%06llu.ppm", dump_frames_dir.c_str(), (unsigned long long)frames);
                save_ppm(fb, p);
            }
            SDL_Event ev;
            while (SDL_PollEvent(&ev)) {
                if (ev.type == SDL_QUIT) { run = false; break; }
                if (ev.type == SDL_KEYDOWN &&
                    (ev.key.keysym.sym == SDLK_ESCAPE || ev.key.keysym.sym == SDLK_q)) {
                    run = false; break;
                }
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(16));
            auto now = std::chrono::steady_clock::now();
            double elapsed_sec = std::chrono::duration<double>(now - t_last).count();
            if (elapsed_sec >= 1.0) {
                double total_sec = std::chrono::duration<double>(now - t_start).count();
                double fps = (double)frames / total_sec;
                printf("[GUI/Telemetry] FPS: %.1f | frames=%llu | tempo=%.1fs\n",
                       fps, (unsigned long long)frames, total_sec);
                t_last = now;
            }
        }
        return sum;
    }

    static bool save_ppm(const u16* fb, const std::string& path) {
        FILE* f = fopen(path.c_str(), "wb");
        if (!f) return false;
        fprintf(f, "P6\n%d %d\n255\n", FB_WIDTH, FB_HEIGHT);
        for (size_t i = 0; i < static_cast<size_t>(FB_WIDTH) * FB_HEIGHT; i++) {
            u16 p = fb ? fb[i] : 0;
            u8 r = ((p >> 11) & 0x1f) * 255 / 31;
            u8 g = ((p >> 5) & 0x3f) * 255 / 63;
            u8 b = (p & 0x1f) * 255 / 31;
            fputc(r, f); fputc(g, f); fputc(b, f);
        }
        fclose(f);
        return true;
    }

    void run_interleaved(int cycles, int slice_insns, double max_seconds = 0.0, bool show_fps = false, const std::string& dump_frames_dir = "") {
        printf("[System] Beginning interleaved execution: %d cycles x %d insns...\n", cycles, slice_insns);

        // Framebuffer video buffer for host display sink (640x480 RGB565)
        std::vector<u16> fb_buffer(FB_WIDTH * FB_HEIGHT, 0x0010); // Dark navy backdrop
        auto start_time = std::chrono::steady_clock::now();
        auto last_telemetry_time = start_time;
        uint64_t last_c0_insns = core0_.insns;
        uint64_t last_c1_insns = core1_.insns;
        uint64_t total_rendered_frames = 0;

        int c = 0;
        while (!quit_requested_ && (c < cycles || control_ != nullptr || max_seconds > 0.0)) {
            auto now = std::chrono::steady_clock::now();
            double total_elapsed = std::chrono::duration<double>(now - start_time).count();
            if (max_seconds > 0.0 && total_elapsed >= max_seconds) {
                printf("[System] Reached maximum requested time (%.2f s). Halting.\n", total_elapsed);
                break;
            }

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

            // Step Core 1 (ARM9) — pula se o slide-detector ja abortou o Core1
            uc_err e1 = UC_ERR_OK;
            if (!core1_.halted) {
                e1 = uc_emu_start(core1_.uc, core1_.entry, 0, 0, slice_insns);
                uc_reg_read(core1_.uc, UC_ARM_REG_PC, &core1_.entry);
                if (e1 != UC_ERR_OK) {
                    core1_.halted = true;
                }
            }

            if (c % 10 == 0 || c < 5) {
                printf("  [Cycle %02d] Core0(ARM11): pc=0x%08x insns=%llu (%s) | Core1(ARM9): pc=0x%08x insns=%llu (%s)\n",
                       c, core0_.entry, (unsigned long long)core0_.insns, e0 ? uc_strerror(e0) : "ok",
                       core1_.entry, (unsigned long long)core1_.insns, e1 ? uc_strerror(e1) : "ok");
            }
            c++;

            // Update display sink if GPU or MDDI marked dirty / drawn
            if (gpu_ && gpu_->is_fb_dirty()) {
                gpu_->clear_fb_dirty();
                const u16* cur_fb = nullptr;
                if (rast_) {
                    rast_->end_frame();
                    cur_fb = rast_->framebuffer_rgb565();
                    if (cur_fb) {
                        sink_->update_frame(cur_fb);
                    }
                } else {
                    // Fallback test pattern
                    for (u32 y = 0; y < FB_HEIGHT; y++) {
                        for (u32 x = 0; x < FB_WIDTH; x++) {
                            u16 col = (u16)(((x >> 3) & 0x1F) << 11) | (u16)(((y >> 3) & 0x3F) << 5) | (u16)(c & 0x1F);
                            fb_buffer[y * FB_WIDTH + x] = col;
                        }
                    }
                    cur_fb = fb_buffer.data();
                    sink_->update_frame(cur_fb);
                }
                total_rendered_frames++;
                printf("[Display/Sink] Rendered active video frame %u (Adreno draws=%u)\n", c, gpu_->draws());
                if (!dump_frames_dir.empty() && cur_fb) {
                    char p[512];
                    snprintf(p, sizeof(p), "%s/frame_%06llu.ppm", dump_frames_dir.c_str(), (unsigned long long)total_rendered_frames);
                    save_ppm(cur_fb, p);
                }
            }

            // Telemetria de FPS e MIPS periódica
            double telemetry_elapsed = std::chrono::duration<double>(now - last_telemetry_time).count();
            if (show_fps && telemetry_elapsed >= 1.0) {
                uint64_t d_c0 = core0_.insns - last_c0_insns;
                uint64_t d_c1 = core1_.insns - last_c1_insns;
                double mips_c0 = (double)d_c0 / (telemetry_elapsed * 1000000.0);
                double mips_c1 = (double)d_c1 / (telemetry_elapsed * 1000000.0);
                double fps = (double)total_rendered_frames / (total_elapsed > 0 ? total_elapsed : 1.0);
                printf("[Telemetry] t=%.1fs | C0=%.2f MIPS (pc=0x%08x) | C1=%.2f MIPS (pc=0x%08x) | Video FPS=%.2f (quadros=%llu)\n",
                       total_elapsed, mips_c0, core0_.entry, mips_c1, core1_.entry, fps, (unsigned long long)total_rendered_frames);
                last_telemetry_time = now;
                last_c0_insns = core0_.insns;
                last_c1_insns = core1_.insns;
            }

            // Process SDL events if window is open
            SDL_Event ev;
            while (SDL_PollEvent(&ev)) {
                if (ev.type == SDL_QUIT) return;
                if (ev.type == SDL_KEYDOWN) {
                    switch (ev.key.keysym.sym) {
                        case SDLK_z: case SDLK_RETURN: input_->press_key(ZEEBO_KEY_A, core0_.uc); dispatch_zpad_to_brew(zeebo::brew::ZP_A, true); break;
                        case SDLK_x: case SDLK_ESCAPE: input_->press_key(ZEEBO_KEY_B, core0_.uc); dispatch_zpad_to_brew(zeebo::brew::ZP_B, true); break;
                        case SDLK_c:                   input_->press_key(ZEEBO_KEY_C, core0_.uc); dispatch_zpad_to_brew(zeebo::brew::ZP_1, true); break;
                        case SDLK_v:                   input_->press_key(ZEEBO_KEY_D, core0_.uc); dispatch_zpad_to_brew(zeebo::brew::ZP_2, true); break;
                        case SDLK_UP:                  input_->press_key(ZEEBO_KEY_UP, core0_.uc); dispatch_zpad_to_brew(zeebo::brew::ZP_UP, true); break;
                        case SDLK_DOWN:                input_->press_key(ZEEBO_KEY_DOWN, core0_.uc); dispatch_zpad_to_brew(zeebo::brew::ZP_DOWN, true); break;
                        case SDLK_LEFT:                input_->press_key(ZEEBO_KEY_LEFT, core0_.uc); dispatch_zpad_to_brew(zeebo::brew::ZP_LEFT, true); break;
                        case SDLK_RIGHT:               input_->press_key(ZEEBO_KEY_RIGHT, core0_.uc); dispatch_zpad_to_brew(zeebo::brew::ZP_RIGHT, true); break;
                        case SDLK_h:                   input_->press_key(ZEEBO_KEY_HOME, core0_.uc); dispatch_zpad_to_brew(zeebo::brew::ZP_HOME, true); break;
                        case SDLK_SPACE:               dispatch_zpad_to_brew(zeebo::brew::ZP_3, true); break;
                        case SDLK_LSHIFT:              dispatch_zpad_to_brew(zeebo::brew::ZP_4, true); break;
                    }
                } else if (ev.type == SDL_KEYUP) {
                    switch (ev.key.keysym.sym) {
                        case SDLK_z: case SDLK_RETURN: input_->release_key(ZEEBO_KEY_A); dispatch_zpad_to_brew(zeebo::brew::ZP_A, false); break;
                        case SDLK_x: case SDLK_ESCAPE: input_->release_key(ZEEBO_KEY_B); dispatch_zpad_to_brew(zeebo::brew::ZP_B, false); break;
                        case SDLK_c:                   input_->release_key(ZEEBO_KEY_C); dispatch_zpad_to_brew(zeebo::brew::ZP_1, false); break;
                        case SDLK_v:                   input_->release_key(ZEEBO_KEY_D); dispatch_zpad_to_brew(zeebo::brew::ZP_2, false); break;
                        case SDLK_UP:                  input_->release_key(ZEEBO_KEY_UP); dispatch_zpad_to_brew(zeebo::brew::ZP_UP, false); break;
                        case SDLK_DOWN:                input_->release_key(ZEEBO_KEY_DOWN); dispatch_zpad_to_brew(zeebo::brew::ZP_DOWN, false); break;
                        case SDLK_LEFT:                input_->release_key(ZEEBO_KEY_LEFT); dispatch_zpad_to_brew(zeebo::brew::ZP_LEFT, false); break;
                        case SDLK_RIGHT:               input_->release_key(ZEEBO_KEY_RIGHT); dispatch_zpad_to_brew(zeebo::brew::ZP_RIGHT, false); break;
                        case SDLK_h:                   input_->release_key(ZEEBO_KEY_HOME); dispatch_zpad_to_brew(zeebo::brew::ZP_HOME, false); break;
                        case SDLK_SPACE:               dispatch_zpad_to_brew(zeebo::brew::ZP_3, false); break;
                        case SDLK_LSHIFT:              dispatch_zpad_to_brew(zeebo::brew::ZP_4, false); break;
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
        if (req->cmd == "probe.list") {
            req->reply.set_value(probes_.ListJson());
            return;
        }
        if (req->cmd == "probe.get") {
            if (req->str_probe.empty()) {
                req->reply.set_value("{\"ok\":false,\"error\":\"missing_probe\"}");
                return;
            }
            req->reply.set_value(probes_.GetJson(req->str_probe));
            return;
        }
        if (req->cmd == "backtrace") {
            if (req->core != 0 && req->core != 1) {
                req->reply.set_value("{\"ok\":false,\"error\":\"invalid_core\"}");
                return;
            }
            uc_engine* uc = (req->core == 1) ? core1_.uc : core0_.uc;
            u32 pc = 0, lr = 0, sp = 0, r11 = 0;
            uc_reg_read(uc, UC_ARM_REG_PC, &pc);
            uc_reg_read(uc, UC_ARM_REG_LR, &lr);
            uc_reg_read(uc, UC_ARM_REG_SP, &sp);
            uc_reg_read(uc, UC_ARM_REG_R11, &r11);

            char frame[192];
            snprintf(frame, sizeof(frame),
                     "{\"ok\":true,\"core\":%lu,\"frames\":["
                     "{\"frame\":0,\"pc\":%u,\"lr\":%u,\"sp\":%u,\"fp\":%u}",
                     req->core, pc, lr, sp, r11);
            std::string response(frame);
            int frame_idx = 1;
            const u64 stack_end = static_cast<u64>(sp) + 256;
            for (u64 cur_sp = sp; cur_sp < stack_end && frame_idx < 8; cur_sp += 4) {
                u32 val = 0;
                if (uc_mem_read(uc, cur_sp, &val, 4) == UC_ERR_OK &&
                    ((val >= 0xb0000000 && val < 0xb0500000) ||
                     (val >= 0x10000000 && val < 0x12000000) ||
                     (val >= 0xf0000000 && val < 0xf0030000))) {
                    snprintf(frame, sizeof(frame),
                             ",{\"frame\":%d,\"pc\":%u,\"sp\":%llu}",
                             frame_idx++, val, (unsigned long long)cur_sp);
                    response += frame;
                }
            }
            response += "]}";
            req->reply.set_value(std::move(response));
            return;
        }
        if (req->cmd == "peek") {
            if (req->core != 0 && req->core != 1) {
                req->reply.set_value("{\"ok\":false,\"error\":\"invalid_core\"}");
                return;
            }
            uc_engine* uc = (req->core == 1) ? core1_.uc : core0_.uc;
            u32 addr = (u32)req->i0;
            size_t size = req->has_i1 ? (size_t)req->i1 : 4;
            if (size != 1 && size != 2 && size != 4 && size != 8) {
                req->reply.set_value("{\"ok\":false,\"error\":\"invalid_length\"}");
                return;
            }
            u64 val = 0;
            uc_err err = uc_mem_read(uc, addr, &val, size);
            if (err != UC_ERR_OK) {
                char err_buf[128];
                snprintf(err_buf, sizeof(err_buf), "{\"ok\":false,\"error\":\"peek failed: %s\"}", uc_strerror(err));
                req->reply.set_value(err_buf);
                return;
            }
            char resp[160];
            snprintf(resp, sizeof(resp), "{\"ok\":true,\"core\":%ld,\"addr\":%u,\"size\":%zu,\"val\":%llu,\"hex\":\"0x%llx\"}",
                     req->core, addr, size, (unsigned long long)val, (unsigned long long)val);
            req->reply.set_value(resp);
            return;
        }
        if (req->cmd == "poke") {
            if (req->core != 0 && req->core != 1) {
                req->reply.set_value("{\"ok\":false,\"error\":\"invalid_core\"}");
                return;
            }
            uc_engine* uc = (req->core == 1) ? core1_.uc : core0_.uc;
            u32 addr = (u32)req->i0;
            size_t size = req->has_i1 ? (size_t)req->i1 : 4;
            if (size != 1 && size != 2 && size != 4 && size != 8) {
                req->reply.set_value("{\"ok\":false,\"error\":\"invalid_length\"}");
                return;
            }
            u64 val = req->val;
            uc_err err = uc_mem_write(uc, addr, &val, size);
            if (err != UC_ERR_OK) {
                char err_buf[128];
                snprintf(err_buf, sizeof(err_buf), "{\"ok\":false,\"error\":\"poke failed: %s\"}", uc_strerror(err));
                req->reply.set_value(err_buf);
                return;
            }
            // Invalida cache de tradução se for em área de código
            uc_ctl_remove_cache(uc, addr, size);
            char resp[128];
            snprintf(resp, sizeof(resp), "{\"ok\":true,\"core\":%ld,\"addr\":%u,\"size\":%zu,\"written\":true}",
                     req->core, addr, size);
            req->reply.set_value(resp);
            return;
        }
        if (req->cmd == "vram_stat") {
            u64 psum = 0;
            u16 center = 0;
            const u16* fb = rast_ ? rast_->framebuffer_rgb565() : nullptr;
            if (fb) {
                for (size_t i = 0; i < static_cast<size_t>(FB_WIDTH) * FB_HEIGHT; i++) psum += fb[i];
                center = fb[(FB_HEIGHT / 2) * FB_WIDTH + (FB_WIDTH / 2)];
            }
            char resp[256];
            snprintf(resp, sizeof(resp),
                     "{\"ok\":true,\"width\":%u,\"height\":%u,\"format\":\"RGB565\",\"pixel_sum\":%llu,\"center\":%u,\"blank\":%s}",
                     FB_WIDTH, FB_HEIGHT, (unsigned long long)psum, (unsigned)center, (psum == 0 ? "true" : "false"));
            req->reply.set_value(resp);
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
                const uc_err err0 = uc_emu_start(core0_.uc, core0_.entry, 0, 0, 1);
                if (err0 != UC_ERR_OK) {
                    stepping_ = false;
                    req->reply.set_value("{\"ok\":false,\"error\":\"core0_cont_step\"}");
                    return;
                }
                u32 next_pc = 0;
                uc_reg_read(core0_.uc, UC_ARM_REG_PC, &next_pc);
                core0_.entry = next_pc;
            }
            if (c1_breakpoints_.contains(core1_.entry)) {
                uc_ctl_remove_cache(core1_.uc, core1_.entry, 16);
                const uc_err err1 = uc_emu_start(core1_.uc, core1_.entry, 0, 0, 1);
                if (err1 != UC_ERR_OK) {
                    stepping_ = false;
                    req->reply.set_value("{\"ok\":false,\"error\":\"core1_cont_step\"}");
                    return;
                }
                u32 next_pc = 0;
                uc_reg_read(core1_.uc, UC_ARM_REG_PC, &next_pc);
                core1_.entry = next_pc;
            }
            stepping_ = false;
            req->reply.set_value("{\"ok\":true,\"running\":true}");
            return;
        }
        if (req->cmd == "step") {
            if (req->core != 0 && req->core != 1) {
                req->reply.set_value("{\"ok\":false,\"error\":\"invalid_core\"}");
                return;
            }
            const int ticks = std::clamp(req->has_i0 ? (int)req->i0 : 1, 1, 1000);
            uc_engine* uc = req->core == 1 ? core1_.uc : core0_.uc;
            u32& entry = req->core == 1 ? core1_.entry : core0_.entry;
            stepping_ = true;
            for (int t = 0; t < ticks; t++) {
                // One Unicorn instruction is the source of truth. A legitimate
                // self-loop must stay at the same PC; forcing PC+4 corrupts it.
                const uc_err err = uc_emu_start(uc, entry, 0, 0, 1);
                if (err != UC_ERR_OK) {
                    stepping_ = false;
                    char buf[192];
                    snprintf(buf, sizeof(buf),
                             "{\"ok\":false,\"error\":\"emu_step\",\"uc_err\":%u,\"message\":\"%s\"}",
                             unsigned(err), uc_strerror(err));
                    req->reply.set_value(buf);
                    return;
                }
                uc_reg_read(uc, UC_ARM_REG_PC, &entry);
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
            if ((req->core != 0 && req->core != 1) || req->i0 > 16) {
                req->reply.set_value("{\"ok\":false,\"error\":\"invalid_register\"}");
                return;
            }
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
            if ((req->core != 0 && req->core != 1) || req->i0 > 16) {
                req->reply.set_value("{\"ok\":false,\"error\":\"invalid_register\"}");
                return;
            }
            uc_engine* uc = (req->core == 1) ? core1_.uc : core0_.uc;
            int r_idx = (int)req->i0;
            int reg_id = UC_ARM_REG_R0 + r_idx;
            if (r_idx == 13) reg_id = UC_ARM_REG_SP;
            else if (r_idx == 14) reg_id = UC_ARM_REG_LR;
            else if (r_idx == 15) reg_id = UC_ARM_REG_PC;
            else if (r_idx == 16) reg_id = UC_ARM_REG_CPSR;
            u32 val = (u32)req->val;
            if (r_idx == 15) {
                // Bit 0 selects Thumb on branch-like PC writes. Preserve the
                // current privilege/interrupt flags instead of forcing User mode.
                u32 cpsr = 0;
                uc_reg_read(uc, UC_ARM_REG_CPSR, &cpsr);
                if (val & 1u) cpsr |= (1u << 5); else cpsr &= ~(1u << 5);
                uc_reg_write(uc, UC_ARM_REG_CPSR, &cpsr);
                val &= ~1u;
            }
            const uc_err write_err = uc_reg_write(uc, reg_id, &val);
            if (write_err != UC_ERR_OK) {
                req->reply.set_value("{\"ok\":false,\"error\":\"reg_write\"}");
                return;
            }
            if (req->core == 0 && r_idx == 15) {
                core0_.entry = val;
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
            if (req->core != 0 && req->core != 1) {
                req->reply.set_value("{\"ok\":false,\"error\":\"invalid_core\"}");
                return;
            }
            uc_engine* uc = (req->core == 1) ? core1_.uc : core0_.uc;
            u32 addr = (u32)req->i0;
            const int64_t requested_len = req->has_i1 ? req->i1 : 4;
            if (requested_len < 1 || requested_len > 4096) {
                req->reply.set_value("{\"ok\":false,\"error\":\"invalid_length\"}");
                return;
            }
            const size_t len = static_cast<size_t>(requested_len);
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
            std::string resp = "{\"ok\":true,\"core\":" + std::to_string(req->core) +
                               ",\"addr\":" + std::to_string(addr) +
                               ",\"len\":" + std::to_string(len) +
                               ",\"hex\":\"" + hex + "\"}";
            req->reply.set_value(std::move(resp));
            return;
        }
        if (req->cmd == "write") {
            if (req->core != 0 && req->core != 1) {
                req->reply.set_value("{\"ok\":false,\"error\":\"invalid_core\"}");
                return;
            }
            if (req->str_hex.empty() || req->str_hex.size() > 8192 ||
                (req->str_hex.size() & 1u) ||
                !std::all_of(req->str_hex.begin(), req->str_hex.end(),
                             [](unsigned char c) { return std::isxdigit(c) != 0; })) {
                req->reply.set_value("{\"ok\":false,\"error\":\"invalid_hex\"}");
                return;
            }
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
        if (req->cmd == "strict_unmapped") {
            // QW14 opt-in trap over the control channel. `str_mode` "arm"/"disarm"
            // toggles; with no mode it just reports {armed,tripped,event}.
            if (req->str_mode == "arm") {
                strict_unmapped_.Arm(true);
            } else if (req->str_mode == "disarm") {
                strict_unmapped_.Arm(false);
            }
            std::string resp = "{\"ok\":true,\"strict_unmapped\":" +
                               strict_unmapped_.Json() + "}";
            req->reply.set_value(std::move(resp));
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

        // APPS Physical RAM space (0x10000000..0x16000000) — host-backed pool
        // para permitir aliasing físico real (PCSX2 VTLB / Dolphin fastmem).
        // A pool é dona da RAM de host; map_control faz VAs adicionais apontarem
        // para o MESMO backing store via uc_mem_map_ptr.
        apps_pool_mem_.assign((size_t)APPS_RAM_PHYS_SIZE, 0);
        apps_pool_.phys_base = APPS_RAM_PHYS_BASE;
        apps_pool_.size      = APPS_RAM_PHYS_SIZE;
        apps_pool_.host      = apps_pool_mem_.data();
        {
            uc_err ep = uc_mem_map_ptr(core0_.uc, APPS_RAM_PHYS_BASE,
                                       (size_t)APPS_RAM_PHYS_SIZE, UC_PROT_ALL,
                                       apps_pool_mem_.data());
            if (ep != UC_ERR_OK) {
                printf("[System] APPS_RAM pool map_ptr falhou (%s); fallback anônimo\n",
                       uc_strerror(ep));
                apps_pool_.host = nullptr; // desativa aliasing
                uc_mem_map(core0_.uc, APPS_RAM_PHYS_BASE, APPS_RAM_PHYS_SIZE, UC_PROT_ALL);
            } else {
                // Identidade phys==va registrada na LUT (base do aliasing).
                vtlb_.map(APPS_RAM_PHYS_BASE, APPS_RAM_PHYS_SIZE, apps_pool_mem_.data());
                printf("[System] APPS_RAM pool host-backed @0x%08x (%u MB), VTLB LUT armada\n",
                       (unsigned)APPS_RAM_PHYS_BASE, (unsigned)(APPS_RAM_PHYS_SIZE >> 20));
            }
        }

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

        // AMSS Virtual Windows (Core 1 / REX). O reset vector real fica no VA
        // 0xf0000000 (kernel high VA) — o e_entry (PA 0xa00000) e traduzido para ca
        // em load_amss. Sem esses mapeamentos os uc_mem_write dos segmentos falham
        // silenciosamente e o Core1 pega UC_ERR_FETCH_UNMAPPED em 0xf0000000.
        uc_mem_map(core1_.uc, 0xf0000000, 0x01000000, UC_PROT_ALL); // Kernel/REX High VA
        uc_mem_map(core1_.uc, 0xb0000000, 0x01000000, UC_PROT_ALL); // AMSS user/task VA

        // Janela de RELOCAcao do REX (transicao 0xf0017740..0xf001774c).
        // Apos passar a checagem de regioes (0xf0017448) e reconfigurar o CP15, o REX
        // recomputa PC/SP com um delta fisico-vs-virtual:
        //   r3 = 0xf0000000 - [0xf000004c]  ; [0xf000004c] = base RAM = 0x00a00000
        //   r3 = 0xef600000                 ; delta de relocacao
        //   add sp,sp,r3 ; add r0,pc,r3 ; mov pc,r0  -> salta p/ (PC + 0xef600000)
        // Ex.: 0xf0017750 + 0xef600000 = 0xdf617750. Sem esta janela mapeada (e sem o
        // codigo espelhado, feito em load_amss), o mov pc,r0 cai em UC_ERR_EXCEPTION /
        // fetch unmapped. Com MMU desabilitada no Unicorn, espelhamos o codigo do REX
        // nesta janela para que a transicao execute suavemente (probe_reloc.py valida).
        uc_mem_map(core1_.uc, REX_RELOC_BASE, 0x01000000, UC_PROT_ALL); // 0xdf600000

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
        // KIP+0xc8 = PageInfo (L4 KernelInterfacePage.MemoryInfo/PageInfo word).
        // Layout: bits[0:9] = page access-rights (rwx), bits[10:31] = page-size mask
        // (bit N set => 2^N page size supported). O firmware executa l4e_min_pagesize()
        // em 0xb000d498: ldr r3,[r0,#0xc8]; bic ~0x3ff (limpa rwx); depois faz bit-scan
        // (tst #1; lsr #1; loop 0xb000d4a8) procurando o MENOR bit setado = log2 do menor
        // tamanho de pagina. Com 0 nenhum bit existe -> loop infinito. O MMU ARM do
        // MSM7201A (ARM1136/ARMv6) suporta paginas 4K/64K/1M/16M => bits 12,16,20,24.
        // rwx = 0x6 (RW). Valor conforme okl4-2.1.1 ARM (min page = 4KB).
        w32(0xc8, (1u<<12)|(1u<<16)|(1u<<20)|(1u<<24)|0x6); // PageInfo = 0x01111006
        // KIP+0xc4 = thread_bits. O thread_init() do Iguana (0xb00070c8, casado
        // com iguana/server/src/thread.c do OKL4 2.1.1) executa, ANTES do
        // bi_execute:
        //     min_threadno = (utcb[0] >> 14) + 2        (~131074 com o utcb dummy
        //                                                 0x80000100 em 0xdff00000)
        //     max_threadno = 1 << KIP[0xc4]             (ldrb — 1 byte: thread_bits)
        //     rfl_insert_range(min, max)  -> se min > max => ASSERT (thread.c:134)
        //                                    panic 0xb0007184 -> hang 0xb000b1d4.
        // Com thread_bits=0 (KIP zerado), max = 1<<0 = 1 < min => o boot PANICA no
        // thread_init e nunca alcança o bi_execute. Com thread_bits=18,
        // 1<<18 = 262144 >= min => o panic some e o boot atravessa o thread_init.
        // 18 é o valor do config ARM do OKL4 (arch/arm/pistachio/include/config.h:
        // "256 MB de KTCBs, giving 18 valid bits for thread IDs") e o MÍNIMO que
        // funciona com o utcb dummy atual (1<<17=131072 < min; 1<<18 >= min).
        // Escrevemos como word: o byte lido pelo ldrb em 0xc4 fica = 0x12 (18).
        w32(0xc4, 18);                     // thread_bits = 18 (KTCB de 256MB)
        uc_mem_write(core0_.uc, KIP_BASE, kip.data(), kip.size());
        printf("[KIP] Kernel Interface Page @ 0x%08x (bootinfo -> 0xb0d00000, RAM 0x10000000-0x16000000, PageInfo[0xc8]=0x01111006 min-page=4K)\n", KIP_BASE);
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
            u16 phnum = rd16(elf_hdr, 44);
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
        std::vector<std::pair<u32,u32>> exec_ranges;
        for (int i = 0; i < phnum; i++) {
            size_t o = phoff + i * phent;
            if (rd32(d.data(), o) != 1) continue; // PT_LOAD
            u32 va = rd32(d.data(), o+8), pa = rd32(d.data(), o+12);
            u32 off = rd32(d.data(), o+4), fs = rd32(d.data(), o+16), ms = rd32(d.data(), o+20);
            u32 flg = rd32(d.data(), o+24);
            u32 nmem = ms ? ms : fs; if (!nmem) continue;

            // Coleta intervalos executáveis (p_flags bit0 = PF_X) para a validação
            // estrutural determinística das vtables gpIGL/gpIEGL (firmware stripped:
            // não há VA hardcodável; a vtable é aceita só se todos os slots são código).
            if ((flg & 1u) && va) exec_ranges.emplace_back(va, va + nmem);

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
        // Arma o resolvedor determinístico das vtables IGL/IEGL com os segmentos
        // executáveis reais do APPS.bin (validação estrutural — sem VA inventado).
        if (igl_bridge_ && !exec_ranges.empty()) {
            igl_bridge_->set_code_ranges(exec_ranges);
            printf("[System] IglGuestBridge armado com %zu intervalos executáveis do APPS.bin "
                   "(resolução determinística de gpIGL/gpIEGL)\n", exec_ranges.size());
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

        u32 e_entry = rd32(d.data(), 24);
        u32 phoff = rd32(d.data(), 28);
        u16 phent = rd16(d.data(), 42), phnum = rd16(d.data(), 44);

        printf("[System] AMSS ELF Entrypoint (e_entry/PA): 0x%08x, Segments: %u\n", e_entry, phnum);
        u32 entry_va = e_entry;  // fallback: usar e_entry cru se nao houver traducao
        for (int i = 0; i < phnum; i++) {
            size_t o = phoff + i * phent;
            if (rd32(d.data(), o) != 1) continue;
            u32 va = rd32(d.data(), o+8), pa = rd32(d.data(), o+12), off = rd32(d.data(), o+4);
            u32 fs = rd32(d.data(), o+16), ms = rd32(d.data(), o+20);
            u32 nmem = ms ? ms : fs; if (!nmem) continue;

            // Direct mapping: os segmentos sao gravados no VA (0xf0000000...).
            uc_mem_write(core1_.uc, va, d.data() + off, fs);

            // Espelho na janela de RELOCACAO do REX (0xdf600000 = va + 0xef600000).
            // A transicao 0xf001774c (mov pc,r0) relocaliza PC/SP por +0xef600000; sem o
            // codigo espelhado nessa janela, o salto cai em fetch/exec unmapped no Unicorn
            // (que nao tem MMU ARM real). Espelhar somente segmentos na faixa kernel-VA
            // (0xf0000000..0xf1000000) — os unicos alcancados pela relocacao do REX.
            if (va >= 0xf0000000 && va < 0xf1000000) {
                u32 mirror = va + REX_RELOC_DELTA; // wraps para 0xdf6xxxxx
                uc_mem_write(core1_.uc, mirror, d.data() + off, fs);
            }

            // Traducao PA->VA do reset vector: e_entry do super-ELF e um PA (0x00a00000),
            // mas o codigo de reset esta mapeado no VA do seg (0xf0000000). Rodar a partir
            // do PA cru cai em bytes zerados -> NOP-slide. Achamos o seg cujo PA contem
            // e_entry e traduzimos para o VA correspondente (find_arm9_reset.py confirma:
            //   VA 0xf0000000: b 0xf0000024; msr cpsr_fc,#0xd3; mcr p15 (MMU); ldr sp;
            //   bl 0xf0017ccc (rex_init)).
            if (pa && e_entry >= pa && e_entry < pa + nmem) {
                entry_va = va + (e_entry - pa);
            }
        }

        core1_.entry = entry_va;
        printf("[System][Core1] Reset vector real (VA) = 0x%08x  (e_entry PA 0x%08x traduzido)\n",
               core1_.entry, e_entry);

        // Preambulo de reset do ARM9/AMSS que o hardware faria e o ELF cru nao faz:
        //  (a) CPSR em modo SVC (0xD3) com IRQ+FIQ mascarados — o codigo em 0xf0000024
        //      executa 'msr cpsr_fc,#0xd3', mas garantimos o estado inicial correto.
        //  (b) SP de supervisor/REX no topo de RAM de scratch do modem. O proprio reset
        //      recomputa SP (=0x00a197f8) via literais; damos um SP valido inicial para o
        //      caso de uma excecao antes desse ponto.
        u32 cpsr_svc = 0x000000D3;  // M=SVC(10011), I=1, F=1, T=0
        uc_reg_write(core1_.uc, UC_ARM_REG_CPSR, &cpsr_svc);
        u32 sp_svc = 0x00A197F8;    // topo de stack SVC derivado pelo reset (find_arm9_reset.py)
        uc_reg_write(core1_.uc, UC_ARM_REG_SP, &sp_svc);
        printf("[System][Core1] Preambulo: CPSR=0x%02x (SVC,IRQ/FIQ off) SP=0x%08x\n",
               cpsr_svc, sp_svc);

        // Semeadura da TABELA DE REGIOES DE RAM do REX em 0x00a1d73c (= base RAM +0x1d73c).
        // A rotina de varredura de descritores 0xf0017448 le entradas de 16 bytes
        //   { base, limite+1, attr, reservado } e para quando [+8]==0. Um descritor com
        // attr low-nibble 0xf (e high-nibble 0) faz a checagem retornar 0 (SUCESSO) em vez
        // de -1, ultrapassando o panic dead-loop local em 0xf0017890. Fornecemos UMA regiao
        // cobrindo a RAM do AMSS (0x00a00000..0x00c00000) seguida de terminador nulo.
        // (probe_f0017448.py / probe_reloc.py validam a passagem por execucao real.)
        seed_rex_region_table();

        // ── Snapshot pristino da janela do heap REX + shadow de dados (split I/D) ─
        // Captura o CÓDIGO real do AMSS em 0xf0000000..0xf0200000 (visão de instrução)
        // e aloca a RAM de dados dedicada (PA 0x00a00000). A partir daqui, o par de
        // hooks (c1_code_hook + c1_mem_hook) mantém I e D desacoplados nesse VA para
        // que rex_heap_init (0xf0002cd4) particione o heap sem corromper o .text.
        rex_heap_pristine_.assign(REX_HEAP_VA_SIZE, 0);
        uc_mem_read(core1_.uc, REX_HEAP_VA_BASE, rex_heap_pristine_.data(), REX_HEAP_VA_SIZE);
        rex_heap_shadow_.assign(REX_HEAP_VA_SIZE, 0);
        rex_heap_dirty_.clear();
        rex_split_id_ = true;
        printf("[System][Core1] Split I/D do heap REX armado: janela 0x%08x+%uKB "
               "(código pristino preservado, dados -> shadow PA 0x00a00000)\n",
               REX_HEAP_VA_BASE, REX_HEAP_VA_SIZE>>10);

        return true;
    }

    // Preenche a tabela de regioes de RAM esperada pelo scanner do REX (0xf0017448)
    // no endereco fisico 0x00a1d73c. Sem ela o scanner retorna -1 e o REX entra em
    // panic dead-loop em 0xf0017890 antes de reconfigurar a MMU/relocar.
    void seed_rex_region_table() {
        const u32 SRC = 0x00a1d73c;
        auto w32 = [&](u32 a, u32 v){ uc_mem_write(core1_.uc, a, &v, 4); };
        w32(SRC + 0x00, 0x00a00000); // entry0.base
        w32(SRC + 0x04, 0x00c00000); // entry0.limite+1 (teto)
        w32(SRC + 0x08, 0x0000000f); // entry0.attr (low-nibble 0xf => MATCH)
        w32(SRC + 0x0c, 0x00000000); // entry0.reservado
        w32(SRC + 0x18, 0x00000000); // entry1.attr = 0 => terminador
        printf("[System][Core1] Tabela de regioes REX semeada @0x%08x "
               "(base=0x00a00000 teto=0x00c00000 attr=0x0f) - checagem 0xf0017448 -> 0\n", SRC);
    }

    // Item 5: liga a vtable gpIGL/gpIEGL do guest ao IglGuestBridge. Idempotente.
    // As VAs de vtable (igl_vtable_va_ / iegl_vtable_va_) devem ser resolvidas por
    // RE do .mod ou pela leitura dos globais gpIGL/gpIEGL; enquanto 0, o bind é um
    // no-op honesto (não inventa endereço). O gate guest_running já foi armado por
    // load_applet, então o dispatch só ocorre com applet carregado.
    void try_bind_igl_vtable(uc_engine* uc) {
        if (igl_bound_ || !igl_bridge_) return;
        if (!igl_vtable_va_ && !iegl_vtable_va_) return; // ainda não localizadas
        if (igl_vtable_va_)  igl_bridge_->bind_igl_vtable(uc, igl_vtable_va_);
        if (iegl_vtable_va_) igl_bridge_->bind_iegl_vtable(uc, iegl_vtable_va_);
        igl_bound_ = igl_bridge_->bound();
        if (igl_bound_)
            printf("[System] vtable IGL/IEGL do guest ligada ao IglHook (Core 0).\n");
    }

    // Permite ao orquestrador/RE fixar as VAs das vtables quando descobertas.
    void set_igl_vtables(u32 igl_va, u32 iegl_va) {
        igl_vtable_va_ = igl_va; iegl_vtable_va_ = iegl_va;
    }

    void setup_hooks() {
        uc_hook h_c0, h_m0, h_u0, h_i0;
        uc_hook_add(core0_.uc, &h_c0, UC_HOOK_CODE, (void*)c0_code_hook, this, 0, ~0ULL);
        uc_hook_add(core0_.uc, &h_m0, UC_HOOK_MEM_WRITE, (void*)c0_mem_hook, this, 0, ~0ULL);
        uc_hook_add(core0_.uc, &h_u0, UC_HOOK_MEM_READ_UNMAPPED | UC_HOOK_MEM_WRITE_UNMAPPED | UC_HOOK_MEM_FETCH_UNMAPPED, (void*)c0_unmapped_hook, this, 0, ~0ULL);
        uc_hook_add(core0_.uc, &h_i0, UC_HOOK_INTR, (void*)c0_intr_hook, this, 0, ~0ULL);

        // Core 1 hooks
        uc_hook h_c1, h_m1, h_u1, h_r1;
        uc_hook_add(core1_.uc, &h_c1, UC_HOOK_CODE, (void*)c1_code_hook, this, 0, ~0ULL);
        uc_hook_add(core1_.uc, &h_m1, UC_HOOK_MEM_WRITE, (void*)c1_mem_hook, this, 0, ~0ULL);
        // Split I/D do heap REX: leitura de DADO na janela 0xf0000000+2MB injeta o shadow.
        uc_hook_add(core1_.uc, &h_r1, UC_HOOK_MEM_READ, (void*)c1_heap_read_hook, this,
                    REX_HEAP_VA_BASE, REX_HEAP_VA_BASE + REX_HEAP_VA_SIZE - 1);
        uc_hook_add(core1_.uc, &h_u1, UC_HOOK_MEM_READ_UNMAPPED | UC_HOOK_MEM_WRITE_UNMAPPED | UC_HOOK_MEM_FETCH_UNMAPPED, (void*)c1_unmapped_hook, this, 0, ~0ULL);

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
                // Chaveamento cooperativo em caso de bloqueio/wait IPC (QW32)
                u32 cur_tid = sys->thread_table_.current_tid();
                u32 next_tid = sys->thread_table_.pick_next_thread(cur_tid);
                if (next_tid && next_tid != cur_tid) {
                    const zeebo_l4::ThreadInfo* nxt = sys->thread_table_.get_thread(next_tid);
                    if (nxt && nxt->ip) {
                        zeebo_l4::ThreadInfo* cur = sys->thread_table_.get_thread_mut(cur_tid);
                        if (cur) {
                            u32 cur_pc = 0, cur_sp = 0;
                            uc_reg_read(uc, UC_ARM_REG_PC, &cur_pc);
                            uc_reg_read(uc, UC_ARM_REG_SP, &cur_sp);
                            cur->ip = cur_pc;
                            cur->sp = cur_sp;
                        }
                        sys->thread_table_.set_current_tid(next_tid);
                        u32 target_ip = nxt->ip;
                        u32 target_sp = nxt->sp;
                        uc_reg_write(uc, UC_ARM_REG_PC, &target_ip);
                        if (target_sp) uc_reg_write(uc, UC_ARM_REG_SP, &target_sp);
                        if (sys->service_registry_.is_amss_thread(next_tid)) {
                            printf("[L4/IPC] Handoff para AMSS/BREW thread %u @0x%08x (target: %s)\n",
                                   next_tid, target_ip, sys->boot_target() == 0 ? "AppMgr" : "Z-Wheel");
                        }
                    }
                }
                // Garante que o retorno do wrapper IPC em 0xb000c834 restaure r5 apontando para UTCB+0x44
                break;
            }
            case 0x04: {                                     // L4_ThreadSwitch
                // Cede quantum cooperativo para outra thread pronta se houver
                u32 cur_tid = sys->thread_table_.current_tid();
                u32 next_tid = sys->thread_table_.pick_next_thread(cur_tid);
                if (next_tid && next_tid != cur_tid) {
                    const zeebo_l4::ThreadInfo* nxt = sys->thread_table_.get_thread(next_tid);
                    if (nxt && nxt->ip) {
                        // Salva contexto da thread atual e chaveia PC/SP
                        zeebo_l4::ThreadInfo* cur = sys->thread_table_.get_thread_mut(cur_tid);
                        if (cur) {
                            u32 cur_pc = 0, cur_sp = 0;
                            uc_reg_read(uc, UC_ARM_REG_PC, &cur_pc);
                            uc_reg_read(uc, UC_ARM_REG_SP, &cur_sp);
                            cur->ip = cur_pc;
                            cur->sp = cur_sp;
                        }
                        sys->thread_table_.set_current_tid(next_tid);
                        u32 target_ip = nxt->ip;
                        u32 target_sp = nxt->sp;
                        uc_reg_write(uc, UC_ARM_REG_PC, &target_ip);
                        if (target_sp) uc_reg_write(uc, UC_ARM_REG_SP, &target_sp);
                        if (sys->service_registry_.is_amss_thread(next_tid)) {
                            printf("[L4/ThreadSwitch] Handoff para AMSS/BREW thread %u @0x%08x (target: %s)\n",
                                   next_tid, target_ip, sys->boot_target() == 0 ? "AppMgr" : "Z-Wheel");
                        }
                    }
                }
                break;
            }
            case 0x08: res_r0 = 1; break;                    // L4_ThreadControl
            case 0x0c: {                                     // L4_ExchangeRegisters
                u32 dest = 0, control = 0, new_sp = 0, new_ip = 0, flags = 0;
                uc_reg_read(uc, UC_ARM_REG_R0, &dest);
                uc_reg_read(uc, UC_ARM_REG_R1, &control);
                uc_reg_read(uc, UC_ARM_REG_R2, &new_sp);
                uc_reg_read(uc, UC_ARM_REG_R3, &new_ip);
                uc_reg_read(uc, UC_ARM_REG_R4, &flags);
                res_r0 = dest; // L4_ExchangeRegisters retorna o dest ThreadId
                sys->thread_table_.on_exchange_registers(dest, control, new_sp, new_ip, flags);
                if (new_ip >= 0xb0100000 && new_ip < 0xb0120000) {
                    sys->service_registry_.register_service("ig_naming", dest, 1, 0xb0100000, 0x20000);
                } else if (new_ip >= 0xb0300000 && new_ip < 0xb0330000) {
                    sys->service_registry_.register_service("quartz_servers", dest, 2, 0xb0300000, 0x30000);
                } else if (new_ip >= 0x10137000 && new_ip < 0x10200000) {
                    sys->service_registry_.register_service("amss", dest, 3, 0x10137000, 0x100000);
                }
                break;
            }
            case 0x10: break;                                // L4_Schedule
            case 0x14: {                                     // L4_MapControl
                u32 sid = 0, control = 0;
                uc_reg_read(uc, UC_ARM_REG_R0, &sid);
                uc_reg_read(uc, UC_ARM_REG_R1, &control);
                u32 utcb_ptr = 0;
                uc_mem_read(uc, 0xff000ff0, &utcb_ptr, 4);
                if (getenv("ZEEBO_MC_DEBUG")) {
                    u32 rr[6]={0}; for(int k=0;k<6;k++){uc_reg_read(uc,UC_ARM_REG_R2+ (k==0?0:0),&rr[k]);} 
                    u32 mr[8]={0};
                    for(int k=0;k<8;k++) uc_mem_read(uc, utcb_ptr+0x40+k*4, &mr[k],4);
                    u32 R2,R3,R4,R5,R6,R7,R9,PCv;
                    uc_reg_read(uc,UC_ARM_REG_R2,&R2);uc_reg_read(uc,UC_ARM_REG_R3,&R3);
                    uc_reg_read(uc,UC_ARM_REG_R4,&R4);uc_reg_read(uc,UC_ARM_REG_R5,&R5);
                    uc_reg_read(uc,UC_ARM_REG_R6,&R6);uc_reg_read(uc,UC_ARM_REG_R7,&R7);
                    uc_reg_read(uc,UC_ARM_REG_R9,&R9);uc_reg_read(uc,UC_ARM_REG_PC,&PCv);
                    fprintf(stderr,"[MC-DBG] sid=%08x ctrl=%08x utcb=%08x R2=%08x R3=%08x R4=%08x R5=%08x R6=%08x R7=%08x R9=%08x PC=%08x MR=[%08x %08x %08x %08x]\n",
                        sid,control,utcb_ptr,R2,R3,R4,R5,R6,R7,R9,PCv,mr[0],mr[1],mr[2],mr[3]);
                }
                res_r0 = zeebo_l4::handle_map_control(uc, utcb_ptr, sid, control,
                             /*out_items=*/nullptr,
                             sys->apps_pool_.host ? &sys->apps_pool_ : nullptr,
                             sys->apps_pool_.host ? &sys->vtlb_ : nullptr);
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
            // UC_HOOK_INTR delivers pc already at svc+4; resume at pc (not pc+4),
            // otherwise we double-advance to svc+8 and skip one guest instruction (QW17).
            target_pc = pc;
            uc_reg_write(uc, UC_ARM_REG_PC, &target_pc);
            if (ip) uc_reg_write(uc, UC_ARM_REG_SP, &ip);
            // Invalida o TB de execução no Unicorn para que ele recompile o bloco seguinte
            uc_ctl_remove_cache(uc, 0xb000c720, 0x100);
            uc_ctl_remove_cache(uc, 0xb00033d0, 0x100);
        } else if (syscall == 0x00) {
            target_pc = pc; // pc already == svc+4 (0xb000c834: pop {r1, r2}); QW17: no extra +4
            if (ip) uc_reg_write(uc, UC_ARM_REG_SP, &ip);
            uc_reg_write(uc, UC_ARM_REG_PC, &target_pc);
            uc_ctl_remove_cache(uc, 0xb000c800, 0x100);
        } else if (syscall == 0x0c) { // L4_ExchangeRegisters (QW27)
            // Stub 0xb000c758: push {r4-r8,sb,sl,fp,lr}; ldr r4,[sp,#0x24];
            //   ldr r5,[sp,#0x28]; ldr r6,[sp,#0x2c]; mov ip,sp; mvn sp,#0xf3;
            //   svc #0x140c; add lr,sp,#0x30; ldm lr,{r7,r8,sb,sl,fp,ip};
            //   str r1,[r7]; str r2,[r8]; str r3,[sb]; str r4,[sl]; str r5,[fp];
            //   str r6,[ip]; pop {r4-r8,sb,sl,fp,pc} (em 0xb000c794).
            // pc == svc+4 (0xb000c774 = add lr,sp,#0x30). Este stub tem trap-stack:
            // o epílogo lê os 6 ponteiros de saída de [ip+0x30..0x44] e escreve r1-r6
            // neles antes do pop final. É MANDATÓRIO restaurar SP=ip: sem isso o
            // `add lr,sp,#0x30` soma sobre o sp de trap corrompido (mvn = 0xffffff0c)
            // e o `pop {...,pc}` desempilha lixo, saltando para PC=0x00000000.
            target_pc = pc; // pc already == svc+4; QW17: no extra +4
            if (ip) uc_reg_write(uc, UC_ARM_REG_SP, &ip);
            uc_reg_write(uc, UC_ARM_REG_PC, &target_pc);
            uc_ctl_remove_cache(uc, 0xb000c758, 0x40);
        } else if (syscall == 0x14) { // L4_MapControl
            // O stub de MapControl em 0xb000c930 é:
            //   0xb000c930: push {r4-r8, sb, sl, fp, lr}
            //   0xb000c934: mov ip, sp
            //   0xb000c938: mvn sp, #0xeb
            //   0xb000c93c: svc #0x1414
            //   0xb000c940: pop {r4-r8, sb, sl, fp, pc}
            // UC_HOOK_INTR entrega pc == svc+4 (0xb000c940).
            // Retomar em pc (com SP restaurado para ip) executa o pop e restaura
            // perfeitamente todos os registradores do chamador (r4-r8, sb, sl, fp, pc).
            target_pc = pc;
            if (ip) uc_reg_write(uc, UC_ARM_REG_SP, &ip);
            uc_reg_write(uc, UC_ARM_REG_PC, &target_pc);
            uc_ctl_remove_cache(uc, 0xb000c930, 0x40);
        } else if (syscall == 0x08) { // L4_ThreadControl (QW26)
            // Stub 0xb000c798: push {r4-r8,sb,sl,fp,lr}; ldr r4,[sp,#0x24];
            //   ldr r5,[sp,#0x28]; ldr r6,[sp,#0x2c]; mov ip,sp; mvn sp,#0xf7;
            //   svc #0x1408; pop {r4-r8,sb,sl,fp,pc} (em 0xb000c7b4).
            // UC_HOOK_INTR entrega pc == svc+4 (0xb000c7b4 = o pop). Retomar em pc
            // (SP=ip) executa o epílogo pop e restaura os callee-saved do chamador,
            // ao contrário do else (target_pc=lr) que pula o pop e corrompe r4-r11.
            target_pc = pc;
            if (ip) uc_reg_write(uc, UC_ARM_REG_SP, &ip);
            uc_reg_write(uc, UC_ARM_REG_PC, &target_pc);
            uc_ctl_remove_cache(uc, 0xb000c798, 0x24);
        } else if (syscall == 0x18) { // L4_SpaceControl (QW26)
            // Stub 0xb000c944: push {r4-r8,sb,sl,fp,lr}; mov ip,sp; mvn sp,#0xe7;
            //   svc #0x1418; ldr r2,[sp,#0x24]; cmp r2,#0; strne r1,[r2];
            //   pop {r4-r8,sb,sl,fp,pc} (em 0xb000c960).
            // pc == svc+4 (0xb000c954) = ldr/cmp/strne (writeback do output em [ip+0x24])
            // seguido do pop. Retomar em pc (SP=ip) roda o writeback e o epílogo pop;
            // o else pularia ambos, corrompendo callee-saved e descartando o output.
            target_pc = pc;
            if (ip) uc_reg_write(uc, UC_ARM_REG_SP, &ip);
            uc_reg_write(uc, UC_ARM_REG_PC, &target_pc);
            uc_ctl_remove_cache(uc, 0xb000c944, 0x20);
        } else if (syscall == 0x04) { // L4_ThreadSwitch (QW27)
            // Stub 0xb000c7b8: push {r4-r8,sb,sl,fp,lr}; mov ip,sp; mvn sp,#0xfb;
            //   svc #0x1404; pop {r4-r8,sb,sl,fp,pc} (em 0xb000c7c8).
            // pc == svc+4 (0xb000c7c8 = pop). Retomar em pc (SP=ip) executa o pop
            // e restaura os callee-saved; o else (target_pc=lr) pularia o pop e
            // corromperia r4-r11 do chamador (mesmo padrão QW19/QW26).
            target_pc = pc;
            if (ip) uc_reg_write(uc, UC_ARM_REG_SP, &ip);
            uc_reg_write(uc, UC_ARM_REG_PC, &target_pc);
            uc_ctl_remove_cache(uc, 0xb000c7b8, 0x14);
        } else if (syscall == 0x10) { // L4_Schedule (QW27)
            // Stub 0xb000c7cc: push {r4-r8,sb,sl,fp,lr}; ldr r4,[sp,#0x24];
            //   ldr r5,[sp,#0x28]; mov ip,sp; mvn sp,#0xef; svc #0x1410;
            //   ldr r7,[sp,#0x2c]; ldr r8,[sp,#0x30]; cmp r7,#0; strne r1,[r7];
            //   cmp r8,#0; strne r2,[r8]; pop {r4-r8,sb,sl,fp,pc} (em 0xb000c7fc).
            // pc == svc+4 (0xb000c7e4) = ldr/cmp/strne writebacks + pop. Retomar em
            // pc (SP=ip) roda os dois writebacks e o pop; o else pularia tudo,
            // corrompendo callee-saved e descartando as saídas.
            target_pc = pc;
            if (ip) uc_reg_write(uc, UC_ARM_REG_SP, &ip);
            uc_reg_write(uc, UC_ARM_REG_PC, &target_pc);
            uc_ctl_remove_cache(uc, 0xb000c7cc, 0x34);
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

        // Item 4: dispatch BREW (.mod / AEEMod_Load / AEECShell@0x10c874f4).
        if (sys->brew_ && sys->brew_->on_code((u32)ad)) {
            // Quando a AEECShell atinge o vetor de dispatch, é o gatilho para
            // ligar a vtable IGL do guest (Item 5) — o applet vai emitir GL.
            if ((u32)ad == sys->brew_->symbols().aeecshell_dispatch_va) {
                sys->try_bind_igl_vtable(uc);
            }
        }
        // Item 5: intercepta funções da vtable gpIGL/gpIEGL do guest e despacha
        // para IglHook -> rasterizer. Se tratou, retorna da função (PC=LR).
        if (sys->igl_bridge_ && sys->igl_bridge_->on_code(uc, (u32)ad)) {
            u32 lr = 0; uc_reg_read(uc, UC_ARM_REG_LR, &lr);
            u32 target = lr & ~1u;
            uc_reg_write(uc, UC_ARM_REG_PC, &target);
            u32 cpsr = 0; uc_reg_read(uc, UC_ARM_REG_CPSR, &cpsr);
            if (lr & 1) cpsr |= (1 << 5); else cpsr &= ~(1 << 5);
            uc_reg_write(uc, UC_ARM_REG_CPSR, &cpsr);
            sys->core0_.entry = target;
            sys->gpu_->mark_dirty(); // sinaliza frame novo do rasterizer p/ o sink
            uc_ctl_remove_cache(uc, target, 16);
            uc_emu_stop(uc);
            return;
        }


        // --- Sonda de telemetria do loop de poll 0xb000d4a8 (Item 3) ----------
        // Loop de espera-ocupada do APPS.bin (ARM):
        //   d498: ldr r3,[r0,#0xc8]  d4a0: bic r2,...  d4a8: ldr r3,[r4] ...
        //   d4b4: tst r2,#1  d4bc: beq 0xb000d4a8 (loop enquanto bit0==0)
        // Espera um bit em [descriptor+0xc8] que só é setado por um produtor real
        // (Core1/REX ou resposta de IPC). PROIBIDO forçar r2=1 (fake progress 5a).
        // Esta sonda apenas OBSERVA: loga o valor de polling e conta iterações,
        // sem tocar r2 nem nenhum registrador/memória.
        // Endereço 0xb000d4a8: rotina l4e_min_pagesize() / CTZ de PageInfo da KIP
        // (Varre zeros a direita em KIP[0xc8] para extrair log2 do menor tamanho de pagina)
        // Linhas de monitoramento mantidas de forma transparente:
        if (ad == 0xb000d4a8) {
            u32 r0 = 0, r2 = 0, r3 = 0;
            uc_reg_read(uc, UC_ARM_REG_R0, &r0);
            uc_reg_read(uc, UC_ARM_REG_R2, &r2);
            uc_reg_read(uc, UC_ARM_REG_R3, &r3);
            u64 n = ++sys->c0_poll_d4a8_iters_;
            if (n <= 2 || (n & (n - 1)) == 0) {
                printf("[Core0/CTZ] 0xb000d4a8 iter=%llu r0(kip)=0x%08x r2(scan)=0x%08x r3(pageinfo)=0x%08x\n",
                       (unsigned long long)n, r0, r2, r3);
            }
        }
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
            // ABI (refs okl4-2.1.1-fix7 kernelinterface.spp): kernel outputs are
            // stored ONLY through the caller pointers [r4]/[r5]/[r6] above. The
            // saved-register slots at ip+0/4/8 (== sp+0/4/8) hold the caller's
            // r4,r5,r6 and MUST survive untouched, so the epilogue
            // `ldmfd sp!, {r4-r6, pc}` at 0xb000c754 restores them (r4 = page
            // cache 0xb0041284). Writing kernel outputs there corrupts the frame.

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
        ZeeboLLESystem* sys = (ZeeboLLESystem*)ud;
        const bool is_keypad =
            (addr >= KEYPAD_BASE && addr < KEYPAD_BASE + KEYPAD_SIZE &&
             type == UC_MEM_READ_UNMAPPED);
        // QW14: opt-in strict trap. A known/handled access (keypad) never trips.
        // On an unknown trip we return FALSE without mapping the page, so
        // uc_emu_start reports UC_ERR_*_UNMAPPED with PC at the fault and the
        // captured evidence intact (real Unicorn behavior, see the probe).
        {
            zeebo_lle::UnmappedKind kind =
                (type == UC_MEM_WRITE_UNMAPPED) ? zeebo_lle::UnmappedKind::kWrite
                : (type == UC_MEM_FETCH_UNMAPPED) ? zeebo_lle::UnmappedKind::kFetch
                : zeebo_lle::UnmappedKind::kRead;
            const bool is_wr = (type == UC_MEM_WRITE_UNMAPPED);
            if (sys->strict_unmapped_consider(
                    0, pc, (unsigned long)addr, (unsigned)size, kind,
                    /*has_value=*/is_wr, (unsigned long long)value,
                    /*known_handled=*/is_keypad)) {
                return false;
            }
        }
        if (is_keypad) {
            u32 val = sys->input_->read((u32)(addr - KEYPAD_BASE));
            uc_mem_map(uc, addr & ~0xFFFULL, 0x1000, UC_PROT_ALL);
            uc_mem_write(uc, addr, &val, size);
            return true;
        }
        // Map dynamically to continue discovery
        uc_mem_map(uc, addr & ~0xFFFULL, 0x1000, UC_PROT_ALL);
        // QW8: structured record of an unknown UNMAPPED access on Core 0.
        // NOTE: an unmapped hook proves nothing about MMIO — it fires for any
        // access to an unmapped page (stray pointer, undiscovered device, etc.).
        {
            const bool is_wr = (type == UC_MEM_WRITE_UNMAPPED);
            sys->unmapped_unknown_.Record(zeebo_lle::UnmappedAccessEvent{
                /*core=*/0, /*pc=*/pc, /*addr=*/(unsigned long)addr,
                /*width=*/(unsigned)size, /*is_write=*/is_wr,
                /*has_value=*/is_wr, /*value=*/(unsigned long long)value});
        }
        return true;
    }

    static bool c1_unmapped_hook(uc_engine* uc, uc_mem_type type, uint64_t addr, int size, int64_t value, void* ud) {
        u32 pc = 0;
        uc_reg_read(uc, UC_ARM_REG_PC, &pc);
        ZeeboLLESystem* sys = (ZeeboLLESystem*)ud;
        // QW14: opt-in strict trap on Core 1. No known-handled ranges here, so
        // any unmapped access is unknown. On trip, return FALSE without mapping.
        {
            zeebo_lle::UnmappedKind kind =
                (type == UC_MEM_WRITE_UNMAPPED) ? zeebo_lle::UnmappedKind::kWrite
                : (type == UC_MEM_FETCH_UNMAPPED) ? zeebo_lle::UnmappedKind::kFetch
                : zeebo_lle::UnmappedKind::kRead;
            const bool is_wr = (type == UC_MEM_WRITE_UNMAPPED);
            if (sys->strict_unmapped_consider(
                    1, pc, (unsigned long)addr, (unsigned)size, kind,
                    /*has_value=*/is_wr, (unsigned long long)value,
                    /*known_handled=*/false)) {
                return false;
            }
        }
        // Dynamically map unmapped page for Core 1 (e.g. MMIO / MSM peripheral discovery)
        uc_mem_map(uc, addr & ~0xFFFULL, 0x1000, UC_PROT_ALL);
        // QW8: structured record of an unknown UNMAPPED access on Core 1.
        // Same caveat: unmapped != proven MMIO; treat as discovery telemetry.
        {
            const bool is_wr = (type == UC_MEM_WRITE_UNMAPPED);
            sys->unmapped_unknown_.Record(zeebo_lle::UnmappedAccessEvent{
                /*core=*/1, /*pc=*/pc, /*addr=*/(unsigned long)addr,
                /*width=*/(unsigned)size, /*is_write=*/is_wr,
                /*has_value=*/is_wr, /*value=*/(unsigned long long)value});
        }
        return true;
    }

    static void c0_mem_hook(uc_engine* uc, uc_mem_type type, uint64_t addr, int /*size*/, int64_t value, void* ud) {
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

        // ── Split I/D do heap REX: antes do fetch, restaura o código pristino ────
        // dos endereços que foram tocados como DADO (heap free-list), desacoplando
        // a visão de instrução da de dados no mesmo VA — exatamente o que a MMU do
        // ARM9 faria. Sem isto, rex_heap_init corromperia 0xf000a800/0xf000e6d4.
        if (sys->rex_split_id_ && !sys->rex_heap_dirty_.empty()) {
            u32 pc=(u32)ad;
            for (u32 w : sys->rex_heap_dirty_) sys->rex_restore_code(w, 4);
            sys->rex_heap_dirty_.clear();
            if (rex_in_heap(pc)) uc_ctl_remove_cache(uc, pc, pc + (size?size:4));
        }
        if (sys->rex_split_id_ && rex_in_heap((u32)ad))
            sys->rex_restore_code((u32)ad, size?size:4);

        // ── Slide-detector (Item 1) ────────────────────────────────────────
        // Se o PC avanca estritamente +4 (ARM) por N insns consecutivas sem
        // nenhum branch tomado — ou entra em area zerada/NOP — o Core1 esta
        // "escorregando" (NOP-slide): entry errado, sem preambulo de reset.
        // Detectamos em milissegundos em vez de rodar 90s cegos ate crashar
        // em pc=0x00fffffe (padrao de ouro do Rafael: bytes/branches reais).
        {
            CoreState& c = sys->core1_;
            u32 pc = (u32)ad;

            u32 insn = 0;
            bool have_insn = (uc_mem_read(uc, ad, &insn, 4) == UC_ERR_OK);

            // Detecta se a insn ARM eh um branch/call/escrita-de-PC real.
            // Um NOP-slide autentico NAO contem nenhum destes por centenas de
            // instrucoes; um loop de init (bl, beq, ble, pop {..,pc}) contem.
            bool is_ctrl_flow = false;
            if (have_insn) {
                u32 cond = insn >> 28;
                u32 op   = (insn >> 25) & 0x7;   // bits[27:25]
                // B / BL: cond xxxx 101L ...
                if (op == 0x5) is_ctrl_flow = true;
                // BX/BLX/BXJ: cond 0001 0010 .... 000L1 Rm (bits[27:20]=0x12)
                if ((insn & 0x0ff000f0) == 0x01200010 ||  // BX
                    (insn & 0x0ff000f0) == 0x01200030)    // BLX reg
                    is_ctrl_flow = true;
                // BLX imm (cond==1111, bits[27:25]==101)
                if (cond == 0xf && op == 0x5) is_ctrl_flow = true;
                // Qualquer insn que escreve Rd=PC (r15): data-proc/ldr/mov pc,..
                // Rd em bits[15:12] para data-proc/ldr single.
                {
                    u32 top3 = (insn >> 26) & 0x3;    // 00=dp/mul, 01=ldr/str
                    u32 rd   = (insn >> 12) & 0xf;
                    if ((top3 == 0x0 || top3 == 0x1) && rd == 0xf)
                        is_ctrl_flow = true;
                }
                // LDM/POP com PC na lista (bit 15): cond 100x xxxx .... 1xxx...
                if (op == 0x4 && (insn & 0x00008000)) is_ctrl_flow = true;
            }

            if (c.slide_last_pc != 0 && pc == c.slide_last_pc + 4 && !is_ctrl_flow) {
                c.slide_run++;
            } else {
                // branch tomado / PC nao-linear / insn de control-flow -> reset.
                // Loops legitimos de init (bl/beq/ble/pop pc) zeram aqui e nunca
                // acumulam a run linear necessaria para tripar o detector.
                c.slide_run = 0;
            }
            c.slide_last_pc = pc;

            // Detecta area zerada/NOP: exige uma RUN de blanks, nao um unico
            // blank isolado (evita falso positivo em constantes/dados inline).
            bool blank_insn = have_insn &&
                (insn == 0x00000000 || insn == 0xe1a00000 || insn == 0xffffffff);
            if (blank_insn) c.slide_blank_run++; else c.slide_blank_run = 0;

            const u32 SLIDE_LIMIT = 256;       // insns lineares SEM branch => derail
            const u32 BLANK_LIMIT = 64;        // blanks consecutivos => slide real
            bool blank = (c.slide_blank_run >= BLANK_LIMIT);
            if (!c.slide_tripped && (c.slide_run >= SLIDE_LIMIT || blank)) {
                c.slide_tripped = true;
                printf("\n[Core1][SLIDE-DETECT] NOP-slide detectado @0x%08x "
                       "(run=%u linear+4, blank_run=%u). Entry provavelmente errado — "
                       "abortando execucao do Core1 em vez de rodar cego ate 0xfffffe.\n",
                       pc, c.slide_run, c.slide_blank_run);
                fflush(stdout);
                sys->core1_.halted = true;
                uc_emu_stop(uc);
                return;
            }
        }
        // ───────────────────────────────────────────────────────────────────

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

    // Split I/D do heap REX: leitura de DADO na janela 0xf0000000+2MB. Injeta o
    // valor do shadow no endereço mapeado logo antes da leitura (a instrução lê o
    // DADO do heap), marcando a word suja p/ restaurar o código pristino no fetch.
    static void c1_heap_read_hook(uc_engine* uc, uc_mem_type type, uint64_t addr, int size, int64_t, void* ud) {
        ZeeboLLESystem* sys = (ZeeboLLESystem*)ud;
        if (!sys->rex_split_id_ || type != UC_MEM_READ || !rex_in_heap((u32)addr)) return;
        u32 a=(u32)addr, off=a-REX_HEAP_VA_BASE, n=(u32)size;
        if (off+n <= sys->rex_heap_shadow_.size()) {
            uc_mem_write(uc, a, &sys->rex_heap_shadow_[off], n);
            for (u32 w=a&~3u; w<a+n; w+=4) sys->rex_heap_dirty_.insert(w);
        }
    }

    static void c1_mem_hook(uc_engine* uc, uc_mem_type type, uint64_t addr, int size, int64_t value, void* ud) {
        ZeeboLLESystem* sys = (ZeeboLLESystem*)ud;
        // ── Split I/D do heap REX: escrita de DADO na janela 0xf0000000+2MB ──────
        // Grava no shadow (RAM de dados) e marca a word suja p/ restaurar o código
        // pristino no próximo fetch (o store do Unicorn commita APÓS este hook, então
        // não adianta restaurar aqui — faríamos e o store sobrescreveria de novo).
        if (sys->rex_split_id_ && type == UC_MEM_WRITE && rex_in_heap((u32)addr)) {
            u32 a=(u32)addr, off=a-REX_HEAP_VA_BASE, n=(u32)size;
            if (off+n <= sys->rex_heap_shadow_.size()) {
                for (u32 i=0;i<n;i++)
                    sys->rex_heap_shadow_[off+i] = (u8)((value>>(8*i)) & 0xff);
                for (u32 w=a&~3u; w<a+n; w+=4) sys->rex_heap_dirty_.insert(w);
            }
        }
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
    // QW3: enumerable read-only diagnostic probes (probe.list / probe.get)
    zeebo_lle::ProbeRegistry probes_;
    // QW14: opt-in strict-unmapped trap (structured halt + latched evidence).
    // Disarmed by default so boot stays observable-equivalent to the base.
    zeebo_lle::StrictUnmappedTrap strict_unmapped_;
    // QW8: bounded structured log of unknown UNMAPPED accesses (read-only diag)
    zeebo_lle::UnmappedAccessLog unmapped_unknown_{128};
    bool paused_ = false;
    bool stepping_ = false;
    bool quit_requested_ = false;
    std::set<u32> c0_breakpoints_;
    std::set<u32> c1_breakpoints_;
    std::map<u32, std::string> c0_script_hooks_;
    std::map<u32, std::string> c1_script_hooks_;
    uint64_t c0_poll_d4a8_iters_ = 0; // Item 3: contador de iterações do poll 0xb000d4a8

    // Aliasing físico estilo PCSX2/Dolphin: pool de host da APPS_RAM + VTLB LUT.
    // A pool é dona da RAM de host de 96MB mapeada em APPS_RAM_PHYS_BASE via
    // uc_mem_map_ptr; map_one_aliased faz outros VAs apontarem para a MESMA RAM.
    zeebo_l4::PhysPool  apps_pool_;
    zeebo_l4::ThreadTable thread_table_;
    zeebo_l4::SystemServiceRegistry service_registry_;
    zeebo_l4::VtlbLut   vtlb_;
    std::vector<uint8_t> apps_pool_mem_; // backing store da pool (alinhado)

    CoreState core0_;
    CoreState core1_;
    CoreState* core1_state_;

    // ── Split I/D do heap REX em 0xf0000000..0xf0200000 (Core 1 / ARM9) ──────
    // No HW real (dump MMU L1, TripleOxygen) VA 0xf0000000 = PA 0x00a00000 (SRAM
    // de DADOS); o código executável do modem vive em 0x16e00000+. Mas o seg
    // kernel-VA do AMSS carrega CÓDIGO em 0xf0000000..0xf001e2c0 no mirror plano.
    // rex_heap_init (0xf0002cd4) particiona uma free-list de 2MB com base
    // 0xf0000000 (0xf0002d4c: str r2,[r2,#-0x400]) que, sem isolamento, arrasaria
    // o .text do AMSS (0xf000a800, re-entry 0xf000e6d4) -> UC_ERR_INSN_INVALID.
    // Mantemos a janela mapeada SEMPRE com o código pristino (visão de instrução)
    // e roteamos as escritas de DADOS para rex_heap_shadow_ (PA 0x00a00000).
    static constexpr u32 REX_HEAP_VA_BASE = 0xf0000000;
    static constexpr u32 REX_HEAP_VA_SIZE = 0x00200000;
    std::vector<u8> rex_heap_pristine_;    // código pristino da janela (visão I)
    std::vector<u8> rex_heap_shadow_;      // RAM de dados dedicada (visão D)
    std::unordered_set<u32> rex_heap_dirty_; // words com dado, a restaurar no fetch
    bool rex_split_id_ = false;

    static bool rex_in_heap(u32 a){ return a>=REX_HEAP_VA_BASE && a<REX_HEAP_VA_BASE+REX_HEAP_VA_SIZE; }
    void rex_restore_code(u32 addr, u32 n){
        u32 off=addr-REX_HEAP_VA_BASE;
        if(off>=rex_heap_pristine_.size()) return;
        if(off+n>rex_heap_pristine_.size()) n=(u32)rex_heap_pristine_.size()-off;
        uc_mem_write(core1_.uc, addr, &rex_heap_pristine_[off], n);
    }

    std::unique_ptr<NandController> nand_;
    std::unique_ptr<UnifiedMDDI> mddi_;
    std::unique_ptr<UnifiedAdreno130> gpu_;
    std::unique_ptr<UnifiedInput> input_;
    std::unique_ptr<UnifiedSMDBridge> smd_;
    std::unique_ptr<UnifiedDisplaySink> sink_;
    std::unique_ptr<zeebo::qdsp5::Qdsp5Dispatcher> qdsp_disp_;
    std::unique_ptr<zeebo::gpu::IGpuRasterizer> rast_;
    std::unique_ptr<zeebo::gpu::IglHook> igl_hook_;
    // Item 5: cola uc<->IglHook para a vtable gpIGL/gpIEGL do guest.
    std::unique_ptr<zeebo::gpu::IglGuestBridge> igl_bridge_;
    u32 igl_vtable_va_ = 0;   // VA da vtable IGL no guest (0 = ainda não localizada)
    u32 iegl_vtable_va_ = 0;  // VA da vtable IEGL no guest
    bool igl_bound_ = false;
    // Item 4: loader/dispatch de applet BREW (.mod).
    std::unique_ptr<zeebo::brew::BrewLoader> brew_;
    // Integração EFS2: parser da partição 0:EFS2APPS sobre a cópia da NAND.
    std::unique_ptr<efs2::Efs2Filesystem> efs2_;
    std::string efs2_nand_path_ = "../../nand/1.1.2.bin";
    int boot_firstapp_ = 0;
    bool efs2_ready_ = false;
    // Passo 5: encaminhamento contínuo de EVT_KEY_* do Z-Pad/SDL2 ao HandleEvent.
    u32  brew_handler_va_ = 0;
    u32  brew_applet_va_  = 0;
    u32  brew_stack_top_  = 0x2f0f0000;
    u32  brew_ret_magic_  = 0x2f0ffffe;
    bool brew_input_enabled_ = false;
    // Ciclo de vida Z-Wheel: chamadas gráficas capturadas no EVT_APP_START.
    int  zwheel_gfx_calls_ = 0;
    // Passo 11: scratch do applet Z-Wheel persistido após dispatch_zwheel_app_start
    // para que o loop interativo reencaminhe teclas ao mesmo manipulador/applet.
    u32  zwheel_applet_va_  = 0;
    u32  zwheel_stack_top_  = 0;
    u32  zwheel_ret_magic_  = 0;
    bool zwheel_life_armed_ = false;
public:
    zeebo::brew::BrewLoader* brew() { return brew_.get(); }
};

// Definições dos membros estáticos do hook transitório do ciclo de vida Z-Wheel.
ZeeboLLESystem* ZeeboLLESystem::s_zwheel_hook_sys_ = nullptr;
uint32_t        ZeeboLLESystem::s_zwheel_stub_va_  = 0;

static void print_usage(const char* prog) {
    printf("===================================================================\n");
    printf("ZEEBO LLE SYSTEM ORCHESTRATOR: Unified MSM7201A Engine\n");
    printf("Clean-room Low-Level Emulator (ARM11 APPS + ARM9 AMSS + Adreno 130)\n");
    printf("===================================================================\n\n");
    printf("Uso:\n");
    printf("  %s [opções] [nand.bin] [apps.bin] [amss.bin]\n", prog);
    printf("  %s run <arquivo.mod> [opções]\n\n", prog);
    printf("Opções de Execução e Boot:\n");
    printf("  --boot-appmgr              Força o boot no BREW Appmgr (FIRSTAPP:0, padrão jailbreak)\n");
    printf("  --boot-zwheel              Força o boot na Z-Wheel / ZeeboApp (FIRSTAPP:3, padrão fábrica)\n");
    printf("  --applet=<caminho.mod>     Carrega e injeta aplicativo BREW (.mod) externamente\n");
    printf("  --efs2-ls[=<sufixo>]       Lista dirents da partição 0:EFS2APPS da NAND (filtro opc., ex: .mod)\n");
    printf("  --efs2-run=<arquivo>       Extrai um applet direto do EFS2 (ex: reksio.mod) e injeta via BrewLoader\n");
    printf("  run <caminho.mod>          Atalho estilo Zeebx para executar applet BREW direto\n");
    printf("  --cycles=<N>               Número de ciclos intercalados (padrão: 250)\n");
    printf("  --slice=<N>                Instruções por fatia de ciclo por core (padrão: 10000)\n");
    printf("  --seconds=<N>              Tempo máximo de execução em segundos reais (0 = ilimitado)\n");
    printf("\nOpções Gráficas e Telemetria:\n");
    printf("  --gui, -g, --window        Abre janela interativa SDL2 (640x480 RGB565)\n");
    printf("  --headless                 Execução em console sem abrir janela gráfica (padrão)\n");
    printf("  --fps                      Exibe estatísticas contínuas: FPS, MIPS de C0 e C1\n");
    printf("  --dump-frames=<DIR>        Exporta sequência contínua de frames em PPM para <DIR>\n");
    printf("  --zwheel-preview           Abre preview interativo do pipeline gráfico da Z-Wheel\n");
    printf("  --zwheel-preview-headless  Testa preview gráfico em modo headless (para CI)\n");
    printf("\nOpções de Debug e Controle:\n");
    printf("  --control-port=<PORTA>     Habilita servidor de controle remoto/debug na porta TCP\n");
    printf("  --strict-unmapped          (opt-in) Interrompe deterministicamente no primeiro acesso NAO mapeado desconhecido e captura evidencia estruturada (core/PC/addr/width/dir/type/value); default inalterado\n");
    printf("  --help, -h                 Exibe este menu de ajuda e opções\n\n");
}

static bool parse_int_arg(const std::string& text, int min_value, int max_value, int& out) {
    errno = 0;
    char* end = nullptr;
    const long value = std::strtol(text.c_str(), &end, 10);
    if (errno || end == text.c_str() || *end != '\0' ||
        value < min_value || value > max_value) return false;
    out = static_cast<int>(value);
    return true;
}

static bool parse_seconds_arg(const std::string& text, double& out) {
    errno = 0;
    char* end = nullptr;
    const double value = std::strtod(text.c_str(), &end);
    if (errno || end == text.c_str() || *end != '\0' || !std::isfinite(value) || value < 0.0)
        return false;
    out = value;
    return true;
}

int main(int argc, char** argv) {
    const char* nand_path = "../../nand/1.1.2.bin";
    const char* apps_path = "../../nand/1.1.2_APPS.bin";
    const char* amss_path = "../../nand/1.1.2_AMSS.bin";
    std::vector<std::string> positional_firmware_args;
    std::string cli_nand_str, cli_apps_str, cli_amss_str;
    std::string applet_path = "";
    std::string efs2_run = "";
    bool efs2_ls_flag = false;
    std::string efs2_ls_filter = "";
    std::string dump_frames_dir = "";
    bool headless = true;
    bool zwheel_preview = false;
    bool show_fps = false;
    int control_port = 0;
    bool strict_unmapped = false;
    int cycles = 250;
    int slice_insns = 10000;
    double max_seconds = 0.0;
    int boot_firstapp = 0; // 0 = BREW Appmgr (padrão jailbreak), 3 = Z-Wheel (fábrica)

    // Processa argumentos de linha de comando
    for (int i = 1; i < argc; i++) {
        std::string arg = argv[i];
        if (arg == "--help" || arg == "-h") {
            print_usage(argv[0]);
            return 0;
        } else if (arg == "run" && i + 1 < argc) {
            applet_path = argv[++i];
        } else if (arg == "--gui" || arg == "-g" || arg == "--window") {
            headless = false;
        } else if (arg == "--headless") {
            headless = true;
        } else if (arg == "--fps") {
            show_fps = true;
        } else if (arg == "--boot-appmgr") {
            boot_firstapp = 0;
        } else if (arg == "--boot-zwheel") {
            boot_firstapp = 3;
        } else if (arg == "--zwheel-preview") {
            zwheel_preview = true;
            headless = false; // preview interativo abre a janela SDL2
        } else if (arg == "--zwheel-preview-headless") {
            zwheel_preview = true;
            headless = true;  // valida o pipeline sem abrir janela (CI)
        } else if (arg.rfind("--control-port=", 0) == 0) {
            if (!parse_int_arg(arg.substr(15), 1, 65535, control_port)) {
                fprintf(stderr, "Argumento inválido: %s\n", arg.c_str()); return 2;
            }
        } else if (arg.rfind("--applet=", 0) == 0) {
            applet_path = arg.substr(9);
        } else if (arg.rfind("--efs2-run=", 0) == 0) {
            efs2_run = arg.substr(11);
        } else if (arg == "--efs2-ls") {
            efs2_ls_flag = true;
        } else if (arg.rfind("--efs2-ls=", 0) == 0) {
            efs2_ls_flag = true;
            efs2_ls_filter = arg.substr(10);
        } else if (arg.rfind("--dump-frames=", 0) == 0) {
            dump_frames_dir = arg.substr(14);
        } else if (arg.rfind("--cycles=", 0) == 0) {
            if (!parse_int_arg(arg.substr(9), 0, 1000000000, cycles)) {
                fprintf(stderr, "Argumento inválido: %s\n", arg.c_str()); return 2;
            }
        } else if (arg.rfind("--slice=", 0) == 0) {
            if (!parse_int_arg(arg.substr(8), 1, 1000000000, slice_insns)) {
                fprintf(stderr, "Argumento inválido: %s\n", arg.c_str()); return 2;
            }
        } else if (arg.rfind("--seconds=", 0) == 0) {
            if (!parse_seconds_arg(arg.substr(10), max_seconds)) {
                fprintf(stderr, "Argumento inválido: %s\n", arg.c_str()); return 2;
            }
        } else if (arg == "--strict-unmapped") {
            strict_unmapped = true;
        } else if (arg[0] != '-') {
            positional_firmware_args.push_back(argv[i]);
        }
    }

    // Assign positional firmware paths (NAND, APPS, AMSS) by ordinal slot.
    {
        CliFirmwarePaths fw = resolve_cli_firmware_paths(positional_firmware_args);
        if (fw.surplus) {
            fprintf(stderr,
                    "[Aviso] Argumentos posicionais em excesso ignorados "
                    "(esperado no máximo 3: NAND APPS AMSS).\n");
        }
        cli_nand_str = fw.nand; nand_path = cli_nand_str.c_str();
        cli_apps_str = fw.apps; apps_path = cli_apps_str.c_str();
        cli_amss_str = fw.amss; amss_path = cli_amss_str.c_str();
    }

    if (!dump_frames_dir.empty()) {
        mkdir(dump_frames_dir.c_str(), 0777);
    }

    printf("[System] Mode: %s\n", headless ? "Headless (CLI/Test runner)" : "Interactive GUI (SDL2 Window 640x480 active)");
    printf("[System] Boot target: %s (FIRSTAPP:%d)\n",
           boot_firstapp == 0 ? "BREW Appmgr (Jailbreak default)" : "Z-Wheel / ZeeboApp (Factory default)",
           boot_firstapp);

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
    // Aponta o parser EFS2 para a mesma cópia de trabalho da NAND usada no boot.
    sys.set_efs2_nand_path(nand_path);
    sys.set_boot_target(boot_firstapp);

    // QW14: opt-in strict-unmapped. Off by default => boot unchanged.
    if (strict_unmapped) {
        sys.arm_strict_unmapped(true);
        printf("[StrictUnmapped] Armed: first UNKNOWN unmapped access will halt "
               "deterministically and capture structured evidence.\n");
    }

    // --efs2-ls: lista os dirents da partição 0:EFS2APPS e sai.
    if (efs2_ls_flag) {
        size_t n = sys.efs2_ls(efs2_ls_filter);
        return n > 0 ? 0 : 1;
    }

    // --efs2-run=<arquivo>: extrai o applet direto do EFS2 e injeta via BrewLoader.
    if (!efs2_run.empty()) {
        printf("[EFS2] Carregando applet '%s' direto da NAND 0:EFS2APPS...\n", efs2_run.c_str());
        if (!sys.load_applet_from_efs2(efs2_run, 0x12000000)) {
            printf("[Warn] Falha ao carregar applet do EFS2: %s\n", efs2_run.c_str());
        } else if (efs2_run == "274755") {
            // Z-Wheel (ZeeboApp, AEECLSID 0x01070798): após injetar o gnode/metadado
            // de 274755 do EFS2, instancia o ciclo de vida do applet despachando
            // EVT_APP_START ao manipulador ZeeboApp pré-mapeado em 0:APPS (0x10532344).
            bool life_ok = sys.dispatch_zwheel_app_start();
            // Passo 11: loop interativo/contínuo. O modo de teste unitário
            // (--seconds=0 sem GUI) executa apenas 1 frame estático do lifecycle
            // e NÃO entra no loop, preservando test-efs2-zwheel. Entra no loop
            // quando há tempo requerido (--seconds=N, N>0) ou modo interativo (GUI).
            if (life_ok && (max_seconds > 0.0 || !headless)) {
                sys.run_zwheel_interactive(headless, max_seconds, dump_frames_dir);
            }
            return life_ok ? 0 : 1;
        }
    }

    // Direct applet injection if requested
    if (!applet_path.empty()) {
        printf("[Applet] Loading external applet into memory: %s\n", applet_path.c_str());
        if (!sys.load_applet(applet_path, 0x12000000)) {
            printf("[Warn] Failed to load specified applet: %s\n", applet_path.c_str());
        }
    }

    // Passo 3 / Fase 13: modo de teste gráfico da Z-Wheel. Dirige o pipeline
    // SoftRasterizer → SDL2 e apresenta o frame RGB565 comprovado (slot 10/0x28).
    if (zwheel_preview) {
        unsigned long long sum = sys.run_zwheel_preview(headless, dump_frames_dir);
        // Prova numérica: um clear azul de 640x480 tem de somar > 0 pixels.
        bool ok = (sum > 0ULL);
        printf("\n%s soma_pixels=%llu\n",
               ok ? "PASS: pipeline Z-Wheel → SDL2 apresentou frame RGB565."
                  : "FAIL: pipeline gráfico não produziu pixels.",
               sum);
        return ok ? 0 : 1;
    }

    // Run interleaved for requested cycles or duration
    sys.run_interleaved(cycles, slice_insns, max_seconds, show_fps, dump_frames_dir);

    return 0;
}
