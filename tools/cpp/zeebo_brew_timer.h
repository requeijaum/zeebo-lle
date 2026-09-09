// zeebo_brew_timer.h — BREW IShell Timer Engine for Zeebo LLE (QW38)
// Models one-shot ISHELL_SetTimer / CancelTimer semantics according to BREW 4.0.2 ABI.
#pragma once

#include <cstdint>
#include <vector>
#include <optional>
#include <algorithm>

namespace zeebo::brew {

struct ExpiredTimer {
    uint32_t callback;
    uint32_t user_data;
    std::optional<uint32_t> r0_override;
};

class BrewTimerQueue {
public:
    struct TimerEntry {
        uint32_t remaining_ms;
        uint32_t callback;
        uint32_t user_data;
        std::optional<uint32_t> r0_override;
    };

    BrewTimerQueue() = default;

    // Schedules or re-arms a timer.
    // If a timer with the exact same (callback, user_data) already exists,
    // in BREW it updates/reschedules the deadline rather than creating duplicates.
    void schedule(uint32_t ms, uint32_t callback, uint32_t user_data,
                  std::optional<uint32_t> r0_override = std::nullopt) {
        if (callback == 0) return;
        for (auto& t : timers_) {
            if (t.callback == callback && t.user_data == user_data) {
                t.remaining_ms = ms;
                t.r0_override = r0_override;
                return;
            }
        }
        timers_.push_back({ms, callback, user_data, r0_override});
    }

    // Cancels pending timer by (callback, user_data). Returns true if found and removed.
    bool cancel(uint32_t callback, uint32_t user_data) {
        auto it = std::remove_if(timers_.begin(), timers_.end(), [&](const TimerEntry& t) {
            return t.callback == callback && t.user_data == user_data;
        });
        if (it != timers_.end()) {
            timers_.erase(it, timers_.end());
            return true;
        }
        return false;
    }

    // Advances time by elapsed_ms, collecting and removing all expired timers in registration order.
    std::vector<ExpiredTimer> tick(uint32_t elapsed_ms) {
        std::vector<ExpiredTimer> expired;
        std::vector<TimerEntry> active;
        for (auto& t : timers_) {
            if (t.remaining_ms <= elapsed_ms) {
                expired.push_back({t.callback, t.user_data, t.r0_override});
            } else {
                t.remaining_ms -= elapsed_ms;
                active.push_back(t);
            }
        }
        timers_ = std::move(active);
        return expired;
    }

    size_t count() const { return timers_.size(); }
    void clear() { timers_.clear(); }

private:
    std::vector<TimerEntry> timers_;
};

} // namespace zeebo::brew
