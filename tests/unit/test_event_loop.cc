#include "rocket/net/event_loop.h"
#include "rocket/net/timer_event.h"
#include <atomic>
#include <chrono>
#include <gtest/gtest.h>
#include <memory>
#include <thread>

namespace rocket {
namespace {

// Runs fn on a fresh EventLoop in a worker thread; returns after the loop stops.
template <typename F>
void WithLoop(F&& fn) {
    std::jthread worker([&fn] {
        EventLoop loop;
        fn(loop);
        if (loop.isLooping() == false) {
            loop.loop();
        }
    });
}

TEST(EventLoop, RunInLoopExecutesImmediatelyOnLoopThread) {
    std::atomic<bool> ran{false};
    std::atomic<bool> on_loop_thread{false};
    WithLoop([&](EventLoop& loop) {
        loop.queueInLoop([&] {
            loop.runInLoop([&] {
                ran = true;
                on_loop_thread = loop.isInLoopThread();
            });
            loop.stop();
        });
    });
    EXPECT_TRUE(ran);
    EXPECT_TRUE(on_loop_thread);
}

TEST(EventLoop, QueueInLoopFromForeignThreadWakesLoop) {
    std::atomic<bool> ran{false};
    WithLoop([&](EventLoop& loop) {
        // Post from yet another thread: queueInLoop must wake the poller.
        std::thread foreign([&] { loop.queueInLoop([&] { ran = true; loop.stop(); }); });
        foreign.detach();
    });
    EXPECT_TRUE(ran);
}

TEST(EventLoop, StopExitsLoop) {
    std::atomic<bool> exited{false};
    std::jthread worker([&] {
        EventLoop loop;
        loop.queueInLoop([&] { loop.stop(); });
        loop.loop();
        exited = true;
    });
    worker.join();
    EXPECT_TRUE(exited);
}

TEST(EventLoop, TimerInLoopEndToEnd) {
    std::atomic<bool> fired{false};
    std::atomic<long long> delay_ms{0};
    WithLoop([&](EventLoop& loop) {
        const auto start = std::chrono::steady_clock::now();
        loop.queueInLoop([&] {
            loop.addTimerEvent(std::make_shared<TimerEvent>(50, false, [&, start] {
                fired = true;
                delay_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                               std::chrono::steady_clock::now() - start)
                               .count();
                loop.stop();
            }));
        });
    });
    EXPECT_TRUE(fired);
    // Wait-timeout based timers: allow generous slack for CI machines.
    EXPECT_GE(delay_ms, 40);
    EXPECT_LE(delay_ms, 2000);
}

TEST(EventLoop, AssertInLoopThreadTerminates) {
    EventLoop loop;
    EXPECT_DEATH(
        {
            std::thread foreign([&] { loop.assertInLoopThread(); });
            foreign.join();
        },
        ".*");
}

TEST(EventLoop, SecondLoopInSameThreadTerminates) {
    EventLoop loop;
    EXPECT_DEATH({ EventLoop another; }, ".*");
}

} // namespace
} // namespace rocket
