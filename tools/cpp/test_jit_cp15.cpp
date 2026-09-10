// test_jit_cp15.cpp — o coprocessador CP15 do backend recompilado precisa
// GUARDAR o que foi escrito.
//
// Defeito que este teste tranca:
//   ZeeboCoprocessor::CompileSendOneWord devolvia Callback{&NopFn}, ou seja,
//   toda escrita MCR era DESCARTADA; e CompileGetOneWord devolvia um scratch
//   zerado para tudo que nao fosse MIDR/CTR. O boot do Core0 escreve TTBR0
//   (c2) e o registrador de controle (c1) e depois os le de volta; o motor
//   interpretado devolvia o valor escrito e o recompilado devolvia 0.
//   Essa era a PRIMEIRA divergencia real entre os dois backends no boot
//   (instrucao #16723, `MRC p15,0,r0,c2,c0,0` -> 0 em vez de 0x1001c000).
//
// Por que nao basta testar "o boot avanca mais": avancar mais nao prova que o
// valor lido esta CORRETO. Aqui a assercao e sobre o valor devolvido.

#include "zeebo_dynarmic_core.h"

#include <cstdio>
#include <cstdint>
#include <cstring>
#include <vector>

namespace {

int g_fail = 0;

void check(const char* what, std::uint32_t got, std::uint32_t want) {
    if (got == want) {
        std::printf("  ok   %-46s = 0x%08x\n", what, got);
    } else {
        std::printf("  FALHA %-45s = 0x%08x (esperado 0x%08x)\n", what, got, want);
        g_fail++;
    }
}

// Memoria plana de teste: o programa fica em 0, com dados sinteticos.
std::vector<std::uint8_t> g_mem;

std::uint8_t  rd8 (void*, std::uint32_t a) { return a < g_mem.size() ? g_mem[a] : 0; }
std::uint16_t rd16(void*, std::uint32_t a) { std::uint16_t v = 0; if (a + 2 <= g_mem.size()) std::memcpy(&v, &g_mem[a], 2); return v; }
std::uint32_t rd32(void*, std::uint32_t a) { std::uint32_t v = 0; if (a + 4 <= g_mem.size()) std::memcpy(&v, &g_mem[a], 4); return v; }
void wr8 (void*, std::uint32_t a, std::uint8_t  v) { if (a < g_mem.size()) g_mem[a] = v; }
void wr16(void*, std::uint32_t a, std::uint16_t v) { if (a + 2 <= g_mem.size()) std::memcpy(&g_mem[a], &v, 2); }
void wr32(void*, std::uint32_t a, std::uint32_t v) { if (a + 4 <= g_mem.size()) std::memcpy(&g_mem[a], &v, 4); }

zeebo::jit::MemoryBridge make_bridge() {
    zeebo::jit::MemoryBridge b{};
    b.read8 = &rd8;   b.read16 = &rd16;  b.read32 = &rd32;
    b.write8 = &wr8;  b.write16 = &wr16; b.write32 = &wr32;
    b.user_data = nullptr;
    return b;
}

void load(const std::vector<std::uint32_t>& code) {
    g_mem.assign(64 * 1024, 0);
    for (std::size_t i = 0; i < code.size(); i++) {
        std::memcpy(&g_mem[i * 4], &code[i], 4);
    }
}

}  // namespace

int main() {
    std::printf("== CP15 do backend recompilado guarda o que foi escrito ==\n");

    const std::uint32_t kMidr = 0x4107B362;  // ARM1136, como o Zeebo
    const std::uint32_t kCtr  = 0x1D152152;

    // Cenario 1: TTBR0 (c2) — exatamente o registrador da divergencia real.
    {
        std::printf("\n-- MCR/MRC p15,0,Rd,c2,c0,0 (TTBR0) --\n");
        load({
            0xE3A00456,  // mov r0, #0x56000000  (imediato ARM valido)
            0xEE020F10,  // mcr p15, 0, r0, c2, c0, 0   <- escreve TTBR0
            0xE3A01000,  // mov r1, #0                  (suja o caminho)
            0xEE121F10,  // mrc p15, 0, r1, c2, c0, 0   <- le TTBR0 de volta
            0xEAFFFFFE,  // b .
        });

        zeebo::jit::Cp15Ids ids{}; ids.midr = kMidr; ids.ctr = kCtr;
        zeebo::jit::DynarmicCore core(make_bridge(), ids);
        core.set_regs_zero();
        core.set_pc(0);
        for (int i = 0; i < 4; i++) core.step_one_insn();

        const std::uint32_t r0 = core.reg(0);
        check("r0 escrito em TTBR0", r0, 0x56000000u);
        // A assercao central: o valor LIDO de volta e o valor ESCRITO.
        check("r1 lido de volta de TTBR0", core.reg(1), r0);
    }

    // Cenario 2: registrador de controle do sistema (c1) — o boot le/modifica/escreve.
    {
        std::printf("\n-- MCR/MRC p15,0,Rd,c1,c0,0 (controle do sistema) --\n");
        load({
            0xE3A00A01,  // mov r0, #0x1000
            0xEE010F10,  // mcr p15, 0, r0, c1, c0, 0
            0xEE11CF10,  // mrc p15, 0, r12, c1, c0, 0
            0xEAFFFFFE,  // b .
        });

        zeebo::jit::Cp15Ids ids{}; ids.midr = kMidr; ids.ctr = kCtr;
        zeebo::jit::DynarmicCore core(make_bridge(), ids);
        core.set_regs_zero();
        core.set_pc(0);
        for (int i = 0; i < 3; i++) core.step_one_insn();

        check("r12 lido de volta do controle", core.reg(12), core.reg(0));
    }

    // Cenario 3: registradores distintos nao podem se sobrescrever.
    // c2,c0,0 (TTBR0) e c2,c0,1 (TTBR1) compartilham CRn: se o banco fosse
    // indexado so por CRn, um sobrescreveria o outro.
    {
        std::printf("\n-- TTBR0 e TTBR1 sao slots independentes --\n");
        load({
            0xE3A00456,  // mov r0, #0x56000000
            0xEE020F10,  // mcr p15, 0, r0, c2, c0, 0   (TTBR0)
            0xE3A01478,  // mov r1, #0x78000000
            0xEE021F30,  // mcr p15, 0, r1, c2, c0, 1   (TTBR1)
            0xEE122F10,  // mrc p15, 0, r2, c2, c0, 0   (le TTBR0)
            0xEE123F30,  // mrc p15, 0, r3, c2, c0, 1   (le TTBR1)
            0xEAFFFFFE,  // b .
        });

        zeebo::jit::Cp15Ids ids{}; ids.midr = kMidr; ids.ctr = kCtr;
        zeebo::jit::DynarmicCore core(make_bridge(), ids);
        core.set_regs_zero();
        core.set_pc(0);
        for (int i = 0; i < 6; i++) core.step_one_insn();

        check("TTBR0 preservado", core.reg(2), core.reg(0));
        check("TTBR1 preservado", core.reg(3), core.reg(1));
    }

    // Cenario 4: MIDR e CTR continuam sendo os IDs, nao o banco gravavel.
    // Sem isso, a correcao poderia ter quebrado a identificacao da CPU.
    {
        std::printf("\n-- MIDR/CTR continuam devolvendo os IDs --\n");
        load({
            0xEE100F10,  // mrc p15, 0, r0, c0, c0, 0   (MIDR)
            0xEE101F30,  // mrc p15, 0, r1, c0, c0, 1   (CTR)
            0xEAFFFFFE,  // b .
        });

        zeebo::jit::Cp15Ids ids{}; ids.midr = kMidr; ids.ctr = kCtr;
        zeebo::jit::DynarmicCore core(make_bridge(), ids);
        core.set_regs_zero();
        core.set_pc(0);
        for (int i = 0; i < 2; i++) core.step_one_insn();

        check("MIDR", core.reg(0), kMidr);
        check("CTR",  core.reg(1), kCtr);
    }

    std::printf("\n==== %s ====\n", g_fail == 0 ? "CP15: todas as asseracoes passaram"
                                                : "CP15: HOUVE FALHA");
    return g_fail == 0 ? 0 : 1;
}
