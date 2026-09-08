// qdsp5_dispatcher.cpp — routing + packet decode. FASE 1 skeleton.
#include "qdsp5_dispatcher.h"
#include <cstring>
#include <cstdio>

namespace zeebo::qdsp5 {

// classify(): AUDMGRPROG (0x30000013) is the VERIFIED audio-manager program
// (notes/qdsp5_proc_ids.md). The proc ORDINAL sub-map is still TODO (Q0.2) — until
// the xdr decl order is confirmed we route by `program` only and return Unknown
// for anything else. We do NOT fake the (program,proc)->task map.
Engine classify(u32 program, u32 proc_id) {
    (void)proc_id;
    // ADSPRTOSATOM (0x3000000a) é o programa que REALMENTE transporta PCM/comandos
    // p/ as command queues do QDSP5 (notes/qdsp5_program_ids.md). O proc
    // adsp_rtos_app_to_modem_command carrega, no sub-payload, a task/queue destino
    // (AUDPP UPAUDPPCMDxQUEUE vs AUDPLAYx BITSTREAMCTRL). Enquanto o layout exato
    // do sub-comando é [infer] (fecha na Frente A com 1 pacote real), roteamos p/
    // AUDPP — é o caminho host-PCM (audpp_host_pcm_write_req) por onde o jogo
    // alimenta samples. TODO(Frente A): separar Audpp/Audplay pelo campo de task.
    if (program == prog::ADSPRTOSATOM) return Engine::Audpp;
    // AUDMGR (0x30000013) só abre/config a sessão de codec — NÃO carrega samples.
    // Mantido roteando p/ Audpp (host-PCM state machine consome os CONFIG/enable).
    if (program == prog::AUDMGR) return Engine::Audpp;
    return Engine::Unknown;
}

Qdsp5Dispatcher::Qdsp5Dispatcher()
    : audpp_(make_audpp_engine()),
      jpeg_(make_jpeg_engine()),
      vfe_(make_vfe_engine()),
      voice_(make_voice_engine()),
      audplay_(make_audplay_engine()),
      audrec_(make_audrec_engine()) {}

IQdspEngine* Qdsp5Dispatcher::engine_for(Engine e) {
    switch (e) {
        case Engine::Audpp:   return audpp_.get();
        case Engine::Jpeg:    return jpeg_.get();
        case Engine::Vfe:     return vfe_.get();
        case Engine::Voice:   return voice_.get();
        case Engine::Audplay: return audplay_.get();
        case Engine::Audrec:  return audrec_.get();
        default:              return nullptr;
    }
}

Reply Qdsp5Dispatcher::feed_raw(const u8* packet, u32 len,
                                const QdspGuest& guest, u32 caller_tcb) {
    Reply r;
    if (!packet || len < sizeof(RpcHeader)) return r;

    // HARDENING (fuzz Q0.1): ao vivo, `len` pode vir de campo do firmware. Um
    // pacote ONCRPC nunca excede kMaxPayload (0x500, budget verificado). Clampa
    // para não copiar/varrer além do frame esperado.
    if (len > kMaxPayload) len = kMaxPayload;

    RpcHeader hdr{};
    std::memcpy(&hdr, packet, sizeof(hdr));

    Command cmd;
    cmd.program = hdr.program;
    cmd.proc_id = hdr.procedure;
    cmd.engine  = classify(hdr.program, hdr.procedure);

    // payload: de +0x80 até len (campo de tamanho real fica em +0x24 no frame do
    // guest; aqui usamos o que o pacote traz para o smoke ser autocontido).
    if (len > kPayloadOff) {
        cmd.payload.assign(packet + kPayloadOff, packet + len);
    }
    return dispatch(cmd, guest, caller_tcb);
}

Reply Qdsp5Dispatcher::dispatch(const Command& cmd, const QdspGuest& guest, u32 caller_tcb) {
    IQdspEngine* eng = engine_for(cmd.engine);
    if (!eng) {
        printf("[qdsp5] UNROUTED prog=0x%08x proc=0x%x (%zu bytes) — engine map TODO Q0.2\n",
               cmd.program, cmd.proc_id, cmd.payload.size());
        return Reply{}; // handled=false
    }
    Reply r = eng->handle(cmd, guest);
    // Entrega o sinal de conclusão ao guest (rex_set_sigs) se houver caminho.
    if (r.handled && on_completion && r.completion_sig)
        on_completion(caller_tcb, r.completion_sig);
    return r;
}

} // namespace zeebo::qdsp5
