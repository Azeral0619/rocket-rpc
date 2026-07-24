#include "rocket/net/timer.h"
#include "rocket/net/timer_event.h"
#include <gtest/gtest.h>
#include <thread>

namespace rocket {
namespace {

TEST(Timer, NoEventReturnsNullopt) {
    Timer timer;
    EXPECT_EQ(timer.msUntilNextExpire(), std::nullopt);
}

TEST(Timer, MsUntilNextExpireReflectsInterval) {
    Timer timer;
    timer.addTimerEvent(std::make_shared<TimerEvent>(500, false, [] {}));
    const auto ms = timer.msUntilNextExpire();
    ASSERT_TRUE(ms.has_value());
    EXPECT_GE(*ms, 0);
    EXPECT_LE(*ms, 500);
}

TEST(Timer, FireExpiredRunsCallback) {
    Timer timer;
    int fired = 0;
    timer.addTimerEvent(std::make_shared<TimerEvent>(0, false, [&] { ++fired; }));
    timer.fireExpired();
    EXPECT_EQ(fired, 1);
    // One-shot: not re-armed.
    timer.fireExpired();
    EXPECT_EQ(fired, 1);
}

TEST(Timer, NotYetExpiredDoesNotFire) {
    Timer timer;
    int fired = 0;
    timer.addTimerEvent(std::make_shared<TimerEvent>(10000, false, [&] { ++fired; }));
    timer.fireExpired();
    EXPECT_EQ(fired, 0);
}

TEST(Timer, ExpireOrderFollowsArriveTime) {
    Timer timer;
    std::vector<int> order;
    // Insert out of order: 30ms, 10ms, 20ms.
    timer.addTimerEvent(std::make_shared<TimerEvent>(30, false, [&] { order.push_back(30); }));
    timer.addTimerEvent(std::make_shared<TimerEvent>(10, false, [&] { order.push_back(10); }));
    timer.addTimerEvent(std::make_shared<TimerEvent>(20, false, [&] { order.push_back(20); }));

    std::this_thread::sleep_for(std::chrono::milliseconds(60));
    timer.fireExpired();
    ASSERT_EQ(order.size(), 3U);
    EXPECT_EQ(order[0], 10);
    EXPECT_EQ(order[1], 20);
    EXPECT_EQ(order[2], 30);
}

TEST(Timer, CancelledEventNeverFires) {
    Timer timer;
    int fired = 0;
    auto event = std::make_shared<TimerEvent>(0, false, [&] { ++fired; });
    timer.addTimerEvent(event);
    timer.cancelTimerEvent(event);
    timer.fireExpired();
    EXPECT_EQ(fired, 0);
    // Cancelled entry pruned from the top as well.
    EXPECT_EQ(timer.msUntilNextExpire(), std::nullopt);
}

TEST(Timer, RepeatedEventRearmsUntilCancelled) {
    Timer timer;
    int fired = 0;
    auto event = std::make_shared<TimerEvent>(0, true, [&] {
        ++fired;
        if (fired == 3) {
            // Cancel from inside the callback: must not be re-armed.
        }
    });
    timer.addTimerEvent(event);

    timer.fireExpired();
    EXPECT_EQ(fired, 1);
    timer.fireExpired();
    EXPECT_EQ(fired, 2);

    event->cancel();
    timer.fireExpired();
    EXPECT_EQ(fired, 2); // cancelled before next round: skipped
}

TEST(Timer, RepeatedEventCancelledInsideCallbackNotRearmed) {
    Timer timer;
    int fired = 0;
    TimerEvent::s_ptr event;
    event = std::make_shared<TimerEvent>(0, true, [&] {
        ++fired;
        event->cancel();
    });
    timer.addTimerEvent(event);
    timer.fireExpired();
    EXPECT_EQ(fired, 1);
    EXPECT_EQ(timer.msUntilNextExpire(), std::nullopt);
}

} // namespace
} // namespace rocket
