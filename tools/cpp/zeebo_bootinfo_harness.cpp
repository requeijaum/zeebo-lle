// zeebo_bootinfo_harness.cpp — QW1: harness isolado de BootInfo/fpage.
// ---------------------------------------------------------------------------
// Lê o firmware REAL da APPS (cópia de trabalho da NAND, `nand/1.1.2_APPS.bin`),
// localiza o bloco __okl4_bootinfo, verifica a magic BI_TAG_EMPTY (0x1960021d),
// enumera as tags BI_TAG_VIRT_POOLS (5) / BI_TAG_PHYS_POOLS (6) e decompõe
// deterministicamente o pool virtual do Iguana em fpages de 1 MiB de VA
// 0xb0d00000 até 0xb6d00000 (96 páginas), reutilizando `zeebo_l4::Fpage`.
//
// Validação por BYTES reais do firmware — nunca por contagem de instruções e
// sem forçar registradores do guest. Clean-room: apenas leitura da cópia de
// trabalho da NAND (o dump original permanece read-only).
//
// Âncoras (file offset em 1.1.2_APPS.bin):
//   0x57000  BI_TAG_EMPTY  magic=0x1960021d
//   0x57010  VIRT_POOLS    0x16e00000..0x7fffffff  (primeiro par)
//   0x5701c  PHYS_POOLS    0x14954000..0x155fffff
// ---------------------------------------------------------------------------
#include "zeebo_bootinfo.h"

#include <cstdio>
#include <cstdlib>
#include <vector>
#include <string>

using zeebo_bi::u8;
using zeebo_bi::u32;

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
        printf("SKIP: APPS firmware não encontrado em %s (cópia de trabalho da NAND ausente)\n", apps_path);
        return 0; // ambiente sem NAND: não falha o build, mas não valida.
    }
    printf("[QW1] APPS firmware: %s (%zu bytes)\n", apps_path, apps.size());

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

    // 2) Enumerar VIRT_POOLS / PHYS_POOLS reais (primeiros pares conhecidos).
    auto virt = bi.pools(zeebo_bi::BI_TAG_VIRT_POOLS);
    auto phys = bi.pools(zeebo_bi::BI_TAG_PHYS_POOLS);
    CHECK(virt.size() == 10);
    CHECK(phys.size() == 5);
    CHECK(virt[0].base == 0x16e00000 && virt[0].end == 0x7fffffff);
    CHECK(phys[0].base == 0x14954000 && phys[0].end == 0x155fffff);
    CHECK(virt[1].base == 0x80100000 && virt[1].end == 0xafffffff);

    // 3) Decompor o pool virtual do Iguana em fpages de 1 MiB até 0xb6d00000.
    //    mempool_init (@0xb000d5b4) seleciona min(virt,phys)=1MiB (size_log2=20)
    //    e incrementa r4 em 0x100000 cobrindo os 96 MiB de APPS_RAM.
    auto fps = zeebo_bi::enumerate_fpages(0xb0d00000u, 0xb6d00000u,
                                          /*page_log2=*/20, /*rwx=*/6);
    CHECK(fps.size() == 96);
    CHECK(fps.front().raw == 0xb0d00146u);          // VA 0xb0d00000, size_log2=20, rwx=6
    CHECK(fps.front().vaddr() == 0xb0d00000u);
    CHECK(fps.front().size_bytes() == (1u << 20));
    CHECK(fps.back().vaddr() == 0xb6c00000u);        // última página de 1 MiB
    CHECK(fps.back().raw == 0xb6c00146u);
    // Cobertura contígua exata: [0xb0d00000, 0xb6d00000).
    u32 covered_end = (u32)(fps.back().vaddr() + fps.back().size_bytes());
    CHECK(covered_end == 0xb6d00000u);

    if (g_fail == 0) printf("[QW1] PASS — BootInfo/fpage harness verde (bytes reais)\n");
    else             printf("[QW1] %d verificações falharam\n", g_fail);
    return g_fail ? 1 : 0;
}
