// test_brew_mod_entry.cpp — Bug 3 regression: carregamento seguro de MOD ARM cru
// (BREW/elf2mod) e ambiente de prefixo.
//
// Um `.mod` BREW é um executável ARMv5 PIC puro cujo AEEMod_Load é a primeira
// função (linker --entry=AEEMod_Load); pode ser carregado em qualquer base
// >0x1000. Frequentemente a 1ª palavra é `b AEEMod_Load`. Variantes com header
// ELF ARM (super-ELF/SDK) usam e_entry. O ambiente elf2mod reserva duas palavras
// em load_base-8 e load_base-4.
//
// Este teste cobre: (1) decode seguro de branch ARM B/BL, (2) resolução de
// entry para MOD cru com/sem branch inicial e para ELF, (3) rejeição de alvos
// fora de bounds / na região reservada, (4) preparo do prefixo elf2mod na RAM
// guest via Unicorn com bounds e entrada inválida.
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>
#include <unicorn/unicorn.h>
#include "zeebo_brew_loader.h"

using zeebo::brew::BrewLoader;
using u8  = uint8_t;
using u32 = uint32_t;

static void put32(std::vector<u8>& b, size_t off, u32 v) {
    if (off + 4 > b.size()) b.resize(off + 4, 0);
    std::memcpy(b.data() + off, &v, 4);
}

// Codifica um branch ARM incondicional (B/BL) de `insn_va` para `target`.
static u32 enc_b(u32 insn_va, u32 target, bool link = false) {
    int32_t off = (int32_t)((int64_t)target - (int64_t)insn_va - 8) >> 2;
    u32 imm24 = (u32)off & 0x00FFFFFF;
    return (link ? 0xEB000000u : 0xEA000000u) | imm24;
}

int main() {
    std::printf("=== Test BREW MOD entry / elf2mod prefix (Bug 3) ===\n");

    const u32 LB = 0x10200000; // base típica (>0x1000)

    // (1) decode_arm_branch: forward B.
    {
        u32 tgt = 0;
        u32 w = enc_b(LB, LB + 0x40);
        assert(BrewLoader::decode_arm_branch(w, LB, &tgt));
        assert(tgt == LB + 0x40);
        // BL também.
        u32 wl = enc_b(LB, LB + 0x1000, true);
        assert(BrewLoader::decode_arm_branch(wl, LB, &tgt));
        assert(tgt == LB + 0x1000);
        // Não-branch (ex.: mov r0,r0) => false.
        assert(!BrewLoader::decode_arm_branch(0xE1A00000u, LB, &tgt));
        // Branch condicional (cond != AL) NÃO conta como entry de prólogo.
        assert(!BrewLoader::decode_arm_branch(0x0A000000u, LB, &tgt)); // BEQ
    }

    // (2a) MOD cru com `b AEEMod_Load` inicial -> entry = alvo.
    {
        std::vector<u8> d(0x200, 0);
        put32(d, 0, enc_b(LB, LB + 0x80));
        u32 kind = 0;
        u32 e = BrewLoader::resolve_mod_entry(d, LB, &kind);
        assert(e == LB + 0x80);
        assert(kind == zeebo::brew::ENTRY_RAW_BRANCH);
    }

    // (2b) MOD cru SEM branch inicial -> AEEMod_Load começa direto no load_va.
    {
        std::vector<u8> d(0x200, 0);
        put32(d, 0, 0xE92D4010); // push {r4, lr} — prólogo real, não-branch
        u32 kind = 0;
        u32 e = BrewLoader::resolve_mod_entry(d, LB, &kind);
        assert(e == LB);
        assert(kind == zeebo::brew::ENTRY_RAW_START);
    }

    // (2c) ELF ARM: usa e_entry (VA absoluta dentro do módulo).
    {
        std::vector<u8> d(0x40, 0);
        d[0]=0x7f; d[1]='E'; d[2]='L'; d[3]='F';
        put32(d, 24, LB + 0x20); // e_entry
        u32 kind = 0;
        u32 e = BrewLoader::resolve_mod_entry(d, LB, &kind);
        assert(e == LB + 0x20);
        assert(kind == zeebo::brew::ENTRY_ELF);
    }

    // (3a) Branch que aponta PARA FORA do módulo (além do fim) -> rejeitado.
    {
        std::vector<u8> d(0x100, 0);
        put32(d, 0, enc_b(LB, LB + 0x4000)); // muito além do fim (0x100)
        u32 kind = 0xdead;
        u32 e = BrewLoader::resolve_mod_entry(d, LB, &kind);
        assert(e == 0);
        assert(kind == zeebo::brew::ENTRY_NONE);
    }

    // (3b) Branch cujo alvo cai na região reservada [load_va-8, load_va) ->
    // rejeitado (é dado do prefixo elf2mod, não código).
    {
        std::vector<u8> d(0x100, 0);
        put32(d, 0, enc_b(LB, LB - 4)); // aponta p/ palavra reservada hi
        u32 kind = 0xdead;
        u32 e = BrewLoader::resolve_mod_entry(d, LB, &kind);
        assert(e == 0);
        assert(kind == zeebo::brew::ENTRY_NONE);
    }

    // (3c) Branch com underflow de 32 bits (base baixa, deslocamento negativo
    // grande) não deve estourar; decode retorna false ou entry rejeitado.
    {
        std::vector<u8> d(0x100, 0);
        put32(d, 0, 0xEA800000u); // imm24 = 0x800000 (mais negativo)
        u32 kind = 0xdead;
        u32 e = BrewLoader::resolve_mod_entry(d, 0x10, &kind); // base baixa
        assert(e == 0);
        assert(kind == zeebo::brew::ENTRY_NONE);
    }

    // (3d) Entrada inválida: vazia e curta.
    {
        std::vector<u8> empty;
        assert(BrewLoader::resolve_mod_entry(empty, LB, nullptr) == 0);
        std::vector<u8> two(2, 0);
        assert(BrewLoader::resolve_mod_entry(two, LB, nullptr) == 0);
    }

    // (4) Ambiente de prefixo elf2mod na RAM guest via Unicorn.
    {
        uc_engine* uc = nullptr;
        uc_err oe = uc_open(UC_ARCH_ARM, UC_MODE_ARM, &uc);
        assert(oe == UC_ERR_OK && uc);
        BrewLoader ld(uc);

        // Injeta um MOD cru com branch inicial: entry resolvido + prefixo pronto.
        std::vector<u8> mod(0x400, 0);
        put32(mod, 0, enc_b(LB, LB + 0xC0));
        assert(ld.inject_bytes(mod, LB, 0, "raw.mod"));
        const auto& m = ld.module();
        assert(m.injected);
        assert(m.entry_va == LB + 0xC0);
        assert(m.entry_kind == zeebo::brew::ENTRY_RAW_BRANCH);
        // Prefixo reservado e mapeado: duas palavras em LB-8 e LB-4.
        assert(m.prefix_ready);
        assert(m.reserved_lo_va == LB - 8);
        assert(m.reserved_hi_va == LB - 4);
        u32 w = 0xffffffff;
        assert(uc_mem_read(uc, LB - 8, &w, 4) == UC_ERR_OK && w == 0);
        assert(uc_mem_read(uc, LB - 4, &w, 4) == UC_ERR_OK && w == 0);

        // Escrita explícita de palavras de prefixo (ex.: pIShell, pModule).
        assert(ld.prepare_elf2mod_prefix(LB, 0xAABBCCDD, 0x11223344));
        assert(uc_mem_read(uc, LB - 8, &w, 4) == UC_ERR_OK && w == 0xAABBCCDD);
        assert(uc_mem_read(uc, LB - 4, &w, 4) == UC_ERR_OK && w == 0x11223344);

        // Bounds: base < 8 não tem espaço p/ prefixo -> false honesto, sem crash.
        assert(!ld.prepare_elf2mod_prefix(4));
        assert(!ld.prepare_elf2mod_prefix(0));

        uc_close(uc);
    }

    std::printf("=== Test BREW MOD entry / elf2mod prefix: PASS ===\n");
    return 0;
}
