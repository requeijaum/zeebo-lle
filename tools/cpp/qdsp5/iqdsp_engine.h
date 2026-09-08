// iqdsp_engine.h — QDSP5 engine abstraction (Citra/rasterizer-style interface).
// Skeleton (FASE 1 do QDSP5_TODO.md §4 Option B: DSP-layer HLE via RPC short-circuit).
// Uma interface, quatro engines (AUDPP/JPEG/VFE/VOICE). NADA processa "de verdade"
// ainda — cada handle() é stub com efeito verificável e devolve um reply sintético.
#pragma once
#include "qdsp5_rpc.h"
#include <memory>
#include <string>

namespace zeebo::qdsp5 {

// Resultado de processar um comando: reply bytes + sinal de conclusão a assertar
// no TCB do chamador (rex_set_sigs — QDSP5_TODO.md §6). Sem isto o jogo trava.
struct Reply {
    bool handled   = false;
    u32  status    = kRpcSuccess;   // status code p/ o header de resposta
    u32  completion_sig = 0;        // bit a passar em rex_set_sigs (0 = desconhecido)
    std::vector<u8> body;           // payload de resposta (vazio p/ maioria)
};

// A abstração única. AudppEngine/JpegEngine/VfeEngine/VoiceEngine implementam.
// Alimentada pelo Qdsp5Dispatcher (que hoje recebe do smd_bridge; amanhã do
// hook no PACKET_CONSUMER 0x16e8cb96 dentro do zeebo_lle_main).
class IQdspEngine {
public:
    virtual ~IQdspEngine() = default;
    virtual Engine engine() const = 0;
    virtual const char* name() const = 0;

    // Processa um comando decodificado. `guest` permite seguir ponteiros do
    // payload (buffers PCM/JPEG em shared RAM). Devolve reply sintético.
    virtual Reply handle(const Command& cmd, const QdspGuest& guest) = 0;
};

// Makers por engine (definidos em cada .cpp; padrão make_* do skeleton GPU).
std::unique_ptr<IQdspEngine> make_audpp_engine();
std::unique_ptr<IQdspEngine> make_jpeg_engine();
std::unique_ptr<IQdspEngine> make_vfe_engine();
std::unique_ptr<IQdspEngine> make_voice_engine();
std::unique_ptr<IQdspEngine> make_audplay_engine();
std::unique_ptr<IQdspEngine> make_audrec_engine();

} // namespace zeebo::qdsp5
