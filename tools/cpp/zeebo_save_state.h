#pragma once

#include <cstdint>
#include <string>
#include <vector>
#include <fstream>
#include <cstdio>
#include <cstring>
#include <unicorn/unicorn.h>

// Versioned Zeebo LLE Dual-Core Save State Format
// Header magic: 'Z' 'B' 'S' 'T' (0x5453425A)
// Version: 1
#pragma pack(push, 1)
struct ZeeboLLEStateHeader {
    uint32_t magic;          // 'ZBST' = 0x5453425A
    uint32_t version;        // 1
    uint64_t c0_insns;
    uint32_t c0_entry;
    uint64_t c1_insns;
    uint32_t c1_entry;
    uint32_t c0_context_size;
    uint32_t c1_context_size;
    uint32_t num_mem_regions;
};

struct ZeeboMemRegionHeader {
    uint64_t begin;
    uint64_t end;
    uint32_t perms;
    uint32_t data_size;
};
#pragma pack(pop)

class ZeeboSaveStateManager {
public:
    static constexpr uint32_t STATE_MAGIC = 0x5453425A; // 'ZBST'
    static constexpr uint32_t STATE_VERSION = 1;

    static bool save_state(const std::string& path,
                           uc_engine* uc0, uint64_t c0_insns, uint32_t c0_entry,
                           uc_engine* uc1, uint64_t c1_insns, uint32_t c1_entry) {
        if (!uc0 || !uc1) return false;

        std::ofstream out(path, std::ios::binary);
        if (!out) return false;

        size_t c0_ctx_sz = uc_context_size(uc0);
        size_t c1_ctx_sz = uc_context_size(uc1);

        std::vector<uint8_t> c0_ctx_buf(c0_ctx_sz);
        std::vector<uint8_t> c1_ctx_buf(c1_ctx_sz);

        uc_context* c0_ctx = (uc_context*)c0_ctx_buf.data();
        uc_context* c1_ctx = (uc_context*)c1_ctx_buf.data();

        if (uc_context_save(uc0, c0_ctx) != UC_ERR_OK) return false;
        if (uc_context_save(uc1, c1_ctx) != UC_ERR_OK) return false;

        // Retrieve memory regions for Core 0 (Shared RAM & virtual windows)
        uc_mem_region* regions = nullptr;
        uint32_t region_count = 0;
        if (uc_mem_regions(uc0, &regions, &region_count) != UC_ERR_OK) return false;

        ZeeboLLEStateHeader hdr{};
        hdr.magic = STATE_MAGIC;
        hdr.version = STATE_VERSION;
        hdr.c0_insns = c0_insns;
        hdr.c0_entry = c0_entry;
        hdr.c1_insns = c1_insns;
        hdr.c1_entry = c1_entry;
        hdr.c0_context_size = (uint32_t)c0_ctx_sz;
        hdr.c1_context_size = (uint32_t)c1_ctx_sz;
        hdr.num_mem_regions = region_count;

        out.write(reinterpret_cast<const char*>(&hdr), sizeof(hdr));
        out.write(reinterpret_cast<const char*>(c0_ctx_buf.data()), c0_ctx_sz);
        out.write(reinterpret_cast<const char*>(c1_ctx_buf.data()), c1_ctx_sz);

        // Serialize mapped memory regions
        for (uint32_t i = 0; i < region_count; i++) {
            uint64_t begin = regions[i].begin;
            uint64_t end = regions[i].end;
            uint32_t size = (uint32_t)(end - begin + 1);

            ZeeboMemRegionHeader reg_hdr{};
            reg_hdr.begin = begin;
            reg_hdr.end = end;
            reg_hdr.perms = regions[i].perms;
            reg_hdr.data_size = size;

            out.write(reinterpret_cast<const char*>(&reg_hdr), sizeof(reg_hdr));

            std::vector<uint8_t> mem_buf(size);
            if (uc_mem_read(uc0, begin, mem_buf.data(), size) == UC_ERR_OK) {
                out.write(reinterpret_cast<const char*>(mem_buf.data()), size);
            } else {
                std::vector<uint8_t> zeros(size, 0);
                out.write(reinterpret_cast<const char*>(zeros.data()), size);
            }
        }

        uc_free(regions);
        return true;
    }

    static bool load_state(const std::string& path,
                           uc_engine* uc0, uint64_t& c0_insns, uint32_t& c0_entry,
                           uc_engine* uc1, uint64_t& c1_insns, uint32_t& c1_entry) {
        if (!uc0 || !uc1) return false;

        std::ifstream in(path, std::ios::binary);
        if (!in) return false;

        ZeeboLLEStateHeader hdr{};
        in.read(reinterpret_cast<char*>(&hdr), sizeof(hdr));
        if (hdr.magic != STATE_MAGIC || hdr.version != STATE_VERSION) {
            std::fprintf(stderr, "[SaveState] Invalid magic or version\n");
            return false;
        }

        c0_insns = hdr.c0_insns;
        c0_entry = hdr.c0_entry;
        c1_insns = hdr.c1_insns;
        c1_entry = hdr.c1_entry;

        std::vector<uint8_t> c0_ctx_buf(hdr.c0_context_size);
        std::vector<uint8_t> c1_ctx_buf(hdr.c1_context_size);

        in.read(reinterpret_cast<char*>(c0_ctx_buf.data()), hdr.c0_context_size);
        in.read(reinterpret_cast<char*>(c1_ctx_buf.data()), hdr.c1_context_size);

        uc_context* c0_ctx = (uc_context*)c0_ctx_buf.data();
        uc_context* c1_ctx = (uc_context*)c1_ctx_buf.data();

        if (uc_context_restore(uc0, c0_ctx) != UC_ERR_OK) return false;
        if (uc_context_restore(uc1, c1_ctx) != UC_ERR_OK) return false;

        // Restore memory regions
        for (uint32_t i = 0; i < hdr.num_mem_regions; i++) {
            ZeeboMemRegionHeader reg_hdr{};
            in.read(reinterpret_cast<char*>(&reg_hdr), sizeof(reg_hdr));

            std::vector<uint8_t> mem_buf(reg_hdr.data_size);
            in.read(reinterpret_cast<char*>(mem_buf.data()), reg_hdr.data_size);

            // Ensure region is mapped
            uc_mem_map(uc0, reg_hdr.begin, reg_hdr.data_size, reg_hdr.perms);
            uc_mem_write(uc0, reg_hdr.begin, mem_buf.data(), reg_hdr.data_size);

            if (uc1) {
                // If it belongs to shared RAM, mirror in Core 1
                if (reg_hdr.begin >= 0x01F00000 && reg_hdr.begin < 0x02000000) {
                    uc_mem_map(uc1, reg_hdr.begin, reg_hdr.data_size, reg_hdr.perms);
                    uc_mem_write(uc1, reg_hdr.begin, mem_buf.data(), reg_hdr.data_size);
                }
            }
        }

        return true;
    }
};
