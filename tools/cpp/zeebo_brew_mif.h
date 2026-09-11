#pragma once
// zeebo_brew_mif.h — parser estrutural de MIF do BREW 4.0.2.
// Clean-room: contrato derivado da estrutura observável dos arquivos MIF.

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <limits>
#include <string>
#include <vector>

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
        if (!data || size < kHeaderSize || read16(data) != kMagic) return info;

        const uint32_t table_offset = read32(data + 0x10);
        const uint32_t section_count = read32(data + 0x14);
        if (section_count == 0 || section_count > (size / sizeof(uint32_t))) return info;

        const size_t bounds_count = static_cast<size_t>(section_count) + 1;
        if (table_offset > size || bounds_count > (size - table_offset) / sizeof(uint32_t)) {
            return info;
        }

        uint32_t previous = read32(data + table_offset);
        if (previous > size) return info;
        for (uint32_t i = 0; i < section_count; ++i) {
            const uint32_t next = read32(data + table_offset + (static_cast<size_t>(i) + 1) * 4);
            if (next < previous || next > size) return MifAppletInfo{};

            if (next - previous == kAppletRecordSize &&
                read32(data + previous + 4) == 0 &&
                read32(data + previous + 12) == 0) {
                const uint32_t clsid = read32(data + previous);
                if (clsid != 0 && !info.valid) {
                    info.clsid = clsid;
                    info.valid = true;
                }
            }
            previous = next;
        }

        // Module filename is a separate string field in observed files. Keep the
        // extraction conservative and bounded; it does not establish validity.
        for (size_t i = 0; i + 4 <= size; ++i) {
            if (std::memcmp(data + i, ".mod", 4) != 0) continue;
            size_t start = i;
            while (start > 0 && data[start - 1] >= 0x20 && data[start - 1] <= 0x7e) --start;
            info.mod_file.assign(reinterpret_cast<const char*>(data + start), i + 4 - start);
            break;
        }
        return info;
    }

    static MifAppletInfo parse(const std::vector<uint8_t>& data) {
        return parse(data.data(), data.size());
    }

private:
    static constexpr uint16_t kMagic = 0x0011;
    static constexpr size_t kHeaderSize = 0x20;
    static constexpr uint32_t kAppletRecordSize = 20;

    static uint16_t read16(const uint8_t* p) {
        return static_cast<uint16_t>(p[0]) |
               (static_cast<uint16_t>(p[1]) << 8);
    }

    static uint32_t read32(const uint8_t* p) {
        return static_cast<uint32_t>(p[0]) |
               (static_cast<uint32_t>(p[1]) << 8) |
               (static_cast<uint32_t>(p[2]) << 16) |
               (static_cast<uint32_t>(p[3]) << 24);
    }
};

} // namespace zeebo::brew
