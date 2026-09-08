#pragma once

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <limits>
#include <string>
#include <unicorn/unicorn.h>

// Versioned Zeebo LLE dual-core save-state format.
#pragma pack(push, 1)
struct ZeeboLLEStateHeader {
    uint32_t magic;
    uint32_t version;
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
    uint32_t core_id;
    uint64_t data_size;
};
#pragma pack(pop)

class ZeeboSaveStateManager {
public:
    static constexpr uint32_t STATE_MAGIC = 0x5453425A; // 'ZBST'
    static constexpr uint32_t STATE_VERSION = 2;
    static constexpr uint64_t MAX_REGION_SIZE = 0x40000000ULL; // 1 GiB sanity cap
    static constexpr uint32_t MAX_REGIONS = 4096;
    static constexpr size_t IO_CHUNK = 1u << 20;

    static bool save_state(const std::string& path,
                           uc_engine* uc0, uint64_t c0_insns, uint32_t c0_entry,
                           uc_engine* uc1, uint64_t c1_insns, uint32_t c1_entry) {
        if (!uc0 || !uc1) return false;

        const size_t c0_ctx_sz = uc_context_size(uc0);
        const size_t c1_ctx_sz = uc_context_size(uc1);
        if (c0_ctx_sz > std::numeric_limits<uint32_t>::max() ||
            c1_ctx_sz > std::numeric_limits<uint32_t>::max()) return false;

        uc_context* c0_ctx = nullptr;
        uc_context* c1_ctx = nullptr;
        if (uc_context_alloc(uc0, &c0_ctx) != UC_ERR_OK ||
            uc_context_alloc(uc1, &c1_ctx) != UC_ERR_OK) {
            if (c0_ctx) uc_free(c0_ctx);
            if (c1_ctx) uc_free(c1_ctx);
            return false;
        }
        if (uc_context_save(uc0, c0_ctx) != UC_ERR_OK ||
            uc_context_save(uc1, c1_ctx) != UC_ERR_OK) {
            uc_free(c0_ctx); uc_free(c1_ctx); return false;
        }

        uc_mem_region* regions0 = nullptr;
        uc_mem_region* regions1 = nullptr;
        uint32_t count0 = 0, count1 = 0;
        if (uc_mem_regions(uc0, &regions0, &count0) != UC_ERR_OK ||
            uc_mem_regions(uc1, &regions1, &count1) != UC_ERR_OK ||
            count0 > MAX_REGIONS || count1 > MAX_REGIONS - count0) {
            if (regions0) uc_free(regions0);
            if (regions1) uc_free(regions1);
            uc_free(c0_ctx); uc_free(c1_ctx);
            return false;
        }

        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        if (!out) {
            uc_free(regions0); uc_free(regions1);
            uc_free(c0_ctx); uc_free(c1_ctx);
            return false;
        }

        ZeeboLLEStateHeader hdr{};
        hdr.magic = STATE_MAGIC;
        hdr.version = STATE_VERSION;
        hdr.c0_insns = c0_insns;
        hdr.c0_entry = c0_entry;
        hdr.c1_insns = c1_insns;
        hdr.c1_entry = c1_entry;
        hdr.c0_context_size = static_cast<uint32_t>(c0_ctx_sz);
        hdr.c1_context_size = static_cast<uint32_t>(c1_ctx_sz);
        hdr.num_mem_regions = count0 + count1;

        out.write(reinterpret_cast<const char*>(&hdr), sizeof(hdr));
        out.write(reinterpret_cast<const char*>(c0_ctx), static_cast<std::streamsize>(c0_ctx_sz));
        out.write(reinterpret_cast<const char*>(c1_ctx), static_cast<std::streamsize>(c1_ctx_sz));

        bool ok = out.good() && write_regions(out, uc0, 0, regions0, count0) &&
                                write_regions(out, uc1, 1, regions1, count1);
        uc_free(regions0); uc_free(regions1);
        uc_free(c0_ctx); uc_free(c1_ctx);
        out.flush();
        return ok && out.good();
    }

    static bool load_state(const std::string& path,
                           uc_engine* uc0, uint64_t& c0_insns, uint32_t& c0_entry,
                           uc_engine* uc1, uint64_t& c1_insns, uint32_t& c1_entry) {
        if (!uc0 || !uc1) return false;
        std::ifstream in(path, std::ios::binary);
        if (!in) return false;

        ZeeboLLEStateHeader hdr{};
        if (!read_exact(in, &hdr, sizeof(hdr)) || hdr.magic != STATE_MAGIC ||
            hdr.version != STATE_VERSION || hdr.num_mem_regions > MAX_REGIONS) {
            std::fprintf(stderr, "[SaveState] Invalid/truncated header or version\n");
            return false;
        }

        const size_t expected0 = uc_context_size(uc0);
        const size_t expected1 = uc_context_size(uc1);
        if (hdr.c0_context_size != expected0 || hdr.c1_context_size != expected1) {
            std::fprintf(stderr, "[SaveState] Unicorn context-size mismatch\n");
            return false;
        }

        uc_context* c0_ctx = nullptr;
        uc_context* c1_ctx = nullptr;
        if (uc_context_alloc(uc0, &c0_ctx) != UC_ERR_OK ||
            uc_context_alloc(uc1, &c1_ctx) != UC_ERR_OK) {
            if (c0_ctx) uc_free(c0_ctx);
            if (c1_ctx) uc_free(c1_ctx);
            return false;
        }
        bool ok = read_exact(in, c0_ctx, expected0) && read_exact(in, c1_ctx, expected1) &&
                  uc_context_restore(uc0, c0_ctx) == UC_ERR_OK &&
                  uc_context_restore(uc1, c1_ctx) == UC_ERR_OK;
        uc_free(c0_ctx); uc_free(c1_ctx);
        if (!ok) return false;

        std::array<uint8_t, IO_CHUNK> chunk{};
        for (uint32_t i = 0; i < hdr.num_mem_regions; ++i) {
            ZeeboMemRegionHeader rh{};
            if (!read_exact(in, &rh, sizeof(rh)) || rh.core_id > 1 || rh.end < rh.begin) return false;
            const uint64_t expected_size = rh.end - rh.begin + 1;
            if (rh.data_size != expected_size || rh.data_size == 0 ||
                rh.data_size > MAX_REGION_SIZE || rh.begin + rh.data_size - 1 != rh.end) return false;

            uc_engine* uc = rh.core_id == 0 ? uc0 : uc1;
            uc_err map_err = uc_mem_map(uc, rh.begin, static_cast<size_t>(rh.data_size), rh.perms);
            if (map_err != UC_ERR_OK && map_err != UC_ERR_MAP) return false;

            uint64_t done = 0;
            while (done < rh.data_size) {
                const size_t n = static_cast<size_t>(std::min<uint64_t>(chunk.size(), rh.data_size - done));
                if (!read_exact(in, chunk.data(), n) ||
                    uc_mem_write(uc, rh.begin + done, chunk.data(), n) != UC_ERR_OK) return false;
                done += n;
            }
        }

        c0_insns = hdr.c0_insns;
        c0_entry = hdr.c0_entry;
        c1_insns = hdr.c1_insns;
        c1_entry = hdr.c1_entry;
        return true;
    }

private:
    static bool read_exact(std::istream& in, void* dst, size_t size) {
        in.read(reinterpret_cast<char*>(dst), static_cast<std::streamsize>(size));
        return in.good() || (in.eof() && static_cast<size_t>(in.gcount()) == size);
    }

    static bool write_regions(std::ostream& out, uc_engine* uc, uint32_t core_id,
                              const uc_mem_region* regions, uint32_t count) {
        std::array<uint8_t, IO_CHUNK> chunk{};
        for (uint32_t i = 0; i < count; ++i) {
            if (regions[i].end < regions[i].begin) return false;
            const uint64_t size = regions[i].end - regions[i].begin + 1;
            if (size == 0 || size > MAX_REGION_SIZE) return false;
            ZeeboMemRegionHeader rh{regions[i].begin, regions[i].end,
                                    regions[i].perms, core_id, size};
            out.write(reinterpret_cast<const char*>(&rh), sizeof(rh));
            uint64_t done = 0;
            while (done < size) {
                const size_t n = static_cast<size_t>(std::min<uint64_t>(chunk.size(), size - done));
                if (uc_mem_read(uc, regions[i].begin + done, chunk.data(), n) != UC_ERR_OK) return false;
                out.write(reinterpret_cast<const char*>(chunk.data()), static_cast<std::streamsize>(n));
                if (!out) return false;
                done += n;
            }
        }
        return true;
    }
};
