// zeebo_uc_exec.h — Bug 4: prova REAL de permissão executável sob Unicorn.
//
// O gate honesto de despacho de manipulador (zeebo_applet_dispatch.h) exige que
// o VA do handler esteja "mapeado/executável". A versão anterior aferia isso com
// host uc_mem_read(), que só prova LEGIBILIDADE — uma página mapeada READ-only /
// non-exec passaria como "mapped", furando a proteção do guest (fake-PASS).
//
// Aqui a checagem é feita via uc_mem_regions(): consultamos as regiões reais
// mapeadas no engine e exigimos que TODO o intervalo [va, va+len) esteja coberto
// por regiões que possuam UC_PROT_EXEC. Sem cobertura contígua ou sem o bit de
// execução em qualquer sub-intervalo → NÃO executável (rejeição honesta).
#pragma once
#include <cstdint>
#include <unicorn/unicorn.h>

namespace zeebo {
namespace applet {

// Retorna true SOMENTE se todo o intervalo [addr, addr+len) estiver mapeado por
// regiões do guest e cada byte do intervalo tiver o bit UC_PROT_EXEC. Cobertura
// parcial, buraco não mapeado, ou ausência de EXEC em qualquer trecho → false.
inline bool uc_range_is_executable(uc_engine* uc, uint64_t addr, uint64_t len) {
    if (!uc || len == 0) return false;
    // Guarda overflow do fim do intervalo.
    if (addr + len < addr) return false;
    const uint64_t last = addr + len - 1;

    uc_mem_region* regions = nullptr;
    uint32_t count = 0;
    if (uc_mem_regions(uc, &regions, &count) != UC_ERR_OK) return false;

    // Varre o intervalo exigindo que cada byte pertença a uma região EXEC. Como
    // as regiões do Unicorn não se sobrepõem, avançamos o cursor pela região que
    // cobre a posição atual; se nenhuma cobrir, ou faltar EXEC, falha.
    bool ok = true;
    uint64_t cur = addr;
    while (cur <= last) {
        bool covered = false;
        for (uint32_t i = 0; i < count; ++i) {
            // uc_mem_region: [begin, end] inclusivo.
            if (cur >= regions[i].begin && cur <= regions[i].end) {
                if ((regions[i].perms & UC_PROT_EXEC) == 0) { ok = false; }
                // Avança até o fim desta região (ou fim do intervalo).
                cur = regions[i].end;
                covered = true;
                break;
            }
        }
        if (!covered || !ok) { ok = false; break; }
        if (cur == UINT64_MAX) break; // evita overflow no ++
        ++cur;
    }
    uc_free(regions);
    return ok;
}

} // namespace applet
} // namespace zeebo
