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
#include <mutex>
#include <functional>
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
#include "zeebo_brew_timer.h"
#include "zeebo_control_server.h"
#include "zeebo_probe_registry.h"
#include "qdsp5/qdsp5_capture_hook.h"
#include "qdsp5/qdsp5_dispatcher.h"
#include "zeebo_audio_sink.h"
#include "gpu/igl_hook.h"
#include "gpu/igpu_rasterizer.h"
#include "gpu/igl_guest_bridge.h"
#include "zeebo_brew_loader.h"
#include "zeebo_applet_dispatch.h"  // Bug 4: seleção honesta de manipulador por módulo
#include "zeebo_uc_exec.h"          // Bug 4: prova REAL de permissão executável (UC_PROT_EXEC)
#include "zeebo_module_gate.h"      // DD0: gate honesto de módulo — sem PASS por carga isolada
#include "zeebo_efs2_fs.h"
#include "zeebo_shared_memory.h"
#define ZEEBO_VIC_WITH_UNICORN
#include "zeebo_vic_irq.h"
#include "zeebo_gpt_timer.h"
#include "zeebo_proccomm.h"
#include "zeebo_cli_paths.h"
#include "zeebo_dynarmic_core.h"
#include "zeebo_slide_detector.h"

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
    
    // Keypad / Controller — MMIO de leitura do estado de teclas. NOTA de hardware:
    // o keypad físico do Zeebo é GPIO/keysense (INT_KEYSENSE=28), NÃO um bloco MMIO
    // em 0xA9A00000. A base usada antes (0xA9A00000) COLIDIA com o endereço real da
    // UART1 e engolia seu console. Movida para um slot não-conflitante apenas para
    // manter o modelo MMIO neutro (o input real é SDL -> press_key -> VIC IRQ28; a
    // leitura via register deste MMIO não é exercitada pelo caminho de input real).
    KEYPAD_BASE         = 0xa9a10000,
    KEYPAD_SIZE         = 0x00010000,

    // ---- UART (verificado: MSM7200/7201 tem 3 UARTs; UART1 = console serial) ----
    // Fontes: tools/zloader/include/msm7k/uart.h (UART1 0xA9A00000, UART2 0xA9B00000,
    // UART3 0xA9C00000) + wiki tripleoxygen (console/UART: UART1 TX=GPIO46 RX=GPIO45,
    // 115200, sequência de setup em tools/zloader/notes.txt: LDR R4,=0xA9A00000 ...).
    // UART1 dirige o console de boot/linux; UART2/3 ficam expostas para captura.
    UART1_BASE          = 0xa9a00000,
    UART2_BASE          = 0xa9b00000,
    UART3_BASE          = 0xa9c00000,
    UART_SIZE           = 0x00010000, // 64KB de registradores por UART
    UART_OFF_TF         = 0x000c,     // TX FIFO / data register (UART_TF)
    UART_OFF_SR         = 0x0008,     // status (UART_SR; bit2 = TX_READY)
    
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

#include "zeebo_video_mmio.h"

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
    UnifiedDisplaySink() : window_(nullptr), renderer_(nullptr), texture_(nullptr), controller_(nullptr), headless_(true), fullscreen_(false) {}
    ~UnifiedDisplaySink() {
        if (controller_) SDL_GameControllerClose(controller_);
        if (texture_)  SDL_DestroyTexture(texture_);
        if (renderer_) SDL_DestroyRenderer(renderer_);
        if (window_)   SDL_DestroyWindow(window_);
        SDL_Quit();
    }
    bool init(bool headless = true) {
        headless_ = headless;
        if (headless_) return true;
        if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMECONTROLLER) < 0) return false;

        // Auto-conecta o primeiro Gamepad / Controller detectado (estilo RetroArch/Dolphin)
        for (int i = 0; i < SDL_NumJoysticks(); ++i) {
            if (SDL_IsGameController(i)) {
                controller_ = SDL_GameControllerOpen(i);
                if (controller_) {
                    printf("[Input/Gamepad] Conectado: %s\n", SDL_GameControllerName(controller_));
                    break;
                }
            }
        }

        window_ = SDL_CreateWindow("Zeebo LLE Unified Emulator (MSM7201A)",
                                   SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                                   FB_WIDTH, FB_HEIGHT,
                                   SDL_WINDOW_SHOWN | SDL_WINDOW_RESIZABLE);
        if (!window_) return false;
        renderer_ = SDL_CreateRenderer(window_, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
        if (!renderer_) renderer_ = SDL_CreateRenderer(window_, -1, SDL_RENDERER_SOFTWARE);
        if (renderer_) {
            // Mantém aspect ratio 4:3 (640x480) limpo em qualquer redimensionamento de janela
            SDL_RenderSetLogicalSize(renderer_, FB_WIDTH, FB_HEIGHT);
        }
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
    void set_title(const std::string& title) {
        if (!headless_ && window_) {
            SDL_SetWindowTitle(window_, title.c_str());
        }
    }
    void toggle_fullscreen() {
        if (!headless_ && window_) {
            fullscreen_ = !fullscreen_;
            SDL_SetWindowFullscreen(window_, fullscreen_ ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0);
        }
    }
    void on_controller_added(int device_index) {
        if (!headless_ && !controller_ && SDL_IsGameController(device_index)) {
            controller_ = SDL_GameControllerOpen(device_index);
            if (controller_) {
                printf("[Input/Gamepad] Conectado dinamicamente: %s\n", SDL_GameControllerName(controller_));
            }
        }
    }
    void on_controller_removed(int instance_id) {
        if (controller_) {
            SDL_Joystick* joy = SDL_GameControllerGetJoystick(controller_);
            if (joy && SDL_JoystickInstanceID(joy) == instance_id) {
                printf("[Input/Gamepad] Desconectado: %s\n", SDL_GameControllerName(controller_));
                SDL_GameControllerClose(controller_);
                controller_ = nullptr;
            }
        }
    }
private:
    SDL_Window*         window_;
    SDL_Renderer*       renderer_;
    SDL_Texture*        texture_;
    SDL_GameController* controller_;
    bool                headless_;
    bool                fullscreen_;
};

// 6. Host Audio Output (SDL2 pull-callback)
// Puxa PCM já mixado do QDSP5/AUDPP (via Qdsp5Dispatcher::mix_audio) e entrega
// a um SDL_AudioDevice real — torna audível o boot da BREW AppMgr, Z-Wheel e
// jogos comerciais (QW-AUD1). Não possui mixer próprio: single-source-of-truth
// é o UnifiedAudioSink já embutido na engine AUDPP.
class UnifiedHostAudio {
public:
    ~UnifiedHostAudio() { shutdown(); }

    bool init(bool headless, uint32_t sample_rate = 44100) {
        headless_ = headless;
        if (headless_) return true;
        if (SDL_WasInit(SDL_INIT_AUDIO) == 0 && SDL_InitSubSystem(SDL_INIT_AUDIO) < 0) {
            printf("[Audio] SDL_InitSubSystem(AUDIO) falhou: %s\n", SDL_GetError());
            return false;
        }

        SDL_AudioSpec want{}, have{};
        want.freq = (int)sample_rate;
        want.format = AUDIO_S16SYS;
        want.channels = 2;
        want.samples = 1024;
        want.callback = &UnifiedHostAudio::sdl_callback_trampoline;
        want.userdata = this;

        dev_ = SDL_OpenAudioDevice(nullptr, 0, &want, &have, SDL_AUDIO_ALLOW_FREQUENCY_CHANGE);
        if (dev_ == 0) {
            printf("[Audio] SDL_OpenAudioDevice falhou: %s\n", SDL_GetError());
            return false;
        }
        SDL_PauseAudioDevice(dev_, 0); // começa a puxar amostras imediatamente
        out_rate_ = have.freq > 0 ? (uint32_t)have.freq : sample_rate;
        printf("[Audio] Host device armado: %d Hz, %d canais, buffer=%d\n", have.freq, have.channels, have.samples);
        return true;
    }

    void shutdown() {
        if (dev_ != 0) {
            // SDL_CloseAudioDevice espera a callback corrente terminar; depois
            // disso nenhuma nova chamada a mix_fn_ pode ocorrer.
            SDL_CloseAudioDevice(dev_);
            dev_ = 0;
        }
    }

    // Chamado pelo orquestrador uma vez, após qdsp_disp_ existir.
    void set_source(std::function<void(int16_t*, size_t)> mix_fn) {
        std::lock_guard<std::mutex> lock(mutex_);
        mix_fn_ = std::move(mix_fn);
    }

    // Frequencia realmente negociada com o host (DD-QW6). Difere de 44100 quando
    // o SDL aplica ALLOW_FREQUENCY_CHANGE; o mixer precisa reamostrar para ela.
    uint32_t out_rate() const { return out_rate_; }

private:
    static void sdl_callback_trampoline(void* userdata, uint8_t* stream, int len) {
        auto* self = static_cast<UnifiedHostAudio*>(userdata);
        self->fill_buffer(stream, len);
    }
    void fill_buffer(uint8_t* stream, int len) {
        const size_t frames = (size_t)len / (2 * sizeof(int16_t)); // stereo S16
        std::lock_guard<std::mutex> lock(mutex_);
        if (mix_fn_) {
            mix_fn_(reinterpret_cast<int16_t*>(stream), frames);
        } else {
            std::memset(stream, 0, (size_t)len);
        }
    }

    std::function<void(int16_t*, size_t)> mix_fn_;
    SDL_AudioDeviceID dev_{0};
    uint32_t out_rate_{44100};
    bool headless_{true};
    std::mutex mutex_;
};

// ---- Core State & Orchestrator Context ----
enum class CoreBackend {
    Unicorn,
    Dynarmic
};

struct CoreState {
    const char* name = nullptr;
    uc_engine* uc = nullptr;
    CoreBackend backend = CoreBackend::Unicorn;
    std::unique_ptr<zeebo::jit::DynarmicCore> jit;
    u32 entry = 0;
    u64 insns = 0;
    bool halted = false;
    // Slide-detector (bug 8): agora orientado a BLOCO (UC_HOOK_BLOCK), sem
    // uc_mem_read por instrucao. Ver zeebo_slide_detector.h.
    zeebo::SlideDetector slide;
};

// Ring buffer do console do kernel OKL4 no Core1 (medido no firmware:
// putchar em 0xf000e6e0, indice em 0xf001da64, wrap em 0x800).
static constexpr u32 OKL4_CON_IDX  = 0xf001da64;
static constexpr u32 OKL4_CON_BUF  = 0xf001da68;
static constexpr u32 OKL4_CON_SIZE = 0x800;

class ZeeboLLESystem {
public:
    ZeeboLLESystem() {
        core1_state_ = nullptr;
    }
    ~ZeeboLLESystem() {
        // DD-QW5: a callback do SDL roda em outra thread e usa qdsp_disp_ por
        // ponteiro cru. Os membros sao destruidos em ordem inversa de declaracao,
        // o que liberaria o dispatcher ANTES de ~UnifiedHostAudio parar a callback.
        // Fecha o device e solta a fonte aqui, antes de qualquer destruicao.
        if (host_audio_) {
            host_audio_->shutdown();
            host_audio_->set_source(nullptr);
        }
        flush_uarts();
    }
    void flush_uarts() {
        for (int i = 1; i <= 3; ++i) {
            if (!uart_buffers_[i].empty()) {
                fprintf(stderr, "[UART#%d] %s\n", i, uart_buffers_[i].c_str());
                uart_buffers_[i].clear();
            }
        }
    }

    // Comandos de inspecao leem sempre o Unicorn. Sob --jit quem executa e o
    // Dynarmic, entao sem este espelhamento eles reportariam registradores
    // parados (PC=0), descrevendo um motor que nao esta rodando.
    void mirror_jit_state_for_inspection() {
        if (core0_.backend == CoreBackend::Dynarmic && core0_.jit) {
            core0_.jit->sync_to_unicorn(core0_.uc);
        }
    }

    // Traco de execucao para comparar backends (ver --trace-core0).
    FILE* trace_file_ = nullptr;
    uint64_t trace_limit_ = 0;

    // Vigia de ESCRITAS numa faixa de enderecos (ver --watch-writes).
    //
    // Motivacao: a primeira divergencia entre os backends (#23726) le
    // 0xf401ffc0 e obtem 0x10090001 no interpretado contra 0 no recompilado.
    // O conteudo nao vem da imagem AMSS nem de escrita do Core0, entao alguem
    // escreve ali em tempo de execucao. Este vigia registra QUEM escreve na
    // faixa, em qual core e com que valor, para comparar os dois caminhos.
    FILE* watch_file_ = nullptr;
    uint32_t watch_lo_ = 0;
    uint32_t watch_hi_ = 0;
    uint64_t watch_hits_ = 0;

    bool open_watch(const std::string& spec, const std::string& path) {
        // spec = "lo-hi" em hexadecimal, ex.: 0xf401f000-0xf4020000
        const size_t dash = spec.find('-');
        if (dash == std::string::npos) {
            printf("[watch] faixa invalida (esperado lo-hi): %s\n", spec.c_str());
            return false;
        }
        watch_lo_ = (uint32_t)strtoull(spec.substr(0, dash).c_str(), nullptr, 0);
        watch_hi_ = (uint32_t)strtoull(spec.substr(dash + 1).c_str(), nullptr, 0);
        if (watch_hi_ <= watch_lo_) {
            printf("[watch] faixa vazia: 0x%08x-0x%08x\n", watch_lo_, watch_hi_);
            return false;
        }
        watch_file_ = path.empty() ? stdout : fopen(path.c_str(), "w");
        if (!watch_file_) {
            printf("[watch] nao foi possivel abrir %s\n", path.c_str());
            return false;
        }
        printf("[watch] vigiando escritas em 0x%08x-0x%08x\n", watch_lo_, watch_hi_);
        return true;
    }

    void note_write(int core, uint32_t addr, int size, uint32_t value, uint32_t pc) {
        if (!watch_file_) return;
        if (addr < watch_lo_ || addr >= watch_hi_) return;
        watch_hits_++;
        fprintf(watch_file_, "core%d pc=0x%08x addr=0x%08x size=%d valor=0x%08x\n",
                core, pc, addr, size, value);
        fflush(watch_file_);
    }

    bool open_trace(const std::string& path, uint64_t limit) {
        trace_file_ = fopen(path.c_str(), "w");
        if (!trace_file_) {
            printf("[trace] nao foi possivel abrir %s\n", path.c_str());
            return false;
        }
        trace_limit_ = limit;
        printf("[trace] gravando traco do Core0 em %s (limite=%llu insns)\n",
               path.c_str(), (unsigned long long)limit);
        return true;
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
                bool contains_mode = filter.size() >= 1 && filter[0] == '*';
                const std::string needle = contains_mode ? filter.substr(1) : filter;
                if (contains_mode) {
                    if (e.name.find(needle) == std::string::npos) continue;
                } else {
                    if (e.name.size() < needle.size() ||
                        e.name.compare(e.name.size() - needle.size(), needle.size(), needle) != 0)
                        continue;
                }
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
            // Jogo / Applet Zeebo (Reksio) verificado no EFS2 da NAND 1.1.2.
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
    // DD0: contexto do hook de PC do módulo — o hook marca `s_module_pc_seen_`
    // quando um PC executado cai na faixa [s_module_lo_, s_module_hi_). Só então
    // o gate honesto pode promover o jogo a EXECUTED (nunca por carga isolada).
    static ZeeboLLESystem* s_module_hook_sys_;
    static uint32_t        s_module_lo_;
    static uint32_t        s_module_hi_;
    static bool            s_module_pc_seen_;
    // DD0 PC-hook: observa cada bloco/instrução executada em Core0 e registra se
    // algum PC pertence à faixa do módulo. Estático porque o Unicorn exige uma
    // função C; o contexto vem do `s_module_*` armado por dispatch_module_app_start.
    static void module_pc_hook(uc_engine*, uint64_t address, uint32_t, void*) {
        const uint32_t pc = static_cast<uint32_t>(address);
        if (zeebo::module_gate::pc_in_module_range(
                s_module_lo_, s_module_hi_ - s_module_lo_, pc)) {
            s_module_pc_seen_ = true;
        }
    }
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

    // Bug 4: Despacho de ciclo de vida com seleção HONESTA de manipulador por
    // módulo. O manipulador fixo da Z-Wheel (0x10532344) SÓ é usado quando o
    // applet é explicitamente a Z-Wheel (274755) — preview/harness preservado.
    // Módulos não relacionados usam o próprio manipulador resolvido (entry_va),
    // e apenas se ele estiver realmente mapeado/executável; caso contrário
    // permanecem honestamente loaded_only (nunca PASS azul).
    //   is_zwheel : o caller rotulou este applet como a Z-Wheel explícita?
    bool dispatch_applet_start(const std::string& app_name, bool is_zwheel) {
        using namespace zeebo::applet;
        // Manipulador candidato resolvido do próprio módulo (0 = não resolvido).
        u32 mod_handler = (brew_ && brew_->has_module()) ? brew_->module().entry_va : 0u;
        bool mapped = false;
        if (mod_handler && core0_.uc) {
            // Bug 4 (gap fechado): prova REAL de permissão de EXECUÇÃO do guest
            // via uc_mem_regions/UC_PROT_EXEC. A checagem antiga (uc_mem_read)
            // só provava legibilidade de host — uma página READ-only/non-exec
            // furava a proteção (fake-PASS). Agora exige UC_PROT_EXEC no VA.
            mapped = zeebo::applet::uc_range_is_executable(
                core0_.uc, mod_handler & ~1u, 2);
        }
        DispatchDecision dec = select_lifecycle_handler(is_zwheel, mod_handler, mapped);
        printf("[BREW/Applet] '%s': seleção de manipulador → %s (handler@0x%08x) [%s]\n",
               app_name.c_str(),
               dec.mode == DISPATCH_ZWHEEL ? "Z-WHEEL" :
               dec.mode == DISPATCH_MODULE ? "MÓDULO"  : "REJEITADO (loaded_only)",
               dec.handler_va, dec.reason);

        if (dec.mode == DISPATCH_ZWHEEL) {
            bool ok = dispatch_zwheel_app_start();
            if (ok)
                printf("[BREW/Applet] PASS: Z-Wheel '%s' entrou em execução e renderizou frame.\n",
                       app_name.c_str());
            return ok;
        }
        if (dec.mode == DISPATCH_REJECT) {
            printf("[BREW/Applet] loaded_only: '%s' carregado mas SEM manipulador honesto "
                   "para executar — sem despacho de ciclo de vida (sem PASS).\n",
                   app_name.c_str());
            return false;
        }
        // DISPATCH_MODULE: despacha EVT_APP_START ao manipulador REAL do módulo.
        return dispatch_module_app_start(app_name, dec.handler_va);
    }

    // Despacha EVT_APP_START ao manipulador REAL resolvido do módulo (não o fixo
    // da Z-Wheel). Executa o handler de verdade sob Unicorn; só retorna true se a
    // execução foi limpa e o applet consumiu o evento (r0==1). Sem forjar frame
    // nem clear azul: honestidade dos gates.
    bool dispatch_module_app_start(const std::string& app_name, u32 handler_va) {
        static constexpr u32 EVT_APP_START = 0x1f96;
        if (!brew_ || !core0_.uc) {
            printf("[BREW/Applet] BrewLoader/Core0 indisponível — dispatch abortado.\n");
            return false;
        }
        const u32 SB = 0x22000000, SS = 0x00100000;
        const u32 APPLET = SB + 0x0100, STACKTP = SB + 0xf000, RETMAG = SB + 0xfffe;
        uc_mem_map(core0_.uc, SB, SS, UC_PROT_ALL); // ok se já mapeado
        std::vector<u8> zeros(SS, 0);
        uc_mem_write(core0_.uc, SB, zeros.data(), zeros.size());

        // DD0: constrói a identidade honesta do módulo a partir do BrewLoader e
        // rejeita ANTES de executar qualquer arquivo inválido/externo (não
        // injetado, vazio, ou sem entry AEEMod_Load resolvido). Preserva os
        // identificadores de proveniência (base/size/entry/kind), inclusive RAW MOD.
        namespace mg = zeebo::module_gate;
        mg::ModuleIdentity ident{};
        if (brew_->has_module()) {
            const auto& m = brew_->module();
            ident = mg::ModuleIdentity{ m.injected, m.load_va, m.size,
                                        m.entry_va, m.entry_kind };
        }
        if (!mg::module_is_valid(ident)) {
            printf("[BREW/Applet] '%s': GATE DD0 → INVALID: módulo inválido/externo "
                   "(injetado=%d size=%u entry=0x%08x kind=%u) — falha ANTES de executar, sem PASS.\n",
                   app_name.c_str(), ident.injected, ident.size, ident.entry_va, ident.entry_kind);
            return false;
        }

        // DD0: arma o hook de PC do módulo. O jogo só recebe PASS se um PC
        // REALMENTE executado cair na faixa [load_va, load_va+size). Carga
        // (loaded_only) não basta; o handler fixo 0x10532344 está fora da faixa.
        s_module_hook_sys_ = this;
        s_module_lo_ = ident.load_va;
        s_module_hi_ = ident.load_va + ident.size;
        s_module_pc_seen_ = false;
        uc_hook h_mod = 0;
        uc_hook_add(core0_.uc, &h_mod, UC_HOOK_CODE, (void*)module_pc_hook, this,
                    ident.load_va, static_cast<uint64_t>(ident.load_va) + ident.size - 1);

        printf("[BREW/Applet] '%s': despachando EVT_APP_START(0x%04x) → HandleEvent@0x%08x "
               "(manipulador REAL do módulo) [faixa DD0 0x%08x..0x%08x)\n",
               app_name.c_str(), EVT_APP_START, handler_va, s_module_lo_, s_module_hi_);
        bool clean = false;
        u32 r0 = brew_->dispatch_event(handler_va, APPLET, EVT_APP_START,
                                       /*keycode=*/0, STACKTP, RETMAG, /*dwparam=*/0, &clean);
        uc_hook_del(core0_.uc, h_mod);
        s_module_hook_sys_ = nullptr;

        const bool consumed = clean && r0 == 1;
        // DD0: PASS honesto exige módulo válido + dispatch limpo/consumido + PC
        // observado dentro da faixa do módulo. Qualquer falta → loaded_only.
        mg::GameState state = mg::classify(ident, consumed, s_module_pc_seen_);
        bool ok = state == mg::GAME_EXECUTED;
        printf("[BREW/Applet] '%s': EVT_APP_START → r0=%u (uc=%s) | PC_no_módulo=%s → estado=%s → %s\n",
               app_name.c_str(), r0, clean ? "clean" : "abortado",
               s_module_pc_seen_ ? "sim" : "não",
               mg::game_state_label(state),
               ok ? "applet EXECUTOU de fato (PASS honesto)"
                  : "loaded_only (sem PC executado no módulo — sem PASS)");
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
        auto t_tick_prev = t_start;
        uint64_t frames = 0, key_dispatches = 0, timer_dispatches = 0;
        bool run = true;

        while (run) {
            // O servidor de controle so e atendido pelo laco principal; sem isto
            // um cliente conectado durante o modo applet fica sem resposta ate
            // o processo encerrar (cano quebrado do lado do agente).
            if (control_) {
                auto reqs = control_->Drain();
                for (auto& req : reqs) process_control_request(req, (int)frames);
                if (quit_requested_) { run = false; break; }
            }
            auto t_now = std::chrono::steady_clock::now();
            uint32_t elapsed_ms = (uint32_t)std::chrono::duration_cast<std::chrono::milliseconds>(t_now - t_tick_prev).count();
            if (elapsed_ms > 0) {
                t_tick_prev = t_now;
                auto expired = brew_timers_.tick(elapsed_ms);
                timer_dispatches += expired.size();
                for (const auto& tm : expired) {
                    (void)tm;
                    // Sincronização de timer BREW: dispara callback da fila
                }
            }
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
                // QW37: Sincroniza estado de framebuffer dirty e contadores Adreno 130
                if (gpu_) {
                    gpu_->write(0x010c, gpu_->read(0x010c) + 1); // Notifica emissão/flush de draw
                }
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
                printf("[Z-Wheel/Loop] FPS=%.1f | frames=%llu | teclas_consumidas=%llu | timers=%llu | t=%.1fs\n",
                       (double)frames / (total > 0 ? total : 1.0),
                       (unsigned long long)frames,
                       (unsigned long long)key_dispatches,
                       (unsigned long long)timer_dispatches, total);
                t_last = now;
            }
            // Headless com control server fica vivo esperando comandos: sem uma
            // pausa o laco ocuparia um nucleo inteiro em repaints inuteis.
            if (!headless) std::this_thread::sleep_for(std::chrono::milliseconds(16));
            else if (control_) std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }

        if (core0_.uc && h_stub) uc_hook_del(core0_.uc, h_stub);
        s_zwheel_hook_sys_ = nullptr;
        printf("[Z-Wheel/Loop] loop encerrado: frames=%llu, teclas consumidas=%llu, timers=%llu.\n",
               (unsigned long long)frames, (unsigned long long)key_dispatches, (unsigned long long)timer_dispatches);
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
    void set_jit_solo(bool solo) { jit_solo_ = solo; }
    bool jit_solo() const { return jit_solo_; }

    bool init(const std::string& nand_path, const std::string& apps_path, const std::string& amss_path, bool headless = true, bool use_dynarmic = false) {
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
        host_audio_ = std::make_unique<UnifiedHostAudio>();
        host_audio_->init(headless);

        // Initialize QDSP5 Dispatcher for Audio RPC & DSP Engine
        qdsp_disp_ = std::make_unique<zeebo::qdsp5::Qdsp5Dispatcher>();
        if (host_audio_) {
            auto* disp_ptr = qdsp_disp_.get();
            host_audio_->set_source([disp_ptr](int16_t* out, size_t frames) {
                if (disp_ptr) disp_ptr->mix_audio(out, frames);
            });
        }
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
        // ORDEM IMPORTA: uc_ctl_set_cpu_model DEVE vir ANTES de uc_ctl_tlb_mode.
        // Invertido, o set_cpu_model retorna UC_ERR_ARG e o modelo e silenciosamente
        // ignorado -- o Core1 ficava no CPU default (nao-ARM926), que rejeita
        // `mrc p15,0,apsr_nzcv,c7,c14,3` (test-and-clean dcache do ARM9) com
        // UC_ERR_INSN_INVALID em 0xf001833c. Medido: com a ordem invertida
        // set_cpu_model=UC_ERR_ARG e a execucao falha; com a ordem correta ambos OK.
        {
            uc_err e_model = uc_ctl_set_cpu_model(core1_.uc, UC_CPU_ARM_926);
            uc_err e_tlb   = uc_ctl_tlb_mode(core1_.uc, UC_TLB_VIRTUAL);
            if (e_model != UC_ERR_OK || e_tlb != UC_ERR_OK) {
                printf("[Fatal][Core1] setup da CPU falhou: set_cpu_model=%s tlb_mode=%s\n",
                       uc_strerror(e_model), uc_strerror(e_tlb));
                return false;
            }
        }
        core1_.name = "ARM9-Modem";
        core1_state_ = &core1_;

        // 5. Build Shared Bus Fabric & Peripherals
        if (!setup_memory_maps()) return false;

        // 6. Load APPS and AMSS Firmwares via Hardware DMOV DMA from NAND
        if (!load_apps_dmov(nand_path, apps_path)) return false;
        if (!load_amss_dmov(nand_path, amss_path)) return false;

        // 7. Register Hardware Hooks & Inter-core routing
        setup_hooks();

        if (use_dynarmic) {
            printf("[System] Initializing Core 0 Dynarmic ARM11 JIT backend...\n");
            zeebo::jit::MemoryBridge bridge;
            bridge.user_data = this;
            bridge.read8 = [](void* ud, uint32_t addr) -> uint8_t {
                ZeeboLLESystem* s = (ZeeboLLESystem*)ud;
                uint8_t b = 0;
                if (s->vtlb_.read(addr, &b, 1)) return b;
                if (s->core0_.uc) uc_mem_read(s->core0_.uc, addr, &b, 1);
                return b;
            };
            bridge.read16 = [](void* ud, uint32_t addr) -> uint16_t {
                ZeeboLLESystem* s = (ZeeboLLESystem*)ud;
                uint16_t val = 0;
                const bool from_vtlb = s->vtlb_.read(addr, &val, 2);
                if (!from_vtlb && s->core0_.uc) uc_mem_read(s->core0_.uc, addr, &val, 2);
                // Mesmo vigia do read32: sem ele, uma divergencia VTLB<->Unicorn
                // em acesso de halfword (LDRH) fica invisivel. Foi exatamente o
                // caso de #181306 (LDRH r3,[r0,#2] com r0=0xb0d00000).
                if (s->watch_file_ && addr >= s->watch_lo_ && addr < s->watch_hi_) {
                    uint16_t alt = 0;
                    if (s->core0_.uc) uc_mem_read(s->core0_.uc, addr, &alt, 2);
                    fprintf(s->watch_file_,
                            "READ16 addr=0x%08x valor=0x%04x origem=%s uc_diz=0x%04x%s\n",
                            addr, val, from_vtlb ? "VTLB" : "UC", alt,
                            (from_vtlb && alt != val) ? "  <<< DIVERGEM" : "");
                    fflush(s->watch_file_);
                }
                return val;
            };
            bridge.read32 = [](void* ud, uint32_t addr) -> uint32_t {
                ZeeboLLESystem* s = (ZeeboLLESystem*)ud;
                uint32_t val = 0;
                bool from_vtlb = s->vtlb_.read_u32(addr, &val);
                if (!from_vtlb && s->core0_.uc) uc_mem_read(s->core0_.uc, addr, &val, 4);
                // Vigia de LEITURA: registra a origem do dado (VTLB ou Unicorn).
                // As duas fontes podem divergir, e e exatamente isso que se quer ver.
                if (s->watch_file_ && addr >= s->watch_lo_ && addr < s->watch_hi_) {
                    uint32_t alt = 0;
                    if (s->core0_.uc) uc_mem_read(s->core0_.uc, addr, &alt, 4);
                    fprintf(s->watch_file_,
                            "READ  addr=0x%08x valor=0x%08x origem=%s uc_diz=0x%08x%s\n",
                            addr, val, from_vtlb ? "VTLB" : "UC", alt,
                            (from_vtlb && alt != val) ? "  <<< DIVERGEM" : "");
                    fflush(s->watch_file_);
                }
                return val;
            };
            bridge.write8 = [](void* ud, uint32_t addr, uint8_t val) {
                ZeeboLLESystem* s = (ZeeboLLESystem*)ud;
                s->vtlb_.write(addr, &val, 1);
                if (s->core0_.uc) uc_mem_write(s->core0_.uc, addr, &val, 1);
            };
            bridge.write16 = [](void* ud, uint32_t addr, uint16_t val) {
                ZeeboLLESystem* s = (ZeeboLLESystem*)ud;
                s->vtlb_.write(addr, &val, 2);
                if (s->core0_.uc) uc_mem_write(s->core0_.uc, addr, &val, 2);
            };
            bridge.write32 = [](void* ud, uint32_t addr, uint32_t val) {
                ZeeboLLESystem* s = (ZeeboLLESystem*)ud;
                // O vigia precisa ser instrumentado AQUI: no caminho recompilado
                // a escrita passa por esta bridge, e uc_mem_write() (API externa)
                // NAO dispara UC_HOOK_MEM_WRITE. Sem isto o vigia fica cego no
                // JIT e reportaria "zero escritas" para qualquer faixa.
                s->note_write(0, addr, 4, val, s->core0_.jit ? s->core0_.jit->pc() : 0);
                const bool ok_vtlb = s->vtlb_.write_u32(addr, val);
                uc_err e = UC_ERR_OK;
                if (s->core0_.uc) {
                    e = uc_mem_write(s->core0_.uc, addr, &val, 4);
                    // Paridade com o backend interpretado: um acesso a pagina
                    // nao mapeada vindo do codigo emulado dispara
                    // c0_unmapped_hook, que mapeia a pagina sob demanda
                    // ("map dynamically to continue discovery") e deixa a
                    // escrita acontecer. Como uc_mem_write() e API externa,
                    // ela NAO dispara hooks: sem este retry a escrita sumiria
                    // em silencio e a releitura devolveria zero — que era
                    // exatamente a divergencia #23726.
                    if (e == UC_ERR_WRITE_UNMAPPED) {
                        uc_mem_map(s->core0_.uc, addr & ~0xFFFULL, 0x1000, UC_PROT_ALL);
                        e = uc_mem_write(s->core0_.uc, addr, &val, 4);
                    }
                }
                // Diagnostico: QUAL dos dois caminhos aceitou a escrita. Sem
                // isto nao da para distinguir "VTLB nao mapeou" de "o Unicorn
                // recusou" — os dois retornos eram descartados em silencio.
                if (s->watch_file_ && addr >= s->watch_lo_ && addr < s->watch_hi_) {
                    fprintf(s->watch_file_,
                            "  ^-- vtlb=%s uc=%s(%d)%s\n",
                            ok_vtlb ? "ACEITOU" : "REJEITOU",
                            e == UC_ERR_OK ? "ACEITOU" : "REJEITOU", (int)e,
                            (!ok_vtlb && e != UC_ERR_OK) ? "  <<< ESCRITA PERDIDA" : "");
                    fflush(s->watch_file_);
                }
            };
            bridge.is_peripheral = [](void* /*ud*/, uint32_t addr) -> bool {
                return is_core0_peripheral(addr);
            };
            bridge.read_peripheral = [](void* ud, uint32_t addr, int size) -> uint32_t {
                ZeeboLLESystem* s = (ZeeboLLESystem*)ud;
                return s->handle_peripheral_read(addr, size);
            };
            bridge.write_peripheral = [](void* ud, uint32_t addr, int size, uint32_t val) {
                ZeeboLLESystem* s = (ZeeboLLESystem*)ud;
                s->handle_peripheral_write(addr, size, val);
            };
            // NAO ligar c0_code_hook aqui: MemoryReadCode dispara na TRADUCAO
            // de bloco, nao a cada instrucao executada, e o hook escreveria
            // registradores no Unicorn -- que nao e o motor em execucao sob
            // --jit. O gancho correto e on_code_exec, ligado logo abaixo.
            bridge.on_code = nullptr;

            core0_.jit = std::make_unique<zeebo::jit::DynarmicCore>(bridge);
            // Gancho por instrucao executada: sincroniza o estado do JIT para o
            // Unicorn espelho, roda o mesmo c0_code_hook usado pelo backend
            // interpretado (dispatch BREW, ponte IGL, sondas) e devolve o estado
            // -- assim uma alteracao de PC feita pelo hook realmente afeta a
            // execucao. Mesmo padrao ja usado por on_svc.
            core0_.jit->on_code_exec = [this](uint32_t pc) {
                core0_.jit->sync_to_unicorn(core0_.uc);
                const uint32_t pc_before = pc;
                c0_code_hook(core0_.uc, pc, 4, this);
                core0_.jit->sync_from_unicorn(core0_.uc);
                // Se o hook desviou o fluxo (ex.: retorno de chamada IGL
                // interceptada), encerra a fatia para o novo PC valer.
                if (core0_.jit->pc() != pc_before) core0_.jit->halt_from_hook();
            };
            core0_.jit->on_svc = [this](uint32_t /*swi*/) {
                // Sincroniza estado para que c0_intr_hook inspecione e trate registradores
                core0_.jit->sync_to_unicorn(core0_.uc);
                c0_intr_hook(core0_.uc, 2, this);
                core0_.jit->sync_from_unicorn(core0_.uc);
            };
            core0_.backend = CoreBackend::Dynarmic;
            printf("[System] Core 0 switched to Dynarmic ARM11 JIT backend.\n");
        }

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
            uc_err e0 = UC_ERR_OK;
            // Core1 ja respeitava seu halted; Core0 nao consultava o dele, entao
            // uma parada por falha era reimpressa a cada ciclo.
            if (core0_.halted) {
                // nada a executar
            } else if (core0_.backend == CoreBackend::Dynarmic && core0_.jit) {
                // Sincroniza estado inicial do Unicorn para o Dynarmic no ciclo 0
                if (c == 0) {
                    core0_.jit->sync_from_unicorn(core0_.uc);
                    core0_.jit->set_pc(core0_.entry);
                }
                uint64_t ticks_run = core0_.jit->run(slice_insns);
                (void)ticks_run;
                core0_.entry = core0_.jit->pc();
                // O caminho Unicorn abaixo checa o erro da fatia; este nao
                // checava nada. Uma instrucao invalida entao virava laco
                // silencioso: PC parado, contador de instrucoes subindo.
                auto fault = core0_.jit->take_fault();
                if (fault.raised || fault.interpreter_fallback) {
                    // Sem o opcode nao da para saber POR QUE o motor parou:
                    // le a palavra no PC da falha pelo mesmo caminho de memoria
                    // que o JIT usa.
                    u32 opcode = 0;
                    uc_mem_read(core0_.uc, fault.pc, &opcode, 4);
                    printf("[Core0/JIT] parada em pc=0x%08x opcode=0x%08x (%s, kind=%u) apos %llu insns\n",
                           fault.pc, opcode,
                           fault.raised ? "excecao" : "fallback de interpretador",
                           fault.kind, (unsigned long long)core0_.insns);
                    fflush(stdout);
                    core0_.halted = true;
                }
                // Sincroniza de volta para garantir que hooks/inspeções vejam os registradores atualizados
                core0_.jit->sync_to_unicorn(core0_.uc);
            } else {
                // QW41: uc_reg_read(PC) nunca retorna o LSB setado (Unicorn reporta
                // o endereço já alinhado), então reconstituir o T-bit a partir do
                // CPSR real antes de retomar a fatia seguinte — senão toda
                // uc_emu_start após a primeira reinicia sempre em modo ARM,
                // mesmo que a execução estivesse correndo em Thumb.
                u32 cpsr0 = 0;
                uc_reg_read(core0_.uc, UC_ARM_REG_CPSR, &cpsr0);
                u32 start_addr0 = core0_.entry | ((cpsr0 >> 5) & 1u);
                e0 = uc_emu_start(core0_.uc, start_addr0, 0, 0, slice_insns);
                uc_reg_read(core0_.uc, UC_ARM_REG_PC, &core0_.entry);
                if (e0 != UC_ERR_OK && e0 != UC_ERR_INSN_INVALID) {
                    printf("[E0-ERROR] cycle=%d err=%d (%s) pc=0x%08x\\n", c, (int)e0, uc_strerror(e0), core0_.entry);
                }
            }

            // Step Core 1 (ARM9) — pula se o slide-detector ja abortou o Core1
            uc_err e1 = UC_ERR_OK;
            if (!core1_.halted) {
                e1 = uc_emu_start(core1_.uc, core1_.entry, 0, 0, slice_insns);
                uc_reg_read(core1_.uc, UC_ARM_REG_PC, &core1_.entry);
                // [QW79] Contexto seguro: fora do TB. Aqui uc_ctl e' reentrante.
                drain_tb_invalidate(core1_.uc);
                if (e1 != UC_ERR_OK) {
                    // O Core1 morria em SILENCIO: so o Core0 tinha [E0-ERROR], entao
                    // uma falha aqui virava "pc parado" sem nenhuma pista no log.
                    u32 lr1 = 0, sp1 = 0, cpsr1 = 0;
                    uc_reg_read(core1_.uc, UC_ARM_REG_LR, &lr1);
                    uc_reg_read(core1_.uc, UC_ARM_REG_SP, &sp1);
                    uc_reg_read(core1_.uc, UC_ARM_REG_CPSR, &cpsr1);
                    printf("[E1-ERROR] cycle=%d err=%d (%s) pc=0x%08x lr=0x%08x sp=0x%08x cpsr=0x%08x modo=%u\n",
                           c, (int)e1, uc_strerror(e1), core1_.entry, lr1, sp1, cpsr1, cpsr1 & 0x1f);
                    core1_.halted = true;
                }
            }

            if (c % 10 == 0 || c < 5) {
                printf("  [Cycle %02d] Core0(ARM11): pc=0x%08x insns=%llu (%s) | Core1(ARM9): pc=0x%08x insns=%llu (%s)\n",
                       c, core0_.entry, (unsigned long long)core0_.insns, e0 ? uc_strerror(e0) : "ok",
                       core1_.entry, (unsigned long long)core1_.insns, e1 ? uc_strerror(e1) : "ok");
            }
            c++;

            // --- Bug 5: GPT advance from emulated progress + real IRQ delivery.
            // Runs BETWEEN uc_emu_start slices (engine quiescent) so mutating the
            // banked IRQ context is safe. The GPT counter advances by how many
            // instructions the cores actually executed this cycle (emulated
            // progress), NOT by any register read. A MATCH crossing raises the
            // GPT VIC line; then, for the interpreter path, an unmasked pending
            // line is delivered as a genuine ARM IRQ exception.
            {
                uint64_t d0 = core0_.insns - last_c0_insns_slice_;
                uint64_t d1 = core1_.insns - last_c1_insns_slice_;
                last_c0_insns_slice_ = core0_.insns;
                last_c1_insns_slice_ = core1_.insns;
                uint32_t ticks = (uint32_t)((d0 + d1) & 0xffffffffu);
                if (gpt_.advance(ticks)) {
                    vic_c0_.raise_line(GPT_VIC_LINE);
                    vic_c1_.raise_line(GPT_VIC_LINE);
                }
                // Deliver only on the interpreter (Unicorn) path and only when the
                // core is not halted. The JIT path is left untouched (its context
                // is not safe to mutate from here).
                if (!core0_.halted && core0_.backend == CoreBackend::Unicorn &&
                    core0_.uc && vic_c0_.irq_asserted()) {
                    // Sync the post-slice PC into the engine before delivery.
                    uc_reg_write(core0_.uc, UC_ARM_REG_PC, &core0_.entry);
                    if (zeebo::vic_deliver_irq(core0_.uc, vic_c0_, c0_vector_base_)) {
                        uc_reg_read(core0_.uc, UC_ARM_REG_PC, &core0_.entry);
                    }
                }
                if (!core1_.halted && core1_.uc && vic_c1_.irq_asserted()) {
                    uc_reg_write(core1_.uc, UC_ARM_REG_PC, &core1_.entry);
                    if (zeebo::vic_deliver_irq(core1_.uc, vic_c1_, c1_vector_base_)) {
                        uc_reg_read(core1_.uc, UC_ARM_REG_PC, &core1_.entry);
                    }
                }
            }
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

            // Telemetria de FPS e MIPS periódica + Título Dinâmico na Janela (estilo Dolphin/RPCS3)
            double telemetry_elapsed = std::chrono::duration<double>(now - last_telemetry_time).count();
            if (telemetry_elapsed >= 1.0) {
                uint64_t d_c0 = core0_.insns - last_c0_insns;
                uint64_t d_c1 = core1_.insns - last_c1_insns;
                double mips_c0 = (double)d_c0 / (telemetry_elapsed * 1000000.0);
                double mips_c1 = (double)d_c1 / (telemetry_elapsed * 1000000.0);
                double fps = (double)total_rendered_frames / (total_elapsed > 0 ? total_elapsed : 1.0);
                if (show_fps) {
                    printf("[Telemetry] t=%.1fs | C0=%.2f MIPS (pc=0x%08x) | C1=%.2f MIPS (pc=0x%08x) | Video FPS=%.2f (quadros=%llu)\n",
                           total_elapsed, mips_c0, core0_.entry, mips_c1, core1_.entry, fps, (unsigned long long)total_rendered_frames);
                }
                char title_buf[160];
                snprintf(title_buf, sizeof(title_buf),
                         "Zeebo LLE [%s] | C0: %.1f MIPS | C1: %.1f MIPS | FPS: %.1f | %s",
                         (core0_.backend == CoreBackend::Dynarmic ? "Dynarmic JIT" : "Unicorn LLE"),
                         mips_c0, mips_c1, fps,
                         (paused_ ? "PAUSADO" : "RODANDO"));
                sink_->set_title(title_buf);

                last_telemetry_time = now;
                last_c0_insns = core0_.insns;
                last_c1_insns = core1_.insns;
            }

            // Process SDL events if window is open
            SDL_Event ev;
            while (SDL_PollEvent(&ev)) {
                if (ev.type == SDL_QUIT) return;
                if (ev.type == SDL_CONTROLLERDEVICEADDED) {
                    sink_->on_controller_added(ev.cdevice.which);
                } else if (ev.type == SDL_CONTROLLERDEVICEREMOVED) {
                    sink_->on_controller_removed(ev.cdevice.which);
                } else if (ev.type == SDL_KEYDOWN) {
                    // Hotkeys globais padrão de emulador (F11 Fullscreen, Space Pause, F12 Screenshot)
                    if (ev.key.keysym.sym == SDLK_F11) {
                        sink_->toggle_fullscreen();
                        continue;
                    }
                    if (ev.key.keysym.sym == SDLK_PAUSE) {
                        paused_ = !paused_;
                        printf("[Emulator] Emulação %s\n", paused_ ? "PAUSADA" : "RETOMADA");
                        continue;
                    }
                    if (ev.key.keysym.sym == SDLK_F12) {
                        const u16* cur_fb = rast_ ? rast_->framebuffer_rgb565() : nullptr;
                        if (cur_fb) {
                            char fname[64];
                            snprintf(fname, sizeof(fname), "zeebo_screenshot_%llu.ppm", (unsigned long long)total_rendered_frames);
                            save_ppm(cur_fb, fname);
                            printf("[Emulator] Screenshot salvo: %s\n", fname);
                        } else {
                            printf("[Emulator] Screenshot falhou: framebuffer indisponível.\n");
                        }
                        continue;
                    }

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
                        case SDL_CONTROLLER_BUTTON_A:          input_->press_key(ZEEBO_KEY_A, core0_.uc); dispatch_zpad_to_brew(zeebo::brew::ZP_A, true); break;
                        case SDL_CONTROLLER_BUTTON_B:          input_->press_key(ZEEBO_KEY_B, core0_.uc); dispatch_zpad_to_brew(zeebo::brew::ZP_B, true); break;
                        case SDL_CONTROLLER_BUTTON_X:          input_->press_key(ZEEBO_KEY_C, core0_.uc); dispatch_zpad_to_brew(zeebo::brew::ZP_1, true); break;
                        case SDL_CONTROLLER_BUTTON_Y:          input_->press_key(ZEEBO_KEY_D, core0_.uc); dispatch_zpad_to_brew(zeebo::brew::ZP_2, true); break;
                        case SDL_CONTROLLER_BUTTON_DPAD_UP:    input_->press_key(ZEEBO_KEY_UP, core0_.uc); dispatch_zpad_to_brew(zeebo::brew::ZP_UP, true); break;
                        case SDL_CONTROLLER_BUTTON_DPAD_DOWN:  input_->press_key(ZEEBO_KEY_DOWN, core0_.uc); dispatch_zpad_to_brew(zeebo::brew::ZP_DOWN, true); break;
                        case SDL_CONTROLLER_BUTTON_DPAD_LEFT:  input_->press_key(ZEEBO_KEY_LEFT, core0_.uc); dispatch_zpad_to_brew(zeebo::brew::ZP_LEFT, true); break;
                        case SDL_CONTROLLER_BUTTON_DPAD_RIGHT: input_->press_key(ZEEBO_KEY_RIGHT, core0_.uc); dispatch_zpad_to_brew(zeebo::brew::ZP_RIGHT, true); break;
                        case SDL_CONTROLLER_BUTTON_LEFTSHOULDER:  dispatch_zpad_to_brew(zeebo::brew::ZP_3, true); break;
                        case SDL_CONTROLLER_BUTTON_RIGHTSHOULDER: dispatch_zpad_to_brew(zeebo::brew::ZP_4, true); break;
                        case SDL_CONTROLLER_BUTTON_BACK:
                        case SDL_CONTROLLER_BUTTON_GUIDE:
                        case SDL_CONTROLLER_BUTTON_START:      input_->press_key(ZEEBO_KEY_HOME, core0_.uc); dispatch_zpad_to_brew(zeebo::brew::ZP_HOME, true); break;
                    }
                } else if (ev.type == SDL_CONTROLLERBUTTONUP) {
                    switch (ev.cbutton.button) {
                        case SDL_CONTROLLER_BUTTON_A:          input_->release_key(ZEEBO_KEY_A); dispatch_zpad_to_brew(zeebo::brew::ZP_A, false); break;
                        case SDL_CONTROLLER_BUTTON_B:          input_->release_key(ZEEBO_KEY_B); dispatch_zpad_to_brew(zeebo::brew::ZP_B, false); break;
                        case SDL_CONTROLLER_BUTTON_X:          input_->release_key(ZEEBO_KEY_C); dispatch_zpad_to_brew(zeebo::brew::ZP_1, false); break;
                        case SDL_CONTROLLER_BUTTON_Y:          input_->release_key(ZEEBO_KEY_D); dispatch_zpad_to_brew(zeebo::brew::ZP_2, false); break;
                        case SDL_CONTROLLER_BUTTON_DPAD_UP:    input_->release_key(ZEEBO_KEY_UP); dispatch_zpad_to_brew(zeebo::brew::ZP_UP, false); break;
                        case SDL_CONTROLLER_BUTTON_DPAD_DOWN:  input_->release_key(ZEEBO_KEY_DOWN); dispatch_zpad_to_brew(zeebo::brew::ZP_DOWN, false); break;
                        case SDL_CONTROLLER_BUTTON_DPAD_LEFT:  input_->release_key(ZEEBO_KEY_LEFT); dispatch_zpad_to_brew(zeebo::brew::ZP_LEFT, false); break;
                        case SDL_CONTROLLER_BUTTON_DPAD_RIGHT: input_->release_key(ZEEBO_KEY_RIGHT); dispatch_zpad_to_brew(zeebo::brew::ZP_RIGHT, false); break;
                        case SDL_CONTROLLER_BUTTON_LEFTSHOULDER:  dispatch_zpad_to_brew(zeebo::brew::ZP_3, false); break;
                        case SDL_CONTROLLER_BUTTON_RIGHTSHOULDER: dispatch_zpad_to_brew(zeebo::brew::ZP_4, false); break;
                        case SDL_CONTROLLER_BUTTON_BACK:
                        case SDL_CONTROLLER_BUTTON_GUIDE:
                        case SDL_CONTROLLER_BUTTON_START:      input_->release_key(ZEEBO_KEY_HOME); dispatch_zpad_to_brew(zeebo::brew::ZP_HOME, false); break;
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
            mirror_jit_state_for_inspection();
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
        if (req->cmd == "dstat") {
            // Estado de diagnóstico vivo do boot (Spec-Driven): PC salvo / LR / SP /
            // R0-R7 do core escolhido + thread atual. NOTA de precisão: o emulador
            // executa em fatias (slice de N instruções); entre slices os registradores
            // do Unicorn nem sempre são legíveis (PC costuma vir 0). A fonte de
            // verdade do PC é core0_.entry/core1_.entry (salvo no fim de cada slice);
            // r0-r7 são lidos via Unicorn e marcados "approx" (precisos logo após um
            // slice, não garantidos no meio de execução). Usado p/ iterar sobre um
            // stall (ex.: loop RLE 0xb0400000) sem recompilar.
            if (req->core != 0 && req->core != 1) {
                req->reply.set_value("{\"ok\":false,\"error\":\"invalid_core\"}");
                return;
            }
            uc_engine* uc = (req->core == 1) ? core1_.uc : core0_.uc;
            const u32 entry_pc = (req->core == 1) ? core1_.entry : core0_.entry;
            u32 r[8] = {0}, pc = 0, sp = 0, lr = 0, cpsr = 0;
            uc_reg_read(uc, UC_ARM_REG_PC, &pc);
            uc_reg_read(uc, UC_ARM_REG_SP, &sp);
            uc_reg_read(uc, UC_ARM_REG_LR, &lr);
            uc_reg_read(uc, UC_ARM_REG_CPSR, &cpsr);
            for (int i = 0; i < 8; ++i) uc_reg_read(uc, UC_ARM_REG_R0 + i, &r[i]);
            const u32 tid = thread_table_.current_tid();
            char buf[640];
            snprintf(buf, sizeof(buf),
                "{\"ok\":true,\"core\":%lu,\"pc_saved\":%u,\"pc\":%u,\"sp\":%u,\"lr\":%u,\"cpsr\":%u,"
                "\"tid\":%u,"
                "\"r0\":%u,\"r1\":%u,\"r2\":%u,\"r3\":%u,\"r4\":%u,\"r5\":%u,\"r6\":%u,\"r7\":%u}",
                req->core, entry_pc, pc, sp, lr, cpsr, tid,
                r[0],r[1],r[2],r[3],r[4],r[5],r[6],r[7]);
            req->reply.set_value(buf);
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
            mirror_jit_state_for_inspection();
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
            mirror_jit_state_for_inspection();
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
    bool setup_memory_maps() {
        // SMEM é RAM física compartilhada: os dois cores devem observar os
        // mesmos bytes, não duas regiões anônimas independentes.
        uc_err smem_err = zeebo::map_shared_region_pair(
            core0_.uc, core1_.uc, SMEM_BASE, SMEM_SIZE, smem_mem_);
        if (smem_err != UC_ERR_OK) {
            fprintf(stderr, "[Fatal] shared SMEM map failed: %s\n", uc_strerror(smem_err));
            return false;
        }

        // Initialize ProcComm and SMSM in the single shared backing.
        std::vector<u8> smem_init(0x1000, 0);
        u32 ready = 1; // PCOM_READY
        memcpy(smem_init.data() + 0x14, &ready, 4); // MDM_STATUS
        u32 apps_state = 0x0000002b; // SMSM_INIT | SMSM_OSENTERED | SMSM_SMDINIT | SMSM_RPCINIT
        memcpy(smem_init.data() + 0x100, &apps_state, 4);
        uc_mem_write(core0_.uc, SMEM_BASE, smem_init.data(), smem_init.size());

        // Bug 6: bind ProcComm to the SAME shared backing both cores see. The
        // command block lives at SMEM_BASE+0x00..0x0f. Mark it idle so a fresh
        // block is not mistaken for a pending command.
        proccomm_.bind(smem_mem_.data(), smem_mem_.size());
        proccomm_.wr(zeebo::PCOM_OFF_APP_STATUS, zeebo::PCOM_STATUS_UNSET);

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
        // Janela de page tables do OKL4 (VIRT_ADDR_PGTABLE). O kernel escreve as
        // page tables por ESTE alias -- medido: escritas nao mapeadas em
        // 0xf4000000 (w32) e 0xf4023fc0 (w8) vindas de 0xf000a7a0/0xf000a7b8,
        // com offset 0x23fc0 espelhando 0xf0024000. Sem esta janela as escritas
        // caem no vazio, add_mapping le a page table de volta como zero e
        // retorna false -> "Assertion r != 0 failed in init.cc".
        uc_mem_map(core1_.uc, 0xf4000000, 0x00100000, UC_PROT_ALL);
        // High vectors do ARM926 (CP15 c1 bit V=1): a tabela de excecoes vive em
        // 0xffff0000, nao em 0x0. Sem esta pagina o kernel OKL4 le lixo ao instalar
        // os handlers e a emulacao morre logo apos "Initialising scheduler...".
        uc_mem_map(core1_.uc, 0xffff0000, 0x00010000, UC_PROT_ALL);
        // MMIO alto que o kernel toca ao instalar handlers/timer (medido via
        // hook de acesso invalido): 0xf9000000 e 0xff000000.
        uc_mem_map(core1_.uc, 0xf9000000, 0x00100000, UC_PROT_ALL);
        uc_mem_map(core1_.uc, 0xff000000, 0x00100000, UC_PROT_ALL);
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
        // UARTs (3 no MSM7200/7201). UART1 = console serial (boot/linux).
        // Mapeadas antes dos periféricos vizinhos p/ expor leitura/escrita dos
        // registradores UART (TX FIFO etc.) ao c0_mem_hook/c0_mem_read_hook.
        uc_mem_map(core0_.uc, UART1_BASE, UART_SIZE, UC_PROT_ALL);
        uc_mem_map(core0_.uc, UART2_BASE, UART_SIZE, UC_PROT_ALL);
        uc_mem_map(core0_.uc, UART3_BASE, UART_SIZE, UC_PROT_ALL);

        // Core 1 (Modem ARM9) também tem acesso ao barramento periférico AHB de UARTs
        uc_mem_map(core1_.uc, UART1_BASE, UART_SIZE, UC_PROT_ALL);
        uc_mem_map(core1_.uc, UART2_BASE, UART_SIZE, UC_PROT_ALL);
        uc_mem_map(core1_.uc, UART3_BASE, UART_SIZE, UC_PROT_ALL);

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
        return true;
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

            // ETAPA QUE FALTAVA: o super-ELF do AMSS traz VA e PA DISTINTOS por
            // segmento (ex.: seg3 va=b0000000 pa=00af0000). Gravavamos so no VA,
            // entao toda leitura feita pelo PA via o endereco VAZIO. Medido:
            //   [VA b0000000]=e35d0000  [PA 00af0000]=00000000
            // O page-table walk do OKL4 opera sobre PA (r7=00af0000 no laco), por
            // isso via zeros. Grava tambem no PA quando ele difere do VA.
            if (pa && pa != va) {
                if (uc_mem_write(core1_.uc, pa, d.data() + off, fs) != UC_ERR_OK)
                    fprintf(stderr, "[SEG] PA 0x%08x (+%u) nao mapeado - segmento nao espelhado\n",
                            pa, fs);
            }

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
        // O shadow de DADOS comeca zerado, mas 0xf000004c cai dentro desta janela
        // e o REX espera ler ali a BASE DA RAM (0x00a00000). Como toda leitura de
        // dado nesta faixa passa a vir do shadow, semear a RAM do Unicorn nao
        // basta -- o scanner 0xf0017448 leria 0 e desistiria (panic 0xf0017890).
        // Por isso o shadow nasce como COPIA do pristino: os dados inicializados
        // do AMSS (incluindo 0xf000004c) continuam visiveis, e so as escritas
        // posteriores do REX divergem do .text.
        rex_heap_shadow_ = rex_heap_pristine_;
        rex_heap_dirty_.clear();
        rex_split_id_ = true;
        printf("[System][Core1] Split I/D do heap REX armado: janela 0x%08x+%uKB "
               "(código pristino preservado, dados -> shadow PA 0x00a00000)\n",
               REX_HEAP_VA_BASE, REX_HEAP_VA_SIZE>>10);

        return true;
    }

    // Semeia a tabela de regioes de RAM consultada pelo scanner do REX
    // (0xf0017448). Endereco e formato foram DESMONTADOS do firmware em runtime,
    // nao supostos -- a versao anterior escrevia em 0x00a1d73c com descritores de
    // 16 bytes e era comprovadamente inerte (o scanner nunca lia aquele endereco).
    //
    // Codigo real do scanner:
    //   f001744c  ldr   r3, [pc, #0xac]     ; lit1 = 0x00024000
    //   f0017450  ldr   r1, [r1]            ; base = 0x00a00000
    //   f0017454  add   r3, r1, r3
    //   f0017458  ldrh  r2, [r3, #0x54]     ; COUNT (halfword) @ base+0x24054
    //   f001745c  cmp   r2, #0
    //   f0017468  bls   0xf00174f8          ; count == 0 -> desiste -> panic
    //   f001746c  ldr   r3, [pc, #0x90]     ; lit2 = 0x000241f0
    //   f0017474  add   r1, r1, r3          ; TABELA @ base+0x241f0
    //   ...laco, passo de 8 bytes (f0017480: add r1, r1, #8):
    //   f0017488  ldrb  r2, [r1]            ; byte0
    //   f001748c  and   r3, r2, #0xf
    //   f0017490  cmp   r3, #0xf            ; low nibble deve ser 0xf
    //   f00174a0  lsrs  r2, r2, #4          ; high nibble deve ser 0
    //   f00174a8  ldrb  r3, [r1, #1]
    //   f00174ac  tst   r3, #2              ; bit 1 do byte1 deve estar LIMPO
    //   f00174b4  ldr   r3, [r1]            ; base da regiao
    //   f00174b8  ldr   r2, [r1, #4]        ; teto da regiao
    //
    // Portanto a entrada tem 8 bytes: uma palavra de base cujos 10 bits baixos
    // carregam os flags (low nibble 0xf, high nibble 0, bit 9 limpo) e uma
    // palavra de teto. O scanner limpa esses bits com bic #0x3fc / bic #3.
    void w32_c1(u32 a, u32 v) { uc_mem_write(core1_.uc, a, &v, 4); }

    void seed_rex_region_table() {
        const u32 BASE  = 0x00a00000;

        // INICIALIZACAO FALTANTE (medida, nao suposta): o scanner faz
        //   f0017450  ldr r1, [r1]   ; r1 = 0xf000004c
        // e espera encontrar ali a BASE DA RAM. Sem ninguem escrever esse valor,
        // r1 vira 0, todo o resto do calculo (base+0x24054, base+0x241f0) aponta
        // para o endereco errado e o scanner desiste em 0xf0017468 -> panic.
        // Rastreado com [PATH]: em f0017454 r1=00000000 (deveria ser 0x00a00000).
        w32_c1(0xf000004c, BASE);
        const u32 COUNT = BASE + 0x24054;   // halfword
        const u32 TABLE = BASE + 0x241f0;   // entradas de 8 bytes

        auto w32 = [&](u32 a, u32 v){ uc_mem_write(core1_.uc, a, &v, 4); };
        auto w16 = [&](u32 a, u16 v){ uc_mem_write(core1_.uc, a, &v, 2); };

        // Uma regiao: [0x00a00000, 0x00c00000). Flags no low nibble = 0xf,
        // high nibble 0 e bit 1 do byte 1 limpo, como o scanner exige.
        w32(TABLE + 0x00, (0x00a00000 & ~0x3ffu) | 0x0f);
        w32(TABLE + 0x04, (0x00c00000 & ~0x3ffu));
        w16(COUNT, 1);

        // Releitura: relatar so o que foi medido (ver commit 0715130).
        u16 rb_cnt = 0; u32 rb_e0 = 0, rb_e1 = 0;
        uc_mem_read(core1_.uc, COUNT, &rb_cnt, 2);
        uc_mem_read(core1_.uc, TABLE + 0x00, &rb_e0, 4);
        uc_mem_read(core1_.uc, TABLE + 0x04, &rb_e1, 4);
        const bool ok = (rb_cnt == 1) && ((rb_e0 & 0xf) == 0xf)
                     && ((rb_e0 >> 4 & 0xf) == 0) && ((rb_e1 & 2) == 0);
        printf("[System][Core1] Tabela de regioes REX: count@0x%08x=%u "
               "entry0@0x%08x={base=0x%08x teto=0x%08x} - releitura %s\n",
               COUNT, rb_cnt, TABLE, rb_e0, rb_e1, ok ? "confere" : "DIVERGE");
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
        uc_hook h_c0, h_m0, h_u0, h_i0, h_r0;
        uc_hook_add(core0_.uc, &h_c0, UC_HOOK_CODE, (void*)c0_code_hook, this, 0, ~0ULL);
        uc_hook_add(core0_.uc, &h_m0, UC_HOOK_MEM_WRITE, (void*)c0_mem_hook, this, 0, ~0ULL);
        // Leitura de registradores de periférico modelados (UART status), injetando
        // o valor do modelo antes do fetch — sem isto, UART/MDDI reads leriam RAM crua.
        uc_hook_add(core0_.uc, &h_r0, UC_HOOK_MEM_READ, (void*)c0_mem_read_hook, this, 0, ~0ULL);
        uc_hook_add(core0_.uc, &h_u0, UC_HOOK_MEM_READ_UNMAPPED | UC_HOOK_MEM_WRITE_UNMAPPED | UC_HOOK_MEM_FETCH_UNMAPPED, (void*)c0_unmapped_hook, this, 0, ~0ULL);
        uc_hook_add(core0_.uc, &h_i0, UC_HOOK_INTR, (void*)c0_intr_hook, this, 0, ~0ULL);

        // Core 1 hooks
        uc_hook h_c1, h_m1, h_u1, h_r1;
        uc_hook_add(core1_.uc, &h_c1, UC_HOOK_CODE, (void*)c1_code_hook, this, 0, ~0ULL);

        // Slide-detector (bug 8): opera na fronteira de BLOCO, nao por
        // instrucao. OPT-IN (diagnostico): so e' instalado quando o usuario
        // pede via ZEEBO_SLIDE_DETECT (ou define ZEEBO_SLIDE_LIMIT). Motivo:
        // um unico bloco basico linear grande e legitimo (ex.: init/memcpy
        // desenrolado, tabela de saltos preenchida) pode ultrapassar o limite
        // e disparar um falso positivo em execucao normal. Deixando o detector
        // desligado por padrao, nenhuma execucao real e' abortada por engano;
        // quem investiga um derail de entry ativa o hook explicitamente.
        {
            static uc_hook h_slide;
            core1_.slide = zeebo::SlideDetector{};
            bool enable = (std::getenv("ZEEBO_SLIDE_DETECT") != nullptr);
            if (const char* s = std::getenv("ZEEBO_SLIDE_LIMIT")) {
                unsigned v = (unsigned)strtoul(s, nullptr, 0);
                if (v) { core1_.slide.slide_limit = v; enable = true; }
            }
            if (enable) {
                uc_hook_add(core1_.uc, &h_slide, UC_HOOK_BLOCK,
                            (void*)c1_block_hook, this, 0, ~0ULL);
            }
        }



        // Console do kernel OKL4 (Core1). O putchar do kernel (0xf000e6e0) grava
        // byte a byte num ring buffer em 0xf001da68 com indice em 0xf001da64
        // (`strb r0,[r2,r3]` / `str r1,[ip]`, wrap em 0x800). Espelhar essas
        // escritas da o log do kernel -- foi assim que o banner
        // "OKL4 - (provider: Open Kernel Labs)" apareceu pela primeira vez.
        // Sem isso o Core1 falha em silencio e so resta o PC para adivinhar.
        {
            static uc_hook h_con;
            uc_hook_add(core1_.uc, &h_con, UC_HOOK_MEM_WRITE,
                        (void*)+[](uc_engine*, uc_mem_type, uint64_t addr,
                                   int size, int64_t value, void*) {
                            if (size != 1) return;
                            if (addr < OKL4_CON_BUF || addr >= OKL4_CON_BUF + OKL4_CON_SIZE) return;
                            static char linha[1024];
                            static size_t n = 0;
                            char c = (char)(value & 0xff);
                            if (c >= 32 && c < 127) {
                                if (n < sizeof(linha) - 1) linha[n++] = c;
                            } else if (n) {
                                linha[n] = 0;
                                printf("[OKL4] %s\n", linha);
                                fflush(stdout);
                                n = 0;
                            }
                            if (n == sizeof(linha) - 1) {
                                linha[n] = 0;
                                printf("[OKL4] %s\n", linha);
                                fflush(stdout);
                                n = 0;
                            }
                        }, this, 0, ~0ULL);
        }


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
            u8 b[4];
            // `off` é u32 de propósito: pc pode estar em 0xb000xxxx+ (stubs L4), então
            // pc-4 excede INT_MAX e um cast `int` viraria negativo (seria sign-estendido
            // a u64 = endereço inválido no uc_mem_read). u32 evita a conversão negativa.
            u32 off = pc - 4;
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
        bool did_handoff = false;  // set quando um chaveamento cooperativo reescreveu PC/SP
        u32 kip_r1 = 0, kip_r2 = 0, kip_r3 = 0;

        // [PROBE-SYSCALL-HIST] diagnóstico temporário: conta cada syscall disparada.
        if (getenv("ZEEBO_SYSCALL_HIST")) {
            static std::map<u32,u64> s_hist;
            static u64 s_total = 0;
            s_hist[syscall]++; s_total++;
            if ((s_total & (s_total-1)) == 0 || s_hist[syscall] == 1) {
                fprintf(stderr, "[SYSHIST] pc=0x%08x syscall=0x%02x count=%llu total=%llu | ",
                        pc, syscall, (unsigned long long)s_hist[syscall], (unsigned long long)s_total);
                for (auto& kv : s_hist) fprintf(stderr, "0x%02x=%llu ", kv.first, (unsigned long long)kv.second);
                fprintf(stderr, "\n");
            }
        }

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
                            // Bug 2: salva o CONTEXTO DE CPU COMPLETO da thread que
                            // bloqueou no IPC (não só ip/sp). Mesmo helper central do
                            // ThreadSwitch — antes o IPC preservava apenas PC/SP.
                            if (cur_tid) {
                                zeebo_l4::CpuContext cctx{};
                                zeebo_l4::cpu_context_read(uc, &cctx);
                                sys->thread_table_.save_context(cur_tid, cctx);
                            }
                            cur->ip = pc;
                            cur->sp = sp_val;
                        }
                        sys->thread_table_.set_current_tid(next_tid);

                        // Bug 1: comuta o ADDRESS SPACE real para o SID da próxima
                        // thread ENTRE fatias (fora de hook — handler de escalonamento).
                        {
                            u32 nsid = sys->thread_table_.thread_space(next_tid);
                            if (std::getenv("ZEEBO_QW99"))
                                fprintf(stderr,"[QW99/IPC-sched] cur=0x%x next=0x%x nsid=0x%x has=%d spaces=%zu\n",
                                    cur_tid, next_tid, nsid, (int)sys->space_manager_.has_space(nsid),
                                    sys->space_manager_.space_count());
                            if (nsid && sys->space_manager_.has_space(nsid))
                                sys->space_manager_.activate(uc, nsid);
                        }

                        // Bug 2: restaura o CONTEXTO COMPLETO da próxima thread se
                        // houver; caso contrário usa ip/sp iniciais (primeira ativação).
                        zeebo_l4::CpuContext nctx{};
                        bool have_ctx = sys->thread_table_.load_context(next_tid, &nctx);
                        u32 target_ip = have_ctx ? nctx.pc : nxt->ip;
                        u32 target_sp = have_ctx ? nctx.sp : nxt->sp;
                        if (have_ctx) {
                            zeebo_l4::cpu_context_write(uc, nctx);
                            did_handoff = true;
                            if (sys->service_registry_.is_amss_thread(next_tid)) {
                                printf("[L4/IPC] Handoff(full-ctx) para AMSS/BREW thread %u @0x%08x (target: %s)\n",
                                       next_tid, target_ip, sys->boot_target() == 0 ? "AppMgr" : "Z-Wheel");
                            }
                            break;
                        }
                        // QW41: T-bit inferido na primeira ativação (LSB do PC).
                        bool want_thumb = (target_ip & 1) || sys->service_registry_.is_amss_thread(next_tid);
                        u32 pc_write = want_thumb ? (target_ip | 1u) : (target_ip & ~1u);
                        uc_reg_write(uc, UC_ARM_REG_PC, &pc_write);
                        if (target_sp) uc_reg_write(uc, UC_ARM_REG_SP, &target_sp);
                        did_handoff = true;
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
                        // Bug 2: salva o CONTEXTO DE CPU COMPLETO da thread atual
                        // (r0..r12, sp, lr, pc, cpsr — inclui o bit T = modo Thumb),
                        // não só ip/sp. Usa o helper CENTRALIZADO cpu_context_read,
                        // compartilhado com o caminho de IPC handoff.
                        if (cur_tid) {
                            zeebo_l4::CpuContext cctx{};
                            zeebo_l4::cpu_context_read(uc, &cctx);
                            sys->thread_table_.save_context(cur_tid, cctx);
                        }
                        sys->thread_table_.set_current_tid(next_tid);

                        // Bug 1: comuta o ADDRESS SPACE real para o SID da próxima
                        // thread ENTRE fatias (fora de qualquer hook — este handler
                        // roda no laço de escalonamento). Se o SID não tem regiões
                        // registradas, mantém a view atual (boot plano).
                        {
                            u32 nsid = sys->thread_table_.thread_space(next_tid);
                            if (nsid && sys->space_manager_.has_space(nsid))
                                sys->space_manager_.activate(uc, nsid);
                        }

                        // Bug 2: se a próxima thread tem contexto salvo, restaura o
                        // CONTEXTO COMPLETO via helper centralizado; caso contrário
                        // usa o ip/sp inicial do ExchangeRegisters.
                        zeebo_l4::CpuContext nctx{};
                        bool have_ctx = sys->thread_table_.load_context(next_tid, &nctx);
                        u32 target_ip = have_ctx ? nctx.pc : nxt->ip;
                        u32 target_sp = have_ctx ? nctx.sp : nxt->sp;
                        if (have_ctx) {
                            // Restauração completa e ABI-correta (r0..r12,SP,LR,CPSR,PC
                            // com T-bit via LSB do PC). cpu_context_write escreve CPSR
                            // antes de SP/LR (banked por modo).
                            zeebo_l4::cpu_context_write(uc, nctx);
                            did_handoff = true;
                            if (sys->service_registry_.is_amss_thread(next_tid)) {
                                printf("[L4/ThreadSwitch] Handoff(full-ctx) para AMSS/BREW thread %u @0x%08x (target: %s)\n",
                                       next_tid, target_ip, sys->boot_target() == 0 ? "AppMgr" : "Z-Wheel");
                            }
                            break;
                        }
                        // Sem contexto salvo: primeira ativação — usa ip/sp iniciais.
                        // QW41: T-bit inferido (escrita direta de PC com bit0).
                        bool want_thumb = (target_ip & 1) || sys->service_registry_.is_amss_thread(next_tid);
                        u32 pc_write = want_thumb ? (target_ip | 1u) : (target_ip & ~1u);
                        uc_reg_write(uc, UC_ARM_REG_PC, &pc_write);
                        if (target_sp) uc_reg_write(uc, UC_ARM_REG_SP, &target_sp);
                        did_handoff = true;
                        if (sys->service_registry_.is_amss_thread(next_tid)) {
                            printf("[L4/ThreadSwitch] Handoff para AMSS/BREW thread %u @0x%08x (target: %s)\n",
                                   next_tid, target_ip, sys->boot_target() == 0 ? "AppMgr" : "Z-Wheel");
                        }
                    }
                }
                break;
            }
            // L4_ThreadControl / L4_SpaceControl: retornam r0=1 (sucesso da ABI L4). O
            // contrato esperado pelos callers do Iguana é "thread/space criado com
            // sucesso"; retornar 1 mantém o boot avançando sem abortar o chamador.
            case 0x08: {                                     // L4_ThreadControl
                // ABI ARM OKL4 2.1.1 (threadcontrol.spp): r0=dest,
                // r1=SpaceSpecifier(SID), r2=Scheduler, r3=Pager. O SID de uma
                // thread vem DAQUI (SpaceSpecifier), não do r5 do ExchangeRegisters.
                u32 tc_dest=0, tc_space=0, tc_sched=0, tc_pager=0;
                uc_reg_read(uc, UC_ARM_REG_R0, &tc_dest);
                uc_reg_read(uc, UC_ARM_REG_R1, &tc_space);
                uc_reg_read(uc, UC_ARM_REG_R2, &tc_sched);
                uc_reg_read(uc, UC_ARM_REG_R3, &tc_pager);
                sys->thread_table_.on_thread_control(tc_dest, tc_space, tc_sched, tc_pager);
                res_r0 = 1;
                break;
            }
            case 0x0c: {                                     // L4_ExchangeRegisters
                u32 dest = 0, control = 0, new_sp = 0, new_ip = 0, flags = 0;
                uc_reg_read(uc, UC_ARM_REG_R0, &dest);
                uc_reg_read(uc, UC_ARM_REG_R1, &control);
                uc_reg_read(uc, UC_ARM_REG_R2, &new_sp);
                uc_reg_read(uc, UC_ARM_REG_R3, &new_ip);
                uc_reg_read(uc, UC_ARM_REG_R4, &flags);
                res_r0 = dest; // L4_ExchangeRegisters retorna o dest ThreadId
                if (getenv("ZEEBO_SYSCALL_HIST")) {
                    fprintf(stderr, "[EXREGS] dest=0x%08x control=0x%08x new_sp=0x%08x new_ip=0x%08x flags=0x%08x DELIVER=%d\n",
                            dest, control, new_sp, new_ip, flags, (control & zeebo_l4::EXREGS_CTRL_DELIVER) ? 1 : 0);
                }
                sys->thread_table_.on_exchange_registers(dest, control, new_sp, new_ip, flags);
                // Bug 2: r5 do ExchangeRegisters é UserDefHandle, NÃO o SID.
                // (ABI ARM OKL4 2.1.1 exchangeregisters.spp: [sp#40]=r5=UserDefHandle;
                //  o SID vem do SpaceSpecifier/r1 do ThreadControl, tratado no case 0x08).
                // Antes o SID era lido erroneamente de r5, associando o espaço de
                // uma thread ao seu handle de usuário. Aqui só guardamos o handle e,
                // se a thread ainda não tem SID, herdamos o da thread corrente
                // (mesmo AS — comportamento correto para clones no mesmo espaço).
                {
                    u32 udh = 0;
                    uc_reg_read(uc, UC_ARM_REG_R5, &udh);
                    sys->thread_table_.set_user_def_handle(dest, udh);
                    if (sys->thread_table_.thread_space(dest) == 0) {
                        u32 inherit = sys->thread_table_.thread_space(sys->thread_table_.current_tid());
                        if (inherit) sys->thread_table_.set_thread_space(dest, inherit);
                    }
                }
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
                // Bug 1: registra o mapeamento na VTLB do space_id (SpaceMap) além
                // da view ativa vtlb_. Assim consultas por SID veem só o que aquele
                // espaço mapeou, sem fundir tasks distintas no mesmo VA.
                std::vector<zeebo_l4::MapItem> mc_items;
                res_r0 = zeebo_l4::handle_map_control(uc, utcb_ptr, sid, control,
                             /*out_items=*/&mc_items,
                             sys->apps_pool_.host ? &sys->apps_pool_ : nullptr,
                             sys->apps_pool_.host ? &sys->space_map_ : (zeebo_l4::SpaceMap*)nullptr);
                // Bug 1 (runtime): registra as regiões deste SID no SpaceManager
                // para permitir a comutação REAL de address space (activate) entre
                // fatias. Só regiões com backing físico na pool (host_of != null)
                // podem ser re-mapeadas por ponteiro; as demais permanecem na view
                // plana e são ignoradas aqui (não há host_ptr para aliasar).
                if (sys->apps_pool_.host) {
                    for (const auto& it : mc_items) {
                        if (it.fpage.is_nil() || it.fpage.is_whole_space()) continue;
                        u64 va   = it.fpage.vaddr();
                        u64 size = it.fpage.size_bytes();
                        u64 phys = it.phys.phys_base();
                        u8* hp   = sys->apps_pool_.host_of(phys);
                        if (std::getenv("ZEEBO_QW99") && (va <= 0xb04151a4ull && 0xb04151a4ull < va+size))
                            fprintf(stderr,"[QW99/MC-record] sid=0x%x va=0x%llx size=0x%llx phys=0x%llx hp=%p contains=%d COVERS_b04151a4\n",
                                sid,(unsigned long long)va,(unsigned long long)size,(unsigned long long)phys,(void*)hp,
                                (int)sys->apps_pool_.contains(phys,size));
                        if (!hp || !sys->apps_pool_.contains(phys, size)) continue;
                        int prot = zeebo_l4::fpage_to_uc_prot(it.fpage);
                        sys->space_manager_.record(sid, va, size, prot, hp);
                    }
                }
                // Mantém a view ativa (fastmem plano) sincronizada com o espaço atual.
                if (sys->apps_pool_.host) {
                    zeebo_l4::handle_map_control(uc, utcb_ptr, sid, control,
                                 nullptr, nullptr, &sys->vtlb_);
                }
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
                // NOTA (test_l4_kip_trap.cpp): NÃO escrever em sp+0/4/8 aqui. No momento
                // do intr hook o SP é o SP DA TRAP (o stub 0xb000c720 fez `mvn sp,#0x4b`
                // => SP=0xFFFFFFB4), scratch que nenhum caminho real do firmware lê. O
                // frame do chamador {r4,r5,r6} vive em `ip` (== SP antigo). A escrita
                // extra era espúria (grava em 0xFFFFFFB4/B8/BC, nunca lido); só escrever
                // em ip+0/4/8 corromperia o r4 salvo (ptr do cache de páginas) — o teste
                // reproduz esse mutante como RED e prova o caminho atual como GREEN.
                break;
            }
            default: res_r0 = 0; break;                      // demais (kputc etc.) no-op
        }

        if (set_kip_ret) {
            uc_reg_write(uc, UC_ARM_REG_R1, &kip_r1);
            uc_reg_write(uc, UC_ARM_REG_R2, &kip_r2);
            uc_reg_write(uc, UC_ARM_REG_R3, &kip_r3);
        }

        // Intercepta e inicializa o espaço de vídeo quando Iguana entra em execução.
        // HEURÍSTICA: o primeiro SVC vindo do range Iguana (pc >= 0xb0000000) marca
        // que o kernel Iguana está ativo — é o ponto onde o boot já ultrapassou as
        // inits e começa a despachar servidores; inicializar o espaço de vídeo aqui
        // garante que o Adreno 130 esteja "ligado" antes do primeiro draw de um
        // applet, sem precisar esperar um sinal explícito de frame. O valor 0x0020
        // em 0x010c é o kick inicial de drawings do Adreno (estado esperado pelo
        // rasterizador ao começar a receber chamadas IGL/GLES); é um hook único
        // (s_gpu_inited), executado apenas na primeira vez.
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
        // QW41: quando o SVC dispara a partir de código FORA dos stubs L4 fixos
        // do kernel (0xb0000000-0xb0020000, todos ARM), a exceção real de
        // hardware zera o T-bit no CPSR — mas o AMSS/BREW é 100% Thumb. Sem
        // restaurar o T-bit ao retomar, o Unicorn decodifica o próximo bloco
        // como ARM e falha com UC_ERR_INSN_INVALID assim que o svc não é dos
        // stubs L4 (ex.: 0x103dcd18, chamada real do AMSS). Os stubs do
        // kernel continuam ARM (T=0); qualquer outro chamador retoma em Thumb.
        // NOTA: setar CPSR aqui não sobrevive, pois cada branch abaixo
        // reescreve PC via uc_reg_write logo em seguida — o Unicorn deriva
        // o T-bit do LSB do PC escrito (convenção BX real), não do CPSR
        // setado manualmente antes. Por isso o bit é aplicado diretamente em
        // cada `target_pc = pc;` abaixo, não como side-effect de CPSR aqui.
        bool caller_is_kernel_stub = (pc >= 0xb0000000u && pc < 0xb0020000u);
        // QW43: ig_naming (0xb0100000-0xb0120000) mantém UMA CÓPIA LOCAL ARM dos stubs
        // de trap L4 (confirmada por disassembly svc #0x1400/0x140c e por source OKL4).
        // Aplicando o guard aqui (uma vez, p/ TODOS os syscalls) em vez de só no 0x0c,
        // o retorno de L4_Ipc (0x00) do ig_naming volta a decodificar ARM — stall 0xb010333a
        // era decode drift por Thumb forçado, não deadlock.
        bool caller_is_ig_naming_arm = (pc >= 0xb0100000u && pc < 0xb0120000u);
        // QW41: helper que injeta o bit T (LSB, convenção BX) em target_pc antes
        // de cada retomada. Deve ser chamado logo antes de CADA
        // `uc_reg_write(uc, UC_ARM_REG_PC, &target_pc)` neste bloco (os stubs L4
        // continuam ARM; qualquer retomada fora deles é Thumb no AMSS/BREW).
        auto apply_tbit = [&](u32 v) -> u32 {
            return (caller_is_kernel_stub || caller_is_ig_naming_arm) ? (v & ~1u) : (v | 1u);
        };
        if (syscall == 0xb4) {
            // UC_HOOK_INTR delivers pc already at svc+4; resume at pc (not pc+4),
            // otherwise we double-advance to svc+8 and skip one guest instruction (QW17).
            target_pc = apply_tbit(pc);
            uc_reg_write(uc, UC_ARM_REG_PC, &target_pc);
            if (ip) uc_reg_write(uc, UC_ARM_REG_SP, &ip);
            // Invalida o TB de execução no Unicorn para que ele recompile o bloco seguinte
            uc_ctl_remove_cache(uc, 0xb000c720, 0x100);
            uc_ctl_remove_cache(uc, 0xb00033d0, 0x100);
        } else if (syscall == 0x00) {
            if (did_handoff) {
                // Handoff cooperativo já reescreveu PC/SP para a thread alvo
                // (ex.: AMSS/BREW @0x10137000). NÃO sobrescrever de volta para
                // o wrapper IPC (0xb000c834), senão o servidor fica preso no wait.
                uc_reg_read(uc, UC_ARM_REG_PC, &target_pc);
                uc_ctl_remove_cache(uc, target_pc, 0x40);
            } else {
                target_pc = apply_tbit(pc); // pc already == svc+4 (0xb000c834: pop {r1, r2}); QW17: no extra +4
                if (ip) uc_reg_write(uc, UC_ARM_REG_SP, &ip);
                uc_reg_write(uc, UC_ARM_REG_PC, &target_pc);
                uc_ctl_remove_cache(uc, 0xb000c800, 0x100);
            }
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
            // QW42: full-trace (ZEEBO_FULL_TRACE) confirmou que o T-bit muda
            // de 0 (ARM) para 1 (Thumb) exatamente na transição
            // 0xb0102c28->0xb0102c2c — ou seja, DENTRO desta mesma svc
            // #0x140c, mas disparada por uma CÓPIA LOCAL do trap-stack
            // embutida em ig_naming (região 0xb0100000-0xb0120000, ARM puro),
            // não pelo stub fixo 0xb000c758. apply_tbit(pc) força Thumb
            // porque pc cai fora de 0xb0000000-0xb0020000, mas essa cópia
            // local também é ARM. Fix restrito a este case: tratar a faixa
            // ig_naming (0xb0100000-0xb0120000) como ARM também, sem alterar
            // o comportamento fora dela (preserva o handoff cooperativo real
            // que usa o stub fixo em 0xb000c758..0xb000c794). PROVADO por
            // execução real: o boot passa a avançar de fato até 0xb0358bb4,
            // 0xb03ba634 e um loop de memcpy legítimo em 0xb0400064
            // (ldrb/strb/subs/bne — código válido copiando dados, não bug).
            bool local_arm_copy = (pc >= 0xb0100000u && pc < 0xb0120000u);
            target_pc = (caller_is_kernel_stub || local_arm_copy) ? (pc & ~1u) : (pc | 1u);
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
            target_pc = apply_tbit(pc);
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
            target_pc = apply_tbit(pc);
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
            target_pc = apply_tbit(pc);
            if (ip) uc_reg_write(uc, UC_ARM_REG_SP, &ip);
            uc_reg_write(uc, UC_ARM_REG_PC, &target_pc);
            uc_ctl_remove_cache(uc, 0xb000c944, 0x20);
        } else if (syscall == 0x04) { // L4_ThreadSwitch (QW27)
            // Stub 0xb000c7b8: push {r4-r8,sb,sl,fp,lr}; mov ip,sp; mvn sp,#0xfb;
            //   svc #0x1404; pop {r4-r8,sb,sl,fp,pc} (em 0xb000c7c8).
            // pc == svc+4 (0xb000c7c8 = pop). Retomar em pc (SP=ip) executa o pop
            // e restaura os callee-saved; o else (target_pc=lr) pularia o pop e
            // corromperia r4-r11 do chamador (mesmo padrão QW19/QW26).
            if (did_handoff) {
                // Handoff cooperativo já reescreveu PC/SP para a thread alvo;
                // não sobrescrever de volta o wrapper do ThreadSwitch.
                uc_reg_read(uc, UC_ARM_REG_PC, &target_pc);
                uc_ctl_remove_cache(uc, target_pc, 0x40);
            } else {
                target_pc = apply_tbit(pc);
                if (ip) uc_reg_write(uc, UC_ARM_REG_SP, &ip);
                uc_reg_write(uc, UC_ARM_REG_PC, &target_pc);
                uc_ctl_remove_cache(uc, 0xb000c7b8, 0x14);
            }
        } else if (syscall == 0x10) { // L4_Schedule (QW27)
            // Stub 0xb000c7cc: push {r4-r8,sb,sl,fp,lr}; ldr r4,[sp,#0x24];
            //   ldr r5,[sp,#0x28]; mov ip,sp; mvn sp,#0xef; svc #0x1410;
            //   ldr r7,[sp,#0x2c]; ldr r8,[sp,#0x30]; cmp r7,#0; strne r1,[r7];
            //   cmp r8,#0; strne r2,[r8]; pop {r4-r8,sb,sl,fp,pc} (em 0xb000c7fc).
            // pc == svc+4 (0xb000c7e4) = ldr/cmp/strne writebacks + pop. Retomar em
            // pc (SP=ip) roda os dois writebacks e o pop; o else pularia tudo,
            // corrompendo callee-saved e descartando as saídas.
            target_pc = apply_tbit(pc);
            if (ip) uc_reg_write(uc, UC_ARM_REG_SP, &ip);
            uc_reg_write(uc, UC_ARM_REG_PC, &target_pc);
            uc_ctl_remove_cache(uc, 0xb000c7cc, 0x34);
        } else {
            if (ip) uc_reg_write(uc, UC_ARM_REG_SP, &ip);
            if (lr) {
                // QW41: LSB de lr já indica o T-bit real (convenção BX); preservar
                // no próprio PC escrito, não via CPSR (não confiável no Unicorn).
                target_pc = lr;
                uc_reg_write(uc, UC_ARM_REG_PC, &target_pc);
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
        // [QW88] Cacheia a flag: este hook executa uma vez por instrução.
        static const bool table_debug_enabled = std::getenv("ZEEBO_TBL_DBG") != nullptr;
        if (table_debug_enabled && (uint32_t)ad >= 0xb040001c && (uint32_t)ad <= 0xb0400034) {
            static uint64_t step = 0;
            if ((uint32_t)ad == 0xb0400024) { // ldm sl!, {r0,r1,r2,r3}
                u32 sl=0, fp=0, sp=0;
                uc_reg_read(uc, UC_ARM_REG_R10, &sl);
                uc_reg_read(uc, UC_ARM_REG_R11, &fp);
                uc_reg_read(uc, UC_ARM_REG_SP, &sp);
                u32 ent[4] = {0};
                uc_mem_read(uc, sl, ent, 16);
                fprintf(stderr, "[QW88] step=%llu sl=0x%08x fp=0x%08x sp=0x%08x -> ent: src=0x%08x dst=0x%08x len=0x%x h=0x%08x\n",
                        (unsigned long long)++step, sl, fp, sp, ent[0], ent[1], ent[2], ent[3]);
            } else if ((uint32_t)ad == 0xb0400020) { // beq 0xb0410070
                u32 cpsr=0; uc_reg_read(uc, UC_ARM_REG_CPSR, &cpsr);
                bool z = (cpsr >> 30) & 1;
                fprintf(stderr, "[QW88] beq b0410070: Z=%d (fim da tabela)\n", (int)z);
            }
        }
        ZeeboLLESystem* sys = (ZeeboLLESystem*)ud;
        sys->core0_.insns++;

        // [QW99] Instrumentação read-only da entrada do scatterload em 0xb0400000.
        // Loga TID, SID ativo, SP/LR, offset físico de backing e hash de 252B em
        // 0xb04151a4, a cada entrada em 0xb0400000. Gated por ZEEBO_QW99.
        static const bool qw99_on = std::getenv("ZEEBO_QW99") != nullptr;
        if (qw99_on && (uint32_t)ad == 0xb0400000) {
            static uint64_t q_entries = 0;
            ++q_entries;
            u32 sp=0, lr=0;
            uc_reg_read(uc, UC_ARM_REG_SP, &sp);
            uc_reg_read(uc, UC_ARM_REG_LR, &lr);
            u32 tid = sys->thread_table_.current_tid();
            u32 tsid = sys->thread_table_.thread_space(tid);
            u32 asid = sys->space_manager_.active_sid();
            bool has = sys->space_manager_.has_space(tsid);
            // backing físico de 0xb04151a4 na pool APPS (host_of via VA identidade? não;
            // usamos a regiao registrada no SpaceManager para o SID da thread).
            u8 buf[252]={0};
            uc_mem_read(uc, 0xb04151a4, buf, sizeof(buf));
            // FNV-1a 32-bit hash (determinístico, sem deps)
            u32 h=2166136261u; for (size_t i=0;i<sizeof(buf);i++){h^=buf[i];h*=16777619u;}
            // offset de backing: procura a regiao do SID cobrindo 0xb0415000
            long backoff = -1; const void* hostp=nullptr;
            if (auto* regs = sys->space_manager_.regions_of(tsid)) {
                for (auto& r : *regs) {
                    if (0xb04151a4ull >= r.va && 0xb04151a4ull < r.va + r.size) {
                        backoff = (long)(0xb04151a4ull - r.va);
                        hostp = r.host ? (r.host + backoff) : nullptr;
                        break;
                    }
                }
            }
            fprintf(stderr,
                "[QW99] entry#%llu tid=0x%x tsid=0x%x active_sid=0x%x has_space=%d sp=0x%08x lr=0x%08x "
                "b04151a4:off=%ld host=%p fnv=0x%08x b0=%02x%02x%02x%02x sm_spaces=%zu\n",
                (unsigned long long)q_entries, tid, tsid, asid, (int)has, sp, lr,
                backoff, hostp, h, buf[0],buf[1],buf[2],buf[3],
                sys->space_manager_.space_count());
        }

        // Traco de execucao do Core0. Ambos os backends passam por este hook,
        // entao gravar aqui produz trajetorias comparaveis instrucao a
        // instrucao -- ao contrario do log periodico, que amostra a cada 10k
        // instrucoes e torna impossivel achar a PRIMEIRA divergencia.
        if (sys->trace_file_) {
            if (sys->trace_limit_ == 0 || sys->core0_.insns <= sys->trace_limit_) {
                u32 r[16];
                for (int i = 0; i < 15; i++) uc_reg_read(uc, UC_ARM_REG_R0 + i, &r[i]);
                u32 cpsr = 0;
                uc_reg_read(uc, UC_ARM_REG_CPSR, &cpsr);
                u32 opc = 0;
                uc_mem_read(uc, (u32)ad, &opc, 4);
                // CPSR COMPLETO, nao so os bits de flag: o modo (bits [4:0]) e
                // indispensavel para instrucoes com banco de registradores
                // (LDM_usr/STM_usr) e para diagnosticar retorno de excecao.
                // Os bits NZCV seguem nos 4 bits altos, entao a comparacao de
                // tracos antiga continua valida.
                fprintf(sys->trace_file_, "%llu %08x %08x %08x",
                        (unsigned long long)sys->core0_.insns, (u32)ad,
                        cpsr, opc);
                for (int i = 0; i < 15; i++) fprintf(sys->trace_file_, " %08x", r[i]);
                fputc('\n', sys->trace_file_);
            } else if (sys->trace_limit_ != 0) {
                fclose(sys->trace_file_);
                sys->trace_file_ = nullptr;
            }
        }

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

    static bool is_core0_peripheral(uint32_t addr) {
        if (addr >= MSM_CSR_BASE + 0x400 && addr <= MSM_CSR_BASE + 0x440) return true; // Doorbell
        if (addr == SMEM_BASE + 0x00 || addr == SMEM_BASE + 0x04) return true;          // ProcComm
        if (addr >= MSM_MDDI_BASE && addr < MSM_MDDI_BASE + MDDI_SIZE) return true;     // MDDI
        if (addr >= ADRENO130_BASE && addr < ADRENO130_BASE + ADRENO130_SIZE) return true; // Adreno
        if ((addr >= UART1_BASE && addr < UART1_BASE + UART_SIZE) ||
            (addr >= UART2_BASE && addr < UART2_BASE + UART_SIZE) ||
            (addr >= UART3_BASE && addr < UART3_BASE + UART_SIZE)) return true;         // UARTs
        if (addr >= KEYPAD_BASE && addr < KEYPAD_BASE + KEYPAD_SIZE) return true;       // Keypad
        return false;
    }

    void handle_peripheral_write(uint32_t addr, int /*size*/, uint32_t value) {
        // Inter-core doorbell A2M
        if (addr >= MSM_CSR_BASE + 0x400 && addr <= MSM_CSR_BASE + 0x440) {
            u32 int_num = (addr - (MSM_CSR_BASE + 0x400)) / 4;
            printf("[Doorbell A2M] Core 0 -> Core 1 INT #%u (val=0x%x)\n", int_num, value);
            if (core1_state_ && core1_state_->uc) {
                u32 vic_status0 = 0;
                uc_mem_read(core1_state_->uc, MSM_VIC_BASE, &vic_status0, 4);
                vic_status0 |= (1 << int_num);
                uc_mem_write(core1_state_->uc, MSM_VIC_BASE, &vic_status0, 4);

                // Inject RPC packets on doorbell trigger using official IDs AUDMGR (0x30000013) / ADSPRTOSATOM (0x3000000a)
                if (smd_) {
                    std::vector<u8> dummy_payload(16, 0x42);
                    smd_->inject_packet(core1_state_->uc, 0x30000013, 0x1b59, dummy_payload);
                    smd_->inject_packet(core1_state_->uc, 0x3000000a, 0x02, dummy_payload);
                }
            }
        }
        // ProcComm command write by Core 0 (bug 6). The old path unconditionally
        // wrote PCOM_CMD_SUCCESS from Core 0 itself — fabricated success. Now the
        // command becomes VISIBLE in shared SMEM (Core0 issue), then a modeled
        // Core 1 service step produces the completion with an HONEST,
        // command-specific status (unsupported stays unsupported).
        else if (addr == SMEM_BASE + 0x00) { // APP_COMMAND
            u32 cmd = value;
            // Make the command visible through the shared backing (both cores).
            proccomm_.core0_issue(cmd);
            printf("[ProcComm] Core 0 issued command 0x%x (visible in SMEM, awaiting modem)\n", cmd);

            // Core 1 (modem) services the pending command. This is the only path
            // that produces completion; it writes an honest status.
            bool serviced = proccomm_.core1_service();
            if (serviced) {
                u32 st = proccomm_.status();
                printf("[ProcComm] Core 1 serviced command 0x%x -> status=%s (0x%x)\n",
                       cmd,
                       st == zeebo::PCOM_CMD_SUCCESS ? "SUCCESS" : "FAIL_UNSUPPORTED",
                       st);
            }

            // Mirror the shared-backing completion words into the Unicorn view so
            // a guest polling APP_COMMAND/APP_STATUS observes the same bytes.
            if (core0_.uc) {
                u32 done = proccomm_.rd(zeebo::PCOM_OFF_APP_COMMAND);
                u32 st   = proccomm_.rd(zeebo::PCOM_OFF_APP_STATUS);
                uc_mem_write(core0_.uc, SMEM_BASE + 0x04, &st, 4);
                uc_mem_write(core0_.uc, SMEM_BASE + 0x00, &done, 4);
            }
            vtlb_.write_u32(SMEM_BASE + 0x04, proccomm_.rd(zeebo::PCOM_OFF_APP_STATUS));
            vtlb_.write_u32(SMEM_BASE + 0x00, proccomm_.rd(zeebo::PCOM_OFF_APP_COMMAND));
        }
        // MDDI write
        else if (addr >= MSM_MDDI_BASE && addr < MSM_MDDI_BASE + MDDI_SIZE) {
            mddi_->write((u32)(addr - MSM_MDDI_BASE), value);
        }
        // Adreno GPU write
        else if (addr >= ADRENO130_BASE && addr < ADRENO130_BASE + ADRENO130_SIZE) {
            gpu_->write((u32)(addr - ADRENO130_BASE), value);
        }
        // UART TX FIFO write -> console com identificador explícito [UART#1, #2, #3]
        else if ((addr >= UART1_BASE && addr < UART1_BASE + UART_SIZE) ||
                 (addr >= UART2_BASE && addr < UART2_BASE + UART_SIZE) ||
                 (addr >= UART3_BASE && addr < UART3_BASE + UART_SIZE)) {
            const int uart_id = (addr >= UART1_BASE && addr < UART1_BASE + UART_SIZE) ? 1 :
                                (addr >= UART2_BASE && addr < UART2_BASE + UART_SIZE) ? 2 : 3;
            const u32 base = (addr >= UART1_BASE && addr < UART1_BASE + UART_SIZE) ? UART1_BASE :
                             (addr >= UART2_BASE && addr < UART2_BASE + UART_SIZE) ? UART2_BASE : UART3_BASE;
            const u32 off = (u32)(addr - base);
            const unsigned char ch = (unsigned char)(value & 0xFF);

            // Trata escritas no TX FIFO (offset 0x0C) ou no registrador de dados da UART
            if (off == UART_OFF_TF || off == 0x00) {
                if (ch == '\n') {
                    fprintf(stderr, "[UART#%d] %s\n", uart_id, uart_buffers_[uart_id].c_str());
                    uart_buffers_[uart_id].clear();
                } else if (ch == '\r') {
                    /* swallow CR */
                } else if (ch >= 0x20 || ch == '\t') {
                    uart_buffers_[uart_id] += (char)ch;
                    if (uart_buffers_[uart_id].size() >= 200) {
                        fprintf(stderr, "[UART#%d] %s\n", uart_id, uart_buffers_[uart_id].c_str());
                        uart_buffers_[uart_id].clear();
                    }
                }
            }
        }
        // Keypad write
        else if (addr >= KEYPAD_BASE && addr < KEYPAD_BASE + KEYPAD_SIZE) {
            input_->write((u32)(addr - KEYPAD_BASE), value);
        }
    }

    uint32_t handle_peripheral_read(uint32_t addr, int size) {
        if (size != 4 && size != 2 && size != 1) return 0;
        const bool is_uart = (addr >= UART1_BASE && addr < UART3_BASE + UART_SIZE);
        if (is_uart) {
            const u32 base = (addr >= UART1_BASE && addr < UART1_BASE + UART_SIZE) ? UART1_BASE
                           : (addr >= UART2_BASE && addr < UART2_BASE + UART_SIZE) ? UART2_BASE
                           : UART3_BASE;
            const u32 off = (u32)(addr - base);
            if (off == UART_OFF_SR) {
                return 0x000C; // TX_READY | TX_EMPTY
            }
        }
        // Registradores de video: as janelas MDDI/Adreno sao mapeadas como RAM
        // comum no Unicorn, entao sem este roteamento o guest lia sempre o
        // conteudo de fundo (zero) em vez do modelo -- CHIP_ID vinha 0.
        if (addr >= MSM_MDDI_BASE && addr < MSM_MDDI_BASE + MDDI_SIZE) {
            if (mddi_) return mddi_->read((u32)(addr - MSM_MDDI_BASE));
        }
        if (addr >= ADRENO130_BASE && addr < ADRENO130_BASE + ADRENO130_SIZE) {
            if (gpu_) return gpu_->read((u32)(addr - ADRENO130_BASE));
        }
        u32 val = 0;
        vtlb_.read_u32(addr, &val);
        return val;
    }

    static void c0_mem_hook(uc_engine* uc, uc_mem_type type, uint64_t addr, int size, int64_t value, void* ud) {
        ZeeboLLESystem* sys = (ZeeboLLESystem*)ud;
        if (type == UC_MEM_WRITE) {
            if (sys->watch_file_) {
                uint32_t pc = 0;
                uc_reg_read(uc, UC_ARM_REG_PC, &pc);
                sys->note_write(0, (uint32_t)addr, size, (uint32_t)value, pc);
            }
            sys->handle_peripheral_write((uint32_t)addr, size, (uint32_t)value);
        }
    }

    // Leitura de registrador de periférico no Core 0. O Unicorn lê a RAM de fundo
    // (mapeada com uc_mem_map); para os periféricos MODELADOS (UART), injetamos o
    // valor de status antes da leitura — análogo ao c1_heap_read_hook. UART_SR
    // (offset 0x08) retorna TX_READY|TX_EMPTY (0x0C) para o console não travar em
    // espera de FIFO; outros offsets mantêm a RAM (modelo neutro).
    static void c0_mem_read_hook(uc_engine* uc, uc_mem_type type, uint64_t addr, int size, int64_t, void* ud) {
        (void)ud; (void)type;
        ZeeboLLESystem* sys = (ZeeboLLESystem*)ud;
        if (size != 4 && size != 2 && size != 1) return;

        // Janelas de video: injeta o valor do modelo antes de o Unicorn
        // devolver a RAM de fundo. Sem isto UnifiedMDDI::read/UnifiedAdreno130::read
        // nunca eram chamados em leitura (apenas escritas eram roteadas).
        if ((addr >= MSM_MDDI_BASE && addr < MSM_MDDI_BASE + MDDI_SIZE) ||
            (addr >= ADRENO130_BASE && addr < ADRENO130_BASE + ADRENO130_SIZE)) {
            const u32 v = sys->handle_peripheral_read((u32)addr, size);
            uc_mem_write(uc, addr, &v, (size_t)size);
            return;
        }

        const bool is_uart =
            (addr >= UART1_BASE && addr < UART3_BASE + UART_SIZE);
        if (!is_uart) return;
        const u32 base = (addr >= UART1_BASE && addr < UART1_BASE + UART_SIZE) ? UART1_BASE
                       : (addr >= UART2_BASE && addr < UART2_BASE + UART_SIZE) ? UART2_BASE
                       : UART3_BASE;
        const u32 off = (u32)(addr - base);
        if (off == UART_OFF_SR) {
            const u32 ready = sys->handle_peripheral_read((u32)addr, size);
            uc_mem_write(uc, addr, &ready, (size_t)size);
        }
    }

    // Slide-detector (bug 8): roda uma vez POR BLOCO BASICO do Core1, nao por
    // instrucao. size = bytes do bloco; size/4 = insns lineares sem branch
    // tomado. Nenhum opcode e' lido no caminho quente. Preserva a mesma
    // semantica de diagnostico do detector antigo (halt + mensagem).
    static void c1_block_hook(uc_engine* uc, uint64_t ad, uint32_t size, void* ud) {
        ZeeboLLESystem* sys = (ZeeboLLESystem*)ud;
        CoreState& c = sys->core1_;
        if (c.slide.on_block((u32)ad, size / 4)) {
            printf("\n[Core1][SLIDE-DETECT] NOP-slide/derail detectado @0x%08x "
                   "(run=%u insns lineares sem branch tomado, limite=%u). Entry "
                   "provavelmente errado — abortando execucao do Core1 em vez de "
                   "rodar cego ate 0xfffffe.\n",
                   (u32)ad, c.slide.linear_run, c.slide.slide_limit);
            fflush(stdout);
            c.halted = true;
            uc_emu_stop(uc);
        }
    }

    static void c1_code_hook(uc_engine* uc, uint64_t ad, uint32_t size, void* ud) {
        ZeeboLLESystem* sys = (ZeeboLLESystem*)ud;
        sys->core1_.insns++;

        // ── [TCB-PROBE] instrumentacao temporaria da cadeia do alocador ──────
        // Cacheia a flag: getenv por instrução domina o custo do hook.
        static const bool tcb_probe_enabled = std::getenv("ZEEBO_TCB_PROBE") != nullptr;
        // Cadeia provada por desassemblagem estatica:
        //   allocate_tcb(f0007008) -> bitmap_alloc(f00067e4) -> refill(f00065f0)
        //   -> pool_alloc(f0002b7c) com pool head 0xf001a508, pedido 0x1000.
        // Objetivo: ver POR QUE f0002b7c devolve 0 (=> panic thread.cc:1273).
        if (tcb_probe_enabled) {
            u32 pc = (u32)ad;
            auto rd = [&](u32 a)->u32 { u32 v=0; uc_mem_read(uc,a,&v,4); return v; };
            auto reg = [&](int r)->u32 { u32 v=0; uc_reg_read(uc,r,&v); return v; };
            if (pc == 0xf0007008) {
                // CORRECAO: f001a52c e a BASE da struct (o init escreve via `ip`),
                // NAO um ponteiro. Ler os campos diretamente.
                const u32 obj = 0xf001a52c;
                fprintf(stderr,"[TCB] allocate_tcb ENTRA  obj=f001a52c campos:"
                        " +0=0x%08x +4=0x%08x +8=0x%08x +c=0x%08x"
                        " | h+10=0x%04x h+12=0x%04x h+14=0x%04x\n",
                        rd(obj), rd(obj+4), rd(obj+8), rd(obj+0xc),
                        (u16)(rd(obj+0x10)&0xffff), (u16)(rd(obj+0x10)>>16),
                        (u16)(rd(obj+0x14)&0xffff));
                {   // ARBITRO: o que diz o SHADOW (visao de dados do Split I/D)?
                    u32 off = 0xf001a538u - REX_HEAP_VA_BASE, sh = 0;
                    if (off + 4 <= sys->rex_heap_shadow_.size())
                        memcpy(&sh, &sys->rex_heap_shadow_[off], 4);
                    fprintf(stderr,"[TCB]   split_id=%d shadow[f001a538]=0x%08x uc[f001a538]=0x%08x %s\n",
                            (int)sys->rex_split_id_, sh, rd(0xf001a538),
                            (sh==0x100 && rd(0xf001a538)==0) ? "<== SHADOW OK, UC CLOBBERED" : "");
                }
            } else if (pc == 0xf00067e4) {
                fprintf(stderr,"[TCB] bitmap_alloc ENTRA  r0=0x%08x [obj+4]=0x%08x"
                        " [obj+8]=0x%08x [obj+c]=0x%08x\n", reg(UC_ARM_REG_R0),
                        rd(reg(UC_ARM_REG_R0)+4), rd(reg(UC_ARM_REG_R0)+8),
                        rd(reg(UC_ARM_REG_R0)+0xc));
            } else if (pc == 0xf00065f0) {
                fprintf(stderr,"[TCB] refill ENTRA        pool_head@f001a508=0x%08x\n",
                        rd(0xf001a508));
            } else if (pc == 0xf0002b7c) {
                fprintf(stderr,"[TCB] pool_alloc ENTRA    r0=0x%08x r1=0x%08x(tam) r2=0x%08x"
                        "  [pool]=0x%08x\n", reg(UC_ARM_REG_R0), reg(UC_ARM_REG_R1),
                        reg(UC_ARM_REG_R2), rd(0xf001a508));
            } else if (pc == 0xf000660c) {
                fprintf(stderr,"[TCB] pool_alloc RETORNA  r0=0x%08x  %s\n",
                        reg(UC_ARM_REG_R0),
                        reg(UC_ARM_REG_R0)==0 ? "<== NULL! causa do panic" : "ok");
            } else if (pc == 0xf0016bec) {
                // CORRECAO (QW59): f0016bec e' o `beq f0016d14`, ou seja o TESTE,
                // nao o panic. Ele e' executado em TODO boot, com ou sem falha --
                // o rotulo antigo ("ponto do panic alcancado") era falso positivo
                // e me fez ler boots saudaveis como panic. O panic real e' o
                // destino f0016d14; o printf de thread.cc:1273 fica em f0016bac.
                fprintf(stderr,"[TCB] teste f0016bec (beq): %s\n",
                        (reg(UC_ARM_REG_R0) & 0xff) ? "passou" : "vai desviar p/ f0016d14");
            } else if (pc == 0xf001681c) {
                fprintf(stderr,"[TCB] init_tcb_allocator ENTRA (f001681c)\n");
            } else if (pc == 0xf00162d8) {
                fprintf(stderr,"[TCB] call-site do init ALCANCADO (f00162d8)\n");
            } else if (pc == 0xf001682c) {
                // logo APOS `str r3,[ip,#0xc]` (r3=0x100) em 0xf0016828
                u32 ip=0; uc_reg_read(uc,UC_ARM_REG_IP,&ip);
                fprintf(stderr,"[TCB] POS-STORE ip=0x%08x  [ip+c]=0x%08x  (esperado 0x00000100) %s\n",
                        ip, rd(ip+0xc), rd(ip+0xc)==0x100?"ok":"<== ESCRITA PERDIDA");
            } else if (pc == 0xf0016870 || pc == 0xf0016880 || pc == 0xf00168a0
                       || pc == 0xf0016900 || pc == 0xf0016a00 || pc == 0xf0016b00
                       || pc == 0xf0016b8c) {
                static u32 ultimo = 0xffffffff;
                u32 v = rd(0xf001a538); // campo +c da struct base f001a52c
                if (v != ultimo) {
                    fprintf(stderr,"[TCB] RASTREIO pc=0x%08x  [f001a538]=0x%08x %s\n",
                            pc, v, v==0?"<== ZEROU AQUI":"");
                    ultimo = v;
                }
            } else if (pc == 0xf0016864) {
                u32 r0=0,r1=0; uc_reg_read(uc,UC_ARM_REG_R0,&r0); uc_reg_read(uc,UC_ARM_REG_R1,&r1);
                fprintf(stderr,"[TCB] init: pool_alloc(r0=0x%08x, tam=0x%08x)\n",r0,r1);
            } else if (pc == 0xf0016868) {
                u32 r0=0; uc_reg_read(uc,UC_ARM_REG_R0,&r0);
                fprintf(stderr,"[TCB] init: pool_alloc devolveu 0x%08x %s\n",
                        r0, r0==0?"<== INIT FALHOU":"ok");
            }
        }

        // ── Split I/D do heap REX: antes do fetch, restaura o código pristino ────
        // dos endereços que foram tocados como DADO (heap free-list), desacoplando
        // a visão de instrução da de dados no mesmo VA — exatamente o que a MMU do
        // ARM9 faria. Sem isto, rex_heap_init corromperia 0xf000a800/0xf000e6d4.
        if (sys->rex_split_id_ && !sys->rex_heap_dirty_.empty()) {
            u32 pc=(u32)ad;
            for (u32 w : sys->rex_heap_dirty_) sys->rex_restore_code(w, 4);
            sys->rex_heap_dirty_.clear();
            if (rex_in_heap(pc)) queue_tb_invalidate(pc, pc + (size?size:4));
        }
        if (sys->rex_split_id_ && rex_in_heap((u32)ad))
            sys->rex_restore_code((u32)ad, size?size:4);

        // ── Slide-detector (bug 8): movido para UC_HOOK_BLOCK ──────────────
        // O detector antigo vivia AQUI (uma vez por instrucao) e fazia um
        // uc_mem_read incondicional do opcode so para reconhecer control-flow
        // — o fetch mais quente do Core1. Agora a deteccao acontece em
        // c1_block_hook(): a fronteira de bloco basico do Unicorn JA e' o
        // sinal de branch tomado, entao nenhum opcode e' lido no hot path.
        // Ver zeebo_slide_detector.h e c1_block_hook().
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
        (void)size;
        const bool is_uart = (addr >= UART1_BASE && addr < UART3_BASE + UART_SIZE);
        if (is_uart && type == UC_MEM_READ) {
            const u32 base = (addr >= UART1_BASE && addr < UART1_BASE + UART_SIZE) ? UART1_BASE
                           : (addr >= UART2_BASE && addr < UART2_BASE + UART_SIZE) ? UART2_BASE
                           : UART3_BASE;
            const u32 off = (u32)(addr - base);
            if (off == UART_OFF_SR) {
                u32 sr_val = 0x000C; // TX_READY | TX_EMPTY
                uc_mem_write(uc, (u32)addr, &sr_val, 4);
                return;
            }
        }
        if (!sys->rex_split_id_ || type != UC_MEM_READ || !rex_in_heap((u32)addr)) return;
        u32 a=(u32)addr, off=a-REX_HEAP_VA_BASE, n=(u32)size;
        if (off+n > sys->rex_heap_shadow_.size()) return;

        // Literal pool: um load PC-relative busca uma CONSTANTE EMBUTIDA no .text,
        // nao um dado do heap. Em silicio nao ha Split I/D e essa leitura enxerga o
        // codigo; servir o shadow aqui e' um artefato do nosso modelo.
        //
        // Medido no boot: o heap kmem do OKL4 e' (f0000000, f0200000) -- cobre o
        // .text do proprio kernel -- e os lacos f0002ca4/f000afcc zeram
        // f0000008..f00060b8, apagando no shadow o literal de add_mapping em
        // 0xf0002b78 (0x61 -> 0). Resultado: `ldr ip,[pc,#0xb0]` lia 0, o teste
        // `cmp lr,ip` + `bhs` saia com r0=0 e o kernel abortava em
        // "Assertion r != 0 failed in file pistachio/arch/arm/src/init.cc".
        {
            u32 pc = 0;
            uc_reg_read(uc, UC_ARM_REG_PC, &pc);
            if (rex_in_heap(pc)) {
                u32 poff = pc - REX_HEAP_VA_BASE, insn = 0;
                if (poff + 4 <= sys->rex_heap_pristine_.size()) {
                    memcpy(&insn, &sys->rex_heap_pristine_[poff], 4);
                    // Aceita as DUAS formas de load PC-relative:
                    //   0x04000000 = offset imediato   -> ldr ip,[pc,#0xb0]   (literal pool)
                    //   0x06000000 = offset registrador -> ldr pc,[pc,r12,lsl#2] (jump table)
                    // O filtro antigo so via a imediata, entao a jump table do kernel em
                    // f0004f74 era servida do shadow zerado e o PC ia para 0.
                    const u32  classe     = insn & 0x0e000000u;
                    const bool is_ldr     = (classe == 0x04000000u) || (classe == 0x06000000u);
                    const bool rn_is_pc   = (((insn >> 16) & 0xfu) == 15u);
                    const bool is_load    = ((insn >> 20) & 1u) != 0;
                    if (is_ldr && rn_is_pc && is_load) {
                        // le o .text pristino; NAO suja a word (nada a restaurar)
                        uc_mem_write(uc, a, &sys->rex_heap_pristine_[off], n);
                        return;
                    }
                }
            }
        }

        // [PROBE] faixa servida do pristino, selecionavel por env para permitir
        // CONTROLE NEGATIVO (MORE_INFO §7): a mesma mecanica numa faixa
        // irrelevante deve NAO destravar o boot. Se destravar, o instrumento
        // provou a si mesmo, nao a causa.
        //   ZEEBO_PROBE=rodata  -> f000e000..f0010000 (tabela de tamanhos, hipotese)
        //   ZEEBO_PROBE=control -> f0012000..f0014000 (faixa vizinha, sem uso conhecido)
        //   ZEEBO_PROBE unset   -> desligado (baseline)
        {
            static int mode = -1;
            if (mode < 0) {
                const char* e = getenv("ZEEBO_PROBE");
                mode = (!e) ? 0 : (strcmp(e, "rodata") == 0 ? 1
                                : (strcmp(e, "control") == 0 ? 2 : 0));
            }
            u32 lo = (mode == 1) ? 0xf000e000u : (mode == 2) ? 0xf0012000u : 0;
            if (mode && a >= lo && a < lo + 0x2000u) {
                uc_mem_write(uc, a, &sys->rex_heap_pristine_[off], n);
                return;
            }
        }
        // ── QW56: NAO servir shadow/pristino sobre .data/.bss do kernel ─────────
        // A janela do Split I/D (0xf0000000+2MB) foi desenhada para o .text do
        // kernel (literal pool / jump table), mas engole `.data` e `.bss`. No
        // pristino o `.bss` e' TODO zero (nao tem filesz no ELF), entao servir
        // essa faixa apaga variaveis globais que o kernel acabou de escrever --
        // e a `uc_mem_write` abaixo reverte a RAM do Unicorn.
        // Medido: init_tcb_allocator escreve 0x100 em f001a538, o shadow guarda
        // 0x100, mas allocate_tcb le 0 => NULL => panic thread.cc:1273.
        // Ver test_split_id_bss_clobber (RED sem esta guarda).
        if (off >= REX_KERNEL_FILESZ) return;

        uc_mem_write(uc, a, &sys->rex_heap_shadow_[off], n);
        for (u32 w=a&~3u; w<a+n; w+=4) sys->rex_heap_dirty_.insert(w);
    }

    static void c1_mem_hook(uc_engine* uc, uc_mem_type type, uint64_t addr, int size, int64_t value, void* ud) {
        ZeeboLLESystem* sys = (ZeeboLLESystem*)ud;
        if (type == UC_MEM_WRITE && sys->watch_file_) {
            uint32_t pc1 = 0;
            uc_reg_read(uc, UC_ARM_REG_PC, &pc1);
            sys->note_write(1, (uint32_t)addr, size, (uint32_t)value, pc1);
        }
        if (type == UC_MEM_WRITE &&
            ((addr >= UART1_BASE && addr < UART1_BASE + UART_SIZE) ||
             (addr >= UART2_BASE && addr < UART2_BASE + UART_SIZE) ||
             (addr >= UART3_BASE && addr < UART3_BASE + UART_SIZE))) {
            sys->handle_peripheral_write((uint32_t)addr, size, (uint32_t)value);
            return;
        }
        // ── Split I/D do heap REX: escrita de DADO na janela 0xf0000000+2MB ──────
        // Grava no shadow (RAM de dados) e marca a word suja p/ restaurar o código
        // pristino no próximo fetch (o store do Unicorn commita APÓS este hook, então
        // não adianta restaurar aqui — faríamos e o store sobrescreveria de novo).
        if (sys->rex_split_id_ && type == UC_MEM_WRITE && rex_in_heap((u32)addr)) {
            u32 a=(u32)addr, off=a-REX_HEAP_VA_BASE, n=(u32)size;
            // QW58: SIMETRIA com c1_heap_read_hook. A leitura nao serve o shadow
            // acima de REX_KERNEL_FILESZ (.bss); se a escrita continuasse gravando
            // la', escrita e leitura viveriam em memorias diferentes e toda
            // variavel do .bss viraria lixo -- foi o que matou o pool do kmem
            // (medido: 63 alocacoes OK sem a guarda, 0 com a guarda assimetrica).
            if (off < REX_KERNEL_FILESZ && off+n <= sys->rex_heap_shadow_.size()) {
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
    std::string uart_buffers_[4]; // buffers de linha formatada por UART (1, 2, 3)
    uint64_t c0_poll_d4a8_iters_ = 0; // Item 3: contador de iterações do poll 0xb000d4a8

    // Aliasing físico estilo PCSX2/Dolphin: pool de host da APPS_RAM + VTLB LUT.
    // A pool é dona da RAM de host de 96MB mapeada em APPS_RAM_PHYS_BASE via
    // uc_mem_map_ptr; map_one_aliased faz outros VAs apontarem para a MESMA RAM.
    zeebo_l4::PhysPool  apps_pool_;
    zeebo_l4::ThreadTable thread_table_;
    zeebo_l4::SystemServiceRegistry service_registry_;
    zeebo_l4::VtlbLut   vtlb_;
    // Bug 1: VTLB por space_id. Cada L4_SpaceId_t recebe sua própria tabela de
    // traduções VA->host, então a mesma VA em espaços distintos não colide.
    // vtlb_ acima permanece a view "ativa" (fastmem plano do boot); space_map_
    // registra o mapeamento por espaço para consultas isoladas por SID.
    zeebo_l4::SpaceMap  space_map_;
    // Bug 1 (runtime): comuta o address space REAL do Unicorn por SID entre
    // fatias de execução (uc_mem_unmap/map_ptr fora de hooks). space_map_ acima
    // só registra traduções para consulta; space_manager_ efetiva a troca.
    zeebo_l4::SpaceManager space_manager_;
    std::vector<uint8_t> smem_mem_;      // único backing físico, visível aos dois cores
    // Bug 5/6: real device models bound to the shared/interpreter path.
    zeebo::VicState  vic_c0_;            // VIC pending/enable latch (Core0 / ARM11)
    zeebo::VicState  vic_c1_;            // VIC pending/enable latch (Core1 / ARM9)
    zeebo::GptTimer  gpt_;               // GPT counter advanced from emulated progress
    zeebo::ProcComm  proccomm_;          // inter-core ProcComm over shared SMEM
    static constexpr unsigned GPT_VIC_LINE = 8; // INT_GP_TIMER (model)
    uint64_t last_c0_insns_slice_ = 0;   // per-cycle insn delta baselines (GPT)
    uint64_t last_c1_insns_slice_ = 0;
    // Vector bases for IRQ delivery: Core0/ARM11 low vectors, Core1/ARM9 high
    // vectors (CP15 c1 V=1, table at 0xffff0000 per the AMSS mapping).
    uint32_t c0_vector_base_ = 0x00000000u;
    uint32_t c1_vector_base_ = 0xffff0000u;
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
    // QW56: fim do conteudo com lastro no ARQUIVO (filesz do seg1 do kernel).
    // Acima disto e' .bss: o pristino e' zero por construcao e servi-lo apaga
    // globais do kernel. Ver c1_heap_read_hook e test_split_id_bss_clobber.
    static constexpr u32 REX_KERNEL_FILESZ = 0x0001a324;
    static constexpr u32 REX_HEAP_VA_SIZE = 0x00200000;
    std::vector<u8> rex_heap_pristine_;    // código pristino da janela (visão I)
    std::vector<u8> rex_heap_shadow_;      // RAM de dados dedicada (visão D)
    std::unordered_set<u32> rex_heap_dirty_; // words com dado, a restaurar no fetch
    bool rex_split_id_ = false;

    static bool rex_in_heap(u32 a){ return a>=REX_HEAP_VA_BASE && a<REX_HEAP_VA_BASE+REX_HEAP_VA_SIZE; }

    // [QW79] Fila de invalidacao de TB adiada.
    // uc_ctl(TB_REMOVE_CACHE) NAO e' reentrante a partir de um code hook: o hook
    // roda via helper_uc_tracecode, de dentro do TB em execucao, e invalidar
    // dali libera o proprio TB -> SIGSEGV (backtrace QW78:
    // tb_invalidate_phys_range_arm <- uc_ctl <- c1_code_hook).
    // Comprovado 2x2: COM = exit 139 sempre; SEM = exit 0 sempre.
    // Solucao: enfileirar aqui, aplicar FORA do uc_emu_start (contexto seguro).
    static std::vector<std::pair<u32,u32>>& tb_inval_queue() {
        static std::vector<std::pair<u32,u32>> q;
        return q;
    }
    static void queue_tb_invalidate(u32 begin, u32 end) {
        // [QW80] SEM teto de descarte. Um teto silencioso (era 4096) chegou a
        // saturar 2x numa janela de 45s, e cada descarte deixaria um TB de
        // codigo automodificado sem invalidar -- traducao velha executando sem
        // aviso. Trocar um crash por corrupcao muda seria um retrocesso.
        // Custo de memoria e' irrelevante (8 bytes por par; drenado a cada
        // fatia de uc_emu_start).
        tb_inval_queue().emplace_back(begin, end);
    }
    static void drain_tb_invalidate(uc_engine* uc) {
        auto& q = tb_inval_queue();
        for (auto& r : q) uc_ctl_remove_cache(uc, r.first, r.second);
        q.clear();
    }
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
    std::unique_ptr<UnifiedHostAudio> host_audio_;
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
    bool jit_solo_ = false;
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
    zeebo::brew::BrewTimerQueue brew_timers_;
public:
    zeebo::brew::BrewLoader* brew() { return brew_.get(); }
};

// Definições dos membros estáticos do hook transitório do ciclo de vida Z-Wheel.
ZeeboLLESystem* ZeeboLLESystem::s_zwheel_hook_sys_ = nullptr;
ZeeboLLESystem* ZeeboLLESystem::s_module_hook_sys_ = nullptr;
uint32_t        ZeeboLLESystem::s_module_lo_ = 0;
uint32_t        ZeeboLLESystem::s_module_hi_ = 0;
bool            ZeeboLLESystem::s_module_pc_seen_ = false;
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
    printf("  --jit                      Habilita Dynarmic JIT para Core0 (ARM11 APPS)\n");
    printf("  --jit-solo                 Executa JIT solo sem lockstep shadow de Unicorn\n");
    printf("  --applet=<caminho.mod>     Carrega e injeta aplicativo BREW (.mod) externamente\n");
    printf("  --efs2-ls[=<sufixo>]       Lista dirents da partição 0:EFS2APPS da NAND (filtro opc., ex: .mod)\n");
    printf("  --efs2-run=<arquivo>       Extrai um applet direto do EFS2 (ex: reksio.mod) e injeta via BrewLoader\n");
    printf("  run <caminho.mod>          Atalho estilo Zeebx para executar applet BREW direto\n");
    printf("  --cycles=<N>               Número de ciclos intercalados (padrão: 250)\n");
    printf("  --slice=<N>                Instruções por fatia de ciclo por core (padrão: 10000)\n");
    printf("  --seconds=<N>              Tempo máximo de execução em segundos reais (0 = ilimitado)\n");
    printf("\nOpções Gráficas e Telemetria:\n");
    printf("  --gui, -g, --window        Abre janela interativa SDL2 (640x480 RGB565 redimensionável)\n");
    printf("  --headless                 Execução em console sem abrir janela gráfica (padrão)\n");
    printf("  --fps                      Exibe estatísticas contínuas: FPS, MIPS de C0 e C1\n");
    printf("  --dump-frames=<DIR>        Exporta sequência contínua de frames em PPM para <DIR>\n");
    printf("  --zwheel-preview           Abre preview interativo do pipeline gráfico da Z-Wheel\n");
    printf("  --zwheel-preview-headless  Testa preview gráfico em modo headless (para CI)\n");
    printf("\nAtalhos e Controles no Modo GUI:\n");
    printf("  F11                        Alternar modo Janela / Tela Cheia (Fullscreen)\n");
    printf("  PAUSE                      Pausar / Retomar execução da CPU\n");
    printf("  F12                        Capturar Screenshot instantâneo (PPM)\n");
    printf("  Gamepad / Z-Pad            D-Pad, Botões A/B/1/2/Home mapeados automaticamente\n");
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
    size_t efs2_ls_max = 200;
    std::string dump_frames_dir = "";
    bool headless = true;
    bool zwheel_preview = false;
    bool show_fps = false;
    int control_port = 0;
    bool strict_unmapped = false;
    bool use_jit = false;
    std::string trace_path;
    std::string watch_spec;
    std::string watch_path;
    uint64_t trace_limit = 0;
    bool jit_solo = false;
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
        } else if (arg.rfind("--efs2-ls-max=", 0) == 0) {
            int max_i = 0;
            if (!parse_int_arg(arg.substr(14), 1, 200000, max_i)) {
                fprintf(stderr, "Argumento inválido: %s\n", arg.c_str()); return 2;
            }
            efs2_ls_max = (size_t)max_i;
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
        } else if (arg.rfind("--trace-core0=", 0) == 0) {
            trace_path = arg.substr(14);
        } else if (arg.rfind("--watch-writes=", 0) == 0) {
            watch_spec = arg.substr(15);
        } else if (arg.rfind("--watch-out=", 0) == 0) {
            watch_path = arg.substr(12);
        } else if (arg.rfind("--trace-limit=", 0) == 0) {
            trace_limit = strtoull(arg.substr(14).c_str(), nullptr, 0);
        } else if (arg == "--jit") {
            use_jit = true;
        } else if (arg == "--jit-solo") {
            use_jit = true;
            jit_solo = true;
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

    if (!trace_path.empty()) sys.open_trace(trace_path, trace_limit);
    if (!watch_spec.empty()) sys.open_watch(watch_spec, watch_path);
    if (!sys.init(nand_path, apps_path, amss_path, headless, use_jit)) {
        printf("[Fatal] System initialization failed\n");
        return 1;
    }
    // Aponta o parser EFS2 para a mesma cópia de trabalho da NAND usada no boot.
    sys.set_efs2_nand_path(nand_path);
    sys.set_boot_target(boot_firstapp);
    sys.set_jit_solo(jit_solo);

    // QW14: opt-in strict-unmapped. Off by default => boot unchanged.
    if (strict_unmapped) {
        sys.arm_strict_unmapped(true);
        printf("[StrictUnmapped] Armed: first UNKNOWN unmapped access will halt "
               "deterministically and capture structured evidence.\n");
    }

    // --efs2-ls: lista os dirents da partição 0:EFS2APPS e sai.
    if (efs2_ls_flag) {
        size_t n = sys.efs2_ls(efs2_ls_filter, efs2_ls_max);
        return n > 0 ? 0 : 1;
    }

    // --efs2-run=<arquivo>: extrai o applet direto do EFS2 e injeta via BrewLoader.
    if (!efs2_run.empty()) {
        printf("[EFS2] Carregando applet '%s' direto da NAND 0:EFS2APPS...\n", efs2_run.c_str());
        if (!sys.load_applet_from_efs2(efs2_run, 0x12000000)) {
            printf("[Warn] Falha ao carregar applet do EFS2: %s\n", efs2_run.c_str());
        } else {
            // Applet / Jogo (Z-Wheel 274755, Reksio reksio.mod, TecToy tectoy.mod ou qualquer .mod do EFS2):
            // após injetar o payload do EFS2, instancia o ciclo de vida do applet despachando
            // EVT_APP_START ao manipulador ZeeboApp pré-mapeado em 0:APPS (0x10532344).
            std::string app_label = (efs2_run == "274755") ? "Z-Wheel (274755)" :
                                    (efs2_run == "reksio.mod") ? "Reksio (reksio.mod)" :
                                    (efs2_run == "tectoy.mod") ? "TecToy (tectoy.mod)" : efs2_run;
            // Bug 4: apenas 274755 é a Z-Wheel explícita; demais módulos usam
            // seleção honesta do próprio manipulador (ou permanecem loaded_only).
            bool life_ok = sys.dispatch_applet_start(app_label, /*is_zwheel=*/efs2_run == "274755");
            // Loop interativo/contínuo quando há tempo requerido (--seconds=N, N>0),
            // GUI, ou um cliente de controle esperando comandos (senão o processo
            // encerraria antes de responder).
            if (control_port > 0 && !life_ok) {
                // loaded_only ainda é uma sessão depurável: segue o boot normal
                // sem fabricar o ciclo de vida ou um frame do applet.
                sys.run_interleaved(cycles, slice_insns, max_seconds, show_fps, dump_frames_dir);
            } else if (life_ok && (max_seconds > 0.0 || !headless || control_port > 0)) {
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
        } else {
            // Executa ciclo de vida automático para applet/jogo externo. Bug 4:
            // um applet externo NÃO é a Z-Wheel — usa seleção honesta do próprio
            // manipulador; arquivo inválido/sem entry permanece loaded_only.
            bool life_ok = sys.dispatch_applet_start(applet_path, /*is_zwheel=*/false);
            if (control_port > 0 && !life_ok) {
                // loaded_only ainda é uma sessão depurável: segue o boot normal
                // sem fabricar o ciclo de vida ou um frame do applet.
                sys.run_interleaved(cycles, slice_insns, max_seconds, show_fps, dump_frames_dir);
            } else if (life_ok && (max_seconds > 0.0 || !headless || control_port > 0)) {
                sys.run_zwheel_interactive(headless, max_seconds, dump_frames_dir);
            }
            return life_ok ? 0 : 1;
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
