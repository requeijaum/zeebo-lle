// test_elf_bounds.cpp — DD: prova que o leitor de cabecalhos de programa recusa
// arquivos incoerentes em vez de ler fora do buffer.
//
// A logica abaixo espelha, byte a byte, o laco de zeebo_elf.cpp (que tem main()
// proprio e depende de unicorn, por isso nao pode ser incluido aqui). Qualquer
// mudanca naquele laco deve ser refletida aqui.
//
// Build: g++ -std=c++23 -O1 -g -fsanitize=address,undefined -o test_elf_bounds test_elf_bounds.cpp
#include <algorithm>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

using u8 = uint8_t;
using u32 = uint32_t;
using u16 = uint16_t;

static constexpr size_t kMaxSegBytes = 64u * 1024u * 1024u;

static u32 rd32(const u8* p, size_t o) { u32 v; std::memcpy(&v, p + o, 4); return v; }

struct LoadResult { int loaded = 0; int skipped = 0; size_t bytes_copied = 0; };

// Espelho do laco de PT_LOAD de zeebo_elf.cpp, sem Unicorn.
static LoadResult parse_phdrs(const std::vector<u8>& d, u32 phoff, u16 phent, u16 phnum) {
    LoadResult r;
    for (int i = 0; i < phnum; i++) {
        size_t o = phoff + (size_t)i * phent;
        // Bug 10: o cabecalho de 32 bytes precisa caber INTEIRO (rd32 le ate
        // o+20..o+23); a guarda antiga "o+4>size" deixava rd32 ler fora.
        if (o + 32 > d.size()) { r.skipped++; continue; }
        u32 ptype = rd32(d.data(), o);
        if (ptype != 1) continue;
        u32 val      = rd32(d.data(), o + 4);   // p_offset
        u32 p_filesz = rd32(d.data(), o + 16);
        u32 p_memsz  = rd32(d.data(), o + 20);
        size_t nmem = p_memsz ? p_memsz : p_filesz;
        if (!nmem) continue;
        if (nmem > kMaxSegBytes) { r.skipped++; continue; }
        std::vector<u8> seg(nmem, 0);
        if (p_filesz) {
            if ((size_t)val > d.size()) { r.skipped++; continue; }
            size_t avail = d.size() - (size_t)val;
            size_t cl = std::min({(size_t)p_filesz, seg.size(), avail});
            std::memcpy(seg.data(), d.data() + val, cl);
            r.bytes_copied += cl;
        }
        r.loaded++;
    }
    return r;
}

// Um cabecalho de programa PT_LOAD de 32 bytes.
static void put_phdr(std::vector<u8>& d, size_t at, u32 off, u32 vaddr, u32 filesz, u32 memsz) {
    auto w = [&](size_t o, u32 v) { std::memcpy(d.data() + at + o, &v, 4); };
    w(0, 1); w(4, off); w(8, vaddr); w(12, vaddr); w(16, filesz); w(20, memsz);
}

int main() {
    const u16 phent = 32;

    // 1. Arquivo valido: o segmento e carregado inteiro.
    {
        std::vector<u8> d(256, 0xAA);
        put_phdr(d, 64, /*off*/128, /*vaddr*/0x10000000, /*filesz*/64, /*memsz*/64);
        auto r = parse_phdrs(d, 64, phent, 1);
        assert(r.loaded == 1 && r.skipped == 0 && r.bytes_copied == 64);
        printf("[ok] arquivo valido carrega 64 bytes\n");
    }

    // 2. Tabela de cabecalhos apontando para fora do arquivo: recusa, nao le fora.
    {
        std::vector<u8> d(256, 0xAA);
        auto r = parse_phdrs(d, /*phoff*/0x00100000, phent, 4);
        assert(r.loaded == 0 && r.skipped == 4);
        printf("[ok] phoff fora do arquivo e recusado (%d cabecalhos)\n", r.skipped);
    }

    // 3. Deslocamento do segmento fora do arquivo: recusa.
    {
        std::vector<u8> d(256, 0xAA);
        put_phdr(d, 64, /*off*/0x40000000, 0x10000000, 64, 64);
        auto r = parse_phdrs(d, 64, phent, 1);
        assert(r.loaded == 0 && r.skipped == 1 && r.bytes_copied == 0);
        printf("[ok] p_offset fora do arquivo e recusado\n");
    }

    // 4. Segmento truncado: copia so o que existe, sem passar do fim.
    {
        std::vector<u8> d(256, 0xAA);
        put_phdr(d, 64, /*off*/200, 0x10000000, /*filesz*/1000, /*memsz*/1000);
        auto r = parse_phdrs(d, 64, phent, 1);
        assert(r.loaded == 1 && r.bytes_copied == 56);  // 256 - 200
        printf("[ok] segmento truncado copia %zu bytes disponiveis\n", r.bytes_copied);
    }

    // 5. p_memsz absurdo: recusa em vez de alocar.
    {
        std::vector<u8> d(256, 0xAA);
        put_phdr(d, 64, /*off*/128, 0x10000000, /*filesz*/16, /*memsz*/0xF0000000u);
        auto r = parse_phdrs(d, 64, phent, 1);
        assert(r.loaded == 0 && r.skipped == 1);
        printf("[ok] p_memsz acima do teto e recusado\n");
    }

    // 6. Cabecalho parcial no fim do arquivo: recusa em vez de ler fora.
    // o=64, arquivo de 82 bytes: o+32=96 > 82, entao rd32(o+16)/rd32(o+20)
    // leriam fora. A guarda "o+32>size" recusa antes de qualquer rd32.
    {
        std::vector<u8> d(82, 0xAA);
        u32 one = 1; std::memcpy(d.data() + 64, &one, 4);  // ptype=1
        auto r = parse_phdrs(d, 64, phent, 1);
        assert(r.loaded == 0 && r.skipped == 1 && r.bytes_copied == 0);
        printf("[ok] cabecalho parcial no fim do arquivo e recusado\n");
    }

    printf("[Test] limites do leitor de ELF: OK\n");
    return 0;
}
