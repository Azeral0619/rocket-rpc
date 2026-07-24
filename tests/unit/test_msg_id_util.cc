#include "rocket/common/msg_id_util.h"
#include <gtest/gtest.h>
#include <set>
#include <thread>
#include <vector>

namespace rocket {
namespace {

TEST(MsgIDUtil, Generates20DigitIds) {
    const std::string id = MsgIDUtil::GenMsgID();
    EXPECT_EQ(id.length(), 20U);
    for (const char c : id) {
        EXPECT_GE(c, '0');
        EXPECT_LE(c, '9');
    }
}

TEST(MsgIDUtil, SequentialCallsIncrement) {
    const std::string first = MsgIDUtil::GenMsgID();
    const std::string second = MsgIDUtil::GenMsgID();
    EXPECT_NE(first, second);
    // Same-thread ids are numeric increments of each other.
    EXPECT_GT(second, first);
}

TEST(MsgIDUtil, ConsecutiveCallsUnique) {
    std::set<std::string> ids;
    for (int i = 0; i < 1000; ++i) {
        ids.insert(MsgIDUtil::GenMsgID());
    }
    EXPECT_EQ(ids.size(), 1000U);
}

TEST(MsgIDUtil, MultiThreadedUnique) {
    constexpr int kThreads = 4;
    constexpr int kPerThread = 500;
    std::vector<std::vector<std::string>> results(kThreads);
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

    std::set<std::string> all;
    for (const auto& per : results) {
        all.insert(per.begin(), per.end());
    }
    EXPECT_EQ(all.size(), static_cast<std::size_t>(kThreads * kPerThread));
}

} // namespace
} // namespace rocket
