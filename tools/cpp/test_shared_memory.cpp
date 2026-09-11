// test_shared_memory.cpp — two Unicorn cores must observe one SMEM backing.
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <vector>
#include <unicorn/unicorn.h>
#include "zeebo_shared_memory.h"

int main() {
    uc_engine *c0 = nullptr, *c1 = nullptr;
    assert(uc_open(UC_ARCH_ARM, UC_MODE_ARM, &c0) == UC_ERR_OK);
    assert(uc_open(UC_ARCH_ARM, UC_MODE_ARM, &c1) == UC_ERR_OK);

    constexpr uint64_t base = 0x01f00000;
    constexpr size_t size = 0x2000;
    std::vector<uint8_t> backing;
    assert(zeebo::map_shared_region_pair(c0, c1, base, size, backing) == UC_ERR_OK);
    assert(backing.size() == size);

    uint32_t from_c0 = 0x12345678;
    uint32_t seen_c1 = 0;
    assert(uc_mem_write(c0, base + 0x100, &from_c0, sizeof(from_c0)) == UC_ERR_OK);
    assert(uc_mem_read(c1, base + 0x100, &seen_c1, sizeof(seen_c1)) == UC_ERR_OK);
    assert(seen_c1 == from_c0);

    uint32_t from_c1 = 0xa5a55a5a;
    uint32_t seen_c0 = 0;
    assert(uc_mem_write(c1, base + 0x104, &from_c1, sizeof(from_c1)) == UC_ERR_OK);
    assert(uc_mem_read(c0, base + 0x104, &seen_c0, sizeof(seen_c0)) == UC_ERR_OK);
    assert(seen_c0 == from_c1);

    uc_close(c1);
    uc_close(c0);
    std::puts("PASS: SMEM has one host backing visible to both cores");
    return 0;
}
