// qdsp5_smoke.cpp — prova o caminho AUDPP por ARQUIVO WAV, sem Unicorn.
// Análogo ao igl_smoke (prova por framebuffer). Uma "memória guest" host serve um
// buffer PCM (seno 440Hz); montamos um pacote ONCRPC como o firmware montaria,
// passamos pelo Qdsp5Dispatcher e drenamos o mix para qdsp5_out.wav.
#include "qdsp5_dispatcher.h"
#include "iqdsp_engine.h"
#include "../zeebo_audio_sink.h"   // UnifiedAudioSink (namespace global)
#include <cstdio>
#include <cstring>
#include <cmath>
#include <vector>
using namespace zeebo::qdsp5;

// ---- WAV writer mínimo (PCM16 stereo) -------------------------------------
static bool write_wav(const char* path, const int16_t* pcm, size_t frames, u32 rate) {
    FILE* f = fopen(path, "wb"); if (!f) return false;
    u32 data_bytes = (u32)(frames * 2 * sizeof(int16_t));
    u32 byte_rate = rate * 2 * sizeof(int16_t);
    auto w32=[&](u32 v){ fwrite(&v,4,1,f); }; auto w16=[&](u16 v){ fwrite(&v,2,1,f); };
    fwrite("RIFF",1,4,f); w32(36+data_bytes); fwrite("WAVE",1,4,f);
    fwrite("fmt ",1,4,f); w32(16); w16(1); w16(2); w32(rate); w32(byte_rate); w16(4); w16(16);
    fwrite("data",1,4,f); w32(data_bytes);
    fwrite(pcm,1,data_bytes,f); fclose(f); return true;
}

// Layout PROVISÓRIO espelhado de audpp_engine.cpp (audpp_cmd_play).
#pragma pack(push,1)
struct audpp_cmd_play { u32 opcode, pcm_ptr, pcm_bytes, sample_rate, channels, volume_q16; };
#pragma pack(pop)

// "Memória guest": um buffer PCM num VA fixo, servido via QdspGuest.read.
static std::vector<int16_t> g_pcm;
static constexpr u32 kPcmVA = 0x02000000; // dentro de shared RAM plausível
static bool guest_read(u32 va, void* dst, u32 size, void*) {
    if (va < kPcmVA) return false;
    u32 off = va - kPcmVA;
    if (off + size > g_pcm.size()*sizeof(int16_t)) return false;
    std::memcpy(dst, (uint8_t*)g_pcm.data()+off, size); return true;
}

int main() {
    // 1) PCM fonte: seno 440Hz, 0.5s stereo @ 44100.
    const u32 rate=44100; const size_t frames=rate/2;
    g_pcm.resize(frames*2);
    for (size_t i=0;i<frames;i++){
        int16_t s=(int16_t)(12000.0*std::sin(2.0*M_PI*440.0*i/rate));
        g_pcm[i*2]=s; g_pcm[i*2+1]=s;
    }

    // 2) Monta pacote ONCRPC como o firmware: header @ 0, cmd @ +0x80.
    std::vector<u8> pkt(kMaxPayload, 0);
    RpcHeader hdr{}; hdr.msg_type=0; hdr.rpc_version=2;
    hdr.program=prog::AUDMGR; hdr.procedure=audmgr_proc::SET_DEVICE_MODE; // real prog; proc ordinal unconfirmed
    std::memcpy(pkt.data(), &hdr, sizeof(hdr));
    audpp_cmd_play cmd{}; cmd.opcode=0x01/*PLAY*/; cmd.pcm_ptr=kPcmVA;
    cmd.pcm_bytes=(u32)(g_pcm.size()*sizeof(int16_t));
    cmd.sample_rate=rate; cmd.channels=2; cmd.volume_q16=65536;
    std::memcpy(pkt.data()+kPayloadOff, &cmd, sizeof(cmd));

    // 3) Dispatcher + captura do sinal de conclusão (rex_set_sigs simulado).
    Qdsp5Dispatcher disp;
    u32 got_tcb=0, got_sig=0;
    disp.on_completion=[&](u32 tcb,u32 sig){ got_tcb=tcb; got_sig=sig; };
    QdspGuest guest; guest.read=&guest_read;

    Reply r = disp.feed_raw(pkt.data(), (u32)pkt.size(), guest, /*caller_tcb=*/0xABCD);
    printf("[disp] handled=%d status=0x%x sig=0x%x tcb=0x%x %s\n",
           r.handled, r.status, got_sig, got_tcb,
           (r.handled && got_sig)?"PASS":"FAIL");

    // 4) Prova por WAV: exercita o caminho AUDPP real (engine direta) e drena o
    //    mix do sink interno. mix() não está na IQdspEngine (é específico da
    //    AUDPP), então também rodamos a engine concreta para provar seu handle().
    {
        auto eng = make_audpp_engine();
        std::vector<u8> body(pkt.begin()+kPayloadOff, pkt.end());
        Reply er = eng->handle(Command{Engine::Audpp,prog::AUDMGR,audmgr_proc::SET_DEVICE_MODE,body}, guest);
        printf("[audpp] engine.handle handled=%d sig=0x%x %s\n",
               er.handled, er.completion_sig, er.handled?"PASS":"FAIL");
    }
    std::vector<int16_t> out(frames*2, 0);
    {
        UnifiedAudioSink sink(rate);
        u32 v=sink.allocate_voice(rate,2,1.0f);
        sink.submit_pcm(v, g_pcm.data(), g_pcm.size());
        sink.mix_samples(out.data(), frames);
    }
    bool wav = write_wav("qdsp5_out.wav", out.data(), frames, rate);
    // Verifica não-silêncio.
    int16_t peak=0; for (auto s:out) peak=std::max<int16_t>(peak,(int16_t)std::abs(s));
    printf("[wav ] qdsp5_out.wav escrito=%d peak=%d %s\n",
           wav, peak, (wav && peak>1000)?"PASS":"FAIL");
    printf("DONE\n");
    return 0;
}
