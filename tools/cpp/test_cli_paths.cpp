// Host-only regression test for CLI positional firmware-path assignment.
//
// Drives the ACTUAL production resolver (resolve_cli_firmware_paths from
// zeebo_cli_paths.h, the same function main() calls) — no Unicorn, no NAND.
//
// Definitive bug (main.cpp ~2993-2998, string-equality slot detection):
// supplying positionals `1.1.2.bin 1.1.2_APPS.bin 1.1.2_AMSS.bin` where the
// first equals the default NAND path leaves slot 0 "looking empty", so APPS
// overwrites NAND and AMSS overwrites APPS -> Core0 loads AMSS bytes as APPS.
//
// Exit code: 0 = all pass, non-zero = failure (CI-visible).

#include <cstdio>
#include <string>
#include <vector>
#include "zeebo_cli_paths.h"

static int g_fails = 0;

static void expect_eq(const char* what, const std::string& got, const std::string& want) {
    if (got != want) {
        fprintf(stderr, "FAIL [%s]: got '%s', want '%s'\n", what, got.c_str(), want.c_str());
        ++g_fails;
    } else {
        printf("ok   [%s] = '%s'\n", what, got.c_str());
    }
}

static void expect_bool(const char* what, bool got, bool want) {
    if (got != want) {
        fprintf(stderr, "FAIL [%s]: got %d, want %d\n", what, (int)got, (int)want);
        ++g_fails;
    } else {
        printf("ok   [%s] = %d\n", what, (int)got);
    }
}

int main() {
    // Case A: omitted -> defaults intact.
    {
        CliFirmwarePaths p = resolve_cli_firmware_paths({});
        expect_eq("omitted.nand", p.nand, kDefaultNandPath);
        expect_eq("omitted.apps", p.apps, kDefaultAppsPath);
        expect_eq("omitted.amss", p.amss, kDefaultAmssPath);
        expect_bool("omitted.surplus", p.surplus, false);
    }

    // Case B (THE BUG): explicit default-valued NAND, then APPS, then AMSS.
    // Each must land in its own slot; APPS must NEVER become the NAND value and
    // AMSS must NEVER become the APPS value.
    {
        CliFirmwarePaths p = resolve_cli_firmware_paths(
            {"../../nand/1.1.2.bin",
             "../../nand/1.1.2_APPS.bin",
             "../../nand/1.1.2_AMSS.bin"});
        expect_eq("explicit-default.nand", p.nand, "../../nand/1.1.2.bin");
        expect_eq("explicit-default.apps", p.apps, "../../nand/1.1.2_APPS.bin");
        expect_eq("explicit-default.amss", p.amss, "../../nand/1.1.2_AMSS.bin");
        expect_bool("explicit-default.surplus", p.surplus, false);
    }

    // Case C: absolute custom paths, all three distinct from defaults.
    {
        CliFirmwarePaths p = resolve_cli_firmware_paths(
            {"/data/nand.bin", "/data/apps.bin", "/data/amss.bin"});
        expect_eq("absolute.nand", p.nand, "/data/nand.bin");
        expect_eq("absolute.apps", p.apps, "/data/apps.bin");
        expect_eq("absolute.amss", p.amss, "/data/amss.bin");
    }

    // Case D: only NAND supplied (equal to default) — APPS/AMSS keep defaults,
    // must not absorb a phantom value.
    {
        CliFirmwarePaths p = resolve_cli_firmware_paths({"../../nand/1.1.2.bin"});
        expect_eq("nand-only.nand", p.nand, "../../nand/1.1.2.bin");
        expect_eq("nand-only.apps", p.apps, kDefaultAppsPath);
        expect_eq("nand-only.amss", p.amss, kDefaultAmssPath);
    }

    // Case E: surplus positionals -> flagged, first three still slot correctly.
    {
        CliFirmwarePaths p = resolve_cli_firmware_paths(
            {"/n.bin", "/a.bin", "/m.bin", "/extra.bin"});
        expect_eq("surplus.nand", p.nand, "/n.bin");
        expect_eq("surplus.apps", p.apps, "/a.bin");
        expect_eq("surplus.amss", p.amss, "/m.bin");
        expect_bool("surplus.flag", p.surplus, true);
    }

    if (g_fails) {
        fprintf(stderr, "\n%d assertion(s) FAILED\n", g_fails);
        return 1;
    }
    printf("\nAll CLI positional-path assertions passed.\n");
    return 0;
}
