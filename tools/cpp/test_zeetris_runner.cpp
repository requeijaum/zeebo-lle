// test_zeetris_runner.cpp — TDD para o ZeetrisRunner encapsulado
#include <cassert>
#include <cstdio>
#include <fstream>
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

    // Testa envio de teclas (AVK_SELECT 0x102)
    bool press_ok = ZeetrisRunner::dispatch_key(uc, ctx, 0x102, true);
    assert(press_ok);
    bool release_ok = ZeetrisRunner::dispatch_key(uc, ctx, 0x102, false);
    assert(release_ok);

    // Testa avanço de múltiplos frames no gameloop
    for (int i = 0; i < 5; ++i) {
        bool frame_ok = ZeetrisRunner::step_frame(uc, ctx);
        assert(frame_ok);
    }
    std::printf("[+] Positivo: ZeetrisRunner avançou 5 frames com sucesso!\n");

    uc_close(uc);
    std::printf("=== Test ZeetrisRunner: PASS ===\n");
    return 0;
}
