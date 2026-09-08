#pragma once
// CLI positional firmware-path assignment.
//
// Extracted verbatim from zeebo_lle_main.cpp main() so the ACTUAL production
// assignment logic can be exercised by a lightweight host-only harness
// (no Unicorn, no NAND). The emulator's main() builds the ordered list of
// positional (non-option) tokens and calls resolve_cli_firmware_paths();
// the same function is what the CLI test drives. This is a pure extraction:
// behavior is identical to the inline loop it replaces.
//
// Slot order is: [0]=NAND, [1]=APPS, [2]=AMSS. Any 4th+ positional is surplus.

#include <string>
#include <vector>

struct CliFirmwarePaths {
    std::string nand;
    std::string apps;
    std::string amss;
    bool surplus = false;  // true when more positional args than slots were given
};

// Default working-copy paths (relative to tools/cpp run dir).
static const char* const kDefaultNandPath = "../../nand/1.1.2.bin";
static const char* const kDefaultAppsPath = "../../nand/1.1.2_APPS.bin";
static const char* const kDefaultAmssPath = "../../nand/1.1.2_AMSS.bin";

inline CliFirmwarePaths resolve_cli_firmware_paths(
    const std::vector<std::string>& positionals,
    const std::string& def_nand = kDefaultNandPath,
    const std::string& def_apps = kDefaultAppsPath,
    const std::string& def_amss = kDefaultAmssPath)
{
    CliFirmwarePaths p{def_nand, def_apps, def_amss, false};

    // Assign each positional to its slot by ordinal INDEX, never by comparing
    // the current value against the default string. String-equality slot
    // detection breaks whenever a supplied path equals a default (e.g. passing
    // the real default NAND path explicitly): the slot still "looks empty", so
    // the next positional overwrites it, cascading APPS into NAND and AMSS into
    // APPS -> Core0 would load AMSS bytes as APPS.
    for (size_t idx = 0; idx < positionals.size(); ++idx) {
        switch (idx) {
            case 0: p.nand = positionals[idx]; break;
            case 1: p.apps = positionals[idx]; break;
            case 2: p.amss = positionals[idx]; break;
            default: p.surplus = true; break;  // surplus args ignored, flagged
        }
    }
    return p;
}
