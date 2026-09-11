// test_nand_relocator_bounds.cpp — DD (bug 10): prova que o relocador NAND
// recusa cabecalhos de programa incoerentes (phoff/phentsize/phnum) em vez de
// (a) alocar uma tabela absurda a partir de phent*phnum sem verificar overflow,
// (b) ler alem do buffer de pagina realmente carregado da NAND, ou
// (c) ler um cabecalho parcial quando phentsize < 32 (ELF32 phdr).
//
// A logica abaixo espelha, campo a campo, o laco de parse de
// zeebo_nand_relocator.cpp (que tem main() proprio e depende de Unicorn, por
// isso nao pode ser incluido aqui). Qualquer mudanca naquele laco deve ser
// refletida aqui.
//
// Build: g++ -std=c++23 -O1 -g -fsanitize=address,undefined -o
//        test_nand_relocator_bounds test_nand_relocator_bounds.cpp
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

using u8 = uint8_t;
using u16 = uint16_t;
using u32 = uint32_t;
using u64 = uint64_t;

// Tamanho de uma pagina NAND realmente lida via DMOV para o buffer DEST.
static constexpr u32 kPageBytes = 2048;
// Program header ELF32 tem 32 bytes; abaixo disso os campos nao cabem.
static constexpr u32 kPhdrMin = 32;

static u32 rd32(const u8* d, size_t o) {
    return (u32)d[o] | ((u32)d[o + 1] << 8) | ((u32)d[o + 2] << 16) | ((u32)d[o + 3] << 24);
}

struct RelocResult {
    bool refused = false;      // recusou a tabela inteira (cabecalho incoerente)
    int loads = 0;             // PT_LOAD contados
    int scanned = 0;           // cabecalhos efetivamente lidos
    size_t table_bytes = 0;    // bytes alocados para a tabela
};

// Espelho do parse de zeebo_nand_relocator.cpp, sem Unicorn.
// `page` e o buffer da pagina carregada; `cap` e quantos bytes foram de fato
// lidos da NAND (kPageBytes). Simula uc_mem_read: ler alem de `cap` seria UB no
// original (ler memoria Unicorn nao inicializada), entao a validacao deve
// garantir que a tabela inteira caiba em [phoff, phoff+table_bytes) <= cap.
static RelocResult parse_phdr_table(const std::vector<u8>& page, u32 cap,
                                    u32 phoff, u16 phent, u16 phnum) {
    RelocResult r;

    // (c) phentsize precisa comportar um phdr ELF32 completo.
    if (phent < kPhdrMin) { r.refused = true; return r; }

    // (a) multiplicacao verificada: phent*phnum em 64 bits, sem overflow u16/int.
    u64 table_bytes = (u64)phent * (u64)phnum;

    // (b) a tabela inteira precisa caber na pagina realmente lida.
    if (phoff > cap || table_bytes > (u64)cap - phoff) { r.refused = true; return r; }

    r.table_bytes = (size_t)table_bytes;
    std::vector<u8> phtab(r.table_bytes);
    // Copia so o que foi lido (simula uc_mem_read dentro dos limites validados).
    std::memcpy(phtab.data(), page.data() + phoff, r.table_bytes);

    for (int i = 0; i < phnum; i++) {
        size_t o = (size_t)i * phent;
        if (o + kPhdrMin > r.table_bytes) break;  // cabecalho parcial: para.
        r.scanned++;
        u32 p_type = rd32(phtab.data(), o);
        if (p_type == 1) r.loads++;
    }
    return r;
}

static void put_phdr(std::vector<u8>& d, size_t at, u32 type, u32 off, u32 vaddr,
                     u32 filesz, u32 memsz) {
    auto w = [&](size_t o, u32 v) { std::memcpy(d.data() + at + o, &v, 4); };
    w(0, type); w(4, off); w(8, vaddr); w(12, vaddr); w(16, filesz); w(20, memsz);
}

int main() {
    const u16 phent = 32;

    // 1. Tabela valida dentro da pagina: conta os PT_LOAD.
    {
        std::vector<u8> page(kPageBytes, 0);
        put_phdr(page, 64, 1, 0x100, 0x10000000, 0x40, 0x40);
        put_phdr(page, 96, 4, 0x140, 0, 0, 0);  // PT_NOTE
        auto r = parse_phdr_table(page, kPageBytes, 64, phent, 2);
        assert(!r.refused && r.scanned == 2 && r.loads == 1);
        printf("[ok] tabela valida conta %d PT_LOAD de %d cabecalhos\n", r.loads, r.scanned);
    }

    // 2. phent*phnum estoura u16/int se nao for verificado em 64 bits.
    {
        std::vector<u8> page(kPageBytes, 0);
        // 0x8000 * 0x8000 = 0x40000000 (nao cabe na pagina): deve recusar.
        auto r = parse_phdr_table(page, kPageBytes, 0, 0x8000, 0x8000);
        assert(r.refused && r.table_bytes == 0);
        printf("[ok] phent*phnum absurdo e recusado sem alocar\n");
    }

    // 3. phoff alem da pagina lida: recusa (nao le memoria nao inicializada).
    {
        std::vector<u8> page(kPageBytes, 0);
        auto r = parse_phdr_table(page, kPageBytes, kPageBytes + 100, phent, 1);
        assert(r.refused);
        printf("[ok] phoff fora da pagina e recusado\n");
    }

    // 4. Tabela que transborda o fim da pagina: recusa.
    {
        std::vector<u8> page(kPageBytes, 0);
        // phoff perto do fim, phnum grande o bastante para passar de kPageBytes.
        auto r = parse_phdr_table(page, kPageBytes, kPageBytes - 40, phent, 4);
        assert(r.refused);
        printf("[ok] tabela que transborda a pagina e recusada\n");
    }

    // 5. phentsize < 32: recusa (cabecalho parcial nao pode ser lido).
    {
        std::vector<u8> page(kPageBytes, 0);
        auto r = parse_phdr_table(page, kPageBytes, 64, /*phent*/8, 4);
        assert(r.refused);
        printf("[ok] phentsize < 32 e recusado\n");
    }

    // 6. phentsize maior que 32 (padding): so os PT_LOAD validos, sem OOB.
    {
        std::vector<u8> page(kPageBytes, 0);
        put_phdr(page, 0, 1, 0x100, 0x10000000, 0x40, 0x40);
        put_phdr(page, 48, 1, 0x200, 0x10001000, 0x40, 0x40);
        auto r = parse_phdr_table(page, kPageBytes, 0, /*phent*/48, 2);
        assert(!r.refused && r.scanned == 2 && r.loads == 2);
        printf("[ok] phentsize=48 com padding le %d cabecalhos sem OOB\n", r.scanned);
    }

    printf("[Test] limites do relocador NAND: OK\n");
    return 0;
}
