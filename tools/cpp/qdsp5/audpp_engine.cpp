// audpp_engine.cpp — QDSP_AUDPPTASK (audio post-processor) — FASE 1 producer REAL.
// A única engine com corpo de verdade: decodifica um audpp_cmd PROVISÓRIO, segue
// o ponteiro de buffer PCM na memória guest e alimenta o UnifiedAudioSink que já
// existe (tools/cpp/zeebo_audio_sink.h). O layout de audpp_cmd é HIPÓTESE até
// Q0.2/Q1.1 confirmarem os campos reais do payload — marcado claramente.
#include "iqdsp_engine.h"
#include "../zeebo_audio_sink.h"   // UnifiedAudioSink (mixer HLE já testado)
#include <cstring>
#include <cstdio>

namespace zeebo::qdsp5 {

// ---- Layout PROVISÓRIO do comando AUDPP (payload @ +0x80) ------------------
// NÃO verificado. Estrutura plausível p/ um "play PCM buffer"; os offsets reais
// virão do hexdump capturado em Q0.2. Mantido pequeno e explícito de propósito.
#pragma pack(push,1)
struct audpp_cmd_play {
    u32 opcode;        // ex.: 0x01 = PLAY, 0x02 = STOP, 0x10 = SET_VOL  (HIPÓTESE)
    u32 pcm_ptr;       // VA guest do buffer PCM
    u32 pcm_bytes;     // tamanho em bytes
    u32 sample_rate;   // Hz
    u32 channels;      // 1 = mono, 2 = stereo
    u32 volume_q16;    // volume 16.16 fixed
};
#pragma pack(pop)

enum : u32 { AUDPP_PLAY = 0x01, AUDPP_STOP = 0x02, AUDPP_SET_VOL = 0x10 };

// ---- State machine host-PCM (audpphostpcm.c, VERIFIED por strings) ---------
// Strings contíguas em APPS provam a sequência: o jogo configura o codec via
// AUDMGR, entra em HPCM_ACTIVE ao começar a escrever PCM, AUDPP_ACTIVE quando o
// pós-processador está consumindo, e RESET ao encerrar. Ver notes/qdsp5_program_ids.md.
// Modelamos o estado p/ (a) logar transições coerentes e (b) recusar PLAY antes de
// CONFIG (o firmware loga "HostPCM: Audmgr is un-configured"). O MAPA opcode->
// transição é [infer] até a Frente A confirmar com 1 pacote real.
enum class HostPcm : u32 { UNCONFIGURED, AUDMGR_CONFIG, HPCM_ACTIVE, AUDPP_ACTIVE, RESET };

static const char* hpcm_name(HostPcm s) {
    switch (s) {
        case HostPcm::UNCONFIGURED:  return "UNCONFIGURED";
        case HostPcm::AUDMGR_CONFIG: return "AUDMGR_CONFIG";
        case HostPcm::HPCM_ACTIVE:   return "HPCM_ACTIVE";
        case HostPcm::AUDPP_ACTIVE:  return "AUDPP_ACTIVE";
        case HostPcm::RESET:         return "RESET";
    }
    return "?";
}

// Bit de conclusão HIPOTÉTICO (rex_set_sigs). Real vem de Q0.2 observando qual
// máscara a thread de áudio espera em rex_wait.
constexpr u32 kAudppDoneSig = 0x00080000; // subset da máscara 0x00180000 do event pump

class AudppEngine final : public IQdspEngine {
public:
    AudppEngine() : sink_(44100) {}

    Engine engine() const override { return Engine::Audpp; }
    const char* name() const override { return "QDSP_AUDPPTASK"; }

    Reply handle(const Command& cmd, const QdspGuest& guest) override {
        Reply r;
        if (cmd.payload.size() < sizeof(audpp_cmd_play)) {
            printf("[audpp] payload curto (%zu B) — comando ignorado (layout TODO Q1.1)\n",
                   cmd.payload.size());
            return r;
        }
        audpp_cmd_play c{};
        std::memcpy(&c, cmd.payload.data(), sizeof(c));

        switch (c.opcode) {
            case AUDPP_PLAY: return do_play(c, guest);
            case AUDPP_STOP:
                sink_ = UnifiedAudioSink(out_rate_);  // reset simples (stub)
                state_ = HostPcm::RESET;
                printf("[audpp] STOP -> host-PCM state=%s\n", hpcm_name(state_));
                r.handled = true; r.completion_sig = kAudppDoneSig; return r;
            case AUDPP_SET_VOL:
                // volume global — stub: só reconhece.
                r.handled = true; r.completion_sig = kAudppDoneSig; return r;
            default:
                printf("[audpp] opcode 0x%x desconhecido (map TODO Q1.1)\n", c.opcode);
                return r;
        }
    }

    // Exposto p/ o smoke test drenar PCM mixado e provar por WAV.
    void mix(int16_t* out, size_t frames) { sink_.mix_samples(out, frames); }
    void mix_audio(int16_t* out, size_t frames) override { sink_.mix_samples(out, frames); }
    uint32_t out_rate() const { return out_rate_; }

private:
    Reply do_play(const audpp_cmd_play& c, const QdspGuest& guest) {
        Reply r;
        // HARDENING (fuzz Q0.1): pcm_bytes vem do payload/firmware — NÃO confiar.
        // 1) Teto sadio: um comando de play não carrega mais que kMaxPcmBytes de
        //    uma vez (buffers maiores viriam fatiados). Rejeita valores absurdos
        //    (ex.: 0xFFFFFFFF) que causariam OOM/DoS na alocação.
        // 2) Só copiamos um nº INTEIRO de amostras int16: n_samples = bytes/2 e
        //    lemos exatamente n_samples*2 bytes — senão um pcm_bytes ímpar faria
        //    guest.rd escrever 1 byte além do fim do vector (heap overflow).
        constexpr u32 kMaxPcmBytes = 4u * 1024 * 1024; // 4 MiB por comando
        if (c.pcm_bytes > kMaxPcmBytes) {
            printf("[audpp] pcm_bytes=%u excede teto %u — rejeitado\n", c.pcm_bytes, kMaxPcmBytes);
            return r;
        }
        u32 n_samples = c.pcm_bytes / (u32)sizeof(int16_t);
        u32 read_bytes = n_samples * (u32)sizeof(int16_t);
        std::vector<int16_t> pcm(n_samples);
        if (!pcm.empty() && !guest.rd(c.pcm_ptr, pcm.data(), read_bytes)) {
            printf("[audpp] falha ao ler PCM em 0x%08x (%u B)\n", c.pcm_ptr, read_bytes);
            return r;
        }
        float vol = c.volume_q16 ? (float)c.volume_q16 / 65536.0f : 1.0f;
        uint32_t ch = c.channels ? c.channels : 2;
        uint32_t sr = c.sample_rate ? c.sample_rate : 44100;

        uint32_t vid = sink_.allocate_voice(sr, ch, vol);
        sink_.submit_pcm(vid, pcm.data(), pcm.size());
        // Transição host-PCM: primeiro PLAY leva CONFIG->HPCM_ACTIVE->AUDPP_ACTIVE.
        if (state_ == HostPcm::UNCONFIGURED || state_ == HostPcm::RESET)
            state_ = HostPcm::AUDMGR_CONFIG;      // sessão abre no 1º comando
        if (state_ == HostPcm::AUDMGR_CONFIG)
            state_ = HostPcm::HPCM_ACTIVE;         // ARM começou a escrever PCM
        state_ = HostPcm::AUDPP_ACTIVE;            // pós-proc consumindo
        printf("[audpp] PLAY voice=%u %uHz ch=%u vol=%.2f (%zu samples) state=%s -> UnifiedAudioSink\n",
               vid, sr, ch, vol, pcm.size(), hpcm_name(state_));

        r.handled = true;
        r.completion_sig = kAudppDoneSig;
        return r;
    }

    uint32_t out_rate_ = 44100;
    HostPcm  state_ = HostPcm::UNCONFIGURED;  // host-PCM state machine (audpphostpcm.c)
    UnifiedAudioSink sink_;
};

std::unique_ptr<IQdspEngine> make_audpp_engine() {
    return std::make_unique<AudppEngine>();
}

} // namespace zeebo::qdsp5
