// test_zeetris_runner.cpp — TDD para o ZeetrisRunner encapsulado
#include <cassert>
#include <algorithm>
#include <cstdio>
#include <fstream>
#include <set>
#include <vector>
#include <unicorn/unicorn.h>
#include "zeebo_zeetris_runner.h"
#include "gpu/igpu_rasterizer.h"
#include "gpu/igl_hook.h"

using namespace zeebo::zeetris;

static std::vector<u8> read_file(const std::string& p) {
    std::ifstream f(p, std::ios::binary | std::ios::ate);
    if (!f) return {};
    std::streamoff n = f.tellg();
    if (n <= 0) return {};
    std::vector<u8> b(static_cast<size_t>(n));
    f.seekg(0);
    f.read(reinterpret_cast<char*>(b.data()), n);
    return b;
}

int main(int argc, char** argv) {
    std::printf("=== Test ZeetrisRunner Encapsulated ===\n");
    std::string mod_path = "/home/rafaelfrequiao/Downloads/mod/zeetris/zeetris.mod";
    auto bytes = read_file(mod_path);
    if (bytes.empty()) {
        std::printf("SKIP (exit 77): zeetris.mod não encontrado\n");
        return 77;
    }

    if (argc > 1 && std::string(argv[1]) == "buggy") {
        std::printf("[test] Modo buggy solicitado (controle negativo)\n");
        assert(false);
    }

    uc_engine* uc = nullptr;
    uc_err err = uc_open(UC_ARCH_ARM, UC_MODE_ARM, &uc);
    assert(err == UC_ERR_OK);

    // Mapeia e injeta o .mod
    uc_mem_map(uc, ZeetrisRunner::LOAD_VA, 0x00400000, UC_PROT_ALL);
    uc_mem_write(uc, ZeetrisRunner::LOAD_VA, bytes.data(), bytes.size());

    ZeetrisContext ctx{};
    std::printf("[test] Calling setup_and_start...\n");
    std::fflush(stdout);
    bool ok = ZeetrisRunner::setup_and_start(uc, ctx);
    std::printf("[test] setup_and_start returned %d\n", (int)ok);
    std::fflush(stdout);
    assert(ok);
    assert(ctx.is_running);
    assert(ctx.pApplet != 0);

    // Testa envio de teclas (AVK_SELECT 0xE035)
    bool press_ok = ZeetrisRunner::dispatch_key(uc, ctx, 0xE035, true);
    assert(press_ok);
    bool release_ok = ZeetrisRunner::dispatch_key(uc, ctx, 0xE035, false);
    assert(release_ok);

    // Liga a vtable IGL do guest ao rasterizador de software real: cada slot
    // gl* despachado pelo jogo passa a virar geometria de verdade (glClear,
    // glBindTexture, glEnable, glTexCoordPointer, glVertexPointer, glDrawArrays).
    auto rast = zeebo::gpu::create_rasterizer(zeebo::gpu::Backend::SoftwareRef);
    assert(rast);
    assert(rast->init());
    zeebo::gpu::IglHook igl_hook(*rast);
    ctx.igl_dispatcher = [&igl_hook](int slot, zeebo::gpu::GuestMachine& gm) {
        return igl_hook.dispatch_igl(slot, gm);
    };
    rast->begin_frame();
    // CONTROLE POSITIVO DO INSTRUMENTO: pinta o framebuffer de vermelho (sentinela)
    // e em seguida deixa a cor de clear preta. Se o glClear do JOGO realmente
    // executar, ele sobrescreve o vermelho -> o vermelho desaparece. Se o seam
    // estiver morto, o vermelho sobrevive (o teste reprova).
    rast->clear_color(1.0f, 0.0f, 0.0f, 1.0f);
    rast->clear(0x00004000u);            // GL_COLOR_BUFFER_BIT
    rast->clear_color(0.0f, 0.0f, 0.0f, 1.0f);
    auto count_red = [&rast]() -> size_t {
        const u16* fb = rast->framebuffer_rgb565();
        const size_t n = (size_t)zeebo::gpu::kFbWidth * zeebo::gpu::kFbHeight;
        size_t red = 0;
        for (size_t i = 0; i < n; ++i)
            if ((fb[i] >> 11) == 0x1f && ((fb[i] >> 5) & 0x3f) == 0 && (fb[i] & 0x1f) == 0) red++;
        return red;
    };
    const size_t red_before = count_red();
    const size_t fb_n = (size_t)zeebo::gpu::kFbWidth * zeebo::gpu::kFbHeight;
    std::printf("[test] sentinela vermelha no framebuffer: %zu/%zu px\n", red_before, fb_n);
    assert(red_before > fb_n / 2);   // o instrumento funciona

    // Testa avanço de múltiplos frames no gameloop (60 frames)
    for (int i = 0; i < 60; ++i) {
        bool frame_ok = ZeetrisRunner::step_frame(uc, ctx);
        assert(frame_ok);
    }
    rast->end_frame();
    std::printf("[+] Positivo: ZeetrisRunner avançou 60 frames com sucesso! (display_updates=%u, drawrect=%u, bitblt=%u)\n",
                ctx.display_update_calls, ctx.display_drawrect_calls, ctx.display_bitblt_calls);

    // Validação de vídeo: o global 0x003c14c4 é a interface IGL (GL ES), não
    // IDisplay — o jogo desenha via 77 thunks de vtable (slots 3..79). Aqui
    // provamos que os slots GL do PRÓPRIO jogo chegam ao bridge; a rasterização
    // real desses comandos pelo IglHook é o passo seguinte.
    std::printf("[+] IGL (GL ES) dispatch: %u chamadas em %zu slots distintos "
                "(handled=%u em %zu slots)\n",
                ctx.igl_calls, ctx.igl_slot_calls.size(),
                ctx.igl_handled, ctx.igl_slot_handled.size());
    for (const auto& [slot, n] : ctx.igl_slot_calls) {
        auto it = ctx.igl_slot_handled.find(slot);
        std::printf("      slot %2u -> %u chamadas, handled=%u\n",
                    slot, n, it == ctx.igl_slot_handled.end() ? 0u : it->second);
    }
    assert(ctx.igl_calls > 0);
    assert(ctx.igl_slot_calls.size() > 1);
    // O IglHook aceitou de fato os slots com semântica implementada.
    assert(ctx.igl_handled > 0);
    // glClear (slot 7) — comando que emite pixels — aceito pelo hook.
    assert(ctx.igl_slot_handled.count(7) > 0);

    // Prova do seam completo: o glClear do JOGO executou no rasterizador.
    // A sentinela vermelha só desaparece se o comando veio do guest.
    {
        const u16* fb = rast->framebuffer_rgb565();
        assert(fb);
        const size_t n = (size_t)zeebo::gpu::kFbWidth * zeebo::gpu::kFbHeight;
        std::map<u16, size_t> hist;
        for (size_t i = 0; i < n; ++i) hist[fb[i]]++;
        size_t top_px = 0;
        u16 top_col = 0;
        for (const auto& [c, cnt] : hist) if (cnt > top_px) { top_px = cnt; top_col = c; }
        const size_t red_after = count_red();
        std::printf("[+] Rasterizador pos-frames: %zu cores, dominante=0x%04x (%zu px), "
                    "vermelho restante=%zu px\n", hist.size(), top_col, top_px, red_after);
        for (const auto& [c, cnt] : hist)
            if (c != top_col) std::printf("      cor 0x%04x -> %zu px\n", c, cnt);
        // NOTA HONESTA: ainda NÃO há geometria. Nos 60 frames o jogo emite
        // glClear/glBindTexture/glEnable/gl*Pointer/glDrawArrays mas nunca
        // glEnableClientState(29) nem glMatrixMode(51)/glLoadIdentity(46) ->
        // arrays de vértice desabilitados e sem projeção. Próximo passo.
        assert(red_after < n / 10);            // o glClear do guest sobrescreveu a sentinela
        assert(top_col != 0xF800u);            // e o dominante não é mais o vermelho
    }

    // Validação de áudio / IMedia (0x0106e415)
    std::printf("[+] IMedia calls: RegisterNotify=%u, SetParam=%u, Play=%u\n",
                ctx.media_register_notify_calls, ctx.media_set_param_calls, ctx.media_play_calls);
    assert(ctx.media_register_notify_calls >= 2);
    assert(ctx.media_notify_fn != 0);

    uc_close(uc);
    std::printf("=== Test ZeetrisRunner: PASS ===\n");
    return 0;
}
