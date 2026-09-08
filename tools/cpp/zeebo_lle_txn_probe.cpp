// zeebo_lle_txn_probe.cpp
// QW6: Transactional conformance probe for the Zeebo ARM11 / ARMv6 core.
//
// Unlike zeebo_lle_mod_probe (which only reports FINAL register state), this
// probe records the ORDERED bus-transaction trace: instruction prefetch (P),
// data load (L) and data store (S), each with width, address and value. It
// also prints the final architectural state (regs + CPSR) so a golden file
// pins BOTH the transaction ordering and the resulting state.
//
// A mutated core -- wrong transaction order, address, width or value -- must
// diverge from the golden even when the final PC / instruction count match.
//
// usage: zeebo_lle_txn_probe <file.bin> [num_instructions] [base_addr_hex]
//
// Output (one line per transaction, then a FINAL line):
//   T%04d P w=%d @0x%08x =0x%08x thumb=%d
//   T%04d L w=%d @0x%08x =0x%08x
//   T%04d S w=%d @0x%08x =0x%08x
//   FINAL r0=.. r1=.. r2=.. r3=.. r4=.. r5=.. r6=.. r7=.. sp=.. lr=.. pc=.. cpsr=..

#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <vector>
#include <fstream>
#include <iterator>
#include <unicorn/unicorn.h>

struct Ctx {
    uc_engine* uc;
    int seq = 0;
    long limit = 0;      // stop emitting after this many transactions (safety)
};

static void hook_code(uc_engine* uc, uint64_t address, uint32_t size, void* user) {
    Ctx* c = static_cast<Ctx*>(user);
    uint32_t cpsr = 0;
    uc_reg_read(uc, UC_ARM_REG_CPSR, &cpsr);
    int thumb = (cpsr & (1u << 5)) != 0 ? 1 : 0;
    uint32_t instr = 0;
    // size reflects the fetched instruction width (2 for Thumb, 4 for ARM).
    uc_mem_read(uc, address, &instr, size <= 4 ? size : 4);
    std::printf("T%04d P w=%u @0x%08x =0x%08x thumb=%d\n",
                c->seq++, size, (uint32_t)address, instr, thumb);
}

static void hook_mem(uc_engine* uc, uc_mem_type type, uint64_t address,
                     int size, int64_t value, void* user) {
    (void)uc;
    Ctx* c = static_cast<Ctx*>(user);
    const char* k = (type == UC_MEM_WRITE) ? "S" : "L";
    std::printf("T%04d %s w=%d @0x%08x =0x%08x\n",
                c->seq++, k, size, (uint32_t)address, (uint32_t)value);
}

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: %s <file.bin> [num_instructions] [base_addr_hex]\n", argv[0]);
        return 1;
    }
    long count = argc >= 3 ? std::atol(argv[2]) : 64;
    uint32_t base = argc >= 4 ? (uint32_t)std::strtoul(argv[3], nullptr, 16) : 0x00100000;

    std::ifstream in(argv[1], std::ios::binary);
    if (!in) { std::fprintf(stderr, "error: couldn't open '%s'\n", argv[1]); return 1; }
    std::vector<uint8_t> data((std::istreambuf_iterator<char>(in)),
                               std::istreambuf_iterator<char>());

    uc_engine* uc = nullptr;
    uc_err err = uc_open(UC_ARCH_ARM, UC_MODE_ARM, &uc);
    if (err != UC_ERR_OK) { std::fprintf(stderr, "uc_open: %s\n", uc_strerror(err)); return 1; }
    uc_ctl_set_cpu_model(uc, UC_CPU_ARM_1176);

    uint32_t map_base = base & ~0xFFFFFu;
    uc_mem_map(uc, map_base, 0x00400000, UC_PROT_ALL);
    uc_mem_map(uc, 0x00000000, 0x00100000, UC_PROT_ALL);
    uc_mem_write(uc, base, data.data(), data.size());

    uint32_t pc = base, sp = base + 0x00200000, zero = 0;
    uc_reg_write(uc, UC_ARM_REG_PC, &pc);
    uc_reg_write(uc, UC_ARM_REG_SP, &sp);
    uc_reg_write(uc, UC_ARM_REG_LR, &zero);
    for (int r = UC_ARM_REG_R0; r <= UC_ARM_REG_R12; ++r)
        uc_reg_write(uc, r, &zero);
    uc_reg_write(uc, UC_ARM_REG_CPSR, &zero);

    Ctx ctx{uc, 0, count};
    uc_hook h_code, h_read, h_write;
    uc_hook_add(uc, &h_code, UC_HOOK_CODE, (void*)hook_code, &ctx, 1, 0);
    uc_hook_add(uc, &h_read, UC_HOOK_MEM_READ, (void*)hook_mem, &ctx, 1, 0);
    uc_hook_add(uc, &h_write, UC_HOOK_MEM_WRITE, (void*)hook_mem, &ctx, 1, 0);

    // Step instruction-by-instruction so we honor the instruction budget and
    // stop cleanly once parked in the spin loop.
    uint32_t last_pc = 0xFFFFFFFF; int spins = 0;
    for (long i = 0; i < count; ++i) {
        uint32_t cur = 0, cpsr = 0;
        uc_reg_read(uc, UC_ARM_REG_PC, &cur);
        uc_reg_read(uc, UC_ARM_REG_CPSR, &cpsr);
        int thumb = (cpsr & (1u << 5)) != 0;
        err = uc_emu_start(uc, cur | (thumb ? 1u : 0u), 0, 0, 1);
        if (err != UC_ERR_OK) { std::printf("STOPPED: %s\n", uc_strerror(err)); break; }
        uint32_t np = 0; uc_reg_read(uc, UC_ARM_REG_PC, &np);
        if (np == cur) { if (++spins >= 2) break; } else spins = 0;
        last_pc = np;
    }
    (void)last_pc;

    uint32_t r[13], rsp, lr, fpc, cpsr;
    for (int i = 0; i < 13; ++i) uc_reg_read(uc, UC_ARM_REG_R0 + i, &r[i]);
    uc_reg_read(uc, UC_ARM_REG_SP, &rsp);
    uc_reg_read(uc, UC_ARM_REG_LR, &lr);
    uc_reg_read(uc, UC_ARM_REG_PC, &fpc);
    uc_reg_read(uc, UC_ARM_REG_CPSR, &cpsr);
    std::printf("FINAL r0=%08x r1=%08x r2=%08x r3=%08x r4=%08x r5=%08x r6=%08x r7=%08x "
                "sp=%08x lr=%08x pc=%08x cpsr=%08x\n",
                r[0], r[1], r[2], r[3], r[4], r[5], r[6], r[7],
                rsp, lr, fpc, cpsr & ~0x1fu);

    uc_close(uc);
    return 0;
}
