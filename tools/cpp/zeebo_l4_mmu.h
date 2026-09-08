// zeebo_l4_mmu.h — OKL4 2.1 (Pistachio) L4_MapControl decoder & dispatcher
// ---------------------------------------------------------------------------
// Reimplementa, de forma autocontida e neutra de kernel, a ABI de L4_MapControl
// (syscall 0x14 / trap L4_TRAP_MAP_CONTROL) do microkernel OKL4 2.1 usado pelo
// Zeebo (ARM1176, arch/arm/pistachio v6).
//
// Fontes de referência (refs/okl4-2.1.1-fix7):
//   pistachio/include/map.h        -> map_control_t, phys_desc_t, perm_desc_t
//   pistachio/include/fpage.h      -> fpage_t (V4 flexpage, bits rwx/size/base)
//   pistachio/include/config.h     -> L4_FPAGE_BASE_BITS = 22 (ARM 32-bit)
//   pistachio/include/types.h      -> enum l4attrib_e (l4mem_*)
//   arch/arm/libs/l4/include/vregs.h -> UTCB layout: mr[] em offset de byte 64
//   pistachio/src/map.cc           -> ordem dos descritores:
//                                       MR[i*2]   = phys_desc_t (base física + attr)
//                                       MR[i*2+1] = fpage_t     (vaddr + size + rwx)
//
// map_control_t (bits, word de 32):
//   [0..5]  n         -> número do maior item; count = n + 1
//   [6..28] reservado
//   [29]    (window, só ARM v5)
//   [30]    q         -> query
//   [31]    m         -> modify
//
// phys_desc_t:
//   [0..5]  attr      -> l4attrib_e (tipo de cache/device)
//   [6..31] base      -> endereço físico >> 10  (get_base() = base << 10)
//
// fpage_t (V4, ARM 32-bit, L4_FPAGE_BASE_BITS = 22):
//   [0]     execute
//   [1]     write
//   [2]     read
//   [3]     meta      (atributos estendidos)
//   [4..9]  size      -> log2 do tamanho; tamanho em bytes = 1 << size
//   [10..31] base     -> vaddr >> 10  (get_base() = base << 10)
// ---------------------------------------------------------------------------
#ifndef ZEEBO_L4_MMU_H
#define ZEEBO_L4_MMU_H

#include <cstdint>
#include <cstdio>
#include <vector>

#ifdef ZEEBO_L4_MMU_WITH_UNICORN
#include <unicorn/unicorn.h>
#endif

namespace zeebo_l4 {

using u32 = uint32_t;
using u64 = uint64_t;

// --- Constantes da ABI ------------------------------------------------------
// Offset em BYTES do array de Message Registers dentro do UTCB do thread.
// (vregs.h: mr[] começa no byte 64; TCR MR offset = 16 words.)
static constexpr u32 UTCB_MR_BYTE_OFFSET = 64;
static constexpr u32 IPC_NUM_MR          = 32;

// L4_FPAGE_BASE_BITS para ARM 32-bit (config.h).
static constexpr u32 FPAGE_BASE_BITS = 22;

// l4attrib_e (types.h)
enum l4attrib_e : u32 {
    l4mem_default      = 0, // default/otimizado (código+dados)
    l4mem_cached       = 1,
    l4mem_uncached     = 2,
    l4mem_writeback    = 3,
    l4mem_writethrough = 4,
    l4mem_coherent     = 5,
    l4mem_io           = 6, // device memory
    l4mem_io_combined  = 7, // device write-combined
};

// --- map_control_t ----------------------------------------------------------
struct MapControl {
    u32 raw;
    explicit MapControl(u32 v = 0) : raw(v) {}

    bool is_modify()  const { return (raw >> 31) & 1u; }
    bool is_query()   const { return (raw >> 30) & 1u; }
    bool is_window()  const { return (raw >> 29) & 1u; } // só ARM v5
    // highest_item() = bits [0..5]. Número de descritores = highest_item()+1.
    u32  highest_item() const { return raw & 0x3fu; }
    u32  count()        const { return highest_item() + 1u; }
};

// --- phys_desc_t ------------------------------------------------------------
struct PhysDesc {
    u32 raw;
    explicit PhysDesc(u32 v = 0) : raw(v) {}

    l4attrib_e attributes() const { return (l4attrib_e)(raw & 0x3fu); }
    // base field = bits [6..31]; endereço físico = base << 10.
    u64 phys_base() const { return ((u64)(raw >> 6)) << 10; }
    bool is_device() const {
        l4attrib_e a = attributes();
        return a == l4mem_io || a == l4mem_io_combined;
    }
};

// --- fpage_t ----------------------------------------------------------------
struct Fpage {
    u32 raw;
    explicit Fpage(u32 v = 0) : raw(v) {}

    bool is_execute() const { return (raw >> 0) & 1u; }
    bool is_write()   const { return (raw >> 1) & 1u; }
    bool is_read()    const { return (raw >> 2) & 1u; }
    bool is_meta()    const { return (raw >> 3) & 1u; }
    u32  size_log2()  const { return (raw >> 4) & 0x3fu; }

    // rwx compacto (bits 0..2): igual a fpage_t::get_rwx() = raw & 7.
    u32  rwx() const { return raw & 7u; }

    // base = bits [10..31]; endereço virtual (get_base) = base << 10.
    // Aqui base já ocupa os bits altos, então get_base = raw & ~0x3ff.
    u64  vaddr() const { return (u64)(raw & ~0x3ffu); }

    // tamanho em bytes = 1 << size_log2 (fpage_t::get_size()).
    u64  size_bytes() const {
        u32 s = size_log2();
        return (s >= 32) ? (u64)1u << 32 : ((u64)1u << s);
    }

    bool is_nil() const { return raw == 0; }
    // fpage completa: size == 1 && base == 0 (fpage_t::is_complete_fpage()).
    bool is_complete() const { return size_log2() == 1 && (raw & ~0x3ffu) == 0; }
};

// --- Par de descritores decodificado ---------------------------------------
struct MapItem {
    PhysDesc phys;
    Fpage    fpage;
};

// Decodifica um par (phys_desc, fpage) a partir de dois valores de MR crus.
inline MapItem decode_item(u32 mr_phys, u32 mr_fpage) {
    return MapItem{ PhysDesc(mr_phys), Fpage(mr_fpage) };
}

// Converte permissões rwx da fpage nos flags de proteção do Unicorn.
// (Bit layout de UC_PROT: READ=1, WRITE=2, EXEC=4 — igual à ordem rwx aqui,
//  mas montamos explicitamente para não depender dessa coincidência.)
inline int fpage_to_uc_prot(const Fpage& f) {
    int prot = 0; // UC_PROT_NONE
    if (f.is_read())    prot |= 1; // UC_PROT_READ
    if (f.is_write())   prot |= 2; // UC_PROT_WRITE
    if (f.is_execute()) prot |= 4; // UC_PROT_EXEC
    return prot;
}

#ifdef ZEEBO_L4_MMU_WITH_UNICORN

// Lê MR[index] cru do UTCB do thread corrente no espaço do Unicorn.
inline u32 read_mr(uc_engine* uc, u32 utcb_base, u32 index) {
    u32 v = 0;
    uc_mem_read(uc, utcb_base + UTCB_MR_BYTE_OFFSET + index * 4u, &v, 4);
    return v;
}

// Mapeia (ou re-protege) uma fpage no Unicorn. Alinha para 4KB, que é a
// granularidade mínima do uc_mem_map. Idempotente: se a região já existe,
// apenas ajusta a proteção via uc_mem_protect.
inline uc_err map_one(uc_engine* uc, const MapItem& it) {
    const u64 PAGE = 0x1000;
    u64 va   = it.fpage.vaddr();
    u64 size = it.fpage.size_bytes();
    if (it.fpage.is_nil() || size == 0) return UC_ERR_OK;

    u64 base = va & ~(PAGE - 1);
    u64 end  = (va + size + PAGE - 1) & ~(PAGE - 1);
    u64 msize = end - base;
    if (msize < PAGE) msize = PAGE;

    int prot = fpage_to_uc_prot(it.fpage);
    if (prot == 0) prot = 1; // pelo menos legível para não criar região inútil

    uc_err e = uc_mem_map(uc, base, (size_t)msize, prot);
    if (e == UC_ERR_MAP) {
        // Já mapeado (overlap): apenas reajusta a proteção.
        e = uc_mem_protect(uc, base, (size_t)msize, prot);
    }

    // --- Sonda de páginas vazias (Item 2) ---------------------------------
    // Detecta o modo de falha clássico do MAP_CONTROL: a página é mapeada com
    // sucesso, porém seu conteúdo é virgem (tudo 0x00 = RAM não populada, ou
    // tudo 0xFF = NAND não escrita). Nesse caso o mapeamento é "correto porém
    // inútil" — falta o loader/relocador copiar a imagem da task ANTES do salto.
    // A sonda é rápida (lê só 32 bytes) e NÃO altera memória nem registradores.
    if (e == UC_ERR_OK && it.fpage.is_execute()) {
        u8 probe[32];
        if (uc_mem_read(uc, va, probe, sizeof probe) == UC_ERR_OK) {
            bool all_zero = true, all_ff = true;
            for (u8 b : probe) {
                if (b != 0x00) all_zero = false;
                if (b != 0xFF) all_ff = false;
            }
            if (all_zero || all_ff) {
                printf("[MMU/WARN] map_one: page @ 0x%08llx is EMPTY/UNINITIALIZED "
                       "(all 0x%02x, size=%llu, x-perm) -> loader nao populou a task; "
                       "derail (NOP-slide) provavel aqui\n",
                       (unsigned long long)va, all_zero ? 0x00 : 0xFF,
                       (unsigned long long)size);
            }
        }
    }
    return e;
}

// Dispatcher principal. Lê os descritores dos MRs do UTCB e executa os
// mapeamentos reais no Unicorn. Retorna o valor de resultado da syscall
// (fpage de resultado do primeiro item na convenção OKL4; aqui 1 = ok).
//
//   uc        : engine do core (Core 0 / ARM11 no Zeebo)
//   utcb_base : endereço base do UTCB do thread corrente
//   space_id  : r0 da syscall (L4_SpaceId_t) — informativo neste shim
//   control   : r1 da syscall (map_control_t cru)
//   out_items : opcional, recebe os itens decodificados (para logging/teste)
inline u32 handle_map_control(uc_engine* uc, u32 utcb_base, u32 space_id,
                              u32 control,
                              std::vector<MapItem>* out_items = nullptr) {
    MapControl ctrl(control);
    u32 n = ctrl.count();
    if (n > IPC_NUM_MR / 2) n = IPC_NUM_MR / 2;

    printf("[L4_MapControl] space=0x%x control=0x%08x %s%scount=%u\n",
           space_id, control,
           ctrl.is_modify() ? "modify " : "",
           ctrl.is_query()  ? "query "  : "", n);

    u32 mapped = 0;
    for (u32 i = 0; i < n; i++) {
        u32 mr_phys  = read_mr(uc, utcb_base, i * 2u);
        u32 mr_fpage = read_mr(uc, utcb_base, i * 2u + 1u);
        MapItem it = decode_item(mr_phys, mr_fpage);
        if (out_items) out_items->push_back(it);

        if (ctrl.is_query()) {
            // Query: apenas reporta, sem alterar o mapeamento.
            printf("  [query %u] va=0x%08llx phys=0x%08llx size=%llu rwx=%u attr=%u\n",
                   i, (unsigned long long)it.fpage.vaddr(),
                   (unsigned long long)it.phys.phys_base(),
                   (unsigned long long)it.fpage.size_bytes(),
                   it.fpage.rwx(), (unsigned)it.phys.attributes());
            continue;
        }

        if (it.fpage.is_nil()) {
            // fpage nil em modify = unmap; aqui apenas ignoramos (shim neutro).
            continue;
        }

        uc_err e = map_one(uc, it);
        printf("  [map %u] va=0x%08llx <- phys=0x%08llx size=%llu rwx=%u attr=%u -> %s\n",
               i, (unsigned long long)it.fpage.vaddr(),
               (unsigned long long)it.phys.phys_base(),
               (unsigned long long)it.fpage.size_bytes(),
               it.fpage.rwx(), (unsigned)it.phys.attributes(),
               uc_strerror(e));
        if (e == UC_ERR_OK) mapped++;
    }

    (void)mapped;
    return 1; // resultado não-nulo: MapControl bem-sucedido
}

#endif // ZEEBO_L4_MMU_WITH_UNICORN

} // namespace zeebo_l4

#endif // ZEEBO_L4_MMU_H
