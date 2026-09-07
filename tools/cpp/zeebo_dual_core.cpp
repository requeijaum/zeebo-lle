// zeebo_dual_core.cpp — Dual-Core Orchestrator for Zeebo (MSM7201A) LLE:
//   Core 0: ARM1176JZ (Applications Processor / OKL4 L4e Kernel + Iguana / BREW)
//   Core 1: ARM926EJ-S (Modem / Baseband Processor / AMSS + REX RTOS)
// Shared Resources:
//   SMEM (Shared Memory): 0x01F00000 (2MB window, proc_comm + SMD channels)
//   A2M / M2A Inter-Core Interrupts: 0xC0100400 (MSM_CSR_BASE + 0x400)
//   Hardware Timer / GPT: 0xC5000100
#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <cstring>
#include <vector>
#include <string>
#include <algorithm>
#include <unicorn/unicorn.h>

using u8  = uint8_t;
using u16 = uint16_t;
using u32 = uint32_t;
using u64 = uint64_t;

// ---- SMEM Memory Layout (from openzeebo zloader / linux msm7201a) ----
enum {
    SMEM_BASE           = 0x01f00000,
    SMEM_SIZE           = 0x00200000, // 2MB
    MSM_CSR_BASE        = 0xc0100000,
    MSM_CSR_SIZE        = 0x00010000, // 64KB
    GPT_TIMER_BASE      = 0xc5000000,
    GPT_TIMER_SIZE      = 0x01000000, // 16MB
    PLEB_RAM            = 0xa0000000,
    XSCALE_DEV          = 0x40000000,
};

// ProcComm registers within SMEM (0x01f00000)
enum {
    APP_COMMAND         = 0x00,
    APP_STATUS          = 0x04,
    APP_DATA1           = 0x08,
    APP_DATA2           = 0x0c,
    MDM_COMMAND         = 0x10,
    MDM_STATUS          = 0x14,
    MDM_DATA1           = 0x18,
    MDM_DATA2           = 0x1c,
};

struct smem_proc_comm {
    u32 command;
    u32 status;
    u32 data1;
    u32 data2;
};

struct smem_shared {
    struct smem_proc_comm proc_comm[4];
    u32 version[32];
};

static u32 rd32(const u8* d, size_t o) {
    return (u32)d[o] | ((u32)d[o+1]<<8) | ((u32)d[o+2]<<16) | ((u32)d[o+3]<<24);
}
static u16 rd16(const u8* d, size_t o) {
    return (u16)d[o] | ((u16)d[o+1]<<8);
}

// Page translation tables for AMSS (ARM9)
static u32 g_arm11_sec[4096];
static void build_arm11_map(){
    for(u32 i=0;i<4096;i++) g_arm11_sec[i]=i<<20;
    auto C=[&](u32 v,u32 p){for(u32 k=0;k<16;k++) g_arm11_sec[(v>>20)+k]=(p&~0xFFFFF)+(k<<20);};
    C(0x00200000,0x10000000);C(0x00b00000,0x100a0000);C(0x00c00000,0x100b0000);
    C(0x00d00000,0x100c0000);C(0x01100000,0x100e0000);C(0x01200000,0x100f0000);
    C(0x10100000,0x100ad400);C(0x11400000,0x100adc00);
}
static u32 va2pa(u32 va){
    u32 idx = va >> 20;
    return (g_arm11_sec[idx] & ~0xFFFFF) | (va & 0xFFFFF);
}

static void map_amss_common(uc_engine* uc){
    uc_mem_map(uc,0x00000000,0x00800000,UC_PROT_ALL);
    uc_mem_map(uc,0x00a00000,0x00600000,UC_PROT_ALL);
    uc_mem_map(uc,0x01000000,0x01000000,UC_PROT_ALL);
    uc_mem_map(uc,0x00c00000,0x00400000,UC_PROT_ALL);
    uc_mem_map(uc,0xff000000,0x00400000,UC_PROT_ALL);
    for(u32 bx : {0xb0000000u,0xc0000000u,0xa0a00000u,0xaa600000u,0xa9700000u,0xa9400000u,0xa9a00000u,0xa9200000u})
        uc_mem_map(uc,bx,0x10000,UC_PROT_ALL);
}

struct CoreState {
    const char* name;
    uc_engine* uc = nullptr;
    u32 entry = 0;
    u64 insns = 0;
    bool halted = false;
};

static u32 rreg(uc_engine* uc, int r) {
    u32 v = 0;
    uc_reg_read(uc, r, &v);
    return v;
}

// Hook for Core 0 (L4e microkernel)
static void core0_code_hook(uc_engine* uc, uint64_t ad, uint32_t size, void* ud) {
    CoreState* cs = (CoreState*)ud;
    cs->insns++;
    // Trap at 0xf001d060 is benign halt/idle loop
}

// Hook for Core 0 (L4e microkernel MMIO)
static CoreState* g_core1_state = nullptr;

static void core0_mem_hook(uc_engine* uc, uc_mem_type type, uint64_t addr, int size, int64_t value, void* ud) {
    // Inter-core doorbell: Core 0 writes to MSM_A2M_INT(n) at 0xC0100400 + n*4
    if (addr >= MSM_CSR_BASE + 0x400 && addr <= MSM_CSR_BASE + 0x440 && type == UC_MEM_WRITE) {
        u32 int_num = (addr - (MSM_CSR_BASE + 0x400)) / 4;
        printf("[Doorbell A2M] Core 0 fired interrupt #%u (val=0x%llx) to Core 1!\n", int_num, (unsigned long long)value);
        
        // Connect to ARM9 VIC: Assert INT_A9_M2A_n on Core 1 VIC status register
        if (g_core1_state && g_core1_state->uc) {
            u32 vic_status0 = 0;
            uc_mem_read(g_core1_state->uc, 0xc0000000, &vic_status0, 4); // VIC_IRQ_STATUS0
            vic_status0 |= (1 << int_num);
            uc_mem_write(g_core1_state->uc, 0xc0000000, &vic_status0, 4);
            printf("[VIC Routing] Core 1 VIC_IRQ_STATUS0 updated to 0x%08x\n", vic_status0);
        }
    }
}

// Hook for Core 1 (AMSS)
static void core1_code_hook(uc_engine* uc, uint64_t ad, uint32_t size, void* ud) {
    CoreState* cs = (CoreState*)ud;
    cs->insns++;
    // Bypass L4e panic vector at 0x00d10588 if encountered
    if (ad == 0x00d10588) {
        u32 lr = rreg(uc, UC_ARM_REG_LR);
        u32 new_pc = lr & ~1;
        uc_reg_write(uc, UC_ARM_REG_PC, &new_pc);
        u32 cpsr = rreg(uc, UC_ARM_REG_CPSR);
        if (lr & 1) cpsr |= (1 << 5); else cpsr &= ~(1 << 5);
        uc_reg_write(uc, UC_ARM_REG_CPSR, &cpsr);
    }
}

// Hook for Core 1 (AMSS MMIO)
static void core1_mem_hook(uc_engine* uc, uc_mem_type type, uint64_t addr, int size, int64_t value, void* ud) {
    if (addr == 0xc5000108 && type == UC_MEM_READ) {
        static u32 virtual_ticker = 100000;
        virtual_ticker += 5000;
        uc_mem_write(uc, 0xc5000108, &virtual_ticker, 4);
    }
}

int main(int argc, char** argv) {
    printf("===================================================================\n");
    printf("  ZEEBO DUAL-CORE HARNESS: ARM11 (Apps/L4e) + ARM9 (Modem/AMSS)    \n");
    printf("===================================================================\n");

    const char* amss_path = argc > 1 ? argv[1] : "../../nand/1.1.2_AMSS.bin";
    const char* okl4_path = argc > 2 ? argv[2] : "../../refs/okl4-arm-build/arm-kernel.elf";

    printf("[DualCore] AMSS image: %s\n", amss_path);
    printf("[DualCore] L4e kernel: %s\n", okl4_path);

    build_arm11_map();

    // Initialize Core 0 (ARM1176JZ — Applications Processor)
    CoreState core0;
    core0.name = "ARM11-Apps";
    uc_err err0 = uc_open(UC_ARCH_ARM, UC_MODE_ARM, &core0.uc);
    if (err0 != UC_ERR_OK) {
        printf("[Fatal] Failed to initialize ARM11 core: %s\n", uc_strerror(err0));
        return 1;
    }
    uc_ctl_set_cpu_model(core0.uc, UC_CPU_ARM_1176);

    // Initialize Core 1 (ARM926EJ-S — Modem / Baseband Processor)
    CoreState core1;
    core1.name = "ARM9-Modem";
    uc_err err1 = uc_open(UC_ARCH_ARM, UC_MODE_ARM, &core1.uc);
    if (err1 != UC_ERR_OK) {
        printf("[Fatal] Failed to initialize ARM9 core: %s\n", uc_strerror(err1));
        return 1;
    }
    uc_ctl_set_cpu_model(core1.uc, UC_CPU_ARM_926);
    g_core1_state = &core1;

    // Shared regions
    uc_mem_map(core0.uc, SMEM_BASE, SMEM_SIZE, UC_PROT_ALL);
    uc_mem_map(core1.uc, SMEM_BASE, SMEM_SIZE, UC_PROT_ALL);
    uc_mem_map(core0.uc, MSM_CSR_BASE, MSM_CSR_SIZE, UC_PROT_ALL);
    uc_mem_map(core1.uc, MSM_CSR_BASE, MSM_CSR_SIZE, UC_PROT_ALL);
    uc_mem_map(core0.uc, GPT_TIMER_BASE, GPT_TIMER_SIZE, UC_PROT_ALL);
    uc_mem_map(core1.uc, GPT_TIMER_BASE, GPT_TIMER_SIZE, UC_PROT_ALL);

    // Initialize SMEM shared structure
    struct smem_shared s_init;
    std::memset(&s_init, 0, sizeof(s_init));
    // Set modem and apps versions in SMEM
    s_init.version[8] = 0x00010001; // Apps version 1.1
    s_init.version[9] = 0x00010002; // Modem version 1.2
    uc_mem_write(core0.uc, SMEM_BASE, &s_init, sizeof(s_init));
    uc_mem_write(core1.uc, SMEM_BASE, &s_init, sizeof(s_init));

    // Setup memory for Core 0 (OKL4 L4e)
    uc_mem_map(core0.uc, PLEB_RAM, 0x2000000, UC_PROT_ALL);
    uc_mem_map(core0.uc, 0xfd000000, 0x100000, UC_PROT_ALL);
    uc_mem_map(core0.uc, 0x00000000, 0x10000, UC_PROT_ALL);
    uc_mem_map(core0.uc, 0xe0000000, 0x01000000, UC_PROT_ALL);
    uc_mem_map(core0.uc, XSCALE_DEV, 0x2000000, UC_PROT_ALL);
    uc_mem_map(core0.uc, 0x41000000, 0x1000000, UC_PROT_ALL);
    uc_mem_map(core0.uc, 0xe1000000, 0x0f000000, UC_PROT_ALL);

    // Setup memory for Core 1 (AMSS)
    map_amss_common(core1.uc);
    uc_mem_map(core1.uc, 0x16e00000, 0x17a60000-0x16e00000, UC_PROT_ALL);
    uc_mem_map(core1.uc, 0x20000000, 0x1000000, UC_PROT_ALL);
    u32 t_init = 100000;
    uc_mem_write(core1.uc, 0xc5000108, &t_init, 4);

    // Load OKL4 Kernel ELF into Core 0
    FILE* f_okl4 = fopen(okl4_path, "rb");
    if (!f_okl4) {
        printf("[Fatal] Cannot open OKL4 kernel ELF: %s\n", okl4_path);
        return 1;
    }
    fseek(f_okl4, 0, SEEK_END); long sz_k = ftell(f_okl4); fseek(f_okl4, 0, SEEK_SET);
    std::vector<u8> dk(sz_k); fread(dk.data(), 1, sz_k, f_okl4); fclose(f_okl4);
    core0.entry = rd32(dk.data(), 24);
    u32 phoff_k = rd32(dk.data(), 28);
    u16 phent_k = rd16(dk.data(), 42), phnum_k = rd16(dk.data(), 44);
    for (int i = 0; i < phnum_k; i++) {
        size_t o = phoff_k + i * phent_k;
        if (rd32(dk.data(), o) != 1) continue;
        u32 pv = rd32(dk.data(), o+8), off = rd32(dk.data(), o+4);
        u32 fs = rd32(dk.data(), o+16), ms = rd32(dk.data(), o+20);
        u32 nmem = ms ? ms : fs; if (!nmem) continue;
        if (uc_mem_map(core0.uc, pv & ~0xFFFu, ((nmem+0xFFF)&~0xFFFu)+0x1000, UC_PROT_ALL) != UC_ERR_OK) {}
        std::vector<u8> seg(nmem, 0);
        if (fs) { size_t cl = std::min((size_t)fs, seg.size()); memcpy(seg.data(), dk.data()+off, cl); }
        uc_mem_write(core0.uc, pv, seg.data(), seg.size());
        u32 paddr = rd32(dk.data(), o+12);
        if (paddr) uc_mem_write(core0.uc, paddr, seg.data(), seg.size());
    }
    u16 zero16 = 0;
    uc_mem_write(core0.uc, 0xa010d054, &zero16, 2);
    uc_mem_write(core0.uc, 0xf000d054, &zero16, 2);
    for (int i = 0; i < 16; ++i) {
        u32 desc = (0xa1000000 + i * 0x100000) | 0xc12;
        uc_mem_write(core0.uc, 0xa0110000 + (0xe00 + i) * 4, &desc, 4);
    }

    // Load AMSS ELF into Core 1
    FILE* f_amss = fopen(amss_path, "rb");
    if (!f_amss) {
        printf("[Fatal] Cannot open AMSS file: %s\n", amss_path);
        return 1;
    }
    fseek(f_amss, 0, SEEK_END); long sz_a = ftell(f_amss); fseek(f_amss, 0, SEEK_SET);
    std::vector<u8> da(sz_a); fread(da.data(), 1, sz_a, f_amss); fclose(f_amss);
    core1.entry = rd32(da.data(), 24);
    u32 phoff_a = rd32(da.data(), 28);
    u16 phent_a = rd16(da.data(), 42), phnum_a = rd16(da.data(), 44);
    for (int i = 0; i < phnum_a; i++) {
        size_t o = phoff_a + i * phent_a;
        if (rd32(da.data(), o) != 1) continue;
        u32 pv = rd32(da.data(), o+8), off = rd32(da.data(), o+4);
        u32 fs = rd32(da.data(), o+16), ms = rd32(da.data(), o+20);
        size_t nmem = ms ? ms : fs; if (!nmem) continue;
        u32 va = va2pa(pv);
        if (uc_mem_map(core1.uc, va & ~0xFFFu, ((nmem+0xFFF)&~0xFFFu)+0x1000, UC_PROT_ALL) != UC_ERR_OK) {}
        std::vector<u8> seg(nmem, 0);
        if (fs) { size_t cl = std::min((size_t)fs, seg.size()); memcpy(seg.data(), da.data()+off, cl); }
        uc_mem_write(core1.uc, va, seg.data(), seg.size());
    }

    // Register hooks
    uc_hook h_c0, h_m0, h_c1, h_m1;
    uc_hook_add(core0.uc, &h_c0, UC_HOOK_CODE, (void*)core0_code_hook, &core0, 0, ~0ULL);
    uc_hook_add(core0.uc, &h_m0, UC_HOOK_MEM_WRITE, (void*)core0_mem_hook, &core0, 0, ~0ULL);
    uc_hook_add(core1.uc, &h_c1, UC_HOOK_CODE, (void*)core1_code_hook, &core1, 0, ~0ULL);
    uc_hook_add(core1.uc, &h_m1, UC_HOOK_MEM_READ | UC_HOOK_MEM_WRITE, (void*)core1_mem_hook, &core1, 0, ~0ULL);

    printf("[DualCore] Booting Core 0 (OKL4 @0x%08x) and Core 1 (AMSS @0x%08x)...\n", core0.entry, core1.entry);

    // Interleaved execution slices
    const int SLICE_INSNS = 10000;
    const int TOTAL_CYCLES = 10;
    for (int cycle = 0; cycle < TOTAL_CYCLES; cycle++) {
        // Step Core 0
        uc_err e0 = uc_emu_start(core0.uc, core0.entry, 0, 0, SLICE_INSNS);
        core0.entry = rreg(core0.uc, UC_ARM_REG_PC);

        // Step Core 1
        uc_err e1 = uc_emu_start(core1.uc, core1.entry, 0, 0, SLICE_INSNS);
        core1.entry = rreg(core1.uc, UC_ARM_REG_PC);

        printf("  [Cycle %02d] Core0: pc=0x%08x insns=%llu (%s) | Core1: pc=0x%08x insns=%llu (%s)\n",
               cycle, core0.entry, (unsigned long long)core0.insns, e0 ? uc_strerror(e0) : "ok",
               core1.entry, (unsigned long long)core1.insns, e1 ? uc_strerror(e1) : "ok");
    }

    printf("[DualCore] Completed dual-core concurrent execution run.\n");

    uc_close(core0.uc);
    uc_close(core1.uc);
    return 0;
}

