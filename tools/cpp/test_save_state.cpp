#include "zeebo_save_state.h"
#include <cassert>
#include <fstream>
#include <iostream>

int main() {
    uc_engine *uc0 = nullptr, *uc1 = nullptr;
    uc_err err;

    err = uc_open(UC_ARCH_ARM, UC_MODE_ARM, &uc0);
    assert(err == UC_ERR_OK);
    err = uc_open(UC_ARCH_ARM, UC_MODE_ARM, &uc1);
    assert(err == UC_ERR_OK);

    // Setup dummy RAM & registers
    uint64_t ram_base = 0x10000000;
    size_t ram_size = 0x100000; // 1MB
    err = uc_mem_map(uc0, ram_base, ram_size, UC_PROT_ALL);
    assert(err == UC_ERR_OK);
    const uint64_t c1_ram_base = 0x00a00000;
    err = uc_mem_map(uc1, c1_ram_base, ram_size, UC_PROT_ALL);
    assert(err == UC_ERR_OK);

    uint32_t val = 0xCAFEBABE;
    uint32_t c1_val = 0x0BADF00D;
    uc_mem_write(uc0, ram_base + 0x100, &val, 4);
    uc_mem_write(uc1, c1_ram_base + 0x200, &c1_val, 4);

    uint32_t r0_val = 0x12345678;
    uint32_t pc_val = 0x10000040;
    uc_reg_write(uc0, UC_ARM_REG_R0, &r0_val);
    uc_reg_write(uc0, UC_ARM_REG_PC, &pc_val);

    const std::string state_path = "/tmp/test_zeebo_state.zbst";

    std::cout << "[Test] Saving state..." << std::endl;
    bool saved = ZeeboSaveStateManager::save_state(state_path, uc0, 1000, 0x10000000, uc1, 500, 0x00A00000);
    assert(saved);

    // Mutate state
    uint32_t zero = 0;
    uc_reg_write(uc0, UC_ARM_REG_R0, &zero);
    uc_mem_write(uc0, ram_base + 0x100, &zero, 4);
    uc_mem_write(uc1, c1_ram_base + 0x200, &zero, 4);

    std::cout << "[Test] Restoring state..." << std::endl;
    uint64_t c0_insns = 0, c1_insns = 0;
    uint32_t c0_entry = 0, c1_entry = 0;
    bool loaded = ZeeboSaveStateManager::load_state(state_path, uc0, c0_insns, c0_entry, uc1, c1_insns, c1_entry);
    assert(loaded);

    assert(c0_insns == 1000);
    assert(c1_insns == 500);

    uint32_t restored_r0 = 0;
    uc_reg_read(uc0, UC_ARM_REG_R0, &restored_r0);
    assert(restored_r0 == r0_val);

    uint32_t restored_val = 0;
    uc_mem_read(uc0, ram_base + 0x100, &restored_val, 4);
    assert(restored_val == val);
    uint32_t restored_c1_val = 0;
    uc_mem_read(uc1, c1_ram_base + 0x200, &restored_c1_val, 4);
    assert(restored_c1_val == c1_val);

    // Corrupted context size and truncated files must fail before restore/OOB.
    const std::string bad_path = "/tmp/test_zeebo_state_bad.zbst";
    {
        std::ifstream src(state_path, std::ios::binary);
        std::ofstream dst(bad_path, std::ios::binary | std::ios::trunc);
        dst << src.rdbuf();
    }
    {
        std::fstream bad(bad_path, std::ios::binary | std::ios::in | std::ios::out);
        ZeeboLLEStateHeader hdr{};
        bad.read(reinterpret_cast<char*>(&hdr), sizeof(hdr));
        hdr.c0_context_size = 1;
        bad.seekp(0);
        bad.write(reinterpret_cast<const char*>(&hdr), sizeof(hdr));
    }
    assert(!ZeeboSaveStateManager::load_state(bad_path, uc0, c0_insns, c0_entry,
                                              uc1, c1_insns, c1_entry));

    std::cout << "[Test] Save state verified successfully! Restored R0=" << std::hex << restored_r0 
              << " RAM=" << restored_val << std::dec << std::endl;

    uc_close(uc0);
    uc_close(uc1);
    return 0;
}
