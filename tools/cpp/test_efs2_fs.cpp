// test_efs2_fs.cpp — Automated harness for the 0:EFS2APPS parser (zeebo_efs2_fs.h).
//
// Validates, by REAL BYTES from nand/1.1.2.bin (never instruction counts):
//   1. The partition opens at 0x3220000 and mirrors the raw dump.
//   2. Dirent scanning recovers the proven record population (69,634).
//   3. A known file dirent (reksio.mod) parses with exact inode/reclen/type/
//      parent_ref fields, and (parent_inode,name) O(1) lookup resolves it.
//   4. Data-cluster reads and indirect-block chaining produce byte-exact
//      payload, verified by length + FNV-1a checksum.
//
// Build: g++ -std=c++23 -O2 -o test_efs2_fs test_efs2_fs.cpp
// Run:   ./test_efs2_fs   (expects nand/1.1.2.bin at repo root)
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include <algorithm>
#include "zeebo_efs2_fs.h"

using namespace efs2;

static int g_pass = 0, g_fail = 0;
static void check(bool c, const char* what) {
    if (c) { printf("  OK   %s\n", what); g_pass++; }
    else   { printf("  FAIL %s\n", what); g_fail++; }
}

int main(int argc, char** argv) {
    const char* nand = (argc > 1) ? argv[1]
                                  : "../../nand/1.1.2.bin";

    printf("== 0:EFS2APPS filesystem parser (zeebo_efs2_fs.h) ==\n");
    printf("nand: %s\n", nand);

    Efs2Filesystem fs;
    check(fs.open(nand), "open NAND dump + map 0:EFS2APPS @0x3220000");
    if (fs.partition_size() == 0) {
        printf("\nfatal: partition empty (dump missing?)\nfail=%d\n", ++g_fail);
        return 1;
    }
    check(fs.partition_offset() == EFS2APPS_OFF,
          "partition offset == 0x3220000 (block 0x191)");
    printf("partition size = %llu bytes (%.1f MiB)\n",
           (unsigned long long)fs.partition_size(),
           fs.partition_size() / (1024.0 * 1024.0));

    // --- Dirent scan: proven population from the dump ---
    size_t n = fs.scan_dirents();
    printf("scanned dirents = %zu\n", n);
    check(n == 69634, "recovered 69634 dirents (byte-proven population)");

    // --- Known file dirent: reksio.mod @ marker 0x32606ef ---
    const Dirent* rk = fs.find_by_name("reksio.mod");
    check(rk != nullptr, "reksio.mod dirent located by name");
    if (rk) {
        check(rk->file_off   == 0x32606efULL, "reksio.mod marker offset == 0x32606ef");
        check(rk->inode      == 0x265e4u,     "reksio.mod inode == 0x265e4");
        check(rk->reclen     == 15,           "reksio.mod reclen == 15");
        check(rk->type       == 5,            "reksio.mod type == 5");
        check(rk->parent_ref == 0x4abef64u,   "reksio.mod parent_ref == 0x4abef64");
        check(rk->parent_inode() == 0x4abefu, "reksio.mod parent_inode == 0x4abef");
        check(rk->parent_tag()   == 0x64,     "reksio.mod parent tag == 0x64");

        // O(1) composite-key lookup must resolve the same record.
        const Dirent* by_key = fs.find(rk->parent_inode(), "reksio.mod");
        check(by_key != nullptr &&
              by_key->name == "reksio.mod" &&
              by_key->parent_inode() == rk->parent_inode(),
              "O(1) (parent_inode,name) lookup resolves reksio.mod");
    }

    // --- Data-cluster read: single 512B cluster 0x6d11 ---
    {
        u8 c[CLUSTER_SIZE];
        bool ok = fs.read_cluster(0x6d11, c);
        std::vector<u8> cv(c, c + CLUSTER_SIZE);
        check(ok, "read data cluster 0x6d11 (512B @ 0x3220000 + 0x6d11*512)");
        check(Efs2Filesystem::checksum32(cv) == 0xa0f4d11fu,
              "cluster 0x6d11 FNV-1a checksum == 0xa0f4d11f");
    }

    // --- Indirect block @0x3b1d400: full 128-cluster chain -> 64 KiB payload ---
    {
        auto ptrs = fs.read_indirect_block_at(0x3b1d400, /*stop_at_terminator=*/false);
        check(ptrs.size() == 128, "indirect block @0x3b1d400 holds 128 u32 ptrs");
        check(!ptrs.empty() && ptrs[0] == 0x6d11u,
              "indirect block first cluster ptr == 0x6d11");

        std::vector<u8> payload = fs.read_data_from_indirect(0x3b1d400);
        check(payload.size() == 65536,
              "chained payload == 65536 bytes (128 x 512B clusters)");
        check(Efs2Filesystem::checksum32(payload) == 0xd9339103u,
              "chained 64KiB payload FNV-1a checksum == 0xd9339103");
    }

    // --- Expanded applet catalog: additional proven indirect blocks ---------
    // Each entry is byte-verified: exact payload length + FNV-1a checksum, plus
    // an embedded ASCII signature confirming the applet/asset identity. These
    // are the same anchors registered in zeebo_lle_main.cpp (efs2_extract).
    auto has_sig = [](const std::vector<u8>& v, const char* s) -> bool {
        std::string n(s);
        return std::search(v.begin(), v.end(), n.begin(), n.end()) != v.end();
    };

    // 274755 — the Z-Wheel / ZeeboApp App ID (AEECLSID 0x01070798). The indirect
    // block @0x3a92000 chains a 64 KiB payload carrying the literal "274755".
    {
        auto ptrs = fs.read_indirect_block_at(0x3a92000, /*stop_at_terminator=*/false);
        check(ptrs.size() == 128, "Z-Wheel 274755 indirect block @0x3a92000 holds 128 ptrs");
        check(!ptrs.empty() && ptrs[0] == 0x1dcau,
              "274755 indirect block first cluster ptr == 0x1dca");
        std::vector<u8> payload = fs.read_data_from_indirect(0x3a92000);
        check(payload.size() == 65536, "274755 chained payload == 65536 bytes");
        check(Efs2Filesystem::checksum32(payload) == 0x544a6f30u,
              "274755 64KiB payload FNV-1a checksum == 0x544a6f30");
        check(has_sig(payload, "274755"),
              "274755 payload carries embedded App-ID signature \"274755\"");
    }

    // tectoy.mod — TecToy/Claro applet config (ZeeboApp branding). The indirect
    // block @0x6026200 chains a 64 KiB payload carrying "tectoy.claro.com.br".
    {
        auto ptrs = fs.read_indirect_block_at(0x6026200, /*stop_at_terminator=*/false);
        check(ptrs.size() == 128, "tectoy.mod indirect block @0x6026200 holds 128 ptrs");
        check(!ptrs.empty() && ptrs[0] == 0x1243u,
              "tectoy.mod indirect block first cluster ptr == 0x1243");
        std::vector<u8> payload = fs.read_data_from_indirect(0x6026200);
        check(payload.size() == 65536, "tectoy.mod chained payload == 65536 bytes");
        check(Efs2Filesystem::checksum32(payload) == 0xf7c3c740u,
              "tectoy.mod 64KiB payload FNV-1a checksum == 0xf7c3c740");
        check(has_sig(payload, "tectoy.claro.com.br"),
              "tectoy.mod payload carries embedded \"tectoy.claro.com.br\" signature");
    }

    // Dirent catalog: tectoy.mod is filiated under inode 0x1fae8 with inode 0x7ff13.
    {
        const Dirent* tc = fs.find_by_name("tectoy.mod");
        check(tc != nullptr, "tectoy.mod dirent located by name");
        if (tc) {
            check(tc->inode == 0x7ff13u, "tectoy.mod inode == 0x7ff13");
            check(tc->parent_inode() == 0x1fae8u, "tectoy.mod parent_inode == 0x1fae8");
        }
    }

    // Failed reopen clears cached bytes and indexes instead of exposing stale NAND.
    check(!fs.open("/definitely/missing/zeebo-nand.bin"), "failed reopen reports false");
    check(fs.partition_size() == 0 && fs.data().empty() && fs.dirents().empty(),
          "failed reopen clears stale partition and dirent state");

    printf("\npass=%d fail=%d\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
