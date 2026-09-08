// stub_engines.cpp — JPEG / VFE / VOICE. FASE 1 stubs HONESTOS.
// Reconhecem o comando, logam, devolvem reply sintético de sucesso p/ não travar
// o firmware, mas NÃO decodificam nada. Corpo real fica p/ Q2 (JPEG), Q3 (VFE),
// Q4 (VOICE) do QDSP5_TODO.md. Zero invenção de comportamento.
#include "iqdsp_engine.h"
#include <cstdio>

namespace zeebo::qdsp5 {

namespace {
Reply ack(const char* who, const Command& cmd) {
    printf("[%s] STUB ack prog=0x%08x proc=0x%x (%zu B) — decode TODO\n",
           who, cmd.program, cmd.proc_id, cmd.payload.size());
    Reply r; r.handled = true; r.status = kRpcSuccess; r.completion_sig = 0; // sig real: TODO
    return r;
}
} // namespace

class JpegEngine final : public IQdspEngine {
public:
    Engine engine() const override { return Engine::Jpeg; }
    const char* name() const override { return "QDSP_JPEGTASK"; }
    Reply handle(const Command& cmd, const QdspGuest&) override { return ack(name(), cmd); }
};
class VfeEngine final : public IQdspEngine {
public:
    Engine engine() const override { return Engine::Vfe; }
    const char* name() const override { return "QDSP_VFETASK"; }
    Reply handle(const Command& cmd, const QdspGuest&) override { return ack(name(), cmd); }
};
class VoiceEngine final : public IQdspEngine {
public:
    Engine engine() const override { return Engine::Voice; }
    const char* name() const override { return "QDSP_VOICEPROCTASK"; }
    Reply handle(const Command& cmd, const QdspGuest&) override { return ack(name(), cmd); }
};
// AUDPLAY0-4: os 5 decoders reais de bitstream/PCM (notes §3). Downstream vai p/ AUDPP.
// Stub honesto até Q1.2 (definir struct do bitstream cmd a partir de captura Q0.1).
class AudplayEngine final : public IQdspEngine {
public:
    Engine engine() const override { return Engine::Audplay; }
    const char* name() const override { return "QDSP_AUDPLAYxTASK"; }
    Reply handle(const Command& cmd, const QdspGuest&) override { return ack(name(), cmd); }
};
// AUDREC: gravação (mic/loopback). Fora do caminho de playback dos jogos, mas parte
// do array QDSP5 — presente p/ completude. Stub honesto.
class AudrecEngine final : public IQdspEngine {
public:
    Engine engine() const override { return Engine::Audrec; }
    const char* name() const override { return "QDSP_AUDRECTASK"; }
    Reply handle(const Command& cmd, const QdspGuest&) override { return ack(name(), cmd); }
};

std::unique_ptr<IQdspEngine> make_jpeg_engine()  { return std::make_unique<JpegEngine>();  }
std::unique_ptr<IQdspEngine> make_vfe_engine()   { return std::make_unique<VfeEngine>();   }
std::unique_ptr<IQdspEngine> make_voice_engine() { return std::make_unique<VoiceEngine>(); }
std::unique_ptr<IQdspEngine> make_audplay_engine(){ return std::make_unique<AudplayEngine>(); }
std::unique_ptr<IQdspEngine> make_audrec_engine() { return std::make_unique<AudrecEngine>();  }

} // namespace zeebo::qdsp5
