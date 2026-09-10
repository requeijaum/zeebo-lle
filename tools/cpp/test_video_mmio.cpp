// test_video_mmio.cpp — prova que leituras de registrador de video chegam aos
// modelos MDDI/Adreno em vez de devolver a RAM de fundo.
//
// Parte 1: contrato dos modelos reais (zeebo_video_mmio.h, o mesmo cabecalho
//          incluido por zeebo_lle_main.cpp -- nao e uma copia).
// Parte 2: teste de ponta a ponta com Unicorn, executando codigo ARM que le
//          ADRENO_CHIP_ID atraves do mesmo padrao de hook do runtime.
//          Sem o roteamento de leitura, o guest le 0.

#include "zeebo_video_mmio.h"
#include <unicorn/unicorn.h>
#include <cstdio>
#include <cstring>
#include <cstdlib>

static int fails = 0;
static void check(bool ok, const char* what, unsigned long got, unsigned long want) {
    printf("[%s] %s (obtido 0x%lx, esperado 0x%lx)\n", ok ? "PASS" : "FAIL", what, got, want);
    if (!ok) fails++;
}

// ---- Parte 1: modelos ----------------------------------------------------
static void test_models() {
    UnifiedAdreno130 gpu;
    check(gpu.read(0x0000) == ZV_CHIP_ID_YAMATO, "Adreno CHIP_ID", gpu.read(0x0000), ZV_CHIP_ID_YAMATO);
    check(gpu.read(0x0110) == 1, "Adreno STATUS pronto", gpu.read(0x0110), 1);

    // WPTR avanca o RPTR e marca framebuffer sujo (contrato do ring buffer).
    gpu.write(0x010c, 0x40);
    check(gpu.read(0x0108) == 0x40, "Adreno RPTR segue WPTR", gpu.read(0x0108), 0x40);
    check(gpu.is_fb_dirty(), "Adreno marca fb sujo", gpu.is_fb_dirty(), 1);
    check(gpu.draws() == 0x40, "Adreno contabiliza draws", gpu.draws(), 0x40);

    // Interrupcao so pode subir com a mascara habilitada.
    UnifiedAdreno130 g2;
    g2.write(0x010c, 4);
    check(g2.read(0x0120) == 0, "Adreno sem INT mascarada", g2.read(0x0120), 0);
    g2.write(0x0124, 1);
    g2.write(0x010c, 8);
    check(g2.read(0x0120) == 1, "Adreno INT com mascara", g2.read(0x0120), 1);
    g2.write(0x0128, 1); // ack
    check(g2.read(0x0120) == 0, "Adreno INT reconhecida", g2.read(0x0120), 0);

    UnifiedMDDI mddi;
    check(mddi.read(0x0004) == 0x00000102, "MDDI VERSION", mddi.read(0x0004), 0x102);
    check(mddi.read(0x0028) == 0x21, "MDDI STATUS inicial", mddi.read(0x0028), 0x21);
    mddi.write(0x0000, 0x0200);            // POWER_UP
    check((mddi.read(0x0028) & 1) == 1, "MDDI LINK_ACTIVE apos power up", mddi.read(0x0028) & 1, 1);
    mddi.write(0x0008, 0xdeadbeef);        // PRI_PTR -> apresenta quadro
    check(mddi.read(0x0008) == 0xdeadbeef, "MDDI PRI_PTR", mddi.read(0x0008), 0xdeadbeef);
    check(mddi.frame_count() == 1, "MDDI conta quadro", mddi.frame_count(), 1);
    mddi.write(0x0000, 0x0400);            // RESET
    check(mddi.read(0x0028) == 0x21, "MDDI reset volta ao inicial", mddi.read(0x0028), 0x21);
}

// ---- Parte 2: ponta a ponta sob Unicorn ----------------------------------
static UnifiedMDDI*      g_mddi = nullptr;
static UnifiedAdreno130* g_gpu  = nullptr;

// Mesmo padrao do c0_mem_read_hook do runtime: injeta o valor do modelo antes
// de o Unicorn resolver a leitura contra a RAM de fundo.
static void read_hook(uc_engine* uc, uc_mem_type, uint64_t addr, int size, int64_t, void*) {
    if (size != 4 && size != 2 && size != 1) return;
    u32 v;
    if (addr >= ZV_MSM_MDDI_BASE && addr < ZV_MSM_MDDI_BASE + ZV_MDDI_SIZE)
        v = g_mddi->read((u32)(addr - ZV_MSM_MDDI_BASE));
    else if (addr >= ZV_ADRENO130_BASE && addr < ZV_ADRENO130_BASE + ZV_ADRENO130_SIZE)
        v = g_gpu->read((u32)(addr - ZV_ADRENO130_BASE));
    else return;
    uc_mem_write(uc, addr, &v, (size_t)size);
}

static void test_guest_reads(bool with_routing) {
    UnifiedMDDI mddi; UnifiedAdreno130 gpu;
    g_mddi = &mddi; g_gpu = &gpu;

    uc_engine* uc = nullptr;
    if (uc_open(UC_ARCH_ARM, UC_MODE_ARM, &uc) != UC_ERR_OK) { printf("[FAIL] uc_open\n"); fails++; return; }
    const uint64_t CODE = 0x10000000;
    uc_mem_map(uc, CODE, 0x1000, UC_PROT_ALL);
    uc_mem_map(uc, ZV_ADRENO130_BASE, 0x100000, UC_PROT_ALL);
    uc_mem_map(uc, ZV_MSM_MDDI_BASE, 0x10000, UC_PROT_ALL);

    uc_hook h;
    if (with_routing)
        uc_hook_add(uc, &h, UC_HOOK_MEM_READ, (void*)read_hook, nullptr, 1, 0);

    // r0 = [0xa0000000]  (CHIP_ID);  r1 = [0xaa600004] (MDDI VERSION)
    static const uint8_t code[] = {
        0x0c, 0x00, 0x9f, 0xe5,  // ldr r0, [pc, #12] -> &CHIP_ID
        0x00, 0x00, 0x90, 0xe5,  // ldr r0, [r0]
        0x08, 0x10, 0x9f, 0xe5,  // ldr r1, [pc, #8]  -> &MDDI_VER
        0x00, 0x10, 0x91, 0xe5,  // ldr r1, [r1]
        0x00, 0xf0, 0x20, 0xe3,  // nop
        0x00, 0x00, 0x00, 0xa0,  // .word 0xa0000000
        0x04, 0x00, 0x60, 0xaa,  // .word 0xaa600004
    };
    uc_mem_write(uc, CODE, code, sizeof(code));
    uc_err e = uc_emu_start(uc, CODE, CODE + 20, 0, 0);
    if (e != UC_ERR_OK) { printf("[FAIL] uc_emu_start: %s\n", uc_strerror(e)); fails++; uc_close(uc); return; }

    uint32_t r0 = 0, r1 = 0;
    uc_reg_read(uc, UC_ARM_REG_R0, &r0);
    uc_reg_read(uc, UC_ARM_REG_R1, &r1);
    uc_close(uc);

    if (with_routing) {
        check(r0 == ZV_CHIP_ID_YAMATO, "guest le CHIP_ID do modelo", r0, ZV_CHIP_ID_YAMATO);
        check(r1 == 0x00000102, "guest le MDDI VERSION do modelo", r1, 0x102);
    } else {
        // Controle negativo: sem roteamento o guest so ve RAM zerada.
        check(r0 == 0 && r1 == 0, "controle negativo: sem roteamento le zero", r0, 0);
    }
}

int main() {
    printf("== modelos ==\n");            test_models();
    printf("== sem roteamento ==\n");     test_guest_reads(false);
    printf("== com roteamento ==\n");     test_guest_reads(true);
    printf(fails ? "\nFALHAS: %d\n" : "\nTODOS OS TESTES PASSARAM\n", fails);
    return fails ? 1 : 0;
}
