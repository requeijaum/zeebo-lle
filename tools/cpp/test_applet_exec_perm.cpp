// test_applet_exec_perm.cpp — Bug 4 (gap fechado): prova REAL, sob Unicorn, de
// que o gate de despacho exige permissão de EXECUÇÃO do guest, não apenas
// legibilidade de host.
//
// Provas:
//   NEGATIVO 1: página do handler mapeada READ-only/non-exec (UC_PROT_READ):
//               uc_mem_read HOST SUCEDE (legível), mas uc_range_is_executable
//               REJEITA — e o gate honesto vira REJECT (loaded_only).
//   NEGATIVO 2: página RW (READ|WRITE, sem EXEC): também REJEITADA.
//   NEGATIVO 3: endereço NÃO mapeado: REJEITADO.
//   NEGATIVO 4: intervalo que cruza para uma página não mapeada (cobertura
//               parcial): REJEITADO.
//   POSITIVO 1: página RX (READ|EXEC): uc_range_is_executable ACEITA e o gate
//               despacha o manipulador REAL do módulo (DISPATCH_MODULE).
//   POSITIVO 2: página RWX (UC_PROT_ALL): ACEITA.
//
// Mutante que substitui uc_range_is_executable por "uc_mem_read == UC_ERR_OK"
// (a lógica antiga) torna NEGATIVO 1 e NEGATIVO 2 VERMELHOS: uma página legível
// mas não executável passaria a fake-PASS.
#include <cassert>
#include <cstdio>
#include <cstring>
#include <unicorn/unicorn.h>
#include "zeebo_uc_exec.h"
#include "zeebo_applet_dispatch.h"

using namespace zeebo::applet;
using u8  = uint8_t;
using u64 = uint64_t;

int main() {
    int failures = 0;
    auto expect = [&](bool cond, const char* name) {
        if (cond) { printf("  [PASS] %s\n", name); }
        else      { printf("  [FAIL] %s\n", name); ++failures; }
    };

    printf("== Bug 4: prova REAL de permissão executável sob Unicorn ==\n");

    uc_engine* uc = nullptr;
    if (uc_open(UC_ARCH_ARM, UC_MODE_ARM, &uc) != UC_ERR_OK || !uc) {
        printf("FAIL: uc_open falhou.\n");
        return 1;
    }

    // Handler candidato do módulo (Thumb bit set, como no fluxo real).
    const u32 MOD_HANDLER = 0x12000401u;
    const u32 PAGE        = MOD_HANDLER & ~0xFFFu; // 0x12000000

    // --- NEGATIVO 1: página READ-only/non-exec ---
    {
        uc_mem_map(uc, PAGE, 0x1000, UC_PROT_READ);
        // A página é LEGÍVEL por host: a checagem antiga (uc_mem_read) passaria.
        u8 probe[2] = {0};
        bool host_readable =
            (uc_mem_read(uc, MOD_HANDLER & ~1u, probe, 2) == UC_ERR_OK);
        expect(host_readable,
               "página RO é legível por host (uc_mem_read OK) — antiga lógica passaria");

        bool exec = uc_range_is_executable(uc, MOD_HANDLER & ~1u, 2);
        expect(!exec,
               "página READ-only NÃO é executável (uc_range_is_executable REJEITA)");

        auto d = select_lifecycle_handler(/*is_zwheel=*/false, MOD_HANDLER,
                                          /*handler_mapped=*/exec);
        expect(d.mode == DISPATCH_REJECT,
               "gate honesto: handler RO permanece loaded_only (REJECT)");
        uc_mem_unmap(uc, PAGE, 0x1000);
    }

    // --- NEGATIVO 2: página RW (sem EXEC) ---
    {
        uc_mem_map(uc, PAGE, 0x1000, UC_PROT_READ | UC_PROT_WRITE);
        bool exec = uc_range_is_executable(uc, MOD_HANDLER & ~1u, 2);
        expect(!exec, "página RW (sem EXEC) NÃO é executável (REJEITA)");
        auto d = select_lifecycle_handler(false, MOD_HANDLER, exec);
        expect(d.mode == DISPATCH_REJECT, "gate: handler RW → loaded_only (REJECT)");
        uc_mem_unmap(uc, PAGE, 0x1000);
    }

    // --- NEGATIVO 3: endereço NÃO mapeado ---
    {
        bool exec = uc_range_is_executable(uc, MOD_HANDLER & ~1u, 2);
        expect(!exec, "endereço não mapeado NÃO é executável (REJEITA)");
    }

    // --- NEGATIVO 4: cobertura parcial (cruza para página não mapeada) ---
    {
        uc_mem_map(uc, PAGE, 0x1000, UC_PROT_READ | UC_PROT_EXEC);
        // Intervalo que começa no fim da página RX e transborda para 0x12001000
        // (não mapeada): cobertura parcial deve REJEITAR.
        u64 start = PAGE + 0x1000 - 2; // últimos 2 bytes da página RX
        bool exec = uc_range_is_executable(uc, start, 8); // atravessa o limite
        expect(!exec, "intervalo com cobertura parcial (cruza p/ não mapeada) REJEITA");
        uc_mem_unmap(uc, PAGE, 0x1000);
    }

    // --- POSITIVO 1: página RX (READ|EXEC) ---
    {
        uc_mem_map(uc, PAGE, 0x1000, UC_PROT_READ | UC_PROT_EXEC);
        bool exec = uc_range_is_executable(uc, MOD_HANDLER & ~1u, 2);
        expect(exec, "página RX (READ|EXEC) É executável (ACEITA)");
        auto d = select_lifecycle_handler(false, MOD_HANDLER, exec);
        expect(d.mode == DISPATCH_MODULE && d.handler_va == MOD_HANDLER,
               "gate: handler RX despacha manipulador REAL do módulo (DISPATCH_MODULE)");
        uc_mem_unmap(uc, PAGE, 0x1000);
    }

    // --- POSITIVO 2: página RWX ---
    {
        uc_mem_map(uc, PAGE, 0x1000, UC_PROT_ALL);
        bool exec = uc_range_is_executable(uc, MOD_HANDLER & ~1u, 2);
        expect(exec, "página RWX (UC_PROT_ALL) É executável (ACEITA)");
        uc_mem_unmap(uc, PAGE, 0x1000);
    }

    uc_close(uc);

    if (failures == 0) {
        printf("PASS: gate exige permissão EXEC real do guest (Bug 4 gap fechado).\n");
        return 0;
    }
    printf("FAIL: %d asserção(ões) de permissão executável falharam.\n", failures);
    return 1;
}
