// test_zeetris_playable.cpp — Teste RED do RCA (3-legged 5-why, rodada 5).
//
// PROPOSITO
// ---------
// O teste `test_zeetris_runner` valida MECANICA DE EXECUCAO (o gameloop retorna
// limpo, contadores sobem). Ele passa com exit 0 enquanto a tela mostra um quad
// branco estatico e nenhum input funciona. Isso e um falso positivo estrutural:
// atividade != progresso.
//
// Este teste valida SEMANTICA DE SIMULACAO. Os tres criterios abaixo sao
// falsificaveis e, no momento da escrita, TODOS REPROVAM o codigo atual:
//
//   C1  tex_loader_calls > 0        (hoje: 0   -> a rotina 0x120056fc nunca roda)
//   C2  hash(fb@F_EARLY) != hash(fb@F_LATE)  (hoje: identico -> tela congelada)
//   C3  pico de platform_buttons != 0 apos tecla (hoje: 0x0000 -> input ignorado)
//
// CAUSA RAIZ QUE ELES COBREM (ver RCA): o .mod BREW e carregado como blob flat
// em 0x12000000; a secao RW (offset 0x3c12d0..EOF, 2428 B) nunca e copiada para
// a RAM de dados e os literais linkados em base 0 nunca sao relocados somando a
// base de carga. Consequencia unica, quatro sintomas: [r3+0x50]==0 barra o
// carregador de textura; o literal 0x1200bc4c==0 gera blx 0; a mascara de botoes
// nunca e escrita; IMedia nunca recebe Play.
//
// CONTROLE NEGATIVO DO INSTRUMENTO: rode com argv[1]=="selftest". O teste injeta
// um framebuffer artificialmente mutante e uma contagem artificial; se os
// detectores C1/C2/C3 nao acusarem PASS nesse cenario, o proprio instrumento
// esta quebrado e o resultado RED nao vale nada.
//
// GATE: exit 77 = SKIP quando o zeetris.mod nao esta presente (regra do Makefile).

#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>
#include <unicorn/unicorn.h>
#include "zeebo_zeetris_runner.h"
#include "gpu/igpu_rasterizer.h"
#include "gpu/igl_hook.h"

using namespace zeebo::zeetris;

namespace {

constexpr int kFrameEarly = 10;
constexpr int kFrameLate  = 300;

std::vector<u8> read_file(const std::string& p) {
    std::ifstream f(p, std::ios::binary | std::ios::ate);
    if (!f) return {};
    std::streamoff n = f.tellg();
    if (n <= 0) return {};
    std::vector<u8> b(static_cast<size_t>(n));
    f.seekg(0);
    f.read(reinterpret_cast<char*>(b.data()), n);
    return b;
}

// FNV-1a 64: barato e suficiente para detectar "o framebuffer mudou?".
uint64_t fb_hash(const u16* fb, size_t n_px) {
    uint64_t h = 1469598103934665603ull;
    if (!fb) return 0;
    const auto* p = reinterpret_cast<const uint8_t*>(fb);
    for (size_t i = 0; i < n_px * sizeof(u16); ++i) {
        h ^= p[i];
        h *= 1099511628211ull;
    }
    return h;
}

struct Verdict {
    bool c1 = false, c2 = false, c3 = false;
    uint32_t tex_calls = 0;
    uint64_t h_early = 0, h_late = 0;
    uint16_t peak_buttons = 0;
    bool all() const { return c1 && c2 && c3; }
};

void report(const char* tag, const Verdict& v) {
    std::printf("\n--- veredito [%s] ---\n", tag);
    std::printf("  C1 carga de textura executou   : %-5s (tex_loader_calls=%u, exigido >0)\n",
                v.c1 ? "PASS" : "FAIL", v.tex_calls);
    std::printf("  C2 framebuffer mudou F%d->F%d : %-5s (0x%016llx vs 0x%016llx)\n",
                kFrameEarly, kFrameLate, v.c2 ? "PASS" : "FAIL",
                (unsigned long long)v.h_early, (unsigned long long)v.h_late);
    std::printf("  C3 input chegou ao jogo        : %-5s (pico da mascara=0x%04x, exigido !=0)\n",
                v.c3 ? "PASS" : "FAIL", v.peak_buttons);
}

// Controle positivo do instrumento: cenario sinteticamente "saudavel".
// Se os detectores nao acusarem PASS aqui, eles estao quebrados.
int run_selftest() {
    std::printf("=== Controle positivo do instrumento (selftest) ===\n");
    const size_t n_px = 640 * 480;
    std::vector<u16> a(n_px, 0x0000), b(n_px, 0x0000);
    b[1234] = 0xF81F;  // um unico pixel diferente

    Verdict v;
    v.tex_calls    = 17;                   // como se o loader tivesse rodado
    v.h_early      = fb_hash(a.data(), n_px);
    v.h_late       = fb_hash(b.data(), n_px);
    v.peak_buttons = 0x0004;               // como se uma tecla tivesse marcado
    v.c1 = v.tex_calls > 0;
    v.c2 = v.h_early != v.h_late;
    v.c3 = v.peak_buttons != 0;
    report("selftest", v);

    if (!v.all()) {
        std::printf("\nINSTRUMENTO QUEBRADO: cenario saudavel nao passou. "
                    "Qualquer resultado RED deste teste e invalido.\n");
        return 1;
    }
    // Contraprova: cenario doente TEM que reprovar.
    Verdict sick;
    sick.tex_calls    = 0;
    sick.h_early      = fb_hash(a.data(), n_px);
    sick.h_late       = fb_hash(a.data(), n_px);
    sick.peak_buttons = 0;
    sick.c1 = sick.tex_calls > 0;
    sick.c2 = sick.h_early != sick.h_late;
    sick.c3 = sick.peak_buttons != 0;
    report("selftest-doente", sick);
    if (sick.c1 || sick.c2 || sick.c3) {
        std::printf("\nINSTRUMENTO QUEBRADO: cenario doente passou em algum criterio.\n");
        return 1;
    }
    std::printf("\nInstrumento VALIDADO: acusa saudavel e reprova doente.\n");
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc > 1 && std::string(argv[1]) == "selftest") {
        return run_selftest();
    }

    std::printf("=== Test Zeetris JOGAVEL (criterios de simulacao, nao de execucao) ===\n");

    const char* env_mod = std::getenv("ZEEBO_ZEETRIS_MOD");
    std::string mod_path = env_mod ? env_mod
                                   : "/home/rafaelfrequiao/Downloads/mod/zeetris/zeetris.mod";
    auto bytes = read_file(mod_path);
    if (bytes.empty()) {
        std::printf("SKIP (exit 77): zeetris.mod nao encontrado em '%s'\n", mod_path.c_str());
        return 77;
    }

    uc_engine* uc = nullptr;
    if (uc_open(UC_ARCH_ARM, UC_MODE_ARM, &uc) != UC_ERR_OK) {
        std::printf("FAIL: uc_open\n");
        return 1;
    }
    uc_mem_map(uc, ZeetrisRunner::LOAD_VA, 0x00400000, UC_PROT_ALL);
    uc_mem_write(uc, ZeetrisRunner::LOAD_VA, bytes.data(), bytes.size());

    auto rast = zeebo::gpu::create_rasterizer(zeebo::gpu::Backend::SoftwareRef);
    if (!rast || !rast->init()) {
        std::printf("FAIL: rasterizador\n");
        return 1;
    }
    zeebo::gpu::IglHook igl_hook(*rast);

    ZeetrisContext ctx{};
    ctx.igl_dispatcher = [&igl_hook](int slot, zeebo::gpu::GuestMachine& gm) {
        return igl_hook.dispatch_igl(slot, gm);
    };

    if (!ZeetrisRunner::setup_and_start(uc, ctx)) {
        std::printf("FAIL: setup_and_start nao armou o ciclo de vida\n");
        return 1;
    }

    const size_t n_px = 640 * 480;
    Verdict v;
    rast->begin_frame();

    // AVK_DOWN, o mesmo usado pelo teste de input existente.
    constexpr u32 kAvkDown = 0xE032u;

    for (int f = 1; f <= kFrameLate; ++f) {
        // Aperta uma tecla algumas vezes ao longo da corrida; C3 mede se a
        // mascara que o JOGO le chega a refletir isso em algum instante.
        if (f == 20 || f == 120 || f == 220) {
            ZeetrisRunner::dispatch_key(uc, ctx, kAvkDown, true);
            if (ctx.platform_buttons > v.peak_buttons) v.peak_buttons = ctx.platform_buttons;
            ZeetrisRunner::step_frame(uc, ctx);
            if (ctx.platform_buttons > v.peak_buttons) v.peak_buttons = ctx.platform_buttons;
            ZeetrisRunner::dispatch_key(uc, ctx, kAvkDown, false);
        }

        if (!ZeetrisRunner::step_frame(uc, ctx)) {
            std::printf("[loop] step_frame falhou no frame %d\n", f);
            break;
        }
        if (ctx.platform_buttons > v.peak_buttons) v.peak_buttons = ctx.platform_buttons;

        rast->end_frame();
        const u16* fb = rast->framebuffer_rgb565();
        if (f == kFrameEarly) v.h_early = fb_hash(fb, n_px);
        if (f == kFrameLate)  v.h_late  = fb_hash(fb, n_px);
        rast->begin_frame();
    }

    v.tex_calls = ctx.tex_loader_calls;
    v.c1 = v.tex_calls > 0;
    v.c2 = (v.h_early != v.h_late) && v.h_early != 0;
    v.c3 = v.peak_buttons != 0;

    const char* neg = std::getenv("ZEEBO_ZEETRIS_NO_GLOBAL_PATCHES");
    report(neg ? "sem remendos de globais (controle negativo)" : "codigo atual", v);

    std::printf("\n[ctx] igl_calls=%u igl_handled=%u slots=%zu | file_open=%u file_read=%u\n",
                ctx.igl_calls, ctx.igl_handled, ctx.igl_slot_calls.size(),
                ctx.file_open_calls, ctx.file_read_calls);

    // CONTROLE POSITIVO DO INSTRUMENTO IN-GUEST. Sem isto, C1 e ambiguo:
    // "a rotina nao executa" e "eu nao consegui medir" produzem o mesmo 0.
    std::printf("[controle-positivo] hook_selftest_calls=%u (hook identico em 0x1200ac5c,\n"
                "                    gameloop que executa todo frame; exigido >0)\n",
                ctx.hook_selftest_calls);
    if (ctx.hook_selftest_calls == 0) {
        std::printf("\n*** INSTRUMENTO INVALIDO: o mecanismo de hook NAO dispara nem no\n"
                    "    gameloop. Logo tex_loader_calls=0 NAO prova nada sobre texturas. ***\n");
        uc_close(uc);
        return 1;
    }
    std::printf("[controle-positivo] OK: o mecanismo de hook funciona, logo C1 e um sinal real.\n");

    if (v.all()) {
        std::printf("\n=== PASS: o jogo carrega textura, a tela evolui e o input chega. ===\n");
        uc_close(uc);
        return 0;
    }
    std::printf("\n=== FAIL (RED esperado ate o loader BREW existir): "
                "o applet executa, mas nao simula. ===\n");
    uc_close(uc);
    return 1;
}
