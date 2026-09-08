// qdsp5_capture_hook.h — Q0.1 dispatcher trap (DROP-IN, header-only, ISOLADO).
//
// Objetivo: capturar in vivo os campos ONCRPC reais que o firmware AMSS escreve
// quando dispara um RPC de áudio — program, procedure, len e um hexdump do payload
// @+0x80 — para CONFIRMAR (não adivinhar) o layout de audmgr_set_device_mode e
// promover audpp_cmd_play de HIPÓTESE a fato.
//
// NÃO altera a base do emulador. O main pluga com UMA linha (ver INTEGRAÇÃO abaixo).
// Fatos ancorados (notes/qdsp5_proc_ids.md):
//   - Core1 (AMSS/ARM926) mapeado em VA 0x16e00000 no emulador -> casa com o
//     disassembly (base 0x163a8000 do dump == 0x16e00000 no uc). Logo os VAs do
//     dispatcher/consumer são lidos direto por uc_mem_read(core1_uc, ...).
//   - AUDMGRPROG = 0x30000013 (VERIFICADO). AUDMGRCB = 0x31000013.
//   - Header ONCRPC: program@+0x0C, version@+0x10, procedure@+0x14 (== +0x20 no
//     layout de fila do firmware), payload@+0x80.
//
// INTEGRAÇÃO (no orchestrator, 1 linha após criar core1_.uc):
//   #include "qdsp5/qdsp5_capture_hook.h"
//   zeebo::qdsp5::install_capture_hook(core1_.uc);
// Requer <unicorn/unicorn.h> já incluído (o main já usa). Sem isso, este header
// compila como no-op puro (parser fica disponível para testes offline).

#pragma once
#include <cstdint>
#include <cstdio>
#include <vector>

namespace zeebo::qdsp5 {

using u8=uint8_t; using u32=uint32_t;

// ---- Endereços do trap (VERIFICADOS por disassembly / FINDINGS) ------------
// Consumer do dispatcher unificado. Hookar a EXECUÇÃO deste PC captura cada
// pacote no momento em que o firmware o processa.
static constexpr u32 kDispatcherVA = 0x16e8cba0; // dispatcher unified (FINDINGS)
static constexpr u32 kConsumerVA   = 0x16e8cb96; // packet consumer  (FINDINGS)
static constexpr u32 kAudmgrProg   = 0x30000013; // AUDMGRPROG
static constexpr u32 kAudmgrCb     = 0x31000013; // AUDMGRCB
static constexpr u32 kAdspRtosAtom = 0x3000000a; // ADSPRTOSATOMPROG — caminho REAL de áudio (app→modem, task-1)
static constexpr u32 kAdspRtosMtoa = 0x3000000b; // ADSPRTOSMTOAPROG — callback modem→app

// Offsets no header ONCRPC (mirror de oncrpc_packet_header do main).
enum : u32 {
    OFF_PROGRAM   = 0x0C,
    OFF_VERSION   = 0x10,
    OFF_PROCEDURE = 0x14,
    OFF_PAYLOAD   = 0x80,
    PKT_STRIDE    = 0x500,
};

// ---- Parser PURO (testável offline, sem Unicorn) ---------------------------
// Recebe os bytes crus de um pacote e emite uma linha de captura. Retorna true
// se é um RPC AUDMGR (program == AUDMGRPROG) — o que interessa para áudio.
struct CapturedRpc {
    u32 program=0, version=0, procedure=0;
    u32 payload_off=OFF_PAYLOAD;
    bool is_audmgr=false;
};

inline u32 rd32le(const u8* p){ return u32(p[0])|(u32(p[1])<<8)|(u32(p[2])<<16)|(u32(p[3])<<24); }

inline CapturedRpc parse_packet(const u8* pkt, size_t len) {
    CapturedRpc c;
    if (len < OFF_PAYLOAD) return c;
    c.program   = rd32le(pkt+OFF_PROGRAM);
    c.version   = rd32le(pkt+OFF_VERSION);
    c.procedure = rd32le(pkt+OFF_PROCEDURE);
    c.is_audmgr = (c.program == kAudmgrProg);
    return c;
}

// Nome legível da proc AUDMGR (ordinais VERIFICADOS, notes §6).
inline const char* audmgr_proc_name(u32 p) {
    switch (p) {
        case 0: return "null";
        case 1: return "enable_client";
        case 2: return "disable_client";
        case 3: return "suspend_event_rsp";
        case 4: return "register_operation_listner";
        case 5: return "unregister_operation_listner";
        case 6: return "register_codec_listener";
        case 7: return "get_rx_sample_rate";
        case 8: return "get_tx_sample_rate";
        case 9: return "set_device_mode";
        default: return "?";
    }
}

// Rótulo do serviço a partir do program id (todos VERIFICADOS, task-1/notes §6).
inline const char* svc_name(u32 prog) {
    if (prog == kAudmgrProg)   return "AUDMGR";
    if (prog == kAudmgrCb)     return "AUDMGRCB";
    if (prog == kAdspRtosAtom) return "ADSPRTOSATOM";  // caminho real de áudio app→modem
    if (prog == kAdspRtosMtoa) return "ADSPRTOSMTOA";  // callback modem→app
    return "other";
}

// Emite a linha de captura + hexdump do payload (até `dump` bytes).
inline void report_capture(const CapturedRpc& c, const u8* pkt, size_t len, size_t dump=64) {
    const char* svc = svc_name(c.program);
    const char* nm  = c.is_audmgr ? audmgr_proc_name(c.procedure) : "?";
    printf("[Q0.1] RPC prog=0x%08x(%s) vers=%u proc=%u(%s) len=%zu\n",
           c.program, svc, c.version, c.procedure, nm, len);
    if (len > OFF_PAYLOAD) {
        size_t n = len - OFF_PAYLOAD; if (n > dump) n = dump;
        printf("[Q0.1]   payload@+0x80:");
        for (size_t i=0;i<n;i++) printf(" %02x", pkt[OFF_PAYLOAD+i]);
        printf("%s\n", (len-OFF_PAYLOAD>dump)?" ...":"");
        // Palpite decodificado sob a struct-de-wire AUDMGR (notes §6c: 2x u32 ou
        // bool+2xu32). Marcado como INFERÊNCIA, não fato.
        if (c.is_audmgr && c.procedure==9 && n>=8) {
            printf("[Q0.1]   [infer] set_device_mode args (leafA guess): "
                   "w0=0x%08x w1=0x%08x\n",
                   rd32le(pkt+OFF_PAYLOAD), rd32le(pkt+OFF_PAYLOAD+4));
        }
    }
}

// ---- Adapter Unicorn (só compila se <unicorn/unicorn.h> presente) ----------
#ifdef UNICORN_MAX_VERSION   // símbolo definido pelo header do unicorn
// não confiável entre versões; usar guarda alternativa:
#endif
#if defined(__UNICORN_H__) || defined(UNICORN_ENGINE) || defined(UC_API_MAJOR) || defined(UC_VERSION_MAJOR)
#define QDSP5_HAVE_UNICORN 1
#endif

#ifdef QDSP5_HAVE_UNICORN
// Callback UC_HOOK_CODE: dispara quando o PC == consumer/dispatcher. Lê o ponteiro
// do pacote de r0 (convenção do consumer AMSS) e faz o dump. Não altera estado.
inline void qdsp5_code_cb(uc_engine* uc, uint64_t address, uint32_t /*size*/, void* /*user*/) {
    if (address != kConsumerVA && address != kDispatcherVA) return;
    u32 pkt_ptr=0;
    if (uc_reg_read(uc, UC_ARM_REG_R0, &pkt_ptr) != UC_ERR_OK || !pkt_ptr) return;
    u8 buf[OFF_PAYLOAD+64];
    if (uc_mem_read(uc, pkt_ptr, buf, sizeof(buf)) != UC_ERR_OK) return;
    // len real do pacote não está em r0; usamos o dump fixo (header+64) — suficiente
    // para ler program/proc e o começo do payload.
    CapturedRpc c = parse_packet(buf, sizeof(buf));
    report_capture(c, buf, sizeof(buf));
}

// Instala o hook. Retorna true se registrado. UMA linha no main.
inline bool install_capture_hook(uc_engine* core1_uc) {
    if (!core1_uc) return false;
    static uc_hook h;
    uc_err e = uc_hook_add(core1_uc, &h, UC_HOOK_CODE, (void*)qdsp5_code_cb,
                           nullptr, kConsumerVA, kDispatcherVA);
    if (e != UC_ERR_OK) {
        printf("[Q0.1] uc_hook_add falhou: %d\n", (int)e);
        return false;
    }
    printf("[Q0.1] capture hook instalado @ consumer=0x%08x dispatcher=0x%08x\n",
           kConsumerVA, kDispatcherVA);
    return true;
}
#else
// No-op quando compilado sem Unicorn (ex.: smoke test). O parser continua usável.
inline bool install_capture_hook(void* /*core1_uc*/) {
    printf("[Q0.1] (sem Unicorn) hook não instalado; parser disponível p/ testes.\n");
    return false;
}
#endif

} // namespace zeebo::qdsp5
