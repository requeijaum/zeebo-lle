// qdsp5_dispatcher.h — routes decoded ONCRPC packets to the right QDSP5 engine.
// Skeleton (FASE 1). Este é o ponto de intercept do Option B: no futuro um
// UC_HOOK_CODE em PACKET_CONSUMER (0x16e8cb96) chama feed_raw() com o pacote que
// o PRÓPRIO firmware montou; hoje os smoke tests chamam feed_raw() direto.
#pragma once
#include "iqdsp_engine.h"
#include <array>
#include <functional>

namespace zeebo::qdsp5 {

class Qdsp5Dispatcher {
public:
    Qdsp5Dispatcher();

    // Sinal de conclusão a ser entregue ao guest (rex_set_sigs). O orquestrador
    // liga isto ao seu emulador; smoke tests capturam num lambda.
    std::function<void(u32 tcb, u32 sig)> on_completion;

    // Decodifica um pacote cru de kMaxPayload bytes (header + body @ +0x80) e
    // roteia p/ engine. `caller_tcb` = quem fez o RPC bloqueante (p/ o sinal).
    Reply feed_raw(const u8* packet, u32 len, const QdspGuest& guest, u32 caller_tcb = 0);

    // Variante já-decodificada (usada por testes e pelo caminho do smd_bridge).
    Reply dispatch(const Command& cmd, const QdspGuest& guest, u32 caller_tcb = 0);

    // Route a command whose destination QUEUE has already been resolved (the
    // verified queue->task association picks the engine). This is the AUDPLAY
    // seam: AUDPLAYx bitstream queues reach the decoder task here, which AUDMGR-
    // -program-only classify() could never do. Rejects (handled=false) when the
    // transport program is not ADSPRTOSATOM or the payload is empty/malformed.
    Reply dispatch_queue(QueueId queue, const Command& cmd,
                         const QdspGuest& guest, u32 caller_tcb = 0);

    IQdspEngine* engine_for(Engine e);

    // Drena PCM mixado da engine AUDPP para o host (QW-AUD1). Silêncio se ainda
    // não houver engine/PCM.
    void mix_audio(int16_t* out, size_t frames) {
        if (IQdspEngine* eng = engine_for(Engine::Audpp)) eng->mix_audio(out, frames);
    }

private:
    std::unique_ptr<IQdspEngine> audpp_, jpeg_, vfe_, voice_, audplay_, audrec_;
};

} // namespace zeebo::qdsp5
