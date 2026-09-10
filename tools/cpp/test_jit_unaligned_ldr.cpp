// test_jit_unaligned_ldr.cpp — LDR desalinhado deve seguir o ARM1136, e os dois
// motores precisam concordar.
//
// RESULTADO: este teste PASSA. Ele NAO reproduz a divergencia do boot.
//
// O TRM DDI0211K (ARM1136 r1p5) p.210 descreve o bit U: com U=0 o processador
// "treats unaligned loads as rotated aligned data accesses". Levantei a
// hipotese de que a divergencia do boot vinha dai. **A hipotese foi refutada
// por medicao**: o Unicorn configurado como ARM1176 devolve leitura LITERAL,
// nao rotacionada. Controle que separa os dois modelos (memoria
// 11223344 55667788, ldr de +2): rotacao daria 0x33441122; o Unicorn devolve
// `0x77881122` = literal. Ler byte-a-byte do endereco cru, como a VTLB faz,
// reproduz o oraculo.
//
// RESSALVA: o Unicorn devolve SCTLR=0 no reset (bit U=0) e AINDA ASSIM le
// literal — ele nao modela a rotacao descrita no TRM. Nao e que o firmware
// habilite U=1; e que o oraculo ignora esse bit. Os dois motores concordam
// entre si (o que este teste trava), mas nenhum foi provado fiel ao silicio
// neste ponto.
//
// O teste fica como REGRESSAO: trava a concordancia dos dois motores em carga
// desalinhada, para que uma futura "correcao" nao introduza rotacao indevida.
// A causa real da divergencia em #23726 continua ABERTA — nao e o LDR em si,
// e o CONTEUDO da memoria lida (os motores leem memorias diferentes).
//
// Contexto: primeira divergencia real do boot depois do CP15 —
// encaminhava o endereco cru para uma leitura byte-a-byte (leitura desalinhada
// literal), enquanto o Unicorn aplicava a semantica do ARM. Primeira
// divergencia real observada no boot depois do CP15: instrucao #23726,
// `ldreq r3,[r5]` @ 0xf000a1d0 com r5=0x00000002 — interpretado devolvia
// 0x10090001 e o recompilado 0x00000000.
//
// O teste compara os DOIS motores sobre a MESMA memoria: nao fixa um valor
// esperado escolhido por mim, exige que o recompilado reproduza o interpretado.

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

std::uint8_t rd8(void*, std::uint32_t a) {
    a -= kBase;
    return a < g_mem.size() ? g_mem[a] : 0;
}
std::uint16_t rd16(void*, std::uint32_t a) {
    std::uint16_t v = 0; a -= kBase;
    if (a + 2 <= g_mem.size()) std::memcpy(&v, &g_mem[a], 2);
    return v;
}
// Le 32 bits literalmente a partir do endereco pedido — e o que a ponte de
// memoria real do emulador faz (VTLB le byte-a-byte) e o que o ARM1136 do
// Zeebo faz com suporte a acesso desalinhado habilitado (bit U = 1).
std::uint32_t rd32(void*, std::uint32_t a) {
    std::uint32_t v = 0;
    const std::uint32_t off = a - kBase;
    if (off + 4 <= g_mem.size()) std::memcpy(&v, &g_mem[off], 4);
    return v;
}
void wr8 (void*, std::uint32_t a, std::uint8_t v)  { a -= kBase; if (a < g_mem.size()) g_mem[a] = v; }
void wr16(void*, std::uint32_t a, std::uint16_t v) { a -= kBase; if (a + 2 <= g_mem.size()) std::memcpy(&g_mem[a], &v, 2); }
void wr32(void*, std::uint32_t a, std::uint32_t v) { a -= kBase; if (a + 4 <= g_mem.size()) std::memcpy(&g_mem[a], &v, 4); }

zeebo::jit::MemoryBridge make_bridge() {
    zeebo::jit::MemoryBridge b{};
    b.read8 = &rd8;  b.read16 = &rd16; b.read32 = &rd32;
    b.write8 = &wr8; b.write16 = &wr16; b.write32 = &wr32;
    b.user_data = nullptr;
    return b;
}

// Executa o mesmo programa nos dois motores e compara Rt.
void compare(const char* nome, std::uint32_t data_word, std::uint32_t addr) {
    // Programa: r5 = addr ; ldr r3,[r5] ; b .
    const std::uint32_t code[] = {
        0xE59F5008,  // ldr r5, [pc, #8]   -> addr
        0xE5953000,  // ldr r3, [r5]
        0xEAFFFFFE,  // b .
        0x00000000,  // padding
        addr,        // literal
    };

    g_mem.assign(kSize, 0);
    std::memcpy(&g_mem[0x100], code, sizeof(code));
    std::memcpy(&g_mem[0x000], &data_word, 4);  // palavra em kBase+0

    // --- Unicorn (oraculo) ---
    std::uint32_t uni_r3 = 0;
    {
        uc_engine* uc = nullptr;
        uc_open(UC_ARCH_ARM, UC_MODE_ARM, &uc);
        uc_ctl_set_cpu_model(uc, UC_CPU_ARM_1176);
        uc_mem_map(uc, kBase, kSize, UC_PROT_ALL);
        uc_mem_write(uc, kBase, g_mem.data(), g_mem.size());
        std::uint32_t sp = kBase + 0x8000;
        uc_reg_write(uc, UC_ARM_REG_SP, &sp);
        uc_emu_start(uc, kBase + 0x100, kBase + 0x108, 0, 2);
        uc_reg_read(uc, UC_ARM_REG_R3, &uni_r3);
        uc_close(uc);
    }

    // --- Dynarmic ---
    std::uint32_t jit_r3 = 0;
    {
        zeebo::jit::Cp15Ids ids{}; ids.midr = 0x4107B362; ids.ctr = 0x1D152152;
        zeebo::jit::DynarmicCore core(make_bridge(), ids);
        core.set_regs_zero();
        core.set_pc(kBase + 0x100);
        core.step_one_insn();
        core.step_one_insn();
        jit_r3 = core.reg(3);
    }

    const bool ok = (uni_r3 == jit_r3);
    if (!ok) g_fail++;
    std::printf("  %-5s %-34s interpretado=0x%08x  recompilado=0x%08x\n",
                ok ? "ok" : "FALHA", nome, uni_r3, jit_r3);
}

}  // namespace

int main() {
    std::printf("== LDR desalinhado: os dois motores devem concordar ==\n");
    std::printf("   (TRM ARM1136 p.210: com U=0, carga desalinhada = palavra\n");
    std::printf("    alinhada rotacionada por 8*(addr & 3) bits)\n\n");

    const std::uint32_t w = 0x11223344;

    compare("alinhado  (addr & 3 == 0)", w, kBase + 0);
    compare("desalinhado +1",            w, kBase + 1);
    compare("desalinhado +2",            w, kBase + 2);  // o caso do boot real
    compare("desalinhado +3",            w, kBase + 3);

    // Caso exato do boot: r5 = 0x...0002.
    std::printf("\n-- caso real do boot (#23726, ldreq r3,[r5], r5 desalinhado +2) --\n");
    compare("padrao do AMSS", 0x00011009, kBase + 2);

    std::printf("\n==== %s ====\n",
                g_fail == 0 ? "LDR desalinhado: motores concordam"
                            : "LDR desalinhado: MOTORES DIVERGEM");
    return g_fail == 0 ? 0 : 1;
}
