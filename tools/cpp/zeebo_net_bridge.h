// zeebo_net_bridge.h — Ponte de rede estilo Flycast p/ o Zeebo LLE (MSM7201A).
// -----------------------------------------------------------------------------
// Intercepta os canais SMD de DADOS (DATA5..DATA20) em SMEM, desencapsula o
// framing PPP/HDLC (ou RmNet/LAN-LLC) para pacotes IP, e faz bridge p/ sockets
// POSIX do host (ou uma pilha lwIP/picoTCP user-space) — exatamente o padrão do
// Flycast modem emulation (picoTCP/lwIP interceptando os buffers de modem PPP/IP
// e bridgeando p/ o host).
//
// SÓ ESQUELETO + constantes VERIFICADAS por RE do firmware (nand/1.1.2_*.bin).
// Ver notes/data_services_net_rpc.md p/ a prova de cada endereço/prog id.
// Campos marcados [infer] são hipótese de layout ainda não byte-provada.
//
// Clean-room: derivado por OBSERVAÇÃO do firmware NAND (permitido). Nada copiado
// do a1Sim. Autor: Rafael Requião.
#pragma once
#include <cstdint>
#include <cstddef>
#include <vector>
#include <array>
#include <functional>

namespace zeebo::net {

using u8 = uint8_t; using u16 = uint16_t; using u32 = uint32_t;

// ============================================================================
// 1. Endereços / constantes VERIFICADAS (primary-source; NÃO inventar)
// ============================================================================
namespace addr {
    constexpr u32 SMEM_BASE          = 0x01F00000; // 2MB shared mem (ARM11<->ARM9)
    constexpr u32 SMEM_SIZE          = 0x00200000;
    constexpr u32 SMD_HALF_CHANNEL   = 0x1755d1dc; // struct smd_half_channel (AMSS)
    constexpr u32 A2M_DOORBELL_BASE  = 0xC0100400; // MSM_A2M_INT(n) = base + n*4
    constexpr u32 ARM9_VIC_BASE      = 0xC0000000; // M2A entrega IRQ ao modem
}

// ONCRPC control-plane de Data Services (VERIFICADO — ver nota §2).
namespace prog {
    constexpr u32 SMD_BRIDGE_ATOM    = 0x30000038; // App->Modem stream bridge (svc=AMSS)
    constexpr u32 SMD_BRIDGE_ATOM_CB = 0x31000038;
    constexpr u32 SMD_BRIDGE_MTOA    = 0x30000039; // Modem->App stream bridge (svc=APPS)
    constexpr u32 SMD_BRIDGE_MTOA_CB = 0x31000039;
    constexpr u32 SMD_PORT_MGR       = 0x30000024; // gerente de portas SMD
    constexpr u32 DSUCSDMPSHIM       = 0x30000047; // CS-data multiproc shim
    constexpr u32 DSMP_UMTS_APPS_APIS= 0x30000046; // UMTS PS multiproc apis
    // DATA_ON_APPS_ATOM_APIS: prog id ainda HIPÓTESE (regstr existe, const não
    // adjacente ao stub — ver nota §4). NÃO fixar até byte-prova.
    constexpr u32 DATA_ON_APPS_ATOM_APIS_INFER = 0x3000003b; // [infer]
}

// ============================================================================
// 2. Convenção de canais SMD (VERIFICADA — tabela em smd_internal.c)
// ============================================================================
enum SmdState : u8 { SMD_SS_CLOSED=0, SMD_SS_OPENING=1, SMD_SS_OPENED=2,
                     SMD_SS_FLUSHING=3 };

// Edge type: a assertion `type == SMD_APPS_MODEM failed` prova que os canais de
// dados são do par Apps<->Modem (convenção SMD_APPS_MODEM_DATA do kernel MSM).
enum SmdEdge : u8 { SMD_APPS_MODEM=0 };

// Nomes reais dos canais de dados/bridge (extraídos do binário, na ordem do dump).
// DATA5..DATA20 = streams de pacote (1 por PDP/iface); BRG_x = SMD_BRIDGE;
// CS_* = circuit-switched/control; RPCCALL/RPCRPY = transporte ONCRPC.
// NOTA: DATA11 está AUSENTE de propósito — o dump não o lista entre DATA10 e
// DATA12; não é erro de digitação da tabela verificada por RE (canal reservado
// ou de uso interno); a enumeração segue a ordem exata do binário.
inline constexpr std::array<const char*, 16> kDataChannels = {
    "DATA5","DATA6","DATA7","DATA8","DATA9","DATA10","DATA12","DATA13",
    "DATA14","DATA15","DATA16","DATA17","DATA18","DATA19","DATA20","BRG_5"
};

// smd_half_channel — o descriptor por-lado do canal (VERIFICADO em FINDINGS 3p).
// Layout do FIFO após o header é [infer] (Qualcomm SMD: header + ring buffer).
#pragma pack(push,1)
struct SmdHalfChannel {
    u8  state;        // SmdState
    u8  fdcted;       // "force-disconnect / flow control" [infer]
    u8  fbcd;         // [infer]
    u8  fda;          // [infer] (link/flow bits)
    u32 read_index;   // read_ptr no ring
    u32 write_index;  // write_ptr no ring
    // seguido pelo ring buffer de bytes do stream (tamanho por-canal em SMEM).
};
#pragma pack(pop)

// ============================================================================
// 3. Framing do link — desencapsula p/ IP (estilo Flycast)
// ============================================================================
// O stream SMD de um canal DATAn carrega OU PPP/HDLC (dial-up PDP; ver
// ps_ppp_fsm.c / ps_hdlc_lib.c) OU RmNet/LAN-LLC (ethernet-like; ps_lan_llc.c).
enum class LinkFraming { PPP_HDLC, RMNET_LLC, RAW_IP };

struct IpPacket { std::vector<u8> bytes; };  // um datagrama IP completo

// ============================================================================
// 4. Backend de rede do host (POSIX sockets ou lwIP/picoTCP user-space)
// ============================================================================
struct INetBackend {
    virtual ~INetBackend() = default;
    // Envia um datagrama IP do guest p/ a Internet real (NAT/raw/pilha user-space).
    virtual void tx_to_host(const IpPacket& pkt) = 0;
    // Coleta datagramas IP vindos do host destinados ao guest (não-bloqueante).
    virtual bool rx_from_host(IpPacket& out) = 0;
};

// Implementação padrão: NAT via socket POSIX do host (a preencher).
class PosixNatBackend : public INetBackend {
public:
    void tx_to_host(const IpPacket& pkt) override;   // TODO: raw/UDP/TCP NAT
    bool rx_from_host(IpPacket& out) override;        // TODO: poll host sockets
};

// ============================================================================
// 5. A ponte: SMD data-plane  <->  host network
// ============================================================================
// Uso: registrar hooks de MMIO/mem em SMEM que chamem on_channel_write quando o
// APPS empurra bytes no ring de um DATAn, e chamar pump() por slice p/ escoar
// respostas do host de volta ao ring + tocar o doorbell M2A.
class NetBridge {
public:
    // guest_ram: ponteiro base p/ a janela SMEM mapeada no host (addr::SMEM_BASE).
    NetBridge(u8* guest_ram_smem, INetBackend* backend);

    // Descoberta/abertura de canal: chamado quando um half-channel transiciona
    // p/ SMD_SS_OPENED. Associa cid -> nome (DATAn) e framing detectado.
    void on_channel_open(u32 cid, const char* name, u32 half_chan_gpa);

    // Produtor: o APPS escreveu `len` bytes no ring do canal `cid`.
    // A ponte drena o ring, desframe (PPP/HDLC|RmNet) -> IP, e tx_to_host.
    void on_channel_write(u32 cid);

    // Por-slice: puxa IP do host, re-frame p/ o link do canal, escreve no ring
    // do lado modem, avança write_index e sinaliza o doorbell M2A -> ARM9 VIC.
    void pump();

    // Callback p/ o orquestrador levantar a IRQ M2A (canal -> linha VIC).
    std::function<void(u32 cid)> raise_m2a_doorbell;

    // Control-plane: responder ONCRPC de setup (SMD_BRIDGE/DATA_ON_APPS/DSUCSD)
    // com sucesso mínimo p/ o APPS acreditar que a iface subiu. Retorna true se
    // o programa foi reconhecido como control-plane de dados.
    bool handle_control_rpc(u32 program, u32 procedure, const u8* body, u32 blen);

private:
    struct Channel {
        u32 cid = 0;
        const char* name = nullptr;
        u32 half_chan_gpa = 0;           // GPA do SmdHalfChannel no SMEM
        LinkFraming framing = LinkFraming::PPP_HDLC;
        std::vector<u8> ppp_reasm;       // reassembly parcial do HDLC
    };
    SmdHalfChannel* hc(u32 cid);          // resolve descriptor no guest_ram_
    void deframe_and_tx(Channel& c, const std::vector<u8>& stream);
    void frame_and_enqueue(Channel& c, const IpPacket& pkt);

    u8* guest_ram_ = nullptr;             // base = addr::SMEM_BASE
    INetBackend* backend_ = nullptr;
    std::vector<Channel> channels_;
};

// ---------------------------------------------------------------------------
// Helpers de framing (declaração; impl em zeebo_net_bridge.cpp)
// ---------------------------------------------------------------------------
// PPP/HDLC (RFC1662): 0x7E flag, 0x7D escape, FCS-16. Devolve IPs completos.
std::vector<IpPacket> ppp_hdlc_deframe(std::vector<u8>& reasm_state,
                                       const u8* data, size_t len);
std::vector<u8>       ppp_hdlc_frame(const IpPacket& ip);

// RmNet: cabeçalho ethernet/LLC (ps_lan_llc.c) sobre o stream -> IP.
std::vector<IpPacket> rmnet_deframe(const u8* data, size_t len);
std::vector<u8>       rmnet_frame(const IpPacket& ip);

} // namespace zeebo::net
