// zeebo_bootinfo_harness.cpp — QW1: harness de BootInfo/fpage por bytes reais.
// ---------------------------------------------------------------------------
// HONESTIDADE DE ESCOPO (repair QW1):
//   Este harness prova DUAS coisas com níveis de evidência DISTINTOS:
//
//   (A) FIRMWARE-DERIVED (provado por BYTES reais da NAND):
//       O bloco __okl4_bootinfo do 1.1.2_APPS.bin (@0x57000) e seus records
//       reais — BI_TAG_EMPTY (magic 0x1960021d), 10x VIRT_POOLS(5) e
//       5x PHYS_POOLS(6). Provado por MUTAÇÃO: alterar os bytes de origem
//       muda/quebra a saída do parser (ver test-bootinfo-derivation).
//
//   (B) MEMPOOL HYPOTHESIS (NÃO derivado do firmware — hipótese explícita):
//       A geração de 96 fpages de 1 MiB em 0xb0d00000..0xb6d00000 é ARITMÉTICA
//       sobre duas constantes fixas. O valor 0xb6d00000 NÃO aparece em nenhum
//       record, byte ou descriptor do firmware, e NENHUM VIRT_POOLS cobre
//       96 MiB a partir de 0xb0d00000 (o pool virtual real mais próximo é
//       0xb0d02000..0xb0dfffff, ~1 MiB). Portanto esta seção é rotulada como
//       hipótese esperada do mempool_init (@0xb000d5b4) e NÃO fecha a parte
//       "fpage firmware-derived" da QW1. Mutar o firmware NÃO altera as 96
//       fpages — prova de que a geração é independente dos bytes.
//
// Validação por BYTES reais — nunca por contagem de instruções, sem Unicorn e
// sem forçar registradores do guest. Clean-room: apenas leitura da cópia de
// trabalho da NAND (o dump original permanece read-only).
//
// GATE HONESTO: se o firmware real estiver ausente, o harness FALHA com exit
// não-zero (não faz SKIP silencioso). O alvo `make test-bootinfo` exige os
// bytes reais; sem eles o CI fica vermelho, nunca falso-verde.
// ---------------------------------------------------------------------------
#include "zeebo_bootinfo.h"

#include <cstdio>
#include <cstdlib>
#include <vector>
#include <string>

using zeebo_bi::u8;
using zeebo_bi::u32;
using zeebo_bi::u64;

static int g_fail = 0;
#define CHECK(cond) do { \
    if (!(cond)) { printf("  FAIL:%d  %s\n", __LINE__, #cond); ++g_fail; } \
    else         { printf("  ok  :%d  %s\n", __LINE__, #cond); } \
} while (0)

static std::vector<u8> read_file(const char* path) {
    FILE* f = std::fopen(path, "rb");
    if (!f) return {};
    std::fseek(f, 0, SEEK_END);
    long n = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    std::vector<u8> buf((size_t)(n < 0 ? 0 : n));
    if (!buf.empty()) { size_t r = std::fread(buf.data(), 1, buf.size(), f); (void)r; }
    std::fclose(f);
    return buf;
}

int main(int argc, char** argv) {
    const char* apps_path = (argc > 1) ? argv[1] : "../../nand/1.1.2_APPS.bin";
    std::vector<u8> apps = read_file(apps_path);
    if (apps.empty()) {
        // GATE HONESTO: sem os bytes reais não há o que validar. Falha com
        // exit não-zero para não produzir falso-verde no CI. Para ambientes
        // sem a NAND, use um alvo separado que NÃO faça parte do `check`.
        printf("FAIL: firmware APPS real ausente em %s\n", apps_path);
        printf("      QW1 exige os bytes reais da NAND (cópia de trabalho).\n");
        printf("      Aponte BOOTINFO_APPS para nand/1.1.2_APPS.bin.\n");
        return 2;
    }
    printf("[QW1] APPS firmware: %s (%zu bytes)\n", apps_path, apps.size());

    // === (A) FIRMWARE-DERIVED — provado por bytes reais ====================
    // 1) Localizar o BootInfo pela magic real e verificar o record BI_TAG_EMPTY.
    zeebo_bi::BootInfo bi;
    bool found = zeebo_bi::locate(apps.data(), apps.size(), bi);
    CHECK(found);
    CHECK(bi.base_offset == 0x57000);
    CHECK(bi.magic == 0x1960021d);
    CHECK(bi.records.size() > 0);
    CHECK(bi.records[0].type == zeebo_bi::BI_TAG_EMPTY);
    CHECK(bi.records[0].len  == 16);
    CHECK(bi.records[0].payload.size() == 3);
    CHECK(bi.records[0].payload[0] == 0x1960021d);

    // 2) Enumerar VIRT_POOLS / PHYS_POOLS reais (pares byte-a-byte).
    auto virt = bi.pools(zeebo_bi::BI_TAG_VIRT_POOLS);
    auto phys = bi.pools(zeebo_bi::BI_TAG_PHYS_POOLS);
    CHECK(virt.size() == 10);
    CHECK(phys.size() == 5);
    CHECK(virt[0].base == 0x16e00000 && virt[0].end == 0x7fffffff);
    CHECK(phys[0].base == 0x14954000 && phys[0].end == 0x155fffff);
    CHECK(virt[1].base == 0x80100000 && virt[1].end == 0xafffffff);
    // Nenhum VIRT_POOLS real cobre 96 MiB a partir de 0xb0d00000: o pool
    // virtual mais próximo desse VA tem ~1 MiB. Prova de que o range de 96 MiB
    // NÃO está codificado em nenhum record.
    bool any_96mib_pool = false;
    for (const auto& p : virt) {
        if (p.base == 0xb0d00000u && (u64)p.end - p.base + 1 >= 0x6000000u)
            any_96mib_pool = true;
    }
    CHECK(!any_96mib_pool);  // 96 MiB @0xb0d00000 NÃO é firmware-derived.

    // 3) PROVA DE DERIVAÇÃO POR MUTAÇÃO (in-memory): alterar os bytes da base
    //    do VIRT_POOLS[0] muda a saída do parser — confirma que a validação
    //    segue os bytes reais e não constantes hardcoded.
    {
        std::vector<u8> mut = apps;
        // VIRT_POOLS[0].base fica em file offset 0x57014 (record @0x57010 +4).
        mut[0x57014] = 0x00; mut[0x57015] = 0x00;
        mut[0x57016] = 0x00; mut[0x57017] = 0xde;  // base -> 0xde000000
        zeebo_bi::BootInfo bi2;
        CHECK(zeebo_bi::locate(mut.data(), mut.size(), bi2));
        auto virt2 = bi2.pools(zeebo_bi::BI_TAG_VIRT_POOLS);
        CHECK(virt2.size() == 10);
        CHECK(virt2[0].base == 0xde000000u);        // mutação refletida
        CHECK(virt2[0].base != virt[0].base);        // difere do original
    }
    // Mutar a magic quebra a localização — a estrutura é byte-verificada.
    {
        std::vector<u8> mut = apps;
        mut[0x57004] ^= 0xff;  // corrompe a magic 0x1960021d
        zeebo_bi::BootInfo bi3;
        // A magic corrompida faz o locate falhar (não há outra ocorrência).
        CHECK(!zeebo_bi::locate(mut.data(), mut.size(), bi3));
    }

    // === (B) MEMPOOL HYPOTHESIS — NÃO firmware-derived =====================
    // A geração abaixo é aritmética sobre constantes fixas; reproduz a hipótese
    // de mempool_init (@0xb000d5b4) mas NÃO é provada pelos bytes do firmware.
    printf("[QW1] (hipotese mempool_init, NAO firmware-derived) 96x1MiB fpages\n");
    auto fps = zeebo_bi::enumerate_fpages(0xb0d00000u, 0xb6d00000u,
                                          /*page_log2=*/20, /*rwx=*/6);
    CHECK(fps.size() == 96);
    CHECK(fps.front().raw == 0xb0d00146u);
    CHECK(fps.front().vaddr() == 0xb0d00000u);
    CHECK(fps.front().size_bytes() == (1u << 20));
    CHECK(fps.back().vaddr() == 0xb6c00000u);
    CHECK(fps.back().raw == 0xb6c00146u);
    u32 covered_end = (u32)(fps.back().vaddr() + fps.back().size_bytes());
    CHECK(covered_end == 0xb6d00000u);
    // PROVA de que é hipótese e NÃO derivação: mutar o firmware inteiro NÃO
    // altera as 96 fpages (elas não leem byte algum do firmware).
    {
        std::vector<u8> mut = apps;
        for (size_t i = 0x57000; i < 0x58000 && i < mut.size(); ++i) mut[i] = 0xa5;
        auto fps2 = zeebo_bi::enumerate_fpages(0xb0d00000u, 0xb6d00000u, 20, 6);
        CHECK(fps2.size() == fps.size());
        CHECK(fps2.front().raw == fps.front().raw);  // independente dos bytes
    }

    if (g_fail == 0) {
        printf("[QW1] PASS — records/pools firmware-derived (bytes+mutacao); "
               "96-fpage rotulado como hipotese mempool (NAO fecha essa parte da QW1)\n");
    } else {
        printf("[QW1] %d verificacoes falharam\n", g_fail);
    }
    return g_fail ? 1 : 0;
}
