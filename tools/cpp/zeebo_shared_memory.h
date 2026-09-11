#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>
#include <unicorn/unicorn.h>

namespace zeebo {

// Owns the bytes in `backing`; callers must keep it alive and must not resize it
// while either Unicorn engine is using the mapping.
inline uc_err map_shared_region_pair(uc_engine* first, uc_engine* second,
                                     uint64_t base, size_t size,
                                     std::vector<uint8_t>& backing,
                                     uint32_t perms = UC_PROT_ALL) {
    constexpr uint64_t kPageSize = 0x1000;
    if (!first || !second || size == 0 || (base & (kPageSize - 1)) != 0 ||
        (size & (kPageSize - 1)) != 0) {
        return UC_ERR_ARG;
    }

    backing.assign(size, 0);
    uc_err err = uc_mem_map_ptr(first, base, size, perms, backing.data());
    if (err != UC_ERR_OK) {
        backing.clear();
        return err;
    }

    err = uc_mem_map_ptr(second, base, size, perms, backing.data());
    if (err != UC_ERR_OK) {
        uc_mem_unmap(first, base, size);
        backing.clear();
        return err;
    }
    return UC_ERR_OK;
}

} // namespace zeebo
