// qdsp5_rpc.h — ONCRPC / SMD command framing for the MSM7201A QDSP5 command plane.
// Skeleton (FASE 1 do QDSP5_TODO.md §1/§2). SÓ a ESTRUTURA verificada por
// disassembly (FINDINGS 2yy/2zz/3a-3c) — endereços e offsets são primary-source;
// os proc IDs concretos ainda são PLACEHOLDER até Q0.2 capturar do firmware real.
#pragma once
#include <cstdint>
#include <vector>

namespace zeebo::qdsp5 {

using u8 = uint8_t; using u16 = uint16_t; using u32 = uint32_t;

// ---- Verified AMSS addresses (FINDINGS 2yy/2zz/3p) ------------------------
// NÃO inventar: todos confirmados por assertion strings ou disassembly.
namespace addr {
    constexpr u32 SMD_HALF_CHANNEL = 0x1755d1dc; // struct smd_half_channel
    constexpr u32 RPC_QUEUE_HEAD   = 0x17571748; // +00 head +04 tail +08 count +1c init
    constexpr u32 DISPATCH_ENGINE  = 0x16e8cba0; // packet dispatch (proc@+0x20, body@+0x80)
    constexpr u32 PACKET_CONSUMER  = 0x16e8cb96; // router r0==2 (SMD_SS_OPENED) target
}

// ---- Packet framing (session 2zz) ----------------------------------------
constexpr u32 kMaxPayload   = 0x500; // 1280 bytes, verified assertion budget
constexpr u32 kPayloadOff   = 0x80;  // body starts here (VERIFIED vs emulador oncrpc_packet_header)
// NB: NÃO usar offsets manuais p/ program/proc — o parse faz memcpy do RpcHeader e
// lê pelos CAMPOS. No struct (== emulador), program@+0x0C, procedure@+0x14. Os antigos
// kProcIdOff=0x20/kLenOff=0x24 estavam ERRADOS e foram removidos (não eram usados).
constexpr u32 kRpcSuccess   = 0x1b59;// RPC success STATUS code (NÃO é um proc id!)

// ---- SMD half-channel states (session 3p / smd_bridge) --------------------
enum SmdState : u8 { SMD_CLOSED=0, SMD_OPENING=1, SMD_OPENED=2, SMD_FLUSHING=3 };

// ---- ONCRPC program numbers (VERIFIED from nand/1.1.2_AMSS.bin) -----------
// See notes/qdsp5_proc_ids.md. AUDMGRPROG confirmed by byte-adjacency to the
// "unable to register (AUDMGRPROG...)" string; callback pairs 0x3100xxxx.
namespace prog {
    constexpr u32 AUDMGR     = 0x30000013; // audio manager service (audmgr_svc.c)
    constexpr u32 AUDMGR_CB  = 0x31000013; // audmgr callback program
    // ADSP_RTOS: o caminho REAL de transporte de PCM/comandos p/ as command
    // queues do QDSP5. AUDPLAY/AUDPP NÃO são programas ONCRPC — são tasks do DSP
    // image alcançadas por estas filas. Prova: adjacência (dist<=32) às strings de
    // stub adsprtosatom_clnt.c/svc.c e adsprtosmtoa_*.c nos dois binários.
    // Ver notes/qdsp5_program_ids.md e notes/QDSP5_INDEX.md.
    constexpr u32 ADSPRTOSATOM = 0x3000000a; // app->modem: INJETA comando/PCM nas filas (proc adsp_rtos_app_to_modem_command)
    constexpr u32 ADSPRTOSMTOA = 0x3000000b; // modem->app: callback de conclusão/eventos do DSP
    // NOTE: 0x30000060 (old "MSM Audio/QDSP service") was FABRICATED — do not use.
}

// ---- AUDMGR procedure ORDINALS (VERIFIED from ONCRPC apis table) ----------
// Recovered from the apis array @ file 0xfab58c in 1.1.2_AMSS.bin (base VA
// 0x163a8000, solved by 10/10 name-pointer hits; stride 0x14, array index =
// proc number). See notes/qdsp5_proc_ids.md §6. These are NOW CONFIRMED, not
// guesses — and they CORRECT the earlier placeholder guesses.
namespace audmgr_proc {
    enum : u32 {
        NULLPROC              = 0,
        ENABLE_CLIENT         = 1,
        DISABLE_CLIENT        = 2,
        SUSPEND_EVENT_RSP     = 3,
        REGISTER_OPR_LISTNER  = 4,
        UNREGISTER_OPR_LISTNER= 5,
        REGISTER_CODEC_LISTNER= 6,
        GET_RX_SAMPLE_RATE    = 7,   // was guessed 8 — corrected
        GET_TX_SAMPLE_RATE    = 8,   // was guessed 9 — corrected
        SET_DEVICE_MODE       = 9,   // was guessed 7 — corrected
    };
}

// ---- The QDSP5 task array (VERIFIED, CORRECTED — larger than sessions 3a-3c) --
// AUDPLAY0-4 are the real PCM/bitstream decoder tasks games feed; AUDPP is the
// post-processor DOWNSTREAM (EQ/mix). See notes/qdsp5_proc_ids.md §3.
enum class Engine : u32 { Audmgr, Audplay, Audpp, Audrec, Jpeg, Vfe, Voice, Unknown };

// Command-queue string-literal addresses (evidência: assertion cmd_size strings).
struct QueueDef { Engine engine; const char* name; u32 str_addr; };
constexpr QueueDef kQueues[] = {
    { Engine::Voice,   "UPVOCPROCQUEUE",                  0x16ea9cb0 },
    { Engine::Vfe,     "VFECOMMANDSCALEQUEUE",            0x16ea9ce8 },
    { Engine::Vfe,     "VFECOMMANDTABLEQUEUE",            0x16ea9d40 },
    { Engine::Vfe,     "VFECOMMANDQUEUE",                 0x16ea9d88 },
    { Engine::Jpeg,    "UPJPEGACTIONCMDQUEUE",            0x16ea9dd0 },
    { Engine::Jpeg,    "UPJPEGCFGCMDQUEUE",               0x16ea9e18 },
    { Engine::Audpp,   "UPAUDPPCMD2QUEUE",                0x16ea9e68 },
    // Added from AMSS assertion strings (addresses TODO: not yet located in dump):
    { Engine::Audpp,   "UPAUDPPCMD1QUEUE",                0 },
    { Engine::Audpp,   "UPAUDPPCMD3QUEUE",                0 },
    { Engine::Audplay, "UPAUDPLAY0BITSTREAMCTRLQUEUE",    0 },
    { Engine::Audplay, "UPAUDPLAY1BITSTREAMCTRLQUEUE",    0 },
    { Engine::Audplay, "UPAUDPLAY2BITSTREAMCTRLQUEUE",    0 },
    { Engine::Audplay, "UPAUDPLAY3BITSTREAMCTRLQUEUE",    0 },
    { Engine::Audplay, "UPAUDPLAY4BITSTREAMCTRLQUEUE",    0 },
    { Engine::Audrec,  "UPAUDRECBITSTREAMQUEUE",          0 },
    { Engine::Audrec,  "UPAUDRECCMDQUEUE",                0 },
};

// ---- QDSP5 command-queue identity (VERIFIED task<->queue association) ------
// The destination command queue is what selects the DSP task. This association
// is PRIMARY-SOURCE (assertion strings `cmd_size <= QDSP_<task>_<queue>...`;
// notes/qdsp5_proc_ids.md §3, notes/qdsp5_program_ids.md). AUDPLAY0-4 bitstream
// queues drive the real per-voice decoder tasks a game feeds (this is the path
// Zeetris MP3 uses); AUDPP queues are the downstream post-processor/host-PCM.
//
// HONEST LIMIT: the byte offset inside the ADSP_RTOS app_to_modem_command
// sub-payload that carries this queue id is UNVERIFIED (Q0.1-pending, needs one
// captured guest packet). So this is the routing SEAM: callers pass a resolved
// QueueId; we do NOT parse a queue id out of a guessed payload layout.
enum class QueueId : u32 {
    Unknown = 0,
    UpAudPlay0BitstreamCtrl,
    UpAudPlay1BitstreamCtrl,
    UpAudPlay2BitstreamCtrl,
    UpAudPlay3BitstreamCtrl,
    UpAudPlay4BitstreamCtrl,
    UpAudPpCmd1,
    UpAudPpCmd2,
    UpAudPpCmd3,
    UpAudRecBitstream,
    UpAudRecCmd,
    UpJpegActionCmd,
    UpJpegCfgCmd,
    VfeCommand,
    VfeCommandScale,
    VfeCommandTable,
    UpVocProc,
};

// Maps a VERIFIED queue id to its DSP task engine. Returns Unknown for anything
// unmapped — no guessing.
Engine classify_queue(QueueId q);

// ---- ONCRPC CALL header (mirror of oncrpc_packet_header in smd_bridge) -----
// program/procedure marcados UNVERIFIED: hardcoded no código atual, sem dump.
#pragma pack(push,1)
struct RpcHeader {
    u32 xid;
    u32 msg_type;      // 0 = CALL
    u32 rpc_version;   // 2
    u32 program;       // 0x30000013 = AUDMGRPROG (VERIFIED, notes/qdsp5_proc_ids.md)
    u32 version;       // AUDMGRVERS
    u32 procedure;     // proc id @ +0x14 (posição do campo) — name known, ORDINAL unconfirmed (Q0.2)
    u32 cred_flavor, cred_length, verf_flavor, verf_length;
    // payload @ +0x80
};
#pragma pack(pop)
static_assert(sizeof(RpcHeader) <= kPayloadOff, "header must fit before +0x80");

// A decoded command as seen by an engine (what the dispatcher hands over).
struct Command {
    Engine   engine   = Engine::Unknown;
    u32      program  = 0;
    u32      proc_id  = 0;
    std::vector<u8> payload;   // bytes copied from +0x80, length from +0x24
};

// ---- Guest-memory accessor (mesma forma do GuestMachine do skeleton GPU) ---
// Permite ao engine seguir ponteiros que o payload carrega (ex.: buffer PCM em
// shared RAM) sem acoplar ao Unicorn — smoke tests injetam um lambda.
struct QdspGuest {
    // Lê `size` bytes do endereço virtual `va` do guest para `dst`. false = falha.
    bool (*read)(u32 va, void* dst, u32 size, void* ctx) = nullptr;
    void* ctx = nullptr;
    bool rd(u32 va, void* dst, u32 size) const {
        return read ? read(va, dst, size, ctx) : false;
    }
};

// Classifica um pacote em engine a partir de program+proc (TABELA A PREENCHER
// em Q0.2 — hoje devolve Unknown p/ tudo, deliberadamente honesto).
Engine classify(u32 program, u32 proc_id);

} // namespace zeebo::qdsp5
