// zeebo_bootinfo.h — QW1: parser/enumerador determinístico do OKL4 __okl4_bootinfo.
// ---------------------------------------------------------------------------
// Autocontido e neutro de kernel. Reproduz o layout do bloco BootInfo emitido
// pelo Elfweaver do OKL4/Iguana e embutido no ELF da partição 0:APPS do Zeebo
// (firmware 1.1.2). Cada record tem cabeçalho de 32 bits:
//
//   word0 = (type << 16) | len_bytes      // len inclui os 4 bytes do cabeçalho
//   word1..                                // payload (len-4 bytes, u32 LE)
//
// Tags relevantes (bootinfo.h do OKL4 2.1):
//   BI_TAG_EMPTY      = 1   payload[0] = magic 0x1960021d
//   BI_TAG_INIT       = 4
//   BI_TAG_VIRT_POOLS = 5   payload = { base, end }   (faixa virtual [base,end])
//   BI_TAG_PHYS_POOLS = 6   payload = { base, end }   (faixa física  [base,end])
//
// Reutiliza zeebo_l4::Fpage (mesma repr. bruta de fpage_t V4 do decoder MMU),
// evitando duplicar a semântica de bits rwx/size/base.
//
// Validado por bytes reais de nand/1.1.2_APPS.bin (BootInfo @ file 0x57000).
// ---------------------------------------------------------------------------
#ifndef ZEEBO_BOOTINFO_H
#define ZEEBO_BOOTINFO_H

#include <cstdint>
#include <cstddef>
#include <vector>

#include "zeebo_l4_mmu.h"  // zeebo_l4::Fpage

namespace zeebo_bi {

using u8  = uint8_t;
using u16 = uint16_t;
using u32 = uint32_t;
using u64 = uint64_t;

// --- Tags do BootInfo -------------------------------------------------------
enum BiTag : u32 {
    BI_TAG_EMPTY      = 1,
    BI_TAG_INIT       = 4,
    BI_TAG_VIRT_POOLS = 5,
    BI_TAG_PHYS_POOLS = 6,
};

static constexpr u32 BI_MAGIC = 0x1960021du; // assinatura do BI_TAG_EMPTY

// --- Record cru -------------------------------------------------------------
struct Record {
    u32              offset;   // file offset absoluto do cabeçalho
    u32              type;     // word0 >> 16
    u32              len;      // word0 & 0xffff (inclui cabeçalho)
    std::vector<u32> payload;  // (len-4)/4 palavras u32 LE
};

// --- Par de pool (VIRT/PHYS) ------------------------------------------------
struct Pool {
    u32 base;
    u32 end;
};

// --- BootInfo decodificado --------------------------------------------------
struct BootInfo {
    u32                 base_offset = 0;      // file offset do primeiro record
    u32                 magic       = 0;      // magic do BI_TAG_EMPTY
    std::vector<Record> records;

    // Coleta todos os pares {base,end} de uma tag de pool na ordem do firmware.
    std::vector<Pool> pools(u32 tag) const {
        std::vector<Pool> out;
        for (const auto& r : records) {
            if (r.type == tag && r.payload.size() >= 2)
                out.push_back(Pool{ r.payload[0], r.payload[1] });
        }
        return out;
    }
};

// Lê u32 LE em `off` (sem checar limites — o chamador garante via locate()).
inline u32 rd32(const u8* p, size_t off) {
    return (u32)p[off] | ((u32)p[off+1] << 8) |
           ((u32)p[off+2] << 16) | ((u32)p[off+3] << 24);
}

// Localiza o bloco BootInfo pela magic real e enumera seus records.
// Retorna false se a magic não for encontrada ou não houver espaço p/ o record.
inline bool locate(const u8* data, size_t size, BootInfo& out) {
    // A magic 0x1960021d é payload[0] do BI_TAG_EMPTY, que abre o bloco.
    // O cabeçalho do EMPTY (word0 = (1<<16)|16 = 0x00010010) precede a magic.
    for (size_t i = 0; i + 8 <= size; i += 4) {
        if (rd32(data, i) == 0x00010010u && rd32(data, i + 4) == BI_MAGIC) {
            out.base_offset = (u32)i;
            out.magic       = BI_MAGIC;
            out.records.clear();
            size_t o = i;
            // Percorre records até len==0 ou fim seguro.
            while (o + 4 <= size) {
                u32 w0 = rd32(data, o);
                u32 len = w0 & 0xffffu;
                u32 tp  = w0 >> 16;
                if (len < 4 || o + len > size) break;
                Record r;
                r.offset = (u32)o;
                r.type   = tp;
                r.len    = len;
                for (u32 k = 4; k + 4 <= len; k += 4)
                    r.payload.push_back(rd32(data, o + k));
                out.records.push_back(r);
                o += len;
                // O bloco BootInfo do Iguana cabe folgadamente em <8 KiB.
                if (o - i > 0x2000) break;
            }
            return !out.records.empty();
        }
    }
    return false;
}

// Decompõe [va_start, va_end) em fpages de 2^page_log2 bytes com perms rwx.
// HIPÓTESE (NÃO firmware-derived): reproduz o comportamento esperado de
// mempool_init (@0xb000d5b4) — min(virt,phys)=1MiB, incremento de 0x100000 por
// página. ATENÇÃO: os argumentos (va_start/va_end) são constantes fornecidas
// pelo chamador; NÃO são lidos de nenhum record/descriptor/byte do firmware. O
// range de 96 MiB 0xb0d00000..0xb6d00000 não aparece em nenhum BootInfo record.
// Cada fpage usa a repr. bruta de zeebo_l4::Fpage: raw = base|(size_log2<<4)|rwx.
inline std::vector<zeebo_l4::Fpage>
enumerate_fpages(u32 va_start, u32 va_end, u32 page_log2, u32 rwx) {
    std::vector<zeebo_l4::Fpage> out;
    u32 step = 1u << page_log2;
    for (u32 va = va_start; va < va_end; va += step) {
        u32 raw = (va & ~0x3ffu) | ((page_log2 & 0x3fu) << 4) | (rwx & 7u);
        out.push_back(zeebo_l4::Fpage(raw));
    }
    return out;
}

} // namespace zeebo_bi

#endif // ZEEBO_BOOTINFO_H
