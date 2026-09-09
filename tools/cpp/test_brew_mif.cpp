// test_brew_mif.cpp — TDD para parser de MIF/MIF2 do BREW 4.0.2 (QW35)
//
// Valida sem dependência de NAND nem execução forjada:
// 1. Rejeição de buffers nulos ou curtos (<32 bytes).
// 2. Extração de CLSID e identificação de referências a .mod em payloads sintéticos.
// 3. Validação estrutural de integridade do formato MIF.

#include <cstdio>
#include <cassert>
#include <vector>
#include <cstring>
#include "zeebo_brew_mif.h"

int main() {
    printf("=== Test BREW MIF Parser (QW35) ===\n");

    // Teste 1: Buffer nulo ou truncado deve retornar valid == false
    {
        auto info = zeebo::brew::MifParser::parse(nullptr, 0);
        assert(!info.valid);
        assert(info.clsid == 0);

        std::vector<uint8_t> short_buf(16, 0);
        info = zeebo::brew::MifParser::parse(short_buf);
        assert(!info.valid);
    }

    // Teste 2: Payload MIF sintético com AEECLSID_APP_MGR (0x01004003) e appmgr.mod
    {
        std::vector<uint8_t> mif_buf(128, 0);
        // Header inicial arbitrário
        mif_buf[0] = 0x11;
        mif_buf[1] = 0x00;
        mif_buf[2] = 0x01;
        mif_buf[3] = 0x00;

        // Injeta CLSID @ offset 32 (little endian)
        uint32_t clsid = 0x01004003;
        std::memcpy(mif_buf.data() + 32, &clsid, 4);

        // Injeta nome de módulo @ offset 48
        const char* mod_name = "brewappmgr.mod";
        std::memcpy(mif_buf.data() + 48, mod_name, std::strlen(mod_name));

        auto info = zeebo::brew::MifParser::parse(mif_buf);
        assert(info.valid);
        assert(info.clsid == 0x01004003);
        assert(info.mod_file == "brewappmgr.mod");
    }

    // Teste 3: Payload de jogo leve (ex: Double Dragon, CLSID 0x0102F789)
    {
        std::vector<uint8_t> game_mif(256, 0);
        uint32_t dd_clsid = 0x0102f789;
        std::memcpy(game_mif.data() + 40, &dd_clsid, 4);
        const char* dd_mod = "doubledragon.mod";
        std::memcpy(game_mif.data() + 64, dd_mod, std::strlen(dd_mod));

        auto info = zeebo::brew::MifParser::parse(game_mif);
        assert(info.valid);
        assert(info.clsid == 0x0102f789);
        assert(info.mod_file == "doubledragon.mod");
    }

    printf("=== Test BREW MIF Parser: PASS ===\n");
    return 0;
}
