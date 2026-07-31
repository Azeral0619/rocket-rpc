#include "rocket/common/msg_id_util.h"
#include <atomic>
#include <chrono>
#include <cstdint>
#include <random>
#include <unistd.h>

namespace rocket {

namespace {

constexpr std::uint64_t kIdBlockSize = 4096;

std::uint64_t initialMessageId() {
    std::uint64_t seed = 0;
    try {
        std::random_device random;
        seed = (static_cast<std::uint64_t>(random()) << 32U) ^
               static_cast<std::uint64_t>(random());
    } catch (...) {
        // steady_clock + pid below still provide a process-specific fallback.
    }
    seed ^= static_cast<std::uint64_t>(
        std::chrono::steady_clock::now().time_since_epoch().count());
    seed ^= static_cast<std::uint64_t>(::getpid()) << 17U;

    // SplitMix64 finalizer: spread weak/random_device fallbacks across all
    // bits so independently-started processes do not share an ID range.
    seed ^= seed >> 30U;
    seed *= 0xbf58476d1ce4e5b9ULL;
    seed ^= seed >> 27U;
    seed *= 0x94d049bb133111ebULL;
    seed ^= seed >> 31U;
    return seed == 0 ? 1 : seed;
}

std::atomic<std::uint64_t> g_next_id{initialMessageId()};
thread_local std::uint64_t t_next_id = 0;
thread_local std::uint64_t t_id_block_end = 0;

} // namespace

std::uint64_t MsgIDUtil::GenMsgID() {
    if (t_next_id == t_id_block_end) {
        t_next_id =
            g_next_id.fetch_add(kIdBlockSize, std::memory_order_relaxed);
        t_id_block_end = t_next_id + kIdBlockSize;
    }
    return t_next_id++;
}

} // namespace rocket
