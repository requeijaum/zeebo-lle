// test_jit_fault_report.cpp — prova que o backend recompilado RELATA a falha
// em vez de girar em falso.
//
// Defeito: ExceptionRaised/InterpreterFallback tinham corpo vazio (ou so
// zeravam ticks). Uma instrucao invalida nao chegava a ninguem: run() voltava,
// o laco principal nao checava nada, reiniciava no mesmo PC, e o contador de
// instrucoes subia indefinidamente com o PC congelado. Foi exatamente o que
// escondeu a parada do boot em 0xf0003adc.
//
// Controle negativo (CN): a memoria em torno do PC de falha e toda zero.
// 0x00000000 e uma instrucao valida em ARM (andeq r0,r0,r0), entao um teste que
// so olhasse "PC parado" nao distinguiria laco legitimo de falha engolida. Por
// isso o teste exige o RELATO da falha, nao o sintoma.

#include "zeebo_dynarmic_core.h"

#include <cstdio>
#include <cstdint>
#include <cstring>
#include <vector>

using zeebo::jit::MemoryBridge;
using zeebo::jit::Cp15Ids;
using zeebo::jit::DynarmicCore;

static std::vector<uint8_t> g_mem;

static uint32_t rd32(uint32_t a) {
    if (a + 4 > g_mem.size()) return 0;
    uint32_t v;
    std::memcpy(&v, &g_mem[a], 4);
    return v;
}

static int g_fail = 0;
static void check(const char* what, bool cond) {
    std::printf("  [%s] %s\n", cond ? "ok" : "FALHA", what);
    if (!cond) g_fail = 1;
}

int main() {
    g_mem.assign(0x10000, 0);

    // Programa: uma instrucao indefinida. O encoding 0xe7f000f0 e a UDF
    // permanentemente indefinida da arquitetura ARM.
    auto w32 = [](uint32_t a, uint32_t v) { std::memcpy(&g_mem[a], &v, 4); };
    w32(0x0000, 0xe3a00001);  // mov r0, #1
    w32(0x0004, 0xe7f000f0);  // udf  -> deve levantar excecao
    w32(0x0008, 0xe3a00002);  // mov r0, #2  (nao deve executar)

    MemoryBridge bridge{};
    bridge.read8 = [](void*, uint32_t a) -> uint8_t { return a < g_mem.size() ? g_mem[a] : 0; };
    bridge.read16 = [](void*, uint32_t a) -> uint16_t { return (uint16_t)rd32(a); };
    bridge.read32 = [](void*, uint32_t a) -> uint32_t { return rd32(a); };
    bridge.write8 = [](void*, uint32_t a, uint8_t v) { if (a < g_mem.size()) g_mem[a] = v; };
    bridge.write16 = [](void*, uint32_t a, uint16_t v) { if (a + 2 <= g_mem.size()) std::memcpy(&g_mem[a], &v, 2); };
    bridge.write32 = [](void*, uint32_t a, uint32_t v) { if (a + 4 <= g_mem.size()) std::memcpy(&g_mem[a], &v, 4); };
    bridge.is_peripheral = [](void*, uint32_t) -> bool { return false; };

    Cp15Ids cp15{};
    cp15.midr = 0x4107B362;

    std::printf("== instrucao indefinida deve ser relatada ==\n");
    {
        DynarmicCore core(bridge, cp15);
        core.set_regs_zero();
        core.set_pc(0x0000);
        core.run(64);

        auto f = core.take_fault();
        // Antes da correcao ambos os campos eram sempre falsos: a falha nao
        // chegava ao chamador de forma alguma.
        check("falha foi relatada ao chamador", f.raised || f.interpreter_fallback);
        check("PC relatado e o da instrucao invalida (0x04)", f.pc == 0x0004);
        // O motor avanca o PC para alem da instrucao invalida, mas NAO a
        // executa: r0 continua 1 (do mov anterior), nao 2.
        check("PC avancou para alem da invalida (0x08)", core.pc() == 0x0008);
        check("r0 preservado do mov anterior (== 1)", core.reg(0) == 1);
        check("instrucao seguinte nao executou (r0 != 2)", core.reg(0) != 2);
    }

    std::printf("== take_fault limpa o estado (nao repete falha antiga) ==\n");
    {
        DynarmicCore core(bridge, cp15);
        core.set_regs_zero();
        core.set_pc(0x0000);
        core.run(64);
        (void)core.take_fault();
        auto f2 = core.take_fault();
        check("segunda leitura vem limpa", !f2.raised && !f2.interpreter_fallback);
    }

    std::printf("== CN: programa valido nao relata falha ==\n");
    {
        // Sem este controle, um take_fault() que retornasse "falha" sempre
        // passaria no teste acima sem detectar nada.
        w32(0x0100, 0xe3a00007);  // mov r0, #7
        w32(0x0104, 0xeafffffe);  // b . (laco legitimo, PC fica parado)

        DynarmicCore core(bridge, cp15);
        core.set_regs_zero();
        core.set_pc(0x0100);
        core.run(64);

        auto f = core.take_fault();
        check("laco legitimo NAO e relatado como falha", !f.raised && !f.interpreter_fallback);
        check("laco legitimo executou (r0 == 7)", core.reg(0) == 7);
        check("PC parado no laco (sintoma igual ao do bug)", core.pc() == 0x0104);
    }

    std::printf(g_fail ? "\nRESULTADO: FALHOU\n" : "\nRESULTADO: todas as asseracoes passaram\n");
    return g_fail;
}
