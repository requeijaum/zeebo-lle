// pm4_adreno.h — PM4 command-stream walker for the Adreno 130 ring buffer.
// FASE 4 producer (GPU_TODO.md §6/§7). Skeleton: header decode + walk loop only.
// NÃO ligar contra hardware antes do MAP_CONTROL (FINDINGS 5a) — sem guest não há
// ring real para consumir. Aqui fica a ESTRUTURA, validada com streams sintéticos.
//
// Linhagem: Adreno = ex-ATI/AMD Imageon. O PM4 do Zeebo (A2xx/Yamato) é a MESMA
// família do PM4 GCN que o ShadPS4 decodifica (Liverpool). Modelo de walk copiado
// (conceito, não código) de shadps4 src/video_core/amdgpu/liverpool.cpp:
//   - header dword: bits[31:30]=type; type3: bits[15:8]=opcode (IT_*),
//     bits[29:16]=count (=N-1, N dwords no corpo); avança NumWords()+1.
//   - type0: escreve `count+1` registradores sequenciais a partir de base.
//   - type2: NOP de 1 dword (padding/realinhamento). type1: reservado.
// IMPORTANTE: os OPCODES do A2xx NÃO são os do GCN. A2xx usa CP_* (freedreno
// a2xx: adreno_pm4.xml). Tabela abaixo = subconjunto A2xx real, não GCN.
#pragma once
#include <cstdint>
#include <functional>
#include "igpu_rasterizer.h"

namespace zeebo::gpu {

// A2xx (Adreno 200) type-3 opcodes — VALORES verificados contra o kernel MSM
// real (drivers/gpu/msm/adreno_pm4types.h). ATENÇÃO DE PROVENIÊNCIA: o Adreno 130
// do Zeebo é ATI **Imageon (Z430/Z180), ANTERIOR ao A2xx**. O formato de PACKET
// PM4 é estável desde o radeon/Imageon, mas a TABELA exata de IT_OPCODE do 130
// pode divergir destes valores A2xx. Tratar como HIPÓTESE até validar contra um
// ring buffer real do Zeebo (bloqueado pelo MAP_CONTROL, FINDINGS 5a).
enum class Cp2xx : u32 {
    Nop            = 0x10,  // CP_NOP
    Reg_Rmw        = 0x21,  // CP_REG_RMW
    DrawIndx       = 0x22,  // CP_DRAW_INDX  (draw com index buffer)
    DrawIndx2      = 0x36,  // CP_DRAW_INDX_2 (draw inline/auto index)
    Set_Constant   = 0x2d,  // CP_SET_CONSTANT (load regs/consts; ver layout abaixo)
    Load_Const_Ctx = 0x2e,  // CP_LOAD_CONSTANT_CONTEXT
    Im_Load        = 0x27,  // CP_IM_LOAD (load shader; raro em GLES1 fixed)
    Im_Load_Imm    = 0x2b,  // CP_IM_LOAD_IMMEDIATE
    Wait_For_Idle  = 0x26,  // CP_WAIT_FOR_IDLE
    Indirect_Buf   = 0x3f,  // CP_INDIRECT_BUFFER_PFE (recursão — ver TODO abaixo)
    Event_Write    = 0x46,  // CP_EVENT_WRITE
    Set_Bin_Mask   = 0x50,  // CP_SET_BIN_MASK  (binning/tile — essencial p/ fidelidade)
    Set_Bin_Select = 0x51,  // CP_SET_BIN_SELECT
};

union Pm4Header {
    u32 raw;
    struct { u32 _pad:30, type:2; } common;          // bits[31:30]
    // type0: base reg em bits[15:0], count(N-1) em bits[29:16]
    // type3: opcode em bits[15:8], count(N-1) em bits[29:16]
    u32 type0_base() const { return raw & 0xFFFF; }
    u32 count()      const { return ((raw >> 16) & 0x3FFF) + 1; } // N dwords no corpo
    u32 type3_op()   const { return (raw >> 8) & 0xFF; }
    u32 type()       const { return (raw >> 30) & 0x3; }
};

// Callbacks que o decoder dispara — implementados por quem liga no IGpuRasterizer.
struct Pm4Sink {
    std::function<void(u32 reg, u32 val)> write_reg;     // type0 / set_constant
    std::function<void(bool indexed, u32 count, Prim p)> draw;
    std::function<void()> wait_idle;
    // Resolve um endereço de IB (GPU VA) para ponteiro host+contagem. Sem isto,
    // TODOS os draws reais (que vivem em indirect buffers por bin) são perdidos.
    // Devolve {nullptr,0} enquanto o MAP_CONTROL não fornecer a tradução VA->host.
    std::function<std::pair<const u32*,size_t>(u32 gpu_va, u32 size_dw)> resolve_ib;
};

// Percorre um ring buffer (dwords) — mesmo esqueleto de walk do Liverpool.
// Retorna nº de packets processados. Guest reads via callback (não deref aqui).
inline u32 pm4_walk(const u32* dcb, size_t n_dwords, const Pm4Sink& sink) {
    size_t i = 0; u32 packets = 0;
    while (i < n_dwords) {
        Pm4Header h{ dcb[i] };
        const u32 t = h.type();
        if (t == 2) { ++i; continue; }                 // type2 = NOP 1-dword
        if (t == 0) {                                   // type0: writes N regs seq
            u32 base = h.type0_base(), cnt = h.count();
            for (u32 k = 0; k < cnt && i+1+k < n_dwords; ++k)
                if (sink.write_reg) sink.write_reg(base + k, dcb[i+1+k]);
            i += 1 + cnt; ++packets; continue;
        }
        if (t == 3) {                                   // type3: IT command
            u32 op = h.type3_op(), cnt = h.count();
            switch (static_cast<Cp2xx>(op)) {
                case Cp2xx::Nop: break;
                case Cp2xx::Wait_For_Idle: if (sink.wait_idle) sink.wait_idle(); break;
                case Cp2xx::DrawIndx:
                    if (sink.draw) { sink.draw(true,  cnt>1?dcb[i+2]:0, Prim::Triangles); }
                    break;
                case Cp2xx::DrawIndx2:
                    if (sink.draw) { sink.draw(false, cnt>0?dcb[i+1]:0, Prim::Triangles); }
                    break;
                case Cp2xx::Set_Constant: {
                    // Layout real (freedreno): DATA_1 = 0x00040000 | (BASE-0x2000),
                    // seguido de N-1 valores sequenciais a partir de BASE. NÃO são
                    // pares (offset,val). base extraído de DATA_1.
                    if (cnt >= 1 && i+1 < n_dwords) {
                        u32 base = (dcb[i+1] & 0xFFFF) + 0x2000; // aprox; refinar c/ dump real
                        for (u32 k=1; k<cnt && i+1+k < n_dwords; ++k)
                            if (sink.write_reg) sink.write_reg(base + (k-1), dcb[i+1+k]);
                    }
                    break;
                }
                case Cp2xx::Indirect_Buf: {
                    // CP_INDIRECT_BUFFER: corpo = [gpu_va, size_dw]. Recursão real
                    // depende de resolve_ib (VA->host), fornecido só após MAP_CONTROL.
                    if (cnt >= 2 && i+2 < n_dwords && sink.resolve_ib) {
                        auto [ib, ib_n] = sink.resolve_ib(dcb[i+1], dcb[i+2]);
                        if (ib && ib_n) packets += pm4_walk(ib, ib_n, sink);
                    }
                    break;
                }
                default: break;                          // opcodes ignorados no bring-up
            }
            i += 1 + cnt; ++packets; continue;
        }
        ++i; // type1 reservado — pula
    }
    return packets;
}

} // namespace zeebo::gpu
