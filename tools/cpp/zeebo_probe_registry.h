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

// ---------------------------------------------------------------------------
// QW14: OPT-IN strict-unmapped trap.
//
// The passive UnmappedAccessLog above records unknown unmapped accesses while
// the default hooks auto-map the page and continue (byte/observable-equivalent
// boot). QW14 adds an OPT-IN mode: when armed, the FIRST unknown unmapped
// access — one NOT serviced by a known/handled range such as the keypad — is
// captured with full context (core, PC, address, width, direction/type and,
// for writes, the value) and the machine is stopped deterministically so a
// ControlServer/CLI client can inspect the structured error/state.
//
// This proves only an unmapped access; it does NOT claim the target is MMIO.
// When DISARMED (the default) Consider() is a no-op and behavior is unchanged.
//
// The C++-side stop policy (chosen from real Unicorn behavior probed in
// probe_unmapped_behavior.cpp): the unmapped hook returns FALSE, which makes
// uc_emu_start return UC_ERR_READ/WRITE/FETCH_UNMAPPED with PC left AT the
// faulting instruction and the page NOT auto-mapped — so the captured PC/addr
// reflect the true fault. (uc_emu_stop()+leave-unmapped yields UC_ERR_MAP;
// auto-mapping the page destroys the evidence.) This class holds only the
// armed/tripped state and latched evidence; the hook consults it.
enum class UnmappedKind { kRead, kWrite, kFetch };

inline const char* UnmappedKindStr(UnmappedKind k) {
    switch (k) {
        case UnmappedKind::kWrite: return "write";
        case UnmappedKind::kFetch: return "fetch";
        case UnmappedKind::kRead:  default: return "read";
    }
}

struct StrictUnmappedRecord {
    bool valid = false;
    unsigned long core = 0;
    unsigned long pc = 0;
    unsigned long addr = 0;
    unsigned width = 0;
    UnmappedKind kind = UnmappedKind::kRead;
    bool has_value = false;
    unsigned long long value = 0;
};

class StrictUnmappedTrap {
public:
    void Arm(bool on) { armed_ = on; }
    bool armed() const { return armed_; }
    bool tripped() const { return rec_.valid; }
    const StrictUnmappedRecord& record() const { return rec_; }

    // Decide whether an unmapped access must trip (stop). Returns true iff the
    // caller should stop the CPU. Disarmed state and known/handled accesses
    // never trip and are never recorded. The FIRST trip is latched so later
    // accesses during teardown cannot overwrite the captured first-fault.
    bool Consider(unsigned long core, unsigned long pc, unsigned long addr,
                  unsigned width, UnmappedKind kind, bool has_value,
                  unsigned long long value, bool known_handled) {
        if (!armed_) return false;
        if (known_handled) return false;
        if (rec_.valid) return true; // already latched; still a trip
        rec_ = StrictUnmappedRecord{true, core, pc, addr, width,
                                    kind, has_value, value};
        return true;
    }

    // {"armed":<bool>,"tripped":<bool>,"event":<obj|null>}
    // The event object carries core/pc/addr/width/dir/type and (writes) value.
    std::string Json() const {
        std::string out = "{\"armed\":";
        out += armed_ ? "true" : "false";
        out += ",\"tripped\":";
        out += rec_.valid ? "true" : "false";
        out += ",\"event\":";
        if (!rec_.valid) {
            out += "null}";
            return out;
        }
        char b[256];
        const char* dir = UnmappedKindStr(rec_.kind);
        if (rec_.has_value) {
            std::snprintf(b, sizeof(b),
                "{\"core\":%lu,\"pc\":%lu,\"addr\":%lu,\"width\":%u,"
                "\"dir\":\"%s\",\"type\":\"%s\",\"value\":%llu}",
                rec_.core, rec_.pc, rec_.addr, rec_.width, dir, dir, rec_.value);
        } else {
            std::snprintf(b, sizeof(b),
                "{\"core\":%lu,\"pc\":%lu,\"addr\":%lu,\"width\":%u,"
                "\"dir\":\"%s\",\"type\":\"%s\"}",
                rec_.core, rec_.pc, rec_.addr, rec_.width, dir, dir);
        }
        out += b;
        out += "}";
        return out;
    }

private:
    bool armed_ = false;
    StrictUnmappedRecord rec_;
};

} // namespace zeebo_lle

#endif // ZEEBO_LLE_PROBE_REGISTRY_H_
