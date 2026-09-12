// test_efs2_guard_real.cpp — Controle negativo REAL: os três blobs históricos do
// 0:EFS2APPS (reksio.mod, 274755, tectoy.mod) que a antiga tabela KnownIB
// reivindicava como payloads de applet são, por bytes, NÃO-módulos. Este teste
// extrai cada um da NAND real e exige MOD_REJECT do guard fail-closed.
//
// Exit codes: 0 = todos rejeitados (correto); 1 = algum aceito (regressão);
// 77 = NAND ausente/ilegível (SKIP — nunca PASS silencioso).
//
// A NAND (nand/1.1.2.bin) é proprietária e gitignored: NUNCA é comitada.
#include <cstdio>
#include <cstring>
#include <vector>
#include "zeebo_efs2_fs.h"
#include "zeebo_efs2_module_guard.h"

using namespace zeebo::efs2_guard;

int main(int argc, char** argv) {
    const char* nand = (argc > 1) ? argv[1] : "../../nand/1.1.2.bin";
    efs2::Efs2Filesystem fs;
    if (!fs.open(nand) || fs.partition_size() == 0) {
        printf("SKIP: NAND ausente/ilegível (%s) — exit 77.\n", nand);
        return 77;
    }
    fs.scan_dirents();

    struct Blob { const char* name; uint64_t indirect_off; } blobs[] = {
        { "reksio.mod", 0x3b1d400ULL },
        { "274755",     0x3a92000ULL },
        { "tectoy.mod", 0x6026200ULL },
    };

    int fail = 0;
    const u32 LOAD = 0x12000000u;
    for (const auto& b : blobs) {
        std::vector<uint8_t> payload = fs.read_data_from_indirect(b.indirect_off);
        u32 w0 = 0; if (payload.size() >= 4) std::memcpy(&w0, payload.data(), 4);
        u32 entry = 0;
        ModuleVerdict v = classify_payload(payload, LOAD, &entry);
        bool ok = (v == MOD_REJECT);
        printf("  [%s] %-12s bytes=%zu w0=0x%08x -> %s\n",
               ok ? "PASS" : "FAIL", b.name, payload.size(), w0, verdict_label(v));
        if (!ok) ++fail;
    }

    if (fail == 0) {
        printf("PASS: os 3 blobs KnownIB históricos são rejeitados (não são módulos).\n");
        return 0;
    }
    printf("FAIL: %d blob(s) aceito(s) como módulo — o guard vazou garbage.\n", fail);
    return 1;
}
