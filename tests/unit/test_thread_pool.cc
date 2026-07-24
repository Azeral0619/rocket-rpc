#include "rocket/common/thread_pool.h"
#include <atomic>
#include <gtest/gtest.h>
#include <thread>

namespace rocket {
namespace {

TEST(ThreadPool, ExecuteRunsTask) {
    std::atomic<bool> ran{false};
    {
        ThreadPool pool(2);
        pool.execute([&] { ran = true; });
        pool.stopAndDrain();
    }
    EXPECT_TRUE(ran);
}

TEST(ThreadPool, SubmitReturnsFutureValue) {
    ThreadPool pool(2);
    auto future = pool.submit([](int a, int b) { return a + b; }, 20, 22);
    EXPECT_EQ(future.get(), 42);
}

TEST(ThreadPool, FuturePropagatesException) {
    ThreadPool pool(1);
    auto future = pool.submit([]() -> int { throw std::runtime_error("boom"); });
    EXPECT_THROW(future.get(), std::runtime_error);
}

TEST(ThreadPool, RunsTasksConcurrently) {
    ThreadPool pool(4);
    std::atomic<int> done{0};
    constexpr int kTasks = 100;
    for (int i = 0; i < kTasks; ++i) {
        pool.execute([&] { done.fetch_add(1, std::memory_order_relaxed); });
    }
    pool.stopAndDrain();
    EXPECT_EQ(done, kTasks);
}

TEST(ThreadPool, StopAndDrainDrainsQueue) {
    std::atomic<int> done{0};
    {
        ThreadPool pool(1);
        constexpr int kTasks = 50;
        for (int i = 0; i < kTasks; ++i) {
            pool.execute([&] {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
                done.fetch_add(1, std::memory_order_relaxed);
            });
        }
        pool.stopAndDrain();
        // Destructor joins workers; by scope exit all queued tasks have run.
    }
    EXPECT_EQ(done, 50);
}

TEST(ThreadPool, SubmitAfterStopThrows) {
    ThreadPool pool(1);
    pool.stopAndDrain();
    EXPECT_THROW(pool.submit([] {}), std::runtime_error);
}

} // namespace
} // namespace rocket
