// test_brew_timer.cpp — TDD para motor de timers BREW IShell (QW38)
#include "zeebo_brew_timer.h"
#include <cassert>
#include <iostream>

using namespace zeebo::brew;

void test_basic_timer() {
    BrewTimerQueue q;
    assert(q.count() == 0);

    // Schedule 50ms timer
    q.schedule(50, 0x12345678, 0x87654321);
    assert(q.count() == 1);

    // Tick 30ms -> not expired
    auto expired = q.tick(30);
    assert(expired.empty());
    assert(q.count() == 1);

    // Tick 20ms -> expired
    expired = q.tick(20);
    assert(expired.size() == 1);
    assert(expired[0].callback == 0x12345678);
    assert(expired[0].user_data == 0x87654321);
    assert(!expired[0].r0_override.has_value());
    assert(q.count() == 0);
}

void test_timer_rearm_dedup() {
    BrewTimerQueue q;
    q.schedule(100, 0x11112222, 0x33334444);
    assert(q.count() == 1);

    // Re-arm with shorter time
    q.schedule(20, 0x11112222, 0x33334444);
    assert(q.count() == 1); // No duplicate

    auto expired = q.tick(25);
    assert(expired.size() == 1);
    assert(expired[0].callback == 0x11112222);
    assert(q.count() == 0);
}

void test_timer_cancel() {
    BrewTimerQueue q;
    q.schedule(100, 0x11112222, 0x33334444);
    q.schedule(200, 0x55556666, 0x77778888);
    assert(q.count() == 2);

    bool cancelled = q.cancel(0x11112222, 0x33334444);
    assert(cancelled);
    assert(q.count() == 1);

    // Cancelling non-existent returns false
    assert(!q.cancel(0x99999999, 0x0));

    auto expired = q.tick(150);
    assert(expired.empty()); // Only 55556666 remains with 50ms left

    expired = q.tick(60);
    assert(expired.size() == 1);
    assert(expired[0].callback == 0x55556666);
    assert(q.count() == 0);
}

void test_r0_override() {
    BrewTimerQueue q;
    q.schedule(40, 0xaaaa0000, 0xbbbb0000, 0xdeadbeef);
    auto expired = q.tick(40);
    assert(expired.size() == 1);
    assert(expired[0].callback == 0xaaaa0000);
    assert(expired[0].user_data == 0xbbbb0000);
    assert(expired[0].r0_override.has_value());
    assert(expired[0].r0_override.value() == 0xdeadbeef);
}

int main() {
    test_basic_timer();
    test_timer_rearm_dedup();
    test_timer_cancel();
    test_r0_override();
    std::cout << "[PASS] BREW Timer TDD passed successfully.\n";
    return 0;
}
