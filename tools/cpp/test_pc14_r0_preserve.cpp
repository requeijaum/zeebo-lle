// test_pc14_r0_preserve.cpp — RED test para a fronteira Core0 PC=0x14.
//
// HIPOTESE SOB TESTE
// Em zeebo_lle_main.cpp, o handler de SVC executa, na linha ~3408:
//
//     uc_reg_write(uc, UC_ARM_REG_R0, &res_r0);
//
// INCONDICIONALMENTE — antes do bloco que trata o handoff cooperativo.
// Quando um syscall provoca handoff (did_handoff = true), PC e SP sao
// reescritos para a thread DESTINO e o codigo tem o cuidado explicito de nao
// restaura-los para o chamador (ver o comentario em `else if (syscall == 0x00)`:
// "NAO sobrescrever de volta para o wrapper IPC ... senao o servidor fica preso
// no wait"). Mas R0 ja foi escrito la atras, com o valor de retorno do
// CHAMADOR. Resultado: a thread destino retoma a execucao com R0 corrompido.
//
// POR QUE ISSO IMPORTA PARA PC=0x14
// A cadeia medida e: 0xb0400150 `movs r4, r0` copia R0 para R4; 0xb04001d4
// `ldr r1,[r4]` deriva R1 de R4; 0xb04001e0 `bx r1` salta. Na 2a invocacao
// (a que quebra) o valor observado e 5 — um inteiro pequeno, nao um ponteiro —
// e `bx 5` aterrissa em PC=0x14. Um R0 corrompido no retorno de um handoff
// explica exatamente esse formato de valor.
//
// O QUE ESTE TESTE FAZ
// Monta um cenario minimo em Unicorn, sem firmware: duas "threads" em paginas
// distintas. A thread A executa SVC; o handler emula o comportamento atual
// (handoff: reescreve PC/SP para a thread B, e escreve R0 incondicionalmente).
// Verifica se o R0 que a thread B enxerga e o dela ou o do chamador A.
//
// CONTROLE POSITIVO DO INSTRUMENTO: o mesmo cenario SEM handoff deve preservar
// R0 normalmente — se ate isso falhar, o harness esta quebrado e o RED nao
// significa nada.
//
// RESULTADO NO FIRMWARE REAL: HIPOTESE REFUTADA.
//
// Este teste confirma a MECANICA (se res_r0 != 0, a thread destino e
// corrompida), mas a medicao no boot real mostrou que a pre-condicao nunca
// ocorre. Instrumentando o ramo de handoff num boot de 10 s do AppMgr:
//
//   [PC14/R0] handoff syscall=0x00: R0 preservado=0x00000000
//             destino_pc=0xb0100000 (res_r0 descartado=0x00000000)
//   [PC14/R0] handoff syscall=0x00: R0 preservado=0x00000000
//             destino_pc=0xb0300000 (res_r0 descartado=0x00000000)
//
// Apenas DOIS handoffs em todo o boot, ambos L4_Ipc, e em ambos res_r0 == 0
// e o R0 vivo tambem era 0. A escrita incondicional e, na pratica, um no-op:
// nunca corrompeu registrador nenhum. Aplicar `if (!did_handoff)` nao mudou
// o boot (PC=0x14 persistiu, 1264 ocorrencias, ~7.043.000 insns em Core0
// antes e depois) e o patch foi REVERTIDO — sem causa comprovada, sem patch.
//
// O valor 5 que chega a R4 em 0xb0400150 vem de outro lugar. Proxima linha de
// investigacao: o `stmdb sp!,{r0-r12}` setup-style em 0xb000c3d4, que salva
// r6=5 na transicao de thread 0x8000c001 — a origem do 5 ja foi localizada ali
// em janelas anteriores, mas falta ligar essa pilha salva ao R0 da restauracao.
//
// Este arquivo fica no repo como REFUTACAO registrada: evita que a mesma
// hipotese seja reaberta e "consertada" sem medicao.
//
// Exit 0 = hipotese REFUTADA (R0 preservado). Exit 1 = mecanica confirmada
// no modelo — o que NAO implica que seja a causa no firmware (nao e).

#include <unicorn/unicorn.h>
#include <cstdio>
#include <cstring>
#include <cstdint>

using u32 = uint32_t;

static const u32 CODE_A = 0x10000000; // "thread" chamadora
static const u32 CODE_B = 0x10001000; // "thread" destino do handoff
static const u32 STACK  = 0x20000000;

static const u32 R0_CALLER = 0x00000005; // valor que o handler quer devolver ao chamador
static const u32 R0_TARGET = 0xb0041268; // ponteiro legitimo que a thread B tinha

static bool g_handoff_mode = false;
static bool g_hooked = false;

static void intr_hook(uc_engine* uc, uint32_t intno, void*) {
    if (intno != 2) return; // so SVC
    g_hooked = true;

    u32 res_r0 = R0_CALLER;

    // --- reproducao fiel da ordem atual do zeebo_lle_main.cpp ---
    // A escrita de R0 acontece ANTES do tratamento de handoff.
    uc_reg_write(uc, UC_ARM_REG_R0, &res_r0);          // linha ~3408

    if (g_handoff_mode) {
        // did_handoff == true: PC/SP vao para a thread destino e NAO sao
        // restaurados para o chamador. R0, porem, ja foi sobrescrito acima.
        u32 target_pc = CODE_B;
        u32 target_sp = STACK + 0x800;
        uc_reg_write(uc, UC_ARM_REG_PC, &target_pc);
        uc_reg_write(uc, UC_ARM_REG_SP, &target_sp);
    } else {
        u32 pc = 0;
        uc_reg_read(uc, UC_ARM_REG_PC, &pc);
        uc_reg_write(uc, UC_ARM_REG_PC, &pc);
    }
}

// Executa o cenario e devolve o R0 observado pela thread que retoma.
static u32 run_scenario(bool handoff) {
    g_handoff_mode = handoff;
    g_hooked = false;

    uc_engine* uc = nullptr;
    if (uc_open(UC_ARCH_ARM, UC_MODE_ARM, &uc) != UC_ERR_OK) {
        std::fprintf(stderr, "uc_open falhou\n");
        return 0xdeadbeef;
    }
    uc_mem_map(uc, CODE_A, 0x1000, UC_PROT_ALL);
    uc_mem_map(uc, CODE_B, 0x1000, UC_PROT_ALL);
    uc_mem_map(uc, STACK,  0x1000, UC_PROT_ALL);

    // Thread A: svc #0 ; nop
    u32 a[2] = { 0xef000000, 0xe1a00000 };
    uc_mem_write(uc, CODE_A, a, sizeof(a));
    // Thread B: apenas nops — queremos so inspecionar R0 ao retomar.
    u32 b[2] = { 0xe1a00000, 0xe1a00000 };
    uc_mem_write(uc, CODE_B, b, sizeof(b));

    uc_hook h = 0;
    uc_hook_add(uc, &h, UC_HOOK_INTR, reinterpret_cast<void*>(&intr_hook), nullptr, 1, 0);

    // A thread B "tinha" R0 = ponteiro legitimo antes de ser suspensa.
    u32 r0 = R0_TARGET;
    u32 sp = STACK + 0x400;
    uc_reg_write(uc, UC_ARM_REG_R0, &r0);
    uc_reg_write(uc, UC_ARM_REG_SP, &sp);

    uc_emu_start(uc, CODE_A, CODE_A + 8, 0, 2);

    u32 out = 0;
    uc_reg_read(uc, UC_ARM_REG_R0, &out);
    uc_close(uc);
    return out;
}

int main() {
    std::printf("=== RED: handoff cooperativo preserva R0 da thread destino? ===\n\n");

    // CONTROLE POSITIVO DO INSTRUMENTO ------------------------------------
    // Sem handoff, o handler DEVE entregar res_r0 ao chamador. Se o harness
    // nao conseguir nem observar isso, nada abaixo tem valor.
    u32 no_handoff = run_scenario(false);
    if (!g_hooked) {
        std::printf("INCONCLUSIVO: o hook de SVC nunca disparou — harness quebrado.\n");
        return 2;
    }
    if (no_handoff != R0_CALLER) {
        std::printf("INCONCLUSIVO: controle positivo falhou "
                    "(esperado R0=0x%08x, obtido 0x%08x)\n", R0_CALLER, no_handoff);
        return 2;
    }
    std::printf("[controle positivo] sem handoff: R0 = 0x%08x (esperado) OK\n", no_handoff);

    // MEDICAO -------------------------------------------------------------
    u32 with_handoff = run_scenario(true);
    std::printf("[medicao]           com handoff: R0 = 0x%08x\n", with_handoff);
    std::printf("                    R0 da thread destino era 0x%08x\n\n", R0_TARGET);

    if (with_handoff == R0_CALLER) {
        std::printf("HIPOTESE CONFIRMADA: apos o handoff, a thread destino retoma com\n");
        std::printf("o R0 do CHAMADOR (0x%08x), nao o seu (0x%08x).\n", R0_CALLER, R0_TARGET);
        std::printf("Valor pequeno em R0 -> `movs r4,r0` -> `ldr r1,[r4]` -> `bx r1`\n");
        std::printf("e o formato exato da falha PC=0x14.\n");
        return 1; // RED
    }
    std::printf("HIPOTESE REFUTADA: R0 preservado (0x%08x). A causa do PC=0x14\n", with_handoff);
    std::printf("esta em outro lugar; nao patchear a escrita de R0.\n");
    return 0;
}
