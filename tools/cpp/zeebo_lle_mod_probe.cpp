// zeebo_lle_mod_probe.cpp
// LLE conformance probe for Zeebo ARM11 / ARMv6 core using Unicorn.
// Mirrors zeebulator_mod_probe API:
//   usage: zeebo_lle_mod_probe <file.bin> [num_instructions] [base_addr_hex]
// Emits output format:
//   pc=0x%08x instr=0x%08x  -> r0=%08x r1=%08x sp=%08x lr=%08x cpsr=%08x

#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <vector>
#include <fstream>
#include <iterator>
#include <unicorn/unicorn.h>

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: %s <file.bin> [num_instructions] [base_addr_hex]\n", argv[0]);
        return 1;
    }

    int count = argc >= 3 ? std::atoi(argv[2]) : 40;
    uint32_t base = argc >= 4 ? static_cast<uint32_t>(std::strtoul(argv[3], nullptr, 16)) : 0x00100000;

    std::ifstream in(argv[1], std::ios::binary);
    if (!in) {
        std::fprintf(stderr, "error: couldn't open '%s'\n", argv[1]);
        return 1;
    }

    std::vector<uint8_t> data((std::istreambuf_iterator<char>(in)),
                               std::istreambuf_iterator<char>());
    std::printf("loaded %zu bytes, base=0x%08x\n", data.size(), base);

    uc_engine* uc = nullptr;
    uc_err err = uc_open(UC_ARCH_ARM, UC_MODE_ARM, &uc);
    if (err != UC_ERR_OK) {
        std::fprintf(stderr, "uc_open failed: %s\n", uc_strerror(err));
        return 1;
    }

    // Set CPU model to ARM1176 (Zeebo ARM11 core)
    uc_ctl_set_cpu_model(uc, UC_CPU_ARM_1136);

    // Map memory: 4MB at base for code/data, and 4MB at 0x00200000..0x00400000 for stack
    uint32_t map_base = base & ~0xFFFFFu;
    uint32_t map_size = 0x00400000; // 4MB
    uc_mem_map(uc, map_base, map_size, UC_PROT_ALL);

    // Also map zero page for vector/null tests if needed
    uc_mem_map(uc, 0x00000000, 0x00100000, UC_PROT_ALL);

    // Write binary to base address
    uc_mem_write(uc, base, data.data(), data.size());

    // Setup initial registers
    uint32_t pc = base;
    uint32_t sp = base + 0x00200000; // stack
    uint32_t zero = 0;

    uc_reg_write(uc, UC_ARM_REG_PC, &pc);
    uc_reg_write(uc, UC_ARM_REG_SP, &sp);
    uc_reg_write(uc, UC_ARM_REG_LR, &zero);
    uc_reg_write(uc, UC_ARM_REG_R0, &zero);
    uc_reg_write(uc, UC_ARM_REG_R1, &zero);
    uc_reg_write(uc, UC_ARM_REG_R2, &zero);
    uc_reg_write(uc, UC_ARM_REG_R3, &zero);
    uc_reg_write(uc, UC_ARM_REG_R4, &zero);
    uc_reg_write(uc, UC_ARM_REG_R5, &zero);
    uc_reg_write(uc, UC_ARM_REG_R6, &zero);
    uc_reg_write(uc, UC_ARM_REG_R7, &zero);
    uc_reg_write(uc, UC_ARM_REG_R8, &zero);
    uc_reg_write(uc, UC_ARM_REG_R9, &zero);
    uc_reg_write(uc, UC_ARM_REG_R10, &zero);
    uc_reg_write(uc, UC_ARM_REG_R11, &zero);
    uc_reg_write(uc, UC_ARM_REG_R12, &zero);
    uc_reg_write(uc, UC_ARM_REG_CPSR, &zero);

    for (int i = 0; i < count; ++i) {
        uint32_t cur_pc = 0, cpsr = 0;
        uc_reg_read(uc, UC_ARM_REG_PC, &cur_pc);
        uc_reg_read(uc, UC_ARM_REG_CPSR, &cpsr);

        uint32_t instr = 0;
        bool is_thumb = (cpsr & (1 << 5)) != 0;
        if (is_thumb) {
            uint16_t t_op = 0;
            uc_mem_read(uc, cur_pc & ~1, &t_op, 2);
            instr = t_op;
        } else {
            uc_mem_read(uc, cur_pc, &instr, 4);
        }

        std::printf("[%3d] pc=0x%08x instr=0x%08x  ", i, cur_pc, instr);

        // Single step 1 instruction
        err = uc_emu_start(uc, cur_pc | (is_thumb ? 1 : 0), 0, 0, 1);
        if (err != UC_ERR_OK) {
            std::printf("STOPPED: %s\n", uc_strerror(err));
            break;
        }

        uint32_t r0 = 0, r1 = 0, r_sp = 0, lr = 0, new_cpsr = 0;
        uc_reg_read(uc, UC_ARM_REG_R0, &r0);
        uc_reg_read(uc, UC_ARM_REG_R1, &r1);
        uc_reg_read(uc, UC_ARM_REG_SP, &r_sp);
        uc_reg_read(uc, UC_ARM_REG_LR, &lr);
        uc_reg_read(uc, UC_ARM_REG_CPSR, &new_cpsr);

        // Mask out mode bits (0x1f) in cpsr so it matches mod_probe comparison if needed, or print cpsr & ~0x1f
        uint32_t norm_cpsr = new_cpsr & ~0x1fu;

        std::printf("-> r0=%08x r1=%08x sp=%08x lr=%08x cpsr=%08x\n",
                    r0, r1, r_sp, lr, norm_cpsr);
    }

    uc_close(uc);
    return 0;
}
