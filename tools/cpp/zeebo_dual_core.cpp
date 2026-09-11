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
#ifndef ZEEBO_VIC_WITH_UNICORN
#define ZEEBO_VIC_WITH_UNICORN
#endif
#include "zeebo_peripheral_bus.h"

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

// Hook for Core 0 (L4e Syscall Engine: MAP_CONTROL svc #0x14)
static void core0_code_hook(uc_engine* uc, uint64_t ad, uint32_t size, void* ud) {
    CoreState* cs = (CoreState*)ud;
    cs->insns++;
}

static void core0_intr_hook(uc_engine* uc, uint32_t intno, void* ud) {
    if (intno == 2) { // ARM SWI/SVC
        u32 pc = 0, sp = 0, lr = 0, r12 = 0;
        uc_reg_read(uc, UC_ARM_REG_PC, &pc);
        uc_reg_read(uc, UC_ARM_REG_SP, &sp);
        uc_reg_read(uc, UC_ARM_REG_LR, &lr);
        uc_reg_read(uc, UC_ARM_REG_R12, &r12);
        u32 op = 0;
        uc_mem_read(uc, (pc - 4) & ~3u, &op, 4);
        u32 svc_num = op & 0x00FFFFFF;
        if (svc_num == 0x14) { // MAP_CONTROL
            u32 success = 0;
            uc_reg_write(uc, UC_ARM_REG_R0, &success);
            if (r12) uc_reg_write(uc, UC_ARM_REG_SP, &r12);
            if (lr)  uc_reg_write(uc, UC_ARM_REG_PC, &lr);
        }
    }
}

// Hook for Core 0 (L4e microkernel MMIO)
static CoreState* g_core1_state = nullptr;

static void core0_unmapped_hook(uc_engine* uc, uc_mem_type type, uint64_t addr, int size, int64_t value, void* ud) {
    u32 pc = 0;
    uc_reg_read(uc, UC_ARM_REG_PC, &pc);
    printf("[Core0 Unmapped] %s at 0x%08llx (size %d, val 0x%llx) at pc=0x%08x\n",
           type == UC_MEM_WRITE_UNMAPPED ? "WRITE" : "READ",
           (unsigned long long)addr, size, (unsigned long long)value, pc);
    // Map dynamically to continue discovery
    uc_mem_map(uc, addr & ~0xFFFULL, 0x1000, UC_PROT_ALL);
}

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
// ---- QW99 Bug 5: production peripheral decoder + deterministic virtual time ----
// The guest's VIC/GPT MMIO is decoded through PeripheralBus so ENABLE/MATCH/
// INTENABLE/ACK/EOI mutate the real models instead of flat RAM. Virtual time is
// advanced by a FIXED quantum per scheduler slice (see the slice loop), NOT by
// Core0.insns+Core1.insns, so the timer is deterministic and independent of how
// many instructions either core happened to retire.
static zeebo::PeripheralBus g_pbus;
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
    (void)size; (void)ud;
    u32 a = (u32)addr;
    // Route real VIC/GPT MMIO through the production decoder (Bug 5). Writes to
    // ENABLE/MATCH/INTENABLE/ACK/EOI mutate the models; reads reflect them back
    // into the flat cell the load returns.
    if (type == UC_MEM_WRITE) {
        g_pbus.mmio_write(a, (u32)value);
        return;
    }
    if (type == UC_MEM_READ) {
        u32 out = 0;
        if (g_pbus.mmio_read(a, &out)) {
            uc_mem_write(uc, a, &out, 4);
            return;
        }
    }
    // Legacy DGT-style ticker cell (non-primary-source address 0xc5000108, kept
    // for the discovery harness's older polling path).
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
    const char* apps_path = argc > 2 ? argv[2] : "../../nand/1.1.2_APPS.bin";

    printf("[DualCore] AMSS image: %s\n", amss_path);
    printf("[DualCore] APPS image: %s\n", apps_path);

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
    uc_mem_map(core0.uc, 0xc0000000, 0x00010000, UC_PROT_ALL); // VIC
    uc_mem_map(core1.uc, 0xc0000000, 0x00010000, UC_PROT_ALL); // VIC
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

    // Setup memory for Core 0 (Real MSM7201A APPS: L4e + Iguana + BREW)
    // Physical RAM window (0x10000000..0x16000000 = 96MB covering all 14 segments up to 0x14953040)
    uc_mem_map(core0.uc, 0x10000000, 0x06000000, UC_PROT_ALL);
    // Virtual MMU mappings for L4e Kernel (0xf0000000) and Iguana User Space (0xb0000000)
    uc_mem_map(core0.uc, 0xf0000000, 0x01000000, UC_PROT_ALL);
    uc_mem_map(core0.uc, 0xb0000000, 0x02000000, UC_PROT_ALL);
    uc_mem_map(core0.uc, 0x00000000, 0x00100000, UC_PROT_ALL); // Exception vectors

    // Load Real APPS ELF into Core 0
    FILE* f_apps = fopen(apps_path, "rb");
    if (!f_apps) {
        printf("[Fatal] Cannot open APPS ELF: %s\n", apps_path);
        return 1;
    }
    fseek(f_apps, 0, SEEK_END); long sz_k = ftell(f_apps); fseek(f_apps, 0, SEEK_SET);
    std::vector<u8> dk(sz_k); fread(dk.data(), 1, sz_k, f_apps); fclose(f_apps);
    core0.entry = rd32(dk.data(), 24); // 0x10000000
    u32 phoff_k = rd32(dk.data(), 28);
    u16 phent_k = rd16(dk.data(), 42), phnum_k = rd16(dk.data(), 44);
    printf("[DualCore] APPS ELF Entrypoint: 0x%08x, Segments: %u\n", core0.entry, phnum_k);

    for (int i = 0; i < phnum_k; i++) {
        size_t o = phoff_k + i * phent_k;
        if (rd32(dk.data(), o) != 1) continue;
        u32 pv = rd32(dk.data(), o+8), off = rd32(dk.data(), o+4);
        u32 paddr = rd32(dk.data(), o+12);
        u32 fs = rd32(dk.data(), o+16), ms = rd32(dk.data(), o+20);
        u32 nmem = ms ? ms : fs; if (!nmem) continue;

        std::vector<u8> seg(nmem, 0);
        if (fs) { size_t cl = std::min((size_t)fs, seg.size()); memcpy(seg.data(), dk.data()+off, cl); }

        // Map and write to virtual address
        uc_mem_map(core0.uc, pv & ~0xFFFu, ((nmem+0xFFF)&~0xFFFu)+0x1000, UC_PROT_ALL);
        uc_mem_write(core0.uc, pv, seg.data(), seg.size());

        // Map and write to physical address if different
        if (paddr && paddr != pv) {
            uc_mem_map(core0.uc, paddr & ~0xFFFu, ((nmem+0xFFF)&~0xFFFu)+0x1000, UC_PROT_ALL);
            uc_mem_write(core0.uc, paddr, seg.data(), seg.size());
        }
    }

    // Setup memory for Core 1 (AMSS)
    map_amss_common(core1.uc);
    uc_mem_map(core1.uc, 0x16e00000, 0x17a60000-0x16e00000, UC_PROT_ALL);
    uc_mem_map(core1.uc, 0x20000000, 0x1000000, UC_PROT_ALL);
    u32 t_init = 100000;
    uc_mem_write(core1.uc, 0xc5000108, &t_init, 4);

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
    uc_hook h_c0, h_m0, h_u0, h_i0, h_c1, h_m1;
    uc_hook_add(core0.uc, &h_c0, UC_HOOK_CODE, (void*)core0_code_hook, &core0, 0, ~0ULL);
    uc_hook_add(core0.uc, &h_m0, UC_HOOK_MEM_WRITE, (void*)core0_mem_hook, &core0, 0, ~0ULL);
    uc_hook_add(core0.uc, &h_u0, UC_HOOK_MEM_UNMAPPED, (void*)core0_unmapped_hook, &core0, 1, 0);
    uc_hook_add(core0.uc, &h_i0, UC_HOOK_INTR, (void*)core0_intr_hook, &core0, 1, 0);
    uc_hook_add(core1.uc, &h_c1, UC_HOOK_CODE, (void*)core1_code_hook, &core1, 0, ~0ULL);
    uc_hook_add(core1.uc, &h_m1, UC_HOOK_MEM_READ | UC_HOOK_MEM_WRITE, (void*)core1_mem_hook, &core1, 0, ~0ULL);

    printf("[DualCore] Booting Core 0 (OKL4 @0x%08x) and Core 1 (AMSS @0x%08x)...\n", core0.entry, core1.entry);

    // Interleaved execution slices
    const int SLICE_INSNS = 10000;
    const int TOTAL_CYCLES = 60;
    // QW99 Bug 5: deterministic virtual time. Advance the peripheral clock by a
    // FIXED quantum per scheduler cycle, independent of Core0/Core1 retired
    // instruction counts, so the GPT is reproducible run-to-run.
    g_pbus.ticks_per_slice = 5000; // model quantum per cycle (calibrated placeholder)
    for (int cycle = 0; cycle < TOTAL_CYCLES; cycle++) {
        // Step Core 0
        uc_err e0 = uc_emu_start(core0.uc, core0.entry, 0, 0, SLICE_INSNS);
        core0.entry = rreg(core0.uc, UC_ARM_REG_PC);

        // Step Core 1
        uc_err e1 = uc_emu_start(core1.uc, core1.entry, 0, 0, SLICE_INSNS);
        core1.entry = rreg(core1.uc, UC_ARM_REG_PC);

        // Advance deterministic virtual time and drive the GPT -> VIC line.
        bool fired = g_pbus.tick_slice();
        // Between slices (engine quiescent): deliver at most one non-reentrant
        // IRQ into Core 1 (the modem VIC path). in_service prevents redelivery /
        // LR_irq/SPSR_irq overwrite before the guest EOIs.
        bool delivered = g_pbus.deliver_irq(core1.uc, /*vector_base=*/0x0);
        if (delivered) core1.entry = rreg(core1.uc, UC_ARM_REG_PC);

        printf("  [Cycle %02d] Core0: pc=0x%08x insns=%llu (%s) | Core1: pc=0x%08x insns=%llu (%s) | vt=%llu gpt=%s irq=%s\n",
               cycle, core0.entry, (unsigned long long)core0.insns, e0 ? uc_strerror(e0) : "ok",
               core1.entry, (unsigned long long)core1.insns, e1 ? uc_strerror(e1) : "ok",
               (unsigned long long)g_pbus.virtual_ticks, fired ? "FIRE" : "-",
               delivered ? "DELIVERED" : "-");
    }

    printf("[DualCore] Completed dual-core concurrent execution run.\n");

    uc_close(core0.uc);
    uc_close(core1.uc);
    return 0;
}

