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
// QW8: structured unknown-MMIO diagnostics.
//
// When the guest touches an address with no registered handler (an "unknown"
// MMIO access), the orchestrator records a structured event instead of relying
// on ad-hoc printf trace blocks. Events are held in a bounded ring so an agent
// can query the most recent ones (via a probe / pause-error payload) with full
// context: core, PC, address, width, direction and — for writes — the value.
struct MmioEvent {
    unsigned long core = 0;
    unsigned long pc = 0;
    unsigned long addr = 0;
    unsigned width = 0;    // access width in bytes
    bool is_write = false;
    bool has_value = false;
    unsigned long long value = 0;
};

class MmioEventLog {
public:
    explicit MmioEventLog(size_t capacity = 64)
        : capacity_(capacity ? capacity : 1) {}

    void Record(const MmioEvent& ev) {
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
    static std::string EventJson(const MmioEvent& ev) {
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
    std::vector<MmioEvent> ring_;
};

} // namespace zeebo_lle

#endif // ZEEBO_LLE_PROBE_REGISTRY_H_
