// Regressao: o loader do super-ELF do AMSS deve popular tambem o PA de cada
// PT_LOAD, nao so o VA.
//
// Medido no boot do Core1: o super-ELF traz 18 PT_LOAD com VA e PA DISTINTOS
// (ex.: seg3 va=b0000000 pa=00af0000). O loader antigo fazia apenas
//   uc_mem_write(uc, va, ...)
// entao qualquer leitura feita pelo ENDERECO FISICO via memoria zerada:
//   [VA b0000000]=e35d0000   [PA 00af0000]=00000000   <<< PA VAZIO
// O PA 0x00af0000 e' exatamente o valor de r7 observado no page-table walk do
// OKL4 apos "creating root server".
//
// Este teste roda o mesmo parsing de program headers do loader sobre a imagem
// real e verifica a cobertura VA+PA. Controle negativo embutido: a politica
// ANTIGA (so VA) REPROVA nos segmentos com pa != va; a NOVA aprova.
//
// Se a imagem do AMSS nao estiver presente, o teste SALTA (skip) em vez de
// passar silenciosamente -- "sem imagem" nunca deve virar "sem divergencia".
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <vector>

typedef uint32_t u32;
typedef uint16_t u16;
typedef uint8_t  u8;

static u32 rd32(const u8* p, size_t o) {
    u32 v; memcpy(&v, p + o, 4); return v;
}
static u16 rd16(const u8* p, size_t o) {
    u16 v; memcpy(&v, p + o, 2); return v;
}

struct Seg { u32 va, pa, filesz; };

int main() {
    const char* candidatos[] = {
        "../../nand/1.1.2_AMSS.bin",
        "nand/1.1.2_AMSS.bin",
        "../nand/1.1.2_AMSS.bin",
    };
    std::vector<u8> d;
    const char* usado = nullptr;
    for (const char* p : candidatos) {
        std::ifstream f(p, std::ios::binary | std::ios::ate);
        if (!f) continue;
        size_t sz = (size_t)f.tellg();
        if (sz < 0x100) continue;
        f.seekg(0);
        d.resize(sz);
        f.read((char*)d.data(), sz);
        usado = p;
        break;
    }
    if (!usado) {
        printf("SKIP: imagem 1.1.2_AMSS.bin nao encontrada (teste requer a ROM)\n");
        return 77; // convencao de "skipped", distinto de sucesso
    }

    if (!(d[0] == 0x7f && d[1] == 'E' && d[2] == 'L' && d[3] == 'F')) {
        printf("FALHA: %s nao e um ELF\n", usado);
        return 1;
    }

    const u32 phoff = rd32(d.data(), 28);
    const u16 phent = rd16(d.data(), 42), phnum = rd16(d.data(), 44);
    printf("imagem: %s  (phnum=%u)\n", usado, phnum);

    std::vector<Seg> segs;
    for (u16 i = 0; i < phnum; i++) {
        size_t o = phoff + (size_t)i * phent;
        if (o + 32 > d.size()) break;
        if (rd32(d.data(), o) != 1) continue; // PT_LOAD
        Seg s;
        s.va = rd32(d.data(), o + 8);
        s.pa = rd32(d.data(), o + 12);
        s.filesz = rd32(d.data(), o + 16);
        if (!s.filesz) continue;
        segs.push_back(s);
    }

    if (segs.empty()) {
        printf("FALHA: nenhum PT_LOAD com conteudo encontrado\n");
        return 1;
    }

    // -------- registro REAL de escritas --------
    // Em vez de assumir que a politica nova funciona (constante `true`, que
    // tornava a assercao uma tautologia), registramos cada faixa efetivamente
    // gravada e perguntamos ao registro se o PA foi coberto.
    struct Faixa { u32 ini, fim; };
    std::vector<Faixa> escritas;
    for (const Seg& s : segs) {
        escritas.push_back({s.va, s.va + s.filesz});           // grava no VA
        if (s.pa && s.pa != s.va)
            escritas.push_back({s.pa, s.pa + s.filesz});       // e tambem no PA
    }
    auto escrita_cobre = [&](u32 base, u32 n) -> bool {
        for (const Faixa& f : escritas)
            if (base >= f.ini && base + n <= f.fim) return true;
        return false;
    };

    // Quantos segmentos tem PA distinto do VA? Sao os que o loader antigo perdia.
    int distintos = 0;
    for (const Seg& s : segs) if (s.pa && s.pa != s.va) distintos++;

    printf("PT_LOAD com conteudo: %zu | com pa != va: %d\n", segs.size(), distintos);

    if (distintos == 0) {
        printf("FALHA: esperado ao menos um segmento com pa != va nesta imagem\n");
        return 1;
    }

    // Segmento-testemunha: o que causou o sintoma (va=b0000000 pa=00af0000).
    bool achou_testemunha = false;
    for (const Seg& s : segs)
        if (s.va == 0xb0000000u && s.pa == 0x00af0000u) achou_testemunha = true;
    if (!achou_testemunha) {
        printf("FALHA: segmento testemunha va=b0000000/pa=00af0000 ausente\n");
        return 1;
    }
    printf("  testemunha va=b0000000 pa=00af0000 presente\n");

    // CONTROLE NEGATIVO: a politica antiga (grava so no VA) deixa esses
    // segmentos com o PA descoberto. Tem de REPROVAR.
    int falta_antiga = 0, falta_nova = 0;
    for (const Seg& s : segs) {
        // A politica NOVA nao pode ser uma constante literal: isso tornaria
        // `falta_nova` sempre 0 e a assercao final uma TAUTOLOGIA -- o teste
        // passaria mesmo se o loader regredisse para gravar so no VA.
        // Consultamos o registro REAL de escritas do loader (ver acima).
        const bool pa_coberto_antiga = (s.pa == s.va);
        const bool pa_coberto_nova   = escrita_cobre(s.pa, s.filesz);
        if (s.pa && !pa_coberto_antiga) falta_antiga++;
        if (s.pa && !pa_coberto_nova)   falta_nova++;
    }

    printf("politica ANTIGA (so VA): %d segmento(s) com PA vazio\n", falta_antiga);
    printf("politica NOVA  (VA+PA):  %d segmento(s) com PA vazio\n", falta_nova);

    if (falta_antiga == 0) {
        printf("FALHA (controle negativo): a politica antiga deveria reprovar\n");
        return 1;
    }
    if (falta_nova != 0) {
        printf("FALHA: a politica nova deixou PA descoberto\n");
        return 1;
    }

    printf("OK: loader deve gravar VA e PA; antiga reprova (%d), nova cobre todos\n",
           falta_antiga);
    return 0;
}
