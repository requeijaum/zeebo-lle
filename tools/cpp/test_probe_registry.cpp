// tools/cpp/test_probe_registry.cpp
//
// Unit tests for the enumerable ProbeRegistry (QW3). Read-only diagnostic
// probes registered by the orchestrator and exposed via the ControlServer
// commands `probe.list` and `probe.get`. Strict TDD: this test is written
// RED before zeebo_probe_registry.h exists.

#include "zeebo_probe_registry.h"

#include <cassert>
#include <cstdio>
#include <string>

int main() {
    zeebo_lle::ProbeRegistry reg;

    // Empty registry lists no probes but is still valid JSON.
    {
        const std::string js = reg.ListJson();
        assert(js == "{\"ok\":true,\"probes\":[]}");
    }

    // Register a handful of read-only probes.
    // The `mmu` probe must expose only fields backed by live guest state:
    // min_page_log2 is DERIVED from the live PageInfo word by DECODING the
    // OKL4 KIP PageInfo layout (bits[0:9]=rights, bits[10:31]=page-size mask),
    // exactly as the firmware's l4e_min_pagesize() does — NOT a raw CTZ of the
    // whole word (which would latch onto a low rights bit). Model that here.
    const unsigned kLivePageInfo = 0x01111006u; // as written to KIP+0xc8
    reg.Register("mmu", "APPS L4 MMU state", [kLivePageInfo] {
        unsigned min_page_log2 =
            zeebo_lle::PageInfoMinPageLog2(kLivePageInfo);
        char b[96];
        std::snprintf(b, sizeof(b),
                      "{\"page_info\":%u,\"min_page_log2\":%u}",
                      kLivePageInfo, min_page_log2);
        return std::string(b);
    });
    reg.Register("bootinfo", "Iguana BootInfo tags", [] {
        return std::string("{\"magic\":\"0x1960021d\",\"tags\":6}");
    });
    reg.Register("irq", "MSM VIC pending mask", [] {
        return std::string("{\"vic_status0\":0}");
    });
    reg.Register("gpu", "Adreno 130 draw counters", [] {
        return std::string("{\"draws\":0,\"fb_dirty\":false}");
    });

    // ListJson enumerates every probe with name + description.
    {
        const std::string js = reg.ListJson();
        assert(js.find("\"name\":\"mmu\"") != std::string::npos);
        assert(js.find("\"desc\":\"APPS L4 MMU state\"") != std::string::npos);
        assert(js.find("\"name\":\"bootinfo\"") != std::string::npos);
        assert(js.find("\"name\":\"irq\"") != std::string::npos);
        assert(js.find("\"name\":\"gpu\"") != std::string::npos);
        assert(js.rfind("{\"ok\":true,\"probes\":[", 0) == 0);
        assert(js.back() == '}');
    }

    // GetJson returns the wrapped probe payload for a known probe. The mmu
    // payload's min_page_log2 must equal the DECODED minimum page-size log2 of
    // the live PageInfo word — proving it is derived from real KIP state.
    {
        const std::string js = reg.GetJson("mmu");
        // 0x01111006 -> page-size mask 0x01111000, CTZ = 12 (4KB min page).
        assert(js == "{\"ok\":true,\"name\":\"mmu\",\"value\":{\"page_info\":17895430,\"min_page_log2\":12}}");
    }

    // ---- OKL4 KIP PageInfo decode: externally pinned vectors ----------------
    // Each vector is an independently-derived (page_info, expected min log2)
    // pair grounded in the OKL4/Iguana PageInfo layout — bits[0:9] are page
    // rights/metadata and bits[10:31] are the page-size mask (bit N set =>
    // page size 2^N supported). The minimum page size is CTZ of the mask, NOT
    // CTZ of the raw word. These pins would FAIL a naive raw-CTZ implementation.
    {
        struct Vec { unsigned page_info; unsigned expect; const char* why; };
        const Vec vs[] = {
            // Live Zeebo KIP: 4K/64K/1M/16M pages | rwx=0x6. Min = 4K => log2 12.
            {0x01111006u, 12u, "4K/64K/1M/16M, rwx set -> 4K min"},
            // Rights bit 1 set but no page-size bits: no valid size mask -> 0.
            {0x00000006u, 0u,  "rights only, no page-size mask -> undefined"},
            // Pure 4K page-size bit, no rights: min = 4K => log2 12.
            {0x00001000u, 12u, "only 4K bit -> 12"},
            // 1MB (bit 20) as smallest supported page, rights 0x3ff all set.
            {0x001003ffu, 20u, "1MB smallest, all rights bits set -> 20"},
            // 64K (bit 16) smallest, plus 16M (bit 24); low bits garbage.
            {0x01010001u, 16u, "64K smallest despite bit0 rights -> 16"},
            // Full mask 4K..2G, all rights: smallest is 4K => 12.
            {0xfffff3ffu, 12u, "all sizes 4K..2G -> 12"},
            // No bits at all: no page info -> 0.
            {0x00000000u, 0u,  "empty PageInfo -> undefined min"},
        };
        for (const auto& v : vs) {
            unsigned got = zeebo_lle::PageInfoMinPageLog2(v.page_info);
            if (got != v.expect) {
                std::fprintf(stderr,
                    "PageInfo 0x%08x: expected min_page_log2=%u got=%u (%s)\n",
                    v.page_info, v.expect, got, v.why);
            }
            assert(got == v.expect);
        }
    }

    // Unknown probe -> structured error, never a crash.
    {
        const std::string js = reg.GetJson("does_not_exist");
        assert(js == "{\"ok\":false,\"error\":\"unknown_probe\"}");
    }

    // Names are unique: re-registering replaces the handler.
    {
        reg.Register("irq", "MSM VIC pending mask", [] {
            return std::string("{\"vic_status0\":42}");
        });
        const std::string js = reg.GetJson("irq");
        assert(js.find("42") != std::string::npos);
    }

    // Count reflects unique names.
    assert(reg.Count() == 4);

    // ---- QW8: partial structured unknown-unmapped-access diagnostics --------
    // An unmapped hook proves nothing about MMIO; this log records ANY unknown
    // unmapped access as passive, bounded telemetry (no pause/error policy).
    zeebo_lle::UnmappedAccessLog mlog(3); // tiny ring to exercise eviction
    assert(mlog.CountSeen() == 0);
    assert(mlog.LatestJson() == "{\"ok\":true,\"events\":[]}");

    // Record a read (no value) and a write (with value).
    mlog.Record(zeebo_lle::UnmappedAccessEvent{
        /*core=*/0, /*pc=*/0xb0001234u, /*addr=*/0xa9000000u,
        /*width=*/4, /*is_write=*/false, /*has_value=*/false, /*value=*/0});
    mlog.Record(zeebo_lle::UnmappedAccessEvent{
        1, 0xf0010000u, 0xc1000040u, 2, true, true, 0xdead});

    assert(mlog.CountSeen() == 2);
    {
        const std::string js = mlog.LatestJson();
        assert(js.rfind("{\"ok\":true,\"events\":[", 0) == 0);
        assert(js.find("\"core\":0") != std::string::npos);
        assert(js.find("\"pc\":2952794676") != std::string::npos); // 0xb0001234
        assert(js.find("\"addr\":2835349504") != std::string::npos); // 0xa9000000
        assert(js.find("\"width\":4") != std::string::npos);
        assert(js.find("\"dir\":\"read\"") != std::string::npos);
        assert(js.find("\"dir\":\"write\"") != std::string::npos);
        assert(js.find("\"value\":57005") != std::string::npos); // 0xdead
    }

    // Ring eviction: capacity 3, push 3 more -> oldest dropped, CountSeen keeps growing.
    mlog.Record(zeebo_lle::UnmappedAccessEvent{0, 1, 0x10, 1, false, false, 0});
    mlog.Record(zeebo_lle::UnmappedAccessEvent{0, 2, 0x20, 1, false, false, 0});
    mlog.Record(zeebo_lle::UnmappedAccessEvent{0, 3, 0x30, 1, false, false, 0});
    assert(mlog.CountSeen() == 5);
    {
        const std::string js = mlog.LatestJson();
        // Only the last 3 retained; the first (pc=0xb0001234) is gone.
        assert(js.find("2952794676") == std::string::npos);
        assert(js.find("\"pc\":3") != std::string::npos);
    }

    return 0;
}
