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
#include <cstring>
#include <map>
#include <vector>

#ifdef ZEEBO_L4_MMU_WITH_UNICORN
#include <unicorn/unicorn.h>
#endif

namespace zeebo_l4 {

using u8  = uint8_t;
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
    // base field = bits [6..31]; endereço físico = base << 6 (granularidade 64B
    // do guest do Zeebo). Diverge do OKL4 fix7 (pistachio/include/map.h, <<10 /
    // gran 1KB): os MRs crus observados no bi_execute (ZEEBO_MC_DEBUG=1) —
    // 0x10081000 -> 0x10081000 (RAM real no pool 0x10000000-0x16000000) e
    // 0x10000000 -> 0x10000000 (RAM base) — só fecham com <<6. Com <<10 dariam
    // 0x100810000/0x100000000 (>4GB), classificando maps de RAM reais como
    // controle de AS (no-op). O hardware real funciona, logo o kernel usa <<6.
    u64 phys_base() const { return ((u64)(raw >> 6)) << 6; }
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

    // fpage que cobre TODO o espaço de endereço (size_log2 >= 32 => 2^32 bytes).
    // Em L4e/OKL4 2.1.1, map_control com uma fpage deste tamanho não é um mapeamento
    // literal de RAM, mas uma OPERAÇÃO DE CONTROLE DE ESPAÇO inteiro (flush/unmap
    // global ou concessão/revogação de permissão sobre todo o AS). Passar size=2^32
    // para uc_mem_map estoura o range de 32 bits e retorna UC_ERR_NOMEM.
    bool is_whole_space() const { return size_log2() >= 32; }
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

// ===========================================================================
// VTLB LUT (Host-side Virtual Translation Lookaside Buffer)
// ---------------------------------------------------------------------------
// Inspiração: VTLB do PCSX2 (LUT direta indexada por página, resolvida no host)
// e Fastmem / Page Aliasing do Dolphin (mmap/uc_mem_map_ptr apontando várias
// VAs para a MESMA memória de host).
//
// Ideia: manter um array indexado por (va >> 12) que guarda o ponteiro de host
// correspondente ao início de cada página de 4KB do guest. Assim telemetria,
// hooks e o BrewLoader traduzem VA->host_ptr em O(1) e leem/escrevem
// diretamente na RAM do host, sem a sobrecarga de uc_mem_read/uc_mem_write
// (que copiam + fazem lookup interno na TB do Unicorn a cada acesso).
//
// O espaço de endereços do Zeebo (ARM 32-bit) tem 2^32 bytes = 2^20 páginas de
// 4KB. Um array denso de 2^20 ponteiros custa 8MB (64-bit host) — aceitável e
// idêntico à abordagem do PCSX2. Índices sem mapeamento ficam nullptr.
// ===========================================================================
class VtlbLut {
public:
    static constexpr u32 PAGE_BITS  = 12;
    static constexpr u64 PAGE_SIZE  = 1u << PAGE_BITS;          // 4096
    static constexpr u64 PAGE_MASK  = PAGE_SIZE - 1;            // 0xfff
    static constexpr u64 NUM_PAGES  = (u64)1 << (32 - PAGE_BITS); // 2^20

    VtlbLut() : lut_(NUM_PAGES, nullptr) {}

    // Registra o mapeamento de [va, va+size) para o host_ptr correspondente.
    // va e size DEVEM ser alinhados a 4KB. host_ptr aponta para o início da
    // região de host que espelha essa faixa de VA (aliasing físico permitido:
    // vários VAs podem apontar para a mesma pool física do host).
    void map(u64 va, u64 size, u8* host_ptr) {
        u64 first = (va & 0xffffffffu) >> PAGE_BITS;
        u64 pages = (size + PAGE_MASK) >> PAGE_BITS;
        for (u64 i = 0; i < pages && (first + i) < NUM_PAGES; i++)
            lut_[first + i] = host_ptr + (i << PAGE_BITS);
    }

    // Remove o mapeamento de [va, va+size).
    void unmap(u64 va, u64 size) {
        u64 first = (va & 0xffffffffu) >> PAGE_BITS;
        u64 pages = (size + PAGE_MASK) >> PAGE_BITS;
        for (u64 i = 0; i < pages && (first + i) < NUM_PAGES; i++)
            lut_[first + i] = nullptr;
    }

    // Traduz VA -> host_ptr em O(1). Retorna nullptr se a página não está
    // mapeada. O offset dentro da página é preservado.
    u8* translate(u64 va) const {
        u8* base = lut_[(va & 0xffffffffu) >> PAGE_BITS];
        return base ? base + (va & PAGE_MASK) : nullptr;
    }

    bool is_mapped(u64 va) const {
        return lut_[(va & 0xffffffffu) >> PAGE_BITS] != nullptr;
    }

    // Leitura/escrita direta na RAM do host via LUT, respeitando limites de
    // página (um acesso que cruza fronteira de página é feito byte-a-byte,
    // pois páginas contíguas em VA podem não ser contíguas no host).
    bool read(u64 va, void* dst, u64 n) const {
        u8* d = (u8*)dst;
        for (u64 i = 0; i < n; i++) {
            u8* p = translate(va + i);
            if (!p) return false;
            d[i] = *p;
        }
        return true;
    }
    bool write(u64 va, const void* src, u64 n) {
        const u8* s = (const u8*)src;
        for (u64 i = 0; i < n; i++) {
            u8* p = translate(va + i);
            if (!p) return false;
            *p = s[i];
        }
        return true;
    }

    // Conveniências de 32 bits (little-endian, como ARM do Zeebo em uso normal).
    bool read_u32(u64 va, u32* out) const  { return read(va, out, 4); }
    bool write_u32(u64 va, u32 v)          { return write(va, &v, 4); }

    // Exposição da tabela crua para integrações fastmem/JIT (como dynarmic)
    u8** raw_lut_data() { return lut_.data(); }
    const u8* const* raw_lut_data() const { return lut_.data(); }

private:
    std::vector<u8*> lut_;
};

// ---------------------------------------------------------------------------
// PhysPool — pool física contígua de host (estilo APPS_RAM), fonte para o
// aliasing via uc_mem_map_ptr. Cada map_control resolve o phys_base dentro
// desta pool e mapeia o MESMO host_ptr no VA pedido, permitindo que dois VAs
// distintos (ex.: identidade + espaço de usuário) compartilhem RAM física.
// ---------------------------------------------------------------------------
struct PhysPool {
    u64 phys_base = 0;   // endereço físico do início da pool no mapa do guest
    u64 size      = 0;   // tamanho em bytes
    u8* host      = nullptr; // memória de host (dona; alinhada a 4KB)

    bool contains(u64 phys, u64 len) const {
        return host && phys >= phys_base && (phys + len) <= (phys_base + size);
    }
    // Retorna o host_ptr para um endereço físico dentro da pool, ou nullptr.
    u8* host_of(u64 phys) const {
        if (!host || phys < phys_base || phys >= phys_base + size) return nullptr;
        return host + (phys - phys_base);
    }
};

#ifdef ZEEBO_L4_MMU_WITH_UNICORN

// Lê MR[index] cru do UTCB do thread corrente no espaço do Unicorn.
inline u32 read_mr(uc_engine* uc, u32 utcb_base, u32 index) {
    u32 v = 0;
    uc_mem_read(uc, utcb_base + UTCB_MR_BYTE_OFFSET + index * 4u, &v, 4);
    return v;
}

// Grava MR[index] cru no UTCB do thread corrente no espaço do Unicorn.
// Contrapartida de read_mr(): os MRs de RETORNO da syscall vivem nos mesmos
// offsets (utcb_base + UTCB_MR_BYTE_OFFSET + index*4). O guest do Iguana
// (mempool_init @0xb000d864..) relê MR[0] (offset 0x40) e MR[1] (offset 0x44)
// logo após o retorno de L4_MapControl para extrair o size_log2 do fpage
// processado e avançar o ponteiro do pool; sem escrever de volta esses MRs,
// r4<<0 não avança e o laço trava.
inline void write_mr(uc_engine* uc, u32 utcb_base, u32 index, u32 val) {
    uc_mem_write(uc, utcb_base + UTCB_MR_BYTE_OFFSET + index * 4u, &val, 4);
}

// Mapeia (ou re-protege) uma fpage no Unicorn. Alinha para 4KB, que é a
// granularidade mínima do uc_mem_map. Idempotente: se a região já existe,
// apenas ajusta a proteção via uc_mem_protect.
inline uc_err map_one(uc_engine* uc, const MapItem& it) {
    const u64 PAGE = 0x1000;
    u64 va   = it.fpage.vaddr();
    u64 size = it.fpage.size_bytes();
    if (it.fpage.is_nil() || size == 0) return UC_ERR_OK;
    // fpage de espaço inteiro (2^32): operação de controle, não mapeamento de RAM.
    // Não repassar ao uc_mem_map (estouraria 32 bits -> UC_ERR_NOMEM).
    // (A justificativa completa do porquê deste critério — e da remoção do antigo
    // guard `phys_base() >= 4GB` — está no dispatcher handle_map_control, QW24.)
    if (it.fpage.is_whole_space()) return UC_ERR_OK;

    u64 base = va & ~(PAGE - 1);
    u64 end  = (va + size + PAGE - 1) & ~(PAGE - 1);
    // Clamp ao espaço endereçável de 32 bits (ARM do Zeebo é 32-bit). size_log2
    // em [24,31] sobre base alta (ex.: 2GiB @ 0xC0000000) faz `end` ultrapassar
    // 0x100000000; repassar esse range ao uc_mem_map cruza a fronteira de 4GB e
    // retorna UC_ERR_NOMEM/ARG (o guard whole-space, s>=32, NÃO cobre esse caso).
    // Trava o mapeamento no topo do espaço: mapeia o que cabe no guest.
    if (base >= 0x100000000ull) return UC_ERR_OK;      // base fora do espaço: controle
    if (end > 0x100000000ull) end = 0x100000000ull;    // clamp ao fim do espaço
    u64 msize = end - base;
    if (msize < PAGE) msize = PAGE;

    int prot = fpage_to_uc_prot(it.fpage);
    // Bug 7: rwx=0 é REVOGAÇÃO explícita de acesso. NÃO forçar prot=1 (o antigo
    // `if(prot==0)prot=1` transformava uma página revogada em legível, anulando
    // a revogação). Uma fpage rwx=0 sobre VA já mapeada deve deixar a página em
    // UC_PROT_NONE (acesso proíbe leitura E escrita). Se a VA ainda não existe,
    // uc_mem_map com prot=0 é rejeitado por algumas versões do Unicorn; então
    // mapeamos legível e imediatamente revogamos via uc_mem_protect(NONE).
    const bool revoke = (prot == 0);

    // UC_ERR_MAP (sobreposição com região já existente, ex.: scratch/IO) apenas
    // reajusta a proteção com uc_mem_protect.
    // Se falhar com UC_ERR_NOMEM (ex.: fpage de 4MB tentando mapear sobre range que cruza
    // ou colide de forma incompatível com região prévia de DMA), tenta split ou protect fallback.
    uc_err e = uc_mem_map(uc, base, (size_t)msize, revoke ? UC_PROT_READ : prot);
    if (e == UC_ERR_MAP || e == UC_ERR_NOMEM) {
        // Já mapeado ou conflito de chunk: reajusta proteção para o valor pedido
        // (incluindo UC_PROT_NONE quando revoke — revogação real).
        uc_err ep = uc_mem_protect(uc, base, (size_t)msize,
                                   revoke ? UC_PROT_NONE : prot);
        if (ep == UC_ERR_OK) {
            e = UC_ERR_OK;
        }
    } else if (e == UC_ERR_OK && revoke) {
        // Mapeamento novo criado legível só para satisfazer o Unicorn; agora
        // aplica a revogação real (UC_PROT_NONE).
        uc_mem_protect(uc, base, (size_t)msize, UC_PROT_NONE);
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

// --- Aliasing via uc_mem_map_ptr (estilo Dolphin fastmem / PCSX2 VTLB) -----
// Mapeia a fpage apontando para a POOL FÍSICA de host, em vez de RAM anônima
// duplicada. Se o phys_base cai dentro da pool, usa uc_mem_map_ptr(host_ptr)
// e registra a tradução VA->host_ptr no VTLB LUT. Assim:
//   - dois VAs sobre o mesmo phys compartilham a mesma RAM de host (aliasing);
//   - telemetria/BrewLoader leem via LUT em O(1) sem uc_mem_read.
// Retorna UC_ERR_OK em sucesso. Fallback: se não há pool cobrindo o phys,
// devolve UC_ERR_ARG para o chamador cair no map_one() anônimo.
inline uc_err map_one_aliased(uc_engine* uc, const MapItem& it,
                              const PhysPool& pool, VtlbLut* lut) {
    const u64 PAGE = 0x1000;
    u64 va   = it.fpage.vaddr();
    u64 size = it.fpage.size_bytes();
    if (it.fpage.is_nil() || size == 0) return UC_ERR_OK;
    // Espaço inteiro (2^32): controle de AS, não aliasing de RAM física.
    if (it.fpage.is_whole_space()) return UC_ERR_OK;

    u64 base  = va & ~(PAGE - 1);
    u64 end   = (va + size + PAGE - 1) & ~(PAGE - 1);
    // Clamp ao espaço de 32 bits (mesmo motivo de map_one): fpage de size_log2
    // em [24,31] sobre base alta cruzaria 0x100000000 e uc_mem_map_ptr retornaria
    // UC_ERR_NOMEM/ARG (cobriria um range fora do guest, onde a pool não alcança).
    if (base >= 0x100000000ull) return UC_ERR_OK;
    if (end > 0x100000000ull) end = 0x100000000ull;
    u64 msize = end - base;
    if (msize < PAGE) msize = PAGE;

    u64 phys  = it.phys.phys_base();
    u8* hp    = pool.host_of(phys);
    if (!hp || !pool.contains(phys, msize)) return UC_ERR_ARG; // sem pool: fallback

    int prot = fpage_to_uc_prot(it.fpage);
    // Bug 7: rwx=0 é revogação; não forçar legível.
    const bool revoke = (prot == 0);

    uc_err e = uc_mem_map_ptr(uc, base, (size_t)msize,
                              revoke ? UC_PROT_READ : prot, hp);
    bool host_ptr_aceito = (e == UC_ERR_OK);
    if (e == UC_ERR_MAP) {
        // Já existe região nesse VA: só reajusta a proteção (host_ptr imutável).
        e = uc_mem_protect(uc, base, (size_t)msize,
                           revoke ? UC_PROT_NONE : prot);
    } else if (e == UC_ERR_OK && revoke) {
        // Revogação real da região recém-mapeada.
        uc_mem_protect(uc, base, (size_t)msize, UC_PROT_NONE);
    }
    // So atualiza a LUT quando o Unicorn REALMENTE adotou este host_ptr.
    //
    // Se veio UC_ERR_MAP, o Unicorn manteve o buffer anterior e apenas a
    // protecao foi reajustada. Atualizar a LUT com `hp` faria a VTLB servir um
    // buffer e o Unicorn servir outro para o MESMO endereco virtual: o backend
    // interpretado le pelo Unicorn e o recompilado le pela VTLB, entao os dois
    // divergem em silencio, sem erro nem aviso.
    //
    // Foi a causa raiz da divergencia #181306 do boot, capturada como
    // "READ16 addr=0xb0d00002 valor=0xea00 origem=VTLB uc_diz=0x0001".
    // Ver test_vtlb_remap_asymmetry.
    if (e == UC_ERR_OK && host_ptr_aceito && lut) lut->map(base, msize, hp);
    return e;
}

// --- Revogação de espaço inteiro (Bug 7, whole-space rwx=0) -----------------
// Uma fpage de 2^32 com rwx=0 é uma OPERAÇÃO DE CONTROLE que revoga o acesso
// sobre TODO o address space (unmap/flush de permissões). Implementamos a
// semântica real: enumeramos as regiões atualmente mapeadas no Unicorn e as
// colocamos em UC_PROT_NONE. Sem isso, o whole-space op era um no-op silencioso
// e a revogação global não tinha efeito observável algum.
inline uc_err revoke_whole_space(uc_engine* uc, const Fpage& f) {
    if (!f.is_whole_space()) return UC_ERR_ARG;
    if (f.rwx() != 0) return UC_ERR_OK; // só rwx=0 revoga; rwx>0 é concessão/no-op aqui
    uc_mem_region* regions = nullptr;
    uint32_t count = 0;
    uc_err e = uc_mem_regions(uc, &regions, &count);
    if (e != UC_ERR_OK) return e;
    for (uint32_t i = 0; i < count; i++) {
        // Só toca o espaço de 32 bits do guest.
        if (regions[i].begin > 0xffffffffull) continue;
        uint64_t b = regions[i].begin;
        uint64_t sz = regions[i].end - regions[i].begin + 1;
        uc_mem_protect(uc, b, (size_t)sz, UC_PROT_NONE);
    }
    uc_free(regions);
    return UC_ERR_OK;
}

// --- SpaceMap (Bug 1): VTLB por space_id -----------------------------------
// Cada L4_SpaceId_t possui seu PRÓPRIO conjunto de traduções VA->host. A mesma
// VA em espaços diferentes pode apontar para RAM física diferente, e uma
// revogação/unmap num espaço NÃO deve afetar o outro. Antes havia uma única
// VtlbLut global compartilhada por todos os espaços, o que fundia mapeamentos
// de tasks distintas no mesmo endereço virtual.
class SpaceMap {
public:
    // Retorna (criando se necessário) a LUT do espaço `space_id`.
    VtlbLut& lut_for(u32 space_id) {
        return spaces_[space_id];
    }
    const VtlbLut* lut_if_exists(u32 space_id) const {
        auto it = spaces_.find(space_id);
        return it != spaces_.end() ? &it->second : nullptr;
    }
    size_t space_count() const { return spaces_.size(); }
    void clear() { spaces_.clear(); }

private:
    std::map<u32, VtlbLut> spaces_;
};

// --- SpaceManager (Bug 1, runtime): TRUE address-space switching -----------
// SpaceMap acima só REGISTRA a tradução VA->host por SID (para consultas). Ele
// NÃO troca o que o Unicorn realmente executa: todo o boot corria sobre um
// único mapeamento plano, então a MESMA VA nunca era de fato comutada entre
// espaços — dois SIDs enxergavam os mesmos bytes.
//
// SpaceManager fecha essa lacuna. Para cada L4_SpaceId_t ele guarda a LISTA de
// regiões (VA, tamanho, prot, host_ptr de backing físico) que aquele espaço
// mapeou. activate(uc, sid) comuta o address space REAL do Unicorn:
//   1) desmapeia (uc_mem_unmap) todas as regiões do espaço atualmente ativo
//      que NÃO pertencem ao alvo (ou cujo backing difere);
//   2) mapeia (uc_mem_map_ptr) as regiões do espaço alvo apontando para o
//      backing físico daquele espaço.
// Assim a mesma VA passa a ler/executar o backing do SID ativo, e uma escrita
// só toca o backing daquele espaço (isolamento real).
//
// SEGURANÇA (Unicorn): uc_mem_unmap/uc_mem_map_ptr NUNCA podem ser chamados de
// dentro de um code/mem hook (remap durante tradução de TB corrompe o cache de
// tradução e trava/segfaulta). activate() é projetado para rodar ENTRE fatias
// de uc_emu_start (no laço de escalonamento), jamais dentro de um hook.
class SpaceManager {
public:
    struct Region {
        u64 va = 0;
        u64 size = 0;
        int prot = 0;      // UC_PROT_*
        u8* host = nullptr; // backing físico de host (dono externo)
    };

    // Registra/atualiza uma região do espaço `sid`. Alinha a 4KB. Se já existe
    // região com mesma VA nesse espaço, substitui (permite re-protect/rebacking).
    void record(u32 sid, u64 va, u64 size, int prot, u8* host) {
        const u64 PAGE = 0x1000;
        u64 base = va & ~(PAGE - 1);
        u64 end  = (va + size + PAGE - 1) & ~(PAGE - 1);
        if (base >= 0x100000000ull) return;
        if (end > 0x100000000ull) end = 0x100000000ull;
        u64 msize = end - base;
        if (msize < PAGE) msize = PAGE;
        auto& regs = spaces_[sid];
        for (auto& r : regs) {
            if (r.va == base) { r.size = msize; r.prot = prot; r.host = host; return; }
        }
        regs.push_back(Region{ base, msize, prot, host });
    }

    bool has_space(u32 sid) const { return spaces_.find(sid) != spaces_.end(); }
    u32  active_sid() const { return active_sid_; }
    size_t space_count() const { return spaces_.size(); }

    const std::vector<Region>* regions_of(u32 sid) const {
        auto it = spaces_.find(sid);
        return it != spaces_.end() ? &it->second : nullptr;
    }

    // OKL4 base<-extension PD sharing. An extension space (created by
    // ThreadControl with Pager == the base space) shares the base PD's
    // mappings via the map window / shared domain. Record that link so lookups
    // in `ext` fall back to `base`. General: no SID/page is hardcoded; the
    // caller passes whatever base the kernel named as the extension's pager.
    // A space is never its own base (ignored) and a nil base clears the link.
    void link_base(u32 ext, u32 base) {
        if (ext == 0 || ext == base) return;
        if (base == 0) { base_of_.erase(ext); return; }
        base_of_[ext] = base;
    }

    // The base space linked to `sid`, or 0 if none.
    u32 base_of(u32 sid) const {
        auto it = base_of_.find(sid);
        return it != base_of_.end() ? it->second : 0;
    }

    // Resolve the host backing that `sid` sees at `va`, honouring base->ext
    // sharing. The extension's OWN region shadows the base's at the same VA
    // (extension wins); otherwise the base chain is consulted. Returns the
    // Region's host pointer (may be nullptr for a flat/unbacked region) or
    // nullptr if no space in the chain maps `va`. A short base chain is walked
    // with cycle protection.
    u8* resolve_host(u32 sid, u64 va) const {
        const u64 PAGE = 0x1000;
        u64 base_va = va & ~(PAGE - 1);
        u32 cur = sid;
        for (int hops = 0; hops < 8 && cur != 0; ++hops) {
            auto it = spaces_.find(cur);
            if (it != spaces_.end()) {
                for (const auto& r : it->second) {
                    if (base_va >= r.va && base_va < r.va + r.size) return r.host;
                }
            }
            u32 nxt = base_of(cur);
            if (nxt == cur) break;
            cur = nxt;
        }
        return nullptr;
    }

    // True iff `sid` (or any base it shares from) maps `va`.
    bool maps(u32 sid, u64 va) const {
        const u64 PAGE = 0x1000;
        u64 base_va = va & ~(PAGE - 1);
        u32 cur = sid;
        for (int hops = 0; hops < 8 && cur != 0; ++hops) {
            auto it = spaces_.find(cur);
            if (it != spaces_.end())
                for (const auto& r : it->second)
                    if (base_va >= r.va && base_va < r.va + r.size) return true;
            u32 nxt = base_of(cur);
            if (nxt == cur) break;
            cur = nxt;
        }
        return false;
    }

    // Effective region set a space sees: its OWN regions plus regions shared
    // from its base PD chain (OKL4 base<-extension map window). The extension's
    // own region shadows the base's at the same VA (extension wins). Shared,
    // used by both the unmap (prior space) and map (target space) passes so no
    // stale base mapping is left behind and none is missing.
    std::vector<Region> effective_regions(u32 sid) const {
        auto it = spaces_.find(sid);
        std::vector<Region> eff = (it != spaces_.end()) ? it->second
                                                        : std::vector<Region>{};
        auto has_va = [&](u64 va){
            for (const auto& r : eff) if (r.va == va) return true;
            return false;
        };
        u32 cur = base_of(sid);
        for (int hops = 0; hops < 8 && cur != 0; ++hops) {
            auto bit = spaces_.find(cur);
            if (bit != spaces_.end())
                for (const auto& br : bit->second)
                    if (!has_va(br.va)) eff.push_back(br);
            u32 nxt = base_of(cur);
            if (nxt == cur) break;
            cur = nxt;
        }
        return eff;
    }

#ifdef ZEEBO_L4_MMU_WITH_UNICORN
    // Comuta o address space REAL do Unicorn para `sid`. Idempotente: reativar
    // o SID já ativo é no-op. DEVE ser chamado FORA de qualquer hook.
    uc_err activate(uc_engine* uc, u32 sid) {
        if (!uc) return UC_ERR_ARG;
        auto it = spaces_.find(sid);
        if (it == spaces_.end()) return UC_ERR_ARG;
        if (sid == active_sid_ && activated_once_) return UC_ERR_OK;

        std::vector<Region> eff = effective_regions(sid);

        // 1) Desmapear regiões do espaço ativo (own + base-chain) que não
        //    coincidem (VA+size+backing) com nenhuma região efetiva do alvo. Só
        //    toca regiões que ESTE manager criou. Ao remover uma região que
        //    havíamos religado por cima de um mapeamento estático/plano NÃO
        //    gerenciado, RESTAURA a presença desse mapeamento (mesma extensão e
        //    proteção) para nunca deixar um buraco permanente no address space.
        if (activated_once_) {
            std::vector<Region> prev_eff = effective_regions(active_sid_);
            for (const auto& pr : prev_eff) {
                bool keep = false;
                for (const auto& tr : eff) {
                    if (tr.va == pr.va && tr.size == pr.size && tr.host == pr.host) {
                        keep = true; break;
                    }
                }
                if (!keep) {
                    uc_mem_unmap(uc, pr.va, (size_t)pr.size);
                    mapped_host_.erase(pr.va);
                    restore_shadow(uc, pr.va);
                }
            }
        }

        // 2) Mapear (ou re-proteger) as regiões do alvo apontando para o backing
        //    físico daquele espaço.
        uc_err first_err = UC_ERR_OK;
        for (const auto& tr : eff) {
            int prot = tr.prot ? tr.prot : UC_PROT_READ;

            // Já temos ESTA VA mapeada com ESTE host (região preservada do
            // espaço anterior): só reafirma a proteção, sem unmap/rebind.
            auto mh = mapped_host_.find(tr.va);
            if (mh != mapped_host_.end() && mh->second == tr.host) {
                uc_err e = uc_mem_protect(uc, tr.va, (size_t)tr.size, prot);
                if (e != UC_ERR_OK && first_err == UC_ERR_OK) first_err = e;
                continue;
            }

            uc_err e = uc_mem_map_ptr(uc, tr.va, (size_t)tr.size, prot, tr.host);
            if (e == UC_ERR_MAP) {
                // VA já mapeada por algo que NÃO é nossa região idêntica. Se
                // temos um backing próprio (ex.: página compartilhada do espaço
                // base OKL4), a VA pode estar coberta por um mapeamento
                // estático/plano com o backing ERRADO; reprotect sozinho não
                // religa o ponteiro. Force o religamento: unmap + map_ptr.
                if (tr.host) {
                    // Se o mapeamento pré-existente NÃO é gerenciado por este
                    // SpaceManager (estático/plano criado externamente),
                    // registra a extensão/proteção dele para poder RECRIAR sua
                    // presença ao sairmos deste espaço — nunca o destruímos
                    // permanentemente.
                    if (mapped_host_.find(tr.va) == mapped_host_.end())
                        record_shadow(uc, tr.va, tr.size, prot);
                    uc_err ue = uc_mem_unmap(uc, tr.va, (size_t)tr.size);
                    if (ue == UC_ERR_OK)
                        e = uc_mem_map_ptr(uc, tr.va, (size_t)tr.size, prot, tr.host);
                    if (e != UC_ERR_OK)
                        e = uc_mem_protect(uc, tr.va, (size_t)tr.size, prot);
                } else {
                    e = uc_mem_protect(uc, tr.va, (size_t)tr.size, prot);
                }
            }
            if (e == UC_ERR_OK) mapped_host_[tr.va] = tr.host;
            if (e != UC_ERR_OK && first_err == UC_ERR_OK) first_err = e;
        }

        active_sid_ = sid;
        activated_once_ = true;
        // [QW-ALIAS] Rastreio read-only de remapeamento que toca a página de
        // 0xb04241a8 (VA 0xb0424000). Se activate() re-liga o backing dessa página
        // ENTRE o ldr e o bx, o host uc_mem_read posterior verá outro conteúdo que
        // o r1 arquitetural já capturado — provando remap-após-leitura (hipótese B).
        if (std::getenv("ZEEBO_PC14_ALIAS")) {
            for (const auto& tr : eff) {
                if (0xb0424000ull >= tr.va && 0xb0424000ull < tr.va + tr.size) {
                    fprintf(stderr, "[ALIAS/activate] sid=0x%x REMAPEOU pagina de 0xb0424000: "
                            "va=0x%llx size=0x%llx host=%p prot=%d\n",
                            sid, (unsigned long long)tr.va,
                            (unsigned long long)tr.size, (void*)tr.host, tr.prot);
                    fflush(stderr);
                }
            }
        }
        return first_err;
    }
#endif

    void clear() {
        spaces_.clear(); base_of_.clear();
#ifdef ZEEBO_L4_MMU_WITH_UNICORN
        mapped_host_.clear(); shadowed_.clear();
#endif
        active_sid_ = 0; activated_once_ = false;
    }

private:
#ifdef ZEEBO_L4_MMU_WITH_UNICORN
    // Descobre a proteção do mapeamento estático/plano que cobre `va` (se
    // houver) e registra a sub-faixa [va, va+size) que estamos prestes a
    // religar, para recriá-la depois. Não guarda conteúdo (o host pertence ao
    // Unicorn, não a nós); guarda apenas presença + proteção.
    void record_shadow(uc_engine* uc, u64 va, u64 size, int prot) {
        if (shadowed_.find(va) != shadowed_.end()) return; // preserva o 1º
        int found_prot = prot;
        uc_mem_region* regions = nullptr; uint32_t count = 0;
        if (uc_mem_regions(uc, &regions, &count) == UC_ERR_OK) {
            for (uint32_t i = 0; i < count; ++i) {
                if (va >= regions[i].begin && va <= regions[i].end) {
                    found_prot = (int)regions[i].perms; break;
                }
            }
            uc_free(regions);
        }
        shadowed_[va] = Region{ va, size, found_prot, nullptr };
    }

    // Recria a presença de um mapeamento estático/plano que havíamos religado,
    // ao sair do espaço que o religou. Mapeia RAM nova própria do Unicorn com a
    // mesma extensão/proteção: garante que a VA continue mapeada (sem buraco
    // permanente). O conteúdo original não é preservável (era backing do
    // Unicorn), mas a presença sim — que é o invariante de segurança.
    void restore_shadow(uc_engine* uc, u64 va) {
        auto sit = shadowed_.find(va);
        if (sit == shadowed_.end()) return;
        const Region& s = sit->second;
        int prot = s.prot ? s.prot : UC_PROT_READ;
        uc_mem_map(uc, s.va, (size_t)s.size, prot); // best-effort; ignora colisão
        shadowed_.erase(sit);
    }
#endif

    std::map<u32, std::vector<Region>> spaces_;
    std::map<u32, u32> base_of_;   // extension sid -> base sid (OKL4 PD sharing)
#ifdef ZEEBO_L4_MMU_WITH_UNICORN
    std::map<u64, u8*>    mapped_host_; // VA -> host atualmente mapeado por nós
    std::map<u64, Region> shadowed_;    // VA -> estático religado a restaurar
#endif
    u32  active_sid_ = 0;
    bool activated_once_ = false;
};

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
                              std::vector<MapItem>* out_items = nullptr,
                              const PhysPool* pool = nullptr,
                              VtlbLut* lut = nullptr) {
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

        // --- Registradores de RETORNO (MRs) ----------------------------------
        // Em L4e/OKL4 2.1.1, MapControl devolve nos MRs a descrição das fpages
        // efetivamente processadas: MR[i*2] = phys_desc resultante, MR[i*2+1] =
        // fpage resultante. O mempool_init do Iguana (@0xb000d864..0xb000d89c)
        // relê MR[1] (offset 0x44) para extrair size_log2 e MR[0] (offset 0x40)
        // para compor o endereço base. Ecoamos os descritores processados como
        // shim neutro: aqui o que foi pedido é o que foi mapeado, então o
        // resultado é byte-idêntico à entrada.
        //
        // ATENÇÃO — evidência não fechada: por ser um echo idêntico à entrada,
        // NÃO é possível provar por black-box (fora de um guest vivo) que este
        // write-back é load-bearing. Um guest real que escreve os MRs de entrada
        // e depois relê os de saída só distinguiria a ausência do write-back se
        // o kernel devolvesse descritores DIFERENTES dos de entrada — o que este
        // shim neutro não faz. Mantido por segurança para o caminho de guest
        // vivo (quando o UTCB de entrada puder diferir do de saída), mas o
        // "fechamento do Passo 13" permanece BLOQUEADO até esse harness existir.
        write_mr(uc, utcb_base, i * 2u,      mr_phys);
        write_mr(uc, utcb_base, i * 2u + 1u, mr_fpage);

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

        // fpage de espaço inteiro (size_log2 >= 32, size=2^32): em L4e/OKL4
        // 2.1.1 isto NÃO é mapeamento literal de RAM, e sim controle sobre todo o
        // address space (flush/unmap global ou concessão/revogação de permissão).
        // Não há RAM de host a mapear; no-op de sucesso evita o UC_ERR_NOMEM (2^32
        // estoura o range de 32 bits do uc_mem_map) que travava o Core 0.
        //
        // QW24: o antigo guard `phys_base() >= 0x100000000` foi REMOVIDO. Ele foi
        // construído sobre o decode errado (<<10 / gran 1KB), que inflava bases de
        // RAM reais (0x10081000 -> 0x100810000) acima de 4GB e as classificava como
        // controle — o no-op mascarava os maps físicos do bi_execute e deixava as
        // páginas sem permissão (WRITE_PROT no memset do BSS). Com o decode correto
        // (<<6 / gran 64B), phys_base() de um raw u32 é sempre < 4GB (máx
        // 0xFFFFFFC0): o guard seria código morto. O único critério de whole-space
        // é a fpage.
        if (it.fpage.is_whole_space()) {
            // Bug 7: whole-space com rwx=0 REVOGA o acesso a todo o AS (não é
            // no-op). rwx>0 permanece controle informativo (concessão global).
            if (it.fpage.rwx() == 0) {
                revoke_whole_space(uc, it.fpage);
                printf("  [ctl %u] whole-space REVOKE (rwx=0): todas as regioes -> PROT_NONE\n", i);
            } else {
                printf("  [ctl %u] whole-space op: va=0x%08llx phys=0x%llx size=%llu "
                       "rwx=%u -> address-space control (perm grant), no RAM map\n",
                       i, (unsigned long long)it.fpage.vaddr(),
                       (unsigned long long)it.phys.phys_base(),
                       (unsigned long long)it.fpage.size_bytes(), it.fpage.rwx());
            }
            mapped++;
            continue;
        }

        uc_err e;
        bool aliased = false;
        if (pool && pool->host && it.phys.phys_base() != 0) {
            e = map_one_aliased(uc, it, *pool, lut);
            if (e == UC_ERR_ARG) {
                // phys fora da pool: cai no mapeamento anônimo normal.
                e = map_one(uc, it);
            } else {
                aliased = true;
            }
        } else {
            e = map_one(uc, it);
        }
        printf("  [map %u] va=0x%08llx <- phys=0x%08llx size=%llu rwx=%u attr=%u %s-> %s\n",
               i, (unsigned long long)it.fpage.vaddr(),
               (unsigned long long)it.phys.phys_base(),
               (unsigned long long)it.fpage.size_bytes(),
               it.fpage.rwx(), (unsigned)it.phys.attributes(),
               aliased ? "[aliased] " : "",
               uc_strerror(e));
        if (e == UC_ERR_OK) mapped++;
    }

    (void)mapped;
    return 1; // resultado não-nulo: MapControl bem-sucedido
}

// Overload (Bug 1): roteia os mapeamentos para a VTLB do space_id via SpaceMap.
// Cada space_id recebe sua própria LUT, então a mesma VA em espaços distintos
// não colide. Reusa o dispatcher base passando a LUT do espaço-alvo.
inline u32 handle_map_control(uc_engine* uc, u32 utcb_base, u32 space_id,
                              u32 control,
                              std::vector<MapItem>* out_items,
                              const PhysPool* pool,
                              SpaceMap* spaces) {
    VtlbLut* lut = spaces ? &spaces->lut_for(space_id) : nullptr;
    return handle_map_control(uc, utcb_base, space_id, control, out_items, pool, lut);
}

#endif // ZEEBO_L4_MMU_WITH_UNICORN

} // namespace zeebo_l4

#endif // ZEEBO_L4_MMU_H
