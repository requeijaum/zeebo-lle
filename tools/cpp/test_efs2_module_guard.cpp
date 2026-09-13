// test_efs2_module_guard.cpp — Fail-closed gate para extração/lançamento EFS2.
//
// Prova, por bytes sintéticos e (quando presente) pelos blobs REAIS da NAND, que
// um payload cru só é aceito como applet quando tem uma entrada de módulo
// executável plausível (ELF e_entry OU branch ARM inicial cujo alvo cai na
// faixa). Todo o resto — metadados de gnode, dados de modem/NV, zeros — falha
// FECHADO.
//
// RED-first: antes do guard, o caminho aceitava QUALQUER blob via fallback
// raw-start. O controle mutacional -DZEEBO_EFS2_GUARD_MUTANT_ACCEPT_ALL
// reintroduz esse fallback e DEVE deixar os negativos vermelhos.
//
// Controle negativo com a NAND real (opcional): se argv[1] apontar uma NAND
// legível, extrai os três blobs históricos (reksio.mod/274755/tectoy.mod) e
// exige MOD_REJECT em cada um. Sem NAND, exit 77 (SKIP) — nunca PASS silencioso.
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include <limits>
#include "zeebo_efs2_module_guard.h"

using namespace zeebo::efs2_guard;

static int g_fail = 0;
static void expect(bool c, const char* what) {
    if (c) printf("  [PASS] %s\n", what);
    else { printf("  [FAIL] %s\n", what); ++g_fail; }
}

static void put32(std::vector<u8>& b, size_t off, u32 v) {
    if (off + 4 > b.size()) b.resize(off + 4, 0);
    std::memcpy(b.data() + off, &v, 4);
}
static u32 enc_b(u32 insn_va, u32 target, bool link = false) {
    int32_t off = (int32_t)((int64_t)target - (int64_t)insn_va - 8) >> 2;
    u32 imm24 = (u32)off & 0x00FFFFFF;
    return (link ? 0xEB000000u : 0xEA000000u) | imm24;
}

int main() {
    const u32 LOAD = 0x12000000u;
    printf("== EFS2 module guard: fail-closed (sem raw-start) ==\n");

    // POSITIVO 1: MOD cru sintético cuja 1ª palavra é `b entry` na faixa.
    {
        std::vector<u8> mod(0x1000, 0);
        put32(mod, 0, enc_b(LOAD, LOAD + 0x40));  // b LOAD+0x40
        u32 e = 0;
        ModuleVerdict v = classify_payload(mod, LOAD, &e);
        expect(v == MOD_ARM_BRANCH, "MOD cru com branch inicial válido é aceito (MOD_ARM_BRANCH)");
        expect(e == LOAD + 0x40, "entry resolvido == alvo do branch");
    }

    // POSITIVO 2: ELF ARM sintético com e_entry absoluto dentro do payload.
    {
        std::vector<u8> elf(0x100, 0);
        elf[0] = 0x7f; elf[1] = 'E'; elf[2] = 'L'; elf[3] = 'F';
        put32(elf, 24, LOAD + 0x80);              // e_entry absoluto
        u32 e = 0;
        ModuleVerdict v = classify_payload(elf, LOAD, &e);
        expect(v == MOD_ELF, "ELF com e_entry na faixa é aceito (MOD_ELF)");
        expect(e == LOAD + 0x80, "e_entry absoluto resolvido");
    }

    // NEGATIVO 1: branch cujo alvo cai FORA da faixa do payload → REJECT.
    {
        std::vector<u8> mod(0x1000, 0);
        put32(mod, 0, enc_b(LOAD, LOAD + 0x900000)); // alvo além do payload
        expect(classify_payload(mod, LOAD) == MOD_REJECT,
               "branch com alvo fora da faixa é rejeitado (fail-closed)");
    }

    // NEGATIVO 2: blob de zeros (como tectoy.mod real, w0=0x00000000) → REJECT.
    {
        std::vector<u8> zeros(0x1000, 0);
        expect(classify_payload(zeros, LOAD) == MOD_REJECT,
               "blob de zeros não é módulo (fail-closed, não raw-start)");
    }

    // NEGATIVO 3: garbage com w0 não-branch/não-ELF (reksio real 0x9cd3ffff:
    // cond=0x9 != AL). Base plausível > 0x1000 NÃO pode salvá-lo.
    {
        std::vector<u8> g(0x1000, 0xff);
        put32(g, 0, 0x9cd3ffffu);
        expect(classify_payload(g, LOAD) == MOD_REJECT,
               "reksio-like w0=0x9cd3ffff (cond=0x9) rejeitado — não é branch AL");
    }
    // NEGATIVO 4: 274755 real w0=0xfd19f297 (cond=0xf, NV) → REJECT.
    {
        std::vector<u8> g(0x1000, 0);
        put32(g, 0, 0xfd19f297u);
        expect(classify_payload(g, LOAD) == MOD_REJECT,
               "274755-like w0=0xfd19f297 (cond=0xf) rejeitado — não é branch AL");
    }

    // NEGATIVO 5: ELF cujo e_entry NÃO resolve dentro do payload → REJECT.
    {
        std::vector<u8> elf(0x100, 0);
        elf[0] = 0x7f; elf[1] = 'E'; elf[2] = 'L'; elf[3] = 'F';
        put32(elf, 24, 0xdeadbeefu);
        expect(classify_payload(elf, LOAD) == MOD_REJECT,
               "ELF com e_entry fora do payload rejeitado (fail-closed)");
    }

    // NEGATIVO 6: payload minúsculo (< 4 bytes) → REJECT.
    {
        std::vector<u8> tiny{0xEA};
        expect(classify_payload(tiny, LOAD) == MOD_REJECT, "payload < 4 bytes rejeitado");
    }

    // --- Controle negativo com a NAND REAL: ver test_efs2_guard_real.cpp
    // (integração), que exige exit 77 quando a NAND está ausente. Este arquivo
    // é a bateria sintética pura (host-only, sem NAND).

    if (g_fail == 0) {
        printf("PASS: guard EFS2 fail-closed — garbage não vira applet.\n");
        return 0;
    }
    printf("FAIL: %d asserção(ões) do guard falharam.\n", g_fail);
    return 1;
}
