#include "rocket/common/msg_id_util.h"
#include <gtest/gtest.h>
#include <set>
#include <thread>
#include <vector>

namespace rocket {
namespace {

TEST(MsgIDUtil, GeneratesNonZeroIds) {
    const std::uint64_t id = MsgIDUtil::GenMsgID();
    EXPECT_NE(id, 0U);
}

TEST(MsgIDUtil, SequentialCallsIncrement) {
    const std::uint64_t first = MsgIDUtil::GenMsgID();
    const std::uint64_t second = MsgIDUtil::GenMsgID();
    EXPECT_NE(first, second);
    // Same-thread ids are numeric increments of each other.
    EXPECT_GT(second, first);
}

TEST(MsgIDUtil, ConsecutiveCallsUnique) {
    std::set<std::uint64_t> ids;
    for (int i = 0; i < 1000; ++i) {
        ids.insert(MsgIDUtil::GenMsgID());
    }
    EXPECT_EQ(ids.size(), 1000U);
}

TEST(MsgIDUtil, MultiThreadedUnique) {
    constexpr int kThreads = 4;
    constexpr int kPerThread = 500;
    std::vector<std::vector<std::uint64_t>> results(kThreads);
    std::vector<std::thread> threads;
    threads.reserve(kThreads);
    for (int t = 0; t < kThreads; ++t) {
        threads.emplace_back([&, t] {
            for (int i = 0; i < kPerThread; ++i) {
                results[t].push_back(MsgIDUtil::GenMsgID());
            }
        });
    }
    for (auto& th : threads) {
        th.join();
    }

    std::set<std::uint64_t> all;
    for (const auto& per : results) {
        all.insert(per.begin(), per.end());
    }
    EXPECT_EQ(all.size(), static_cast<std::size_t>(kThreads * kPerThread));
}

} // namespace
} // namespace rocket
