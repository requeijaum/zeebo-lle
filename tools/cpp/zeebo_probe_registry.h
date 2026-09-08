// tools/cpp/zeebo_probe_registry.h
//
// QW3: Minimal enumerable ProbeRegistry for the Zeebo LLE orchestrator.
//
// A probe is a NAMED, READ-ONLY diagnostic that returns a JSON fragment when
// queried. Probes are registered by the orchestrator over live subsystems
// (MMU/BootInfo, IRQ/VIC, GPU, ...) and enumerated/queried through the
// ControlServer commands `probe.list` and `probe.get`. There is deliberately
// no `probe.set` — probes never mutate guest state, keeping the surface safe
// for autonomous agents.
//
// The registry itself is intentionally dependency-free (header-only, std only)
// so it can be unit-tested host-side without Unicorn.

#ifndef ZEEBO_LLE_PROBE_REGISTRY_H_
#define ZEEBO_LLE_PROBE_REGISTRY_H_

#include <functional>
#include <string>
#include <vector>
#include <cstdio>

namespace zeebo_lle {

// A probe handler returns a self-contained JSON value (object, array or
// scalar) as a string. It must be side-effect free w.r.t. guest state.
using ProbeFn = std::function<std::string()>;

// Decode the OKL4/Iguana KIP PageInfo word (KIP+0xc8) into the log2 of the
// smallest supported page size, exactly as the firmware's l4e_min_pagesize()
// routine at 0xb000d498 does:
//
//   b000d498  ldr  r3,[r0,#0xc8]     ; PageInfo
//   b000d49c  bic  r2,r3,#0x3fc      ; clear bits[2:9]
//   b000d4a0  bic  r2,r2,#3          ; clear bits[0:1]  => r2 = page-size mask
//   ... CTZ over the surviving mask (bits[10:31]) -> log2(min page size)
//
// PageInfo layout (L4 KernelInterfacePage):
//   bits[0:9]   = page access-rights / metadata (rwx) — NOT a page size
//   bits[10:31] = page-size mask; bit N set => page size 2^N is supported
//
// So the minimum page-size log2 is CTZ of (page_info & ~0x3ff), never CTZ of
// the raw word (which would spuriously latch onto a low rights bit). Returns 0
// when no page-size bit is set (no valid size mask -> undefined min page).
inline unsigned PageInfoMinPageLog2(unsigned page_info) {
    unsigned mask = page_info & ~0x3ffu; // strip bits[0:9] rights/metadata
    if (mask == 0u) return 0u;           // no page-size bit -> no valid minimum
    return (unsigned)__builtin_ctz(mask);
}

class ProbeRegistry {
public:
    ProbeRegistry() = default;

    // Register (or replace) a read-only probe under a unique name.
    void Register(const std::string& name, const std::string& desc, ProbeFn fn) {
        for (auto& p : probes_) {
            if (p.name == name) {
                p.desc = desc;
                p.fn = std::move(fn);
                return;
            }
        }
        probes_.push_back(Entry{name, desc, std::move(fn)});
    }

    size_t Count() const { return probes_.size(); }

    // {"ok":true,"probes":[{"name":..,"desc":..},...]}
    std::string ListJson() const {
        std::string out = "{\"ok\":true,\"probes\":[";
        for (size_t i = 0; i < probes_.size(); ++i) {
            if (i) out.push_back(',');
            out += "{\"name\":\"";
            out += JsonEscape(probes_[i].name);
            out += "\",\"desc\":\"";
            out += JsonEscape(probes_[i].desc);
            out += "\"}";
        }
        out += "]}";
        return out;
    }

    // {"ok":true,"name":..,"value":<probe payload>} or an unknown_probe error.
    std::string GetJson(const std::string& name) const {
        for (const auto& p : probes_) {
            if (p.name == name) {
                std::string payload = p.fn ? p.fn() : std::string("null");
                std::string out = "{\"ok\":true,\"name\":\"";
                out += JsonEscape(name);
                out += "\",\"value\":";
                out += payload;
                out += "}";
                return out;
            }
        }
        return "{\"ok\":false,\"error\":\"unknown_probe\"}";
    }

private:
    struct Entry {
        std::string name;
        std::string desc;
        ProbeFn fn;
    };

    static std::string JsonEscape(const std::string& s) {
        std::string out;
        out.reserve(s.size());
        for (char c : s) {
            switch (c) {
                case '"':  out += "\\\""; break;
                case '\\': out += "\\\\"; break;
                case '\n': out += "\\n";  break;
                case '\r': out += "\\r";  break;
                case '\t': out += "\\t";  break;
                default:   out.push_back(c); break;
            }
        }
        return out;
    }

    std::vector<Entry> probes_;
};

// ---------------------------------------------------------------------------
// QW8: partial structured unknown-unmapped-access diagnostics.
//
// When the guest touches an address with no mapped page, Unicorn's
// UC_HOOK_MEM_*_UNMAPPED fires. This captures ANY unmapped access — it does
// NOT prove the target is an MMIO peripheral register; it may be a stray
// pointer, an unmapped RAM/stack region, or a not-yet-discovered device. So we
// record it honestly as an "unknown unmapped access", not as "unknown MMIO".
// Events are held in a bounded ring so an agent can query the most recent ones
// with full context: core, PC, address, width, direction and — for writes —
// the value. This is a passive, bounded diagnostic log: it does NOT pause the
// machine, raise an error, or alter auto-map behavior.
struct UnmappedAccessEvent {
    unsigned long core = 0;
    unsigned long pc = 0;
    unsigned long addr = 0;
    unsigned width = 0;    // access width in bytes
    bool is_write = false;
    bool has_value = false;
    unsigned long long value = 0;
};

class UnmappedAccessLog {
public:
    explicit UnmappedAccessLog(size_t capacity = 64)
        : capacity_(capacity ? capacity : 1) {}

    void Record(const UnmappedAccessEvent& ev) {
        ++seen_;
        if (ring_.size() < capacity_) {
            ring_.push_back(ev);
        } else {
            ring_[head_] = ev;
        }
        head_ = (head_ + 1) % capacity_;
    }

    // Total events observed since start (not clamped to capacity).
    unsigned long long CountSeen() const { return seen_; }

    // Render one event as a JSON object.
    static std::string EventJson(const UnmappedAccessEvent& ev) {
        char b[256];
        if (ev.has_value) {
            std::snprintf(b, sizeof(b),
                "{\"core\":%lu,\"pc\":%lu,\"addr\":%lu,\"width\":%u,"
                "\"dir\":\"%s\",\"value\":%llu}",
                ev.core, ev.pc, ev.addr, ev.width,
                ev.is_write ? "write" : "read", ev.value);
        } else {
            std::snprintf(b, sizeof(b),
                "{\"core\":%lu,\"pc\":%lu,\"addr\":%lu,\"width\":%u,\"dir\":\"%s\"}",
                ev.core, ev.pc, ev.addr, ev.width,
                ev.is_write ? "write" : "read");
        }
        return std::string(b);
    }

    // {"ok":true,"events":[...]} in chronological (oldest-first) order.
    std::string LatestJson() const {
        std::string out = "{\"ok\":true,\"events\":[";
        const size_t n = ring_.size();
        for (size_t i = 0; i < n; ++i) {
            // When full, oldest is at head_; otherwise entries are in push order.
            size_t idx = (ring_.size() < capacity_) ? i : (head_ + i) % capacity_;
            if (i) out.push_back(',');
            out += EventJson(ring_[idx]);
        }
        out += "]}";
        return out;
    }

private:
    size_t capacity_;
    size_t head_ = 0;
    unsigned long long seen_ = 0;
    std::vector<UnmappedAccessEvent> ring_;
};

} // namespace zeebo_lle

#endif // ZEEBO_LLE_PROBE_REGISTRY_H_
