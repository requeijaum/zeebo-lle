#pragma once
// zeebo_gpt_timer.h — Minimal MSM GPT (General Purpose Timer) counter model
// that advances deterministically from emulated progress (bug 5).
//
// Scope (honest): a free-running up-counter with one MATCH compare. The counter
// advances from *emulated instruction/tick progress*, NOT from being read — a
// guest polling the count register sees monotonic advance driven by how much
// the CPU actually executed, so a busy-wait on a deadline terminates instead of
// spinning forever. When the counter reaches/passes MATCH while enabled, the
// timer asserts its interrupt line exactly once per crossing; the emulator
// routes that into a VIC line.

#include <cstdint>

namespace zeebo {

struct GptTimer {
    uint32_t count  = 0;     // free-running counter (CLK_CTL scaled ticks)
    uint32_t match  = 0;     // MATCH_VAL compare
    bool     enabled = false;// TIMER_ENABLE
    bool     match_armed = true; // fire once until re-armed (write to match/clear)
    bool     clr_on_match = false; // GPT_ENABLE_CLR_ON_MATCH_EN: zero count on fire

    void enable()  { enabled = true; }
    void disable() { enabled = false; }

    void set_match(uint32_t m) { match = m; match_armed = true; }
    void clear()   { count = 0; match_armed = true; }

    // Advance the counter by `ticks` of emulated progress. Returns true if the
    // MATCH compare fired on this advance (rising crossing), meaning the caller
    // should raise the timer's VIC line.
    bool advance(uint32_t ticks) {
        if (!enabled || ticks == 0) return false;
        uint32_t before = count;
        count += ticks; // 32-bit wraparound is intentional (hardware wraps)
        if (!match_armed) return false;
        // Fire if the compare value lies in (before, count] accounting for wrap.
        bool crossed;
        if (count >= before) {
            crossed = (match > before) && (match <= count);
        } else { // wrapped
            crossed = (match > before) || (match <= count);
        }
        if (crossed) { match_armed = false; if (clr_on_match) count = 0; return true; }
        return false;
    }
};

} // namespace zeebo
