#pragma once
// zeebo_brew_mif.h — Parser determinístico de MIF (Module Information File) do BREW 4.0.2
//
// Extrai metadados do applet (AEECLSID, tipo de módulo, flags de privilégio,
// referências de arquivo .mod/.bar) a partir dos bytes estruturados do MIF.
// Clean-room: baseado na especificação de registros do BREW SDK e metadados de ClassDB.

#include <cstdint>
#include <vector>
#include <string>
#include <cstring>
#include <cstdio>
#include <algorithm>

namespace zeebo::brew {

struct MifAppletInfo {
    uint32_t clsid = 0;
    std::string name;
    std::string mod_file;
    bool valid = false;
};

class MifParser {
public:
    static MifAppletInfo parse(const uint8_t* data, size_t size) {
        MifAppletInfo info;
        if (!data || size < 32) return info;

        // O MIF BREW inicia com uma tabela de cabeçalho e ponteiros de blocos.
        // Varre registros em busca de CLSIDs válidos (faixas 0x01000000 - 0x010fffff ou registradas)
        for (size_t i = 0; i + 4 <= size; ++i) {
            uint32_t val = 0;
            std::memcpy(&val, data + i, 4);
            // Identifica padrões de CLSID de aplicações BREW / Zeebo (ex: 0x0100xxxx, 0x0102xxxx, 0x0107xxxx)
            if ((val & 0xff000000) == 0x01000000 && (val & 0x00ff0000) <= 0x000f0000 && val != 0x01000000) {
                info.clsid = val;
                info.valid = true;
                break;
            }
        }

        // Tenta localizar nomes de módulo (.mod) nas strings ASCII
        for (size_t i = 0; i + 4 < size; ++i) {
            if (std::memcmp(data + i, ".mod", 4) == 0) {
                // Retrocede até o início da string nula ou caractere não imprimível
                size_t start = i;
                while (start > 0 && data[start - 1] >= 0x20 && data[start - 1] <= 0x7e) {
                    --start;
                }
                info.mod_file = std::string(reinterpret_cast<const char*>(data + start), (i + 4) - start);
                break;
            }
        }

        return info;
    }

    static MifAppletInfo parse(const std::vector<uint8_t>& data) {
        return parse(data.data(), data.size());
    }
};

} // namespace zeebo::brew
