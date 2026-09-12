// test_jit_cp15_reset.cpp — o estado de RESET do CP15 tem de bater entre os
// dois motores.
//
// Origem do achado: leitura do fonte do QEMU (target/arm/tcg/cpu32.c). O
// Unicorn e um fork do QEMU, entao os valores de reset que ele usa sao os do
// QEMU. Para a familia ARM1136 (e tambem para o 1176):
//
//     cpu->midr         = 0x4107b362   (arm1136_r2) / 0x4117b363 (arm1136)
//     cpu->ctr          = 0x01dd20d2
//     cpu->reset_sctlr  = 0x00050078   <-- NAO e zero
//
// Defeito que este teste tranca: o nosso banco CP15 comeca inteiramente
// ZERADO. Uma leitura de SCTLR (c1,c0,0) antes da primeira escrita devolvia
// 0x00000000, enquanto o motor interpretado devolve o valor de reset real.
// Mesma classe do defeito corrigido em 5471a8b (banco devolvia zero), mas
// naquele caso o problema era nao guardar o que foi ESCRITO; aqui e nao ter o
// valor INICIAL correto antes de qualquer escrita.
//
// Por que importa: o boot do AMSS le SCTLR, altera bits e escreve de volta
// (sequencia classica read-modify-write). Partindo de 0, todos os bits que o
// reset deveria trazer ligados (W, P, D, L) se perdem no primeiro
// read-modify-write, e o kernel segue por um caminho diferente do real.
//
// O teste NAO fixa constantes escolhidas por mim: compara contra o Unicorn.

#include "zeebo_dynarmic_core.h"

#include <unicorn/unicorn.h>

#include <cstdio>
#include <cstdint>
#include <cstring>
#include <vector>

namespace {

int g_fail = 0;
constexpr std::uint32_t kBase = 0x10000000;
constexpr std::size_t   kSize = 0x10000;
std::vector<std::uint8_t> g_mem;

std::uint8_t  rd8 (void*, std::uint32_t a) { a -= kBase; return a < g_mem.size() ? g_mem[a] : 0; }
std::uint16_t rd16(void*, std::uint32_t a) { std::uint16_t v=0; a-=kBase; if (a+2<=g_mem.size()) std::memcpy(&v,&g_mem[a],2); return v; }
std::uint32_t rd32(void*, std::uint32_t a) { std::uint32_t v=0; a-=kBase; if (a+4<=g_mem.size()) std::memcpy(&v,&g_mem[a],4); return v; }
void wr8 (void*, std::uint32_t a, std::uint8_t v)  { a-=kBase; if (a<g_mem.size()) g_mem[a]=v; }
void wr16(void*, std::uint32_t a, std::uint16_t v) { a-=kBase; if (a+2<=g_mem.size()) std::memcpy(&g_mem[a],&v,2); }
void wr32(void*, std::uint32_t a, std::uint32_t v) { a-=kBase; if (a+4<=g_mem.size()) std::memcpy(&g_mem[a],&v,4); }

zeebo::jit::MemoryBridge make_bridge() {
    zeebo::jit::MemoryBridge b{};
    b.read8=&rd8; b.read16=&rd16; b.read32=&rd32;
    b.write8=&wr8; b.write16=&wr16; b.write32=&wr32;
    b.user_data=nullptr;
    return b;
}

void check(const char* nome, std::uint32_t uni, std::uint32_t jit) {
    const bool ok = (uni == jit);
    if (!ok) g_fail++;
    std::printf("  %-5s %-28s interpretado=0x%08x  recompilado=0x%08x\n",
                ok ? "ok" : "FALHA", nome, uni, jit);
}

// Le um registrador CP15 nos dois motores, SEM escrita previa (estado de reset).
void compare_reset_read(const char* nome, std::uint32_t mrc_opcode) {
    const std::uint32_t code[] = { mrc_opcode, 0xEAFFFFFE };

    g_mem.assign(kSize, 0);
    std::memcpy(&g_mem[0x100], code, sizeof(code));

    std::uint32_t uni_r0 = 0;
    {
        uc_engine* uc = nullptr;
        uc_open(UC_ARCH_ARM, UC_MODE_ARM, &uc);
        uc_ctl_set_cpu_model(uc, UC_CPU_ARM_1136);
        uc_mem_map(uc, kBase, kSize, UC_PROT_ALL);
        uc_mem_write(uc, kBase, g_mem.data(), g_mem.size());
        uc_emu_start(uc, kBase + 0x100, kBase + 0x104, 0, 1);
        uc_reg_read(uc, UC_ARM_REG_R0, &uni_r0);
        uc_close(uc);
    }

    std::uint32_t jit_r0 = 0;
    {
        zeebo::jit::Cp15Ids ids{};
        zeebo::jit::DynarmicCore core(make_bridge(), ids);
        core.set_regs_zero();
        core.set_pc(kBase + 0x100);
        core.step_one_insn();
        jit_r0 = core.reg(0);
    }

    check(nome, uni_r0, jit_r0);
}

}  // namespace

int main() {
    std::printf("== estado de RESET do CP15: os dois motores devem concordar ==\n");
    std::printf("   (QEMU target/arm/tcg/cpu32.c: reset_sctlr = 0x00050078)\n\n");

    // mrc p15,0,r0,c1,c0,0 -> SCTLR (registrador de controle do sistema)
    compare_reset_read("SCTLR (c1,c0,0) no reset", 0xEE110F10);
    // mrc p15,0,r0,c0,c0,1 -> CTR (cache type)
    compare_reset_read("CTR   (c0,c0,1) no reset", 0xEE100F30);
    // mrc p15,0,r0,c0,c0,0 -> MIDR
    compare_reset_read("MIDR  (c0,c0,0) no reset", 0xEE100F10);

    std::printf("\n==== %s ====\n",
                g_fail == 0 ? "CP15 no reset: motores concordam"
                            : "CP15 no reset: MOTORES DIVERGEM");
    return g_fail == 0 ? 0 : 1;
}
