// test_zeetris_runner.cpp — TDD para o ZeetrisRunner encapsulado
#include <cassert>
#include <algorithm>
#include <cstdio>
#include <fstream>
#include <set>
#include <vector>
#include <unicorn/unicorn.h>
#include "zeebo_zeetris_runner.h"

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

    // Testa avanço de múltiplos frames no gameloop (60 frames)
    for (int i = 0; i < 60; ++i) {
        bool frame_ok = ZeetrisRunner::step_frame(uc, ctx);
        assert(frame_ok);
    }
    std::printf("[+] Positivo: ZeetrisRunner avançou 60 frames com sucesso! (display_updates=%u, drawrect=%u, bitblt=%u)\n",
                ctx.display_update_calls, ctx.display_drawrect_calls, ctx.display_bitblt_calls);

    // Validação de vídeo: o global 0x003c14c4 é a interface IGL (GL ES), não
    // IDisplay — o jogo desenha via 77 thunks de vtable (slots 3..79). Aqui
    // provamos que os slots GL do PRÓPRIO jogo chegam ao bridge; a rasterização
    // real desses comandos pelo IglHook é o passo seguinte.
    std::printf("[+] IGL (GL ES) dispatch: %u chamadas em %zu slots distintos\n",
                ctx.igl_calls, ctx.igl_slot_calls.size());
    for (const auto& [slot, n] : ctx.igl_slot_calls) {
        std::printf("      slot %2u -> %u chamadas\n", slot, n);
    }
    assert(ctx.igl_calls > 0);
    assert(ctx.igl_slot_calls.size() > 1);

    // Validação de áudio / IMedia (0x0106e415)
    std::printf("[+] IMedia calls: RegisterNotify=%u, SetParam=%u, Play=%u\n",
                ctx.media_register_notify_calls, ctx.media_set_param_calls, ctx.media_play_calls);
    assert(ctx.media_register_notify_calls >= 2);
    assert(ctx.media_notify_fn != 0);

    uc_close(uc);
    std::printf("=== Test ZeetrisRunner: PASS ===\n");
    return 0;
}
