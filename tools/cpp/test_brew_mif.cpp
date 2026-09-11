// test_brew_mif.cpp — structural MIF parser regression tests
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>
#include "zeebo_brew_mif.h"

static void put16(std::vector<uint8_t>& b, size_t off, uint16_t v) {
    std::memcpy(b.data() + off, &v, sizeof(v));
}
static void put32(std::vector<uint8_t>& b, size_t off, uint32_t v) {
    std::memcpy(b.data() + off, &v, sizeof(v));
}

// Minimal structurally valid MIF: section 0 is arbitrary; section 1 is a
// 20-byte applet record {clsid,0,value,0,value}.
static std::vector<uint8_t> make_mif(uint32_t clsid) {
    std::vector<uint8_t> b(0x64, 0);
    put16(b, 0x00, 0x0011); // magic
    put16(b, 0x02, 1);      // version
    put16(b, 0x04, 1);
    put16(b, 0x06, 0);
    put32(b, 0x08, 0x20);
    put32(b, 0x0c, 0);
    put32(b, 0x10, 0x20);   // section-bound table
    put32(b, 0x14, 2);      // two sections => three bounds
    put32(b, 0x18, 0x40);
    put32(b, 0x1c, 0x24);
    put32(b, 0x20, 0x40);
    put32(b, 0x24, 0x50);
    put32(b, 0x28, 0x64);
    put32(b, 0x50, clsid);
    put32(b, 0x54, 0);
    put32(b, 0x58, 7);
    put32(b, 0x5c, 0);
    put32(b, 0x60, 1);
    return b;
}

int main() {
    std::printf("=== Test BREW MIF Parser (structural) ===\n");

    assert(!zeebo::brew::MifParser::parse(nullptr, 0).valid);
    assert(!zeebo::brew::MifParser::parse(std::vector<uint8_t>(16)).valid);

    // Real target contract: DD's ClassID comes from a 20-byte applet section.
    auto dd = make_mif(0x0102f789);
    auto info = zeebo::brew::MifParser::parse(dd);
    assert(info.valid && info.clsid == 0x0102f789);

    // ClassIDs are identifiers, not a Qualcomm-range heuristic.
    auto outside_old_range = make_mif(0xbf2e2021);
    info = zeebo::brew::MifParser::parse(outside_old_range);
    assert(info.valid && info.clsid == 0xbf2e2021);

    // A random 0x010xxxxx word outside an applet section is not an applet.
    auto decoy = make_mif(0x12345678);
    put32(decoy, 0x30, 0x01004003);
    info = zeebo::brew::MifParser::parse(decoy);
    assert(info.valid && info.clsid == 0x12345678);

    // A 20-byte section without the two structural zero fields is not an applet.
    auto non_applet = make_mif(0x0102f789);
    put32(non_applet, 0x54, 1);
    assert(!zeebo::brew::MifParser::parse(non_applet).valid);

    auto bad_magic = make_mif(0x0102f789);
    put16(bad_magic, 0, 0x9999);
    assert(!zeebo::brew::MifParser::parse(bad_magic).valid);

    auto truncated = make_mif(0x0102f789);
    truncated.resize(0x60);
    assert(!zeebo::brew::MifParser::parse(truncated).valid);

    std::printf("=== Test BREW MIF Parser: PASS ===\n");
    return 0;
}
