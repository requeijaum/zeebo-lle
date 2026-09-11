// test_applet_dispatch.cpp — Bug 4 regression: seleção honesta de manipulador
// de ciclo de vida por módulo.
//
// Provas:
//  POSITIVO 1: applet Z-Wheel explícito (274755) mantém o manipulador ZeeboApp
//              fixo 0x10532344 (preview/harness preservado).
//  POSITIVO 2: módulo não-Z-Wheel com manipulador real resolvido E mapeado usa
//              o SEU próprio handler (não o fixo).
//  NEGATIVO 1: módulo não-Z-Wheel sem manipulador resolvido (entry_va=0)
//              permanece loaded_only (REJECT), nunca cai no handler fixo.
//  NEGATIVO 2: módulo não-Z-Wheel cujo handler resolvido é o próprio 0x10532344
//              (empréstimo proibido) é REJEITADO.
//  NEGATIVO 3: módulo não-Z-Wheel com handler resolvido mas NÃO mapeado é
//              REJEITADO (loaded_only), nunca PASS azul.
//
// Mutante que reintroduz o despacho fixo (forçar 0x10532344 para todo módulo)
// torna os NEGATIVOS vermelhos.
#include <cassert>
#include <cstdio>
#include "zeebo_applet_dispatch.h"

using namespace zeebo::applet;

int main() {
    int failures = 0;
    auto expect = [&](bool cond, const char* name) {
        if (cond) { printf("  [PASS] %s\n", name); }
        else      { printf("  [FAIL] %s\n", name); ++failures; }
    };

    printf("== Bug 4: seleção honesta de manipulador por módulo ==\n");

    // POSITIVO 1: Z-Wheel explícita → handler fixo preservado.
    {
        auto d = select_lifecycle_handler(/*is_zwheel=*/true,
                                          /*requested=*/0, /*mapped=*/false);
        expect(d.mode == DISPATCH_ZWHEEL && d.handler_va == ZWHEEL_HANDLER_VA,
               "Z-Wheel explícita usa manipulador ZeeboApp 0x10532344");
    }

    // POSITIVO 2: módulo real resolvido e mapeado → usa o próprio handler.
    {
        const u32 mod_handler = 0x12000401u; // entry Thumb do módulo (!= fixo)
        auto d = select_lifecycle_handler(/*is_zwheel=*/false,
                                          mod_handler, /*mapped=*/true);
        expect(d.mode == DISPATCH_MODULE && d.handler_va == mod_handler,
               "módulo não-Z-Wheel usa seu manipulador real resolvido");
        expect(d.handler_va != ZWHEEL_HANDLER_VA,
               "módulo real NÃO usa o handler fixo da Z-Wheel");
    }

    // NEGATIVO 1: entry não resolvido → loaded_only.
    {
        auto d = select_lifecycle_handler(/*is_zwheel=*/false,
                                          /*requested=*/0, /*mapped=*/false);
        expect(d.mode == DISPATCH_REJECT && d.handler_va == 0u,
               "módulo sem entry resolvido permanece loaded_only (REJECT)");
    }

    // NEGATIVO 2: empréstimo do handler fixo proibido.
    {
        auto d = select_lifecycle_handler(/*is_zwheel=*/false,
                                          ZWHEEL_HANDLER_VA, /*mapped=*/true);
        expect(d.mode == DISPATCH_REJECT,
               "módulo não pode emprestar 0x10532344 da Z-Wheel (REJECT)");
    }

    // NEGATIVO 3: handler resolvido mas não mapeado → loaded_only.
    {
        auto d = select_lifecycle_handler(/*is_zwheel=*/false,
                                          0x12000401u, /*mapped=*/false);
        expect(d.mode == DISPATCH_REJECT,
               "handler resolvido mas não mapeado permanece loaded_only (REJECT)");
    }

    if (failures == 0) {
        printf("PASS: seleção honesta de manipulador por módulo (Bug 4).\n");
        return 0;
    }
    printf("FAIL: %d asserção(ões) de seleção de manipulador falharam.\n", failures);
    return 1;
}
