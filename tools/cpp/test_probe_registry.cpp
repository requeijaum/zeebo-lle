// tools/cpp/test_probe_registry.cpp
//
// Unit tests for the enumerable ProbeRegistry (QW3). Read-only diagnostic
// probes registered by the orchestrator and exposed via the ControlServer
// commands `probe.list` and `probe.get`. Strict TDD: this test is written
// RED before zeebo_probe_registry.h exists.

#include "zeebo_probe_registry.h"

#include <cassert>
#include <string>

int main() {
    zeebo_lle::ProbeRegistry reg;

    // Empty registry lists no probes but is still valid JSON.
    {
        const std::string js = reg.ListJson();
        assert(js == "{\"ok\":true,\"probes\":[]}");
    }

    // Register a handful of read-only probes.
    reg.Register("mmu", "APPS L4 MMU state", [] {
        return std::string("{\"enabled\":true,\"min_page\":4096}");
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

    // GetJson returns the wrapped probe payload for a known probe.
    {
        const std::string js = reg.GetJson("mmu");
        assert(js == "{\"ok\":true,\"name\":\"mmu\",\"value\":{\"enabled\":true,\"min_page\":4096}}");
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

    // ---- QW8: structured unknown-MMIO event log -------------------------
    zeebo_lle::MmioEventLog mlog(3); // tiny ring to exercise eviction
    assert(mlog.CountSeen() == 0);
    assert(mlog.LatestJson() == "{\"ok\":true,\"events\":[]}");

    // Record a read (no value) and a write (with value).
    mlog.Record(zeebo_lle::MmioEvent{
        /*core=*/0, /*pc=*/0xb0001234u, /*addr=*/0xa9000000u,
        /*width=*/4, /*is_write=*/false, /*has_value=*/false, /*value=*/0});
    mlog.Record(zeebo_lle::MmioEvent{
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
    mlog.Record(zeebo_lle::MmioEvent{0, 1, 0x10, 1, false, false, 0});
    mlog.Record(zeebo_lle::MmioEvent{0, 2, 0x20, 1, false, false, 0});
    mlog.Record(zeebo_lle::MmioEvent{0, 3, 0x30, 1, false, false, 0});
    assert(mlog.CountSeen() == 5);
    {
        const std::string js = mlog.LatestJson();
        // Only the last 3 retained; the first (pc=0xb0001234) is gone.
        assert(js.find("2952794676") == std::string::npos);
        assert(js.find("\"pc\":3") != std::string::npos);
    }

    return 0;
}
