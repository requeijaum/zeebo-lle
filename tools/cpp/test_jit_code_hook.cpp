// test_jit_code_hook.cpp — prova que o gancho por instrucao do backend
// recompilado (DynarmicCore::on_code_exec) observa cada PC EXECUTADO e que uma
// alteracao de registrador feita pelo gancho afeta de fato a execucao.
//
// Defeito original: o gancho era ligado em MemoryBridge::on_code, que dispara
// na TRADUCAO de bloco -- uma vez por bloco compilado, nao a cada execucao --
// e escrevia registradores no motor interpretado, que nao e o que executa sob
// --jit. Um laco de N voltas rendia apenas o punhado de PCs traduzidos.

#include "zeebo_dynarmic_core.h"
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <vector>
#include <map>

static int fails = 0;
static void check(bool ok, const char* what, long got, long want) {
    printf("[%s] %s (obtido %ld, esperado %ld)\n", ok ? "PASS" : "FAIL", what, got, want);
    if (!ok) fails++;
}

// Memoria plana de teste: 64 KiB a partir de 0.
static uint8_t g_mem[0x10000];
static uint8_t  r8 (void*, uint32_t a) { return a < sizeof(g_mem) ? g_mem[a] : 0; }
static uint16_t r16(void*, uint32_t a) { uint16_t v=0; if (a+2<=sizeof(g_mem)) memcpy(&v,g_mem+a,2); return v; }
static uint32_t r32(void*, uint32_t a) { uint32_t v=0; if (a+4<=sizeof(g_mem)) memcpy(&v,g_mem+a,4); return v; }
static void w8 (void*, uint32_t a, uint8_t v)  { if (a < sizeof(g_mem)) g_mem[a]=v; }
static void w16(void*, uint32_t a, uint16_t v) { if (a+2<=sizeof(g_mem)) memcpy(g_mem+a,&v,2); }
static void w32(void*, uint32_t a, uint32_t v) { if (a+4<=sizeof(g_mem)) memcpy(g_mem+a,&v,4); }

// Contagem de traducoes, para exibir o contraste com a execucao.
static std::map<uint32_t,int> g_translated;
static void on_code_translate(void*, uint32_t pc) { g_translated[pc]++; }

static void emit(uint32_t addr, const std::vector<uint32_t>& ins) {
    for (size_t i = 0; i < ins.size(); i++) memcpy(g_mem + addr + i*4, &ins[i], 4);
}

int main() {
    // Programa: r0 = 8; laco { r0 = r0 - 1 } enquanto r0 != 0; depois trava.
    //   0x00: mov r0, #8
    //   0x04: subs r0, r0, #1
    //   0x08: bne 0x04
    //   0x0c: b 0x0c            (auto-laco terminal)
    const uint32_t ENTRY = 0x00000000;
    memset(g_mem, 0, sizeof(g_mem));
    emit(ENTRY, {
        0xe3a00008,  // mov  r0, #8
        0xe2500001,  // subs r0, r0, #1
        0x1afffffd,  // bne  -> 0x04  (PC+8 + (-3*4) = 0x04)
        0xeafffffe,  // b    -> 0x0c
    });

    zeebo::jit::MemoryBridge br{};
    br.read8=r8; br.read16=r16; br.read32=r32;
    br.write8=w8; br.write16=w16; br.write32=w32;
    br.on_code = on_code_translate;   // gancho de TRADUCAO (o antigo)

    zeebo::jit::DynarmicCore core(br);
    core.set_regs_zero();
    core.set_cpsr(0x000001d3);        // modo supervisor, ARM
    core.set_pc(ENTRY);

    // Gancho de EXECUCAO: registra cada PC visto.
    std::vector<uint32_t> seen;
    core.on_code_exec = [&](uint32_t pc) { seen.push_back(pc); };

    core.run(40);

    long loop_body = 0, loop_branch = 0;
    for (uint32_t pc : seen) { if (pc == 0x04) loop_body++; if (pc == 0x08) loop_branch++; }

    // O laco roda 8 vezes: o gancho de execucao precisa ver as 8 passagens.
    check(loop_body == 8, "gancho de execucao ve cada volta do laco", loop_body, 8);
    check(loop_branch == 8, "gancho de execucao ve cada desvio", loop_branch, 8);

    // Contraste: a traducao ocorre uma vez por bloco compilado, muito menos que
    // as passagens executadas. E o defeito exato do gancho antigo.
    long translated_body = g_translated.count(0x04) ? g_translated[0x04] : 0;
    check(translated_body < loop_body,
          "traducao dispara menos que execucao (gancho antigo era insuficiente)",
          translated_body, loop_body);

    // ---- O gancho consegue desviar o fluxo de verdade? --------------------
    // Programa 2: auto-laco em 0x100. O gancho detecta o PC e escreve um novo
    // PC; sem efeito real sobre o motor, ficaria preso para sempre.
    const uint32_t ENTRY2 = 0x00000100, ESCAPE = 0x00000200;
    emit(ENTRY2, { 0xeafffffe });                 // b .
    emit(ESCAPE, { 0xe3a0002a, 0xeafffffe });     // mov r0,#42 ; b .

    zeebo::jit::DynarmicCore core2(br);
    core2.set_regs_zero();
    core2.set_cpsr(0x000001d3);
    core2.set_pc(ENTRY2);

    bool redirected = false;
    core2.on_code_exec = [&](uint32_t pc) {
        if (pc == ENTRY2 && !redirected) { redirected = true; core2.set_pc(ESCAPE); }
    };
    core2.run(50);

    check(redirected, "gancho foi acionado no auto-laco", redirected, 1);
    check(core2.reg(0) == 42, "desvio do gancho teve efeito real na execucao", core2.reg(0), 42);

    printf(fails ? "\nFALHAS: %d\n" : "\nTODOS OS TESTES PASSARAM\n", fails);
    return fails ? 1 : 0;
}
