// test_slide_detector.cpp — Bug 8: detector de NOP-slide via UC_HOOK_BLOCK,
// SEM o uc_mem_read por-instrucao que era o hotspot do Core1.
//
// Prova, rodando codigo REAL no Unicorn (interpretador puro, sem JIT):
//   POSITIVO: uma corrida linear de NOPs (>= slide_limit insns) DISPARA.
//   NEGATIVO: um loop apertado legitimo que executa MUITO mais que slide_limit
//             instrucoes NAO dispara — porque o branch tomado reinicia a run
//             a cada iteracao (blocos pequenos, nunca contiguos o suficiente).
//
// O detector (zeebo_slide_detector.h) so ve (start, size) do bloco: zero
// leituras de opcode no caminho quente.
#include "zeebo_slide_detector.h"
#include <unicorn/unicorn.h>
#include <cstdio>
#include <cstdint>
#include <cstring>

typedef uint32_t u32;

static zeebo::SlideDetector g_det;
static bool g_tripped_at_hook = false;

static void block_hook(uc_engine* uc, uint64_t addr, uint32_t size, void* ud) {
    (void)ud;
    u32 count = size / 4;
    if (g_det.on_block((u32)addr, count)) {
        g_tripped_at_hook = true;
        uc_emu_stop(uc);
    }
}

static const u32 BASE = 0x1000;

// Roda `code` (bytes) a partir de BASE; devolve true se o detector disparou.
static bool run_case(const uint8_t* code, size_t len, u32 slide_limit,
                     size_t max_insns) {
    uc_engine* uc = nullptr;
    if (uc_open(UC_ARCH_ARM, UC_MODE_ARM, &uc) != UC_ERR_OK) { printf("uc_open FAIL\n"); return false; }
    // ARM11 (ARMv6) — interpretador; nao habilitamos JIT em lugar nenhum.
    uc_ctl_set_cpu_model(uc, UC_CPU_ARM_1136);
    uc_mem_map(uc, BASE, 0x10000, UC_PROT_ALL);
    uc_mem_write(uc, BASE, code, len);

    g_det = zeebo::SlideDetector{};
    g_det.slide_limit = slide_limit;
    g_tripped_at_hook = false;

    uc_hook h;
    uc_hook_add(uc, &h, UC_HOOK_BLOCK, (void*)block_hook, nullptr, 1, 0);

    uc_emu_start(uc, BASE, BASE + len, 0, max_insns);
    uc_close(uc);
    return g_tripped_at_hook;
}

int main() {
    printf("== test_slide_detector (bug 8) ==\n");
    const u32 LIMIT = 256;
    int fails = 0;

    // ── POSITIVO: NOP-slide linear ────────────────────────────────────────
    // 400 NOPs (mov r0,r0 = 0xe1a00000) seguidos. Um unico bloco basico
    // enorme (sem branch) => a run linear ultrapassa LIMIT e dispara.
    {
        const size_t N = 400;
        uint8_t code[N * 4];
        for (size_t i = 0; i < N; i++) {
            u32 nop = 0xe1a00000u;
            std::memcpy(code + i * 4, &nop, 4);
        }
        bool tripped = run_case(code, sizeof(code), LIMIT, N + 8);
        printf("  [%s] POSITIVO slide de %zu NOPs: tripped=%d (esperado 1)\n",
               tripped ? "OK " : "FAIL", N, tripped);
        if (!tripped) fails++;
    }

    // ── NEGATIVO: loop apertado legitimo ──────────────────────────────────
    // r0 = 5000; label: subs r0,r0,#1; bne label; depois nop.
    // Executa 10000 instrucoes (muito > LIMIT) mas cada iteracao e' um bloco
    // de 2 insns terminado por branch tomado => a run linear NUNCA acumula
    // alem de 2. NAO pode disparar.
    {
        uint8_t code[16];
        u32 mov  = 0xe3a00f4eu; // mov r0, #0x138*? -> ajustamos abaixo via count real
        // mov r0, #5000 nao cabe em imm8 rot; use r0 = 250 e conte iteracoes.
        // Melhor: mov r0,#250 (imm valido) => 250 iteracoes * 2 = 500 insns > LIMIT.
        mov = 0xe3a000fau;      // mov r0, #250
        u32 subs = 0xe2500001u; // subs r0, r0, #1
        u32 bne  = 0x1afffffdu; // bne (PC-8 -> volta para subs)
        u32 nop  = 0xe1a00000u;
        std::memcpy(code + 0,  &mov,  4);
        std::memcpy(code + 4,  &subs, 4);
        std::memcpy(code + 8,  &bne,  4);
        std::memcpy(code + 12, &nop,  4);
        bool tripped = run_case(code, sizeof(code), LIMIT, 5000);
        // 1 + 250*2 = 501 insns executadas, > LIMIT=256, mas sem run linear.
        printf("  [%s] NEGATIVO loop 250x (501 insns): tripped=%d (esperado 0)\n",
               tripped ? "FAIL" : "OK ", tripped);
        if (tripped) fails++;
    }

    if (fails) { printf("TESTE_EXIT=1 (%d falha(s))\n", fails); return 1; }
    printf("TESTE_EXIT=0\n");
    return 0;
}
