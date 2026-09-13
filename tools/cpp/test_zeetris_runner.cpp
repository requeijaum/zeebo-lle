// test_zeetris_runner.cpp — TDD para o ZeetrisRunner encapsulado
#include <cassert>
#include <algorithm>
#include <cstdio>
#include <fstream>
#include <set>
#include <cstdlib>
#include <cstdint>
#include <string>
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
    // Liga o dispatcher IGL antes de setup_and_start para capturar slots de init (ex.: slot 16 glCompressedTexSubImage2D)
    auto rast = zeebo::gpu::create_rasterizer(zeebo::gpu::Backend::SoftwareRef);
    assert(rast);
    assert(rast->init());
    zeebo::gpu::IglHook igl_hook(*rast);
    ctx.igl_dispatcher = [&igl_hook](int slot, zeebo::gpu::GuestMachine& gm) {
        return igl_hook.dispatch_igl(slot, gm);
    };

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

    // EXPERIMENTO DIAGNÓSTICO (env-gated, OFF por padrão): o jogo nunca chama
    // glEnableClientState (slot 29), então `assemble()` no IglHook monta vértices
    // zerados. Sintetizamos o enable para responder à pergunta: "os dados de
    // vértice do jogo bastam para aparecer geometria?". Se aparecer, o único
    // bloqueio é o flag de enable; se não aparecer, faltam textura/matriz.
    if (std::getenv("ZEEBO_ZEETRIS_FORCE_ARRAYS")) {
        auto mk = [](u32 cap) {
            zeebo::gpu::GuestMachine m;
            m.arg = [cap](int) -> u32 { return cap; };
            m.set_ret = [](u32) {};
            m.read = [](u32, void*, u32) { return false; };
            return m;
        };
        auto gm_v = mk(0x8074u);  // GL_VERTEX_ARRAY
        auto gm_t = mk(0x8088u);  // GL_TEXTURE_COORD_ARRAY
        bool a = igl_hook.dispatch_igl(29, gm_v);
        bool b = igl_hook.dispatch_igl(29, gm_t);
        std::printf("[diag] glEnableClientState sintetizado: VERTEX_ARRAY=%d TEXCOORD=%d\n",
                    (int)a, (int)b);
    }

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
        // O glClear do guest sobrescreveu a sentinela, e agora o desenho do
        // PRÓPRIO jogo tem de aparecer: geometria além da cor de clear.
        assert(red_after < n / 10);
        assert(top_col != 0xF800u);
        assert(hist.size() >= 2);          // há mais de uma cor
        assert(n - top_px > n / 50);       // >2% da tela é geometria do jogo
    }

    // Controles: o jogo REALMENTE reage à tecla? Em vez de assumir, mede-se o
    // estado do applet por checksum antes e depois de despachar a tecla. Se o
    // handle_event muda qualquer palavra de estado, o checksum difere.
    {
        auto checksum = [uc, &ctx]() -> uint64_t {
            // Cobre as três regiões plausíveis de estado: AppContext, a área
            // sb (app_ctx+0x9000, onde vive o flag de transição de tela) e os
            // dados do módulo. Regiões não mapeadas são ignoradas e reportadas.
            const struct { u32 va, len; const char* nome; } regs[] = {
                { ctx.app_ctx_va,        0x1000,  "AppContext" },
                { ctx.app_ctx_va + 0x9000, 0x1000, "sb(+0x9000)" },
                { 0x123c0000u,           0x10000, "mod-data" },
                { 0x30004000u,           0xC000,  "heap/applet" },
            };
            uint64_t h = 1469598103934665603ull;
            for (const auto& r : regs) {
                std::vector<u8> buf(r.len);
                if (uc_mem_read(uc, r.va, buf.data(), r.len) != UC_ERR_OK) {
                    std::printf("[ctrl] regiao %s (0x%08x) NAO mapeada\n", r.nome, r.va);
                    continue;
                }
                for (u8 b : buf) { h ^= b; h *= 1099511628211ull; }
            }
            return h;
        };
        const uint64_t before = checksum();
        bool k1 = ZeetrisRunner::dispatch_key(uc, ctx, 0xE032, true);   // AVK_DOWN press
        bool k2 = ZeetrisRunner::dispatch_key(uc, ctx, 0xE032, false);  // AVK_DOWN release
        const uint64_t after = checksum();
        std::printf("[+] Controles: AVK_DOWN press=%d release=%d; estado do applet "
                    "0x%016llx -> 0x%016llx (%s)\n",
                    (int)k1, (int)k2, (unsigned long long)before, (unsigned long long)after,
                    before == after ? "SEM reacao" : "REAGIU");
        assert(k1 && k2);                 // a tecla foi consumida pelo HandleEvent
        assert(ctx.igl_calls > 0);        // e o loop de desenho continua vivo
    }

    // Diagnóstico: algum evento de aplicação faz o jogo inicializar o próprio
    // contexto ([pApplet+0x20]) ou entrar na carga de assets?
    if (std::getenv("ZEEBO_ZEETRIS_EVENTS")) {
        const struct { u32 evt; const char* nome; } evts[] = {
            // MEDIDO: no ramo evt==0 o jogo faz malloc(size) e grava o resultado em
            // [applet+0x20] (0x1200ab4c..0x1200ab68) e chama o init (0x120019f4).
            {0x0000u, "evt=0 (aloca ctx)"},
            {0x0001u, "EVT_APP_START"},   {0x0002u, "EVT_APP_STOP"},
            {0x0003u, "EVT_APP_SUSPEND"}, {0x0004u, "EVT_APP_RESUME"},
            {0x0005u, "EVT_BROWSE_URL"},  {0x0006u, "EVT_BROWSE_FILE"},
            {0x0007u, "EVT_BROWSE_MEDIA"},{0x0008u, "EVT_SCREEN_ORIENT"},
            {0x0009u, "EVT_APP_TERMINATE"},
        };
        for (const auto& e : evts) {
            u32 before_ctx = 0;
            uc_mem_read(uc, ctx.pApplet + 0x20, &before_ctx, 4);
            const u32 tex_before = ctx.tex_loader_calls;
            const size_t slots_before = ctx.igl_slot_calls.size();
            bool ok = ZeetrisRunner::dispatch_app_event(uc, ctx, e.evt);
            for (int i = 0; i < 4; ++i) ZeetrisRunner::step_frame(uc, ctx);
            u32 after_ctx = 0;
            uc_mem_read(uc, ctx.pApplet + 0x20, &after_ctx, 4);
            std::printf("[events] evt=0x%04x %-18s handled=%d applet+0x20: 0x%08x->0x%08x "
                        "| loader_textura %u->%u | slots GL %zu->%zu\n",
                        e.evt, e.nome, (int)ok, before_ctx, after_ctx,
                        tex_before, ctx.tex_loader_calls, slots_before,
                        ctx.igl_slot_calls.size());
        }
    }

    // Diagnóstico: a máscara de botões da plataforma move o jogo?
    // Env-gated. Escreve a máscara no ponto que o poll lê e mede se o estado do
    // jogo (checksum) muda -- e se surgem slots GL novos (textura/upload).
    if (std::getenv("ZEEBO_ZEETRIS_BUTTONS")) {
        auto cksum = [uc, &ctx]() -> uint64_t {
            uint64_t h = 1469598103934665603ull;
            const u32 regs[3][2] = {{0x003c141cu, 0x30}, {0x003c14acu, 0x20}, {0x30010000u, 0x400}};
            for (auto& r : regs) {
                std::vector<u8> buf(r[1]);
                if (uc_mem_read(uc, r[0], buf.data(), r[1]) != UC_ERR_OK) continue;
                for (u8 b : buf) { h ^= b; h *= 1099511628211ull; }
            }
            return h;
        };
        const u16 masks[] = {0x0001, 0x0002, 0x0004, 0x0008, 0x0010, 0x0020,
                             0x0080, 0x0200, 0xffff};
        std::printf("[buttons] a máscara chega ao input do jogo? (slots GL=%zu)\n",
                    ctx.igl_slot_calls.size());
        (void)cksum;
        for (u16 m : masks) {
            for (int i = 0; i < 8; ++i) ctx.btn_handler_calls[i] = 0;
            const size_t slots_before = ctx.igl_slot_calls.size();
            // O gameloop despacha por BORDA DE SOLTURA (prev & ~cur): pressiona
            // por 1 frame e solta por 3 para gerar a borda do bit.
            ZeetrisRunner::set_platform_buttons(uc, ctx, m);
            ZeetrisRunner::step_frame(uc, ctx);
            ZeetrisRunner::set_platform_buttons(uc, ctx, 0);
            for (int i = 0; i < 12; ++i) ZeetrisRunner::step_frame(uc, ctx);
            std::string fired;
            for (int i = 0; i < 8; ++i)
                if (ctx.btn_handler_calls[i])
                    fired += " bit" + std::to_string(1u << i) + "=" +
                             std::to_string(ctx.btn_handler_calls[i]);
            std::printf("[buttons] mask=0x%04x -> handlers:%s | slots GL %zu->%zu | "
                        "loader_textura=%u | IMedia(play=%u)\n",
                        m, fired.empty() ? " NENHUM" : fired.c_str(),
                        slots_before, ctx.igl_slot_calls.size(),
                        ctx.tex_loader_calls, ctx.media_play_calls);
        }
        ZeetrisRunner::set_platform_buttons(uc, ctx, 0);
    }

    // Diagnóstico do estado interno do jogo: o gameloop testa [0x123c141c+4] antes
    // de atualizar, e o bloco de input (que faria o poll da máscara) está morto.
    if (std::getenv("ZEEBO_ZEETRIS_STATE")) {
        std::printf("[state] poll da mascara executado %u vezes; bloco de input %u vezes\n",
                    ctx.input_poll_calls, ctx.input_block_calls);
        std::printf("[state] rotina de escrita no struct de input: %u execucoes\n",
                    ctx.input_store_calls);
        std::printf("[state] rotina de CARGA DE TEXTURA (0x120056fc): %u execucoes\n",
                    ctx.tex_loader_calls);
        u32 appctx_field = 0;
        if (uc_mem_read(uc, ctx.pApplet + 0x20, &appctx_field, 4) == UC_ERR_OK)
            std::printf("[state] [pApplet+0x20] (AppContext) = 0x%08x  %s\n",
                        appctx_field,
                        appctx_field == 0x30010000u ? "(o valor sintetico do runner)"
                                                    : "(NAO e o sintetico)");
        std::printf("[state] slots GL tocados: %zu, IGL calls=%u\n",
                    ctx.igl_slot_calls.size(), ctx.igl_calls);
        // Os dois espelhos: o módulo acessa seus dados por 0x003Cxxxx (absoluto)
        // e por 0x123Cxxxx (relocado). Só um deles carrega o estado vivo.
        const u32 bases[2] = {0x123c141cu, 0x003c141cu};
        const char* nomes[2] = {"0x123C141C (relocado)", "0x003C141C (espelho)"};
        for (int b = 0; b < 2; ++b) {
            std::printf("[state] struct do gameloop em %s:\n", nomes[b]);
            for (u32 off = 0; off <= 0x28; off += 4) {
                u32 v = 0;
                if (uc_mem_read(uc, bases[b] + off, &v, 4) == UC_ERR_OK)
                    std::printf("        [+0x%02x] = 0x%08x (%u)\n", off, v, v);
                else std::printf("        [+0x%02x] = <nao mapeado>\n", off);
            }
        }
        // Flags de tela que o gameloop consulta. Com o contexto sendo o do JOGO
        // (evt=0 -> malloc(0x9094)), sb = [pApplet+0x20] + 0x9000 -> dentro do
        // bloco (0x9000 < 0x9094, medido no literal do malloc).
        u32 gctx = 0;
        uc_mem_read(uc, ctx.pApplet + 0x20, &gctx, 4);
        std::printf("[state] contexto do JOGO = 0x%08x (malloc 0x9094); sb = 0x%08x\n",
                    gctx, gctx + 0x9000);
        if (gctx) {
            std::printf("[state] inicio do contexto do jogo:\n");
            for (u32 off = 0; off <= 0x20; off += 4) {
                u32 v = 0;
                if (uc_mem_read(uc, gctx + off, &v, 4) == UC_ERR_OK)
                    std::printf("        [ctx+0x%02x] = 0x%08x (%u)\n", off, v, v);
            }
        }
        std::printf("[state] sb = 0x%08x; flags do gameloop:\n", gctx + 0x9000);
        for (u32 off = 0; off <= 0x40; off += 4) {
            u32 v = 0;
            if (uc_mem_read(uc, gctx + 0x9000 + off, &v, 4) == UC_ERR_OK)
                std::printf("        [sb+0x%02x] = 0x%08x (%u)\n", off, v, v);
        }
        const u32 ib[2] = {0x123c14acu, 0x003c14acu};
        for (int b = 0; b < 2; ++b) {
            std::printf("[state] struct de input em 0x%08x:\n", ib[b]);
            for (u32 off = 0; off <= 0x18; off += 4) {
                u32 v = 0;
                if (uc_mem_read(uc, ib[b] + off, &v, 4) == UC_ERR_OK)
                    std::printf("        [+0x%02x] = 0x%08x (%u)\n", off, v, v);
                else std::printf("        [+0x%02x] = <nao mapeado>\n", off);
            }
        }
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
