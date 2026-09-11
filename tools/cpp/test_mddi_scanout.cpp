// test_mddi_scanout.cpp — prova guest-faithful do scanout MDDI RGB565.
//
// Contrato de evidencia (observado, nao copiado de fonte de terceiros):
//   - PRI_PTR e MMIO 0xaa600008; o guest escreve nele o ponteiro para o inicio
//     de uma lista ligada de `mddi_llentry` que o motor MDDI consome via DMA.
//   - Cada entrada de VIDEO_STREAM (type=16) em formato RGB565 (0x5565) carrega
//     um span xy (x,y,w,h) e um ponteiro de dados para pixels RGB565.
//   - O motor precisa LER a memoria do guest; aqui a leitura e injetada por um
//     callback (sem leituras globais do host), o mesmo padrao do runtime.
//
// Este teste FALHA (RED) enquanto UnifiedMDDI so incrementa frame_count e nao
// consome a lista ligada nem escreve num sink. A producao minima o torna GREEN.
//
// Controles negativos exigidos:
//   1. PRI_PTR = 0        -> sink intocado.
//   2. pacote nao-video   -> sink intocado.
//   3. caminho Adreno/MDDI existente preservado (frame_count continua contando).

#include "zeebo_video_mmio.h"
#include <cstdio>
#include <map>
#include <functional>

static int fails = 0;
static void check(bool ok, const char* what, unsigned long got, unsigned long want) {
    printf("[%s] %s (obtido 0x%lx, esperado 0x%lx)\n", ok ? "PASS" : "FAIL", what, got, want);
    if (!ok) fails++;
}

// Memoria do guest simulada: mapa de words de 32 bits. O reader injetado le
// SOMENTE deste mapa — nenhuma leitura global do host.
struct GuestMem {
    std::map<u32,u32> words;
    void w32(u32 a, u32 v) { words[a & ~3u] = v; }
    // empacota dois pixels RGB565 por word (little-endian dentro do word).
    void wpix(u32 base, u32 i, u16 px) {
        u32 a = base + i*2;
        u32 word = read(a & ~3u);
        if (a & 2u) word = (word & 0x0000ffffu) | ((u32)px << 16);
        else        word = (word & 0xffff0000u) | px;
        words[a & ~3u] = word;
    }
    u32 read(u32 a) const {
        auto it = words.find(a & ~3u);
        return it != words.end() ? it->second : 0;
    }
};

// Escreve um mddi_llentry (8 words) na memoria do guest em `at`.
static void put_entry(GuestMem& g, u32 at, const mddi_llentry& e) {
    g.w32(at + 0,  e.type);
    g.w32(at + 4,  e.format);
    g.w32(at + 8,  e.x);
    g.w32(at + 12, e.y);
    g.w32(at + 16, e.w);
    g.w32(at + 20, e.h);
    g.w32(at + 24, e.data_ptr);
    g.w32(at + 28, e.next);
}

static void fill_region(GuestMem& g, u32 data_ptr, u32 w, u32 h, u16 color) {
    for (u32 i = 0; i < w*h; ++i) g.wpix(data_ptr, i, color);
}

// ---- positivo: duas regioes RGB565 visiveis ------------------------------
static void test_positive_two_regions() {
    GuestMem g;
    const u32 LIST = 0x11000000, E1 = 0x11000100, E2 = 0x11000200;
    const u32 D1 = 0x11010000,   D2 = 0x11020000;
    const u16 RED = 0xF800, BLUE = 0x001F;

    // Regiao 1: retangulo vermelho 4x4 no canto (10,20).
    fill_region(g, D1, 4, 4, RED);
    put_entry(g, E1, mddi_llentry{MDDI_VIDEO_STREAM, MDDI_FMT_RGB565, 10, 20, 4, 4, D1, E2});
    // Regiao 2: retangulo azul 8x2 em (100,200).
    fill_region(g, D2, 8, 2, BLUE);
    put_entry(g, E2, mddi_llentry{MDDI_VIDEO_STREAM, MDDI_FMT_RGB565, 100, 200, 8, 2, D2, 0});
    g.w32(LIST, E1); // PRI_PTR aponta para uma celula que contem a cabeca.
    // (usamos LIST diretamente como cabeca da lista ligada:)

    MddiScanoutSink sink;
    UnifiedMDDI mddi;
    mddi.attach_scanout([&g](u32 a){ return g.read(a); }, &sink);

    mddi.write(0x0008, E1); // PRI_PTR = cabeca da lista

    check(sink.regions_drawn == 2, "duas regioes de video desenhadas", sink.regions_drawn, 2);
    check(sink.at(10, 20) == RED,   "pixel (10,20) vermelho",  sink.at(10,20), RED);
    check(sink.at(13, 23) == RED,   "pixel (13,23) vermelho",  sink.at(13,23), RED);
    check(sink.at(100,200) == BLUE, "pixel (100,200) azul",    sink.at(100,200), BLUE);
    check(sink.at(107,201) == BLUE, "pixel (107,201) azul",    sink.at(107,201), BLUE);
    // fora das regioes permanece zero
    check(sink.at(0,0) == 0,        "pixel (0,0) intocado",    sink.at(0,0), 0);
    // caminho existente preservado: contou o quadro.
    check(mddi.frame_count() == 1,  "frame_count preservado",  mddi.frame_count(), 1);
}

// ---- negativo 1: PRI_PTR = 0 nao toca o sink -----------------------------
static void test_negative_null_ptr() {
    GuestMem g;
    MddiScanoutSink sink;
    UnifiedMDDI mddi;
    mddi.attach_scanout([&g](u32 a){ return g.read(a); }, &sink);
    mddi.write(0x0008, 0);
    check(sink.regions_drawn == 0, "PRI_PTR=0: nenhuma regiao", sink.regions_drawn, 0);
    check(sink.at(10,20) == 0,     "PRI_PTR=0: sink intocado",  sink.at(10,20), 0);
}

// ---- negativo 2: pacote nao-video nao toca o sink ------------------------
static void test_negative_non_video() {
    GuestMem g;
    const u32 E1 = 0x12000100, D1 = 0x12010000;
    fill_region(g, D1, 4, 4, 0xF800);
    // type != VIDEO_STREAM (ex.: register-access packet type=146) -> ignorado.
    put_entry(g, E1, mddi_llentry{146, MDDI_FMT_RGB565, 10, 20, 4, 4, D1, 0});
    MddiScanoutSink sink;
    UnifiedMDDI mddi;
    mddi.attach_scanout([&g](u32 a){ return g.read(a); }, &sink);
    mddi.write(0x0008, E1);
    check(sink.regions_drawn == 0, "pacote nao-video: nenhuma regiao", sink.regions_drawn, 0);
    check(sink.at(10,20) == 0,     "pacote nao-video: sink intocado",  sink.at(10,20), 0);
    check(mddi.frame_count() == 1, "nao-video ainda conta o quadro",   mddi.frame_count(), 1);
}

int main() {
    printf("== positivo: duas regioes RGB565 ==\n");     test_positive_two_regions();
    printf("== negativo: PRI_PTR = 0 ==\n");              test_negative_null_ptr();
    printf("== negativo: pacote nao-video ==\n");         test_negative_non_video();
    printf(fails ? "\nFALHAS: %d\n" : "\nTODOS OS TESTES PASSARAM\n", fails);
    return fails ? 1 : 0;
}
