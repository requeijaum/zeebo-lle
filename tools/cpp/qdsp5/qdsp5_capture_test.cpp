// qdsp5_capture_test.cpp — prova o parser Q0.1 offline (sem Unicorn, sem boot).
// Monta um pacote ONCRPC AUDMGR set_device_mode como o firmware escreveria e
// verifica que o parser extrai program/proc/payload corretos.
#include "qdsp5_capture_hook.h"
#include <cstring>
#include <cstdlib>

using namespace zeebo::qdsp5;

static void put32(u8* p, u32 v){ p[0]=v; p[1]=v>>8; p[2]=v>>16; p[3]=v>>24; }

int main() {
    int fails = 0;

    // ---- Caso 1: AUDMGR set_device_mode (proc 9) ----
    std::vector<u8> pkt(OFF_PAYLOAD + 8, 0);
    put32(&pkt[0x00], 0xAABBCCDD);          // xid
    put32(&pkt[0x04], 0);                    // msg_type CALL
    put32(&pkt[0x08], 2);                    // rpc_version
    put32(&pkt[OFF_PROGRAM],   kAudmgrProg); // 0x30000013
    put32(&pkt[OFF_VERSION],   1);
    put32(&pkt[OFF_PROCEDURE], 9);           // set_device_mode
    put32(&pkt[OFF_PAYLOAD+0], 0x00000002);  // leafA w0 (ex.: rx_dev)
    put32(&pkt[OFF_PAYLOAD+4], 0x0000AC44);  // leafA w1 (ex.: 44100)

    CapturedRpc c = parse_packet(pkt.data(), pkt.size());
    report_capture(c, pkt.data(), pkt.size());
    if (!c.is_audmgr)            { printf("FAIL: não reconheceu AUDMGR\n"); fails++; }
    if (c.procedure != 9)        { printf("FAIL: proc != 9\n"); fails++; }
    if (std::strcmp(audmgr_proc_name(9),"set_device_mode")) { printf("FAIL: nome proc\n"); fails++; }

    // ---- Caso 2: pacote de outro serviço (não AUDMGR) ----
    std::vector<u8> other(OFF_PAYLOAD, 0);
    put32(&other[OFF_PROGRAM], 0x3000006d);  // outro program
    CapturedRpc c2 = parse_packet(other.data(), other.size());
    if (c2.is_audmgr)            { printf("FAIL: falso positivo AUDMGR\n"); fails++; }

    // ---- Caso 3: pacote curto (guard) ----
    std::vector<u8> tiny(0x10, 0);
    CapturedRpc c3 = parse_packet(tiny.data(), tiny.size());
    if (c3.program != 0)         { printf("FAIL: pacote curto deveria dar program=0\n"); fails++; }

    // ---- install_capture_hook em no-op (sem Unicorn) não deve crashar ----
    install_capture_hook(nullptr);

    if (fails==0) printf("\nALL PASS (parser Q0.1 validado offline)\n");
    else          printf("\n%d FALHAS\n", fails);
    return fails==0 ? 0 : 1;
}
