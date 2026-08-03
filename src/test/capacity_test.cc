// Rate-controlled logger throughput and enqueue-latency benchmark.
// Usage: capacity_test [queue_kb] [threads] [seconds] [total_logs_per_sec]
// Pass 0 as total_logs_per_sec to run producers at full speed.
// NOLINTBEGIN
#include "rocket/common/log.h"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <thread>
#include <vector>

namespace {

constexpr std::uint64_t kNanosecondsPerSecond = 1'000'000'000ULL;
constexpr std::uint64_t kLatencySampleEvery = 32;
constexpr std::uint64_t kRateLimitBatch = 32;

std::size_t nextPowerOfTwo(std::size_t value) {
    if (value < 2) return 2;
    --value;
    for (std::size_t shift = 1; shift < sizeof(value) * 8; shift <<= 1) {
        value |= value >> shift;
    }
    return value + 1;
}

std::uint64_t percentile(const std::vector<std::uint64_t>& samples,
                         double value) {
    if (samples.empty()) return 0;
    const auto index = static_cast<std::size_t>(
        static_cast<double>(samples.size() - 1) * value / 100.0);
    return samples[index];
}

} // namespace

int main(int argc, char* argv[]) {
    int queue_kb = 160;
    int n_threads = 1;
    int duration_s = 5;
    std::uint64_t target_rate = 0;

    if (argc > 1) queue_kb = std::atoi(argv[1]);
    if (argc > 2) n_threads = std::atoi(argv[2]);
    if (argc > 3) duration_s = std::atoi(argv[3]);
    if (argc > 4) target_rate = std::strtoull(argv[4], nullptr, 10);

    if (queue_kb <= 0 || n_threads <= 0 || duration_s <= 0) {
        std::cerr << "queue_kb, threads and seconds must be positive\n";
        return 1;
    }
    if (target_rate > 0 &&
        target_rate < static_cast<std::uint64_t>(n_threads)) {
        std::cerr << "target_logs_per_sec must be 0 or at least threads\n";
        return 1;
    }

    std::size_t qbytes = static_cast<std::size_t>(queue_kb) * 1024;
    const std::size_t slots = nextPowerOfTwo(
        qbytes / sizeof(rocket::Logger::LogEntry));

    rocket::Logger::Options opts;
    opts.file_path = "/tmp/cap_test.log";
    opts.per_thread_queue_bytes = qbytes;
    opts.flush_interval_ms = 1000;

    auto& logger = rocket::Logger::getInstance();
    logger.start(opts);

    // Warmup
    const int warmup_logs = static_cast<int>(
        std::min<std::size_t>(3000, slots / 2));
    for (int i = 0; i < warmup_logs; ++i) ROCKET_LOG_INFO("warmup {}", i);
    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    // Run either at full speed or at a configured aggregate rate. Each
    // producer is paced independently, while latency sampling is sparse
    // enough not to distort the hot path.
    std::atomic<bool> stop{false};
    std::atomic<bool> start{false};
    std::atomic<int> ready{0};
    std::vector<std::thread> workers;
    std::vector<std::uint64_t> counts(n_threads, 0);
    std::vector<std::vector<std::uint64_t>> latency_samples(n_threads);
    std::chrono::steady_clock::time_point start_time;
    std::chrono::steady_clock::time_point end_time;

    for (int t = 0; t < n_threads; ++t) {
        workers.emplace_back([&, t]() {
            // Register and warm this thread's SPSC queue before measurement.
            const int thread_warmup_logs = static_cast<int>(
                std::min<std::size_t>(128, slots / 4));
            for (int i = 0; i < thread_warmup_logs; ++i) {
                ROCKET_LOG_INFO("thread warmup {} {}", t, i);
            }
            latency_samples[t].reserve(
                target_rate > 0
                    ? static_cast<std::size_t>(target_rate * duration_s /
                                               n_threads /
                                               kLatencySampleEvery + 64)
                    : 64 * 1024);

            ready.fetch_add(1, std::memory_order_release);
            while (!start.load(std::memory_order_acquire)) {
                std::this_thread::yield();
            }

            const std::uint64_t thread_rate = target_rate == 0
                ? 0
                : target_rate / static_cast<std::uint64_t>(n_threads) +
                      (static_cast<std::uint64_t>(t) <
                               target_rate % static_cast<std::uint64_t>(n_threads)
                           ? 1
                           : 0);
            std::uint64_t produced = 0;

            while (!stop.load(std::memory_order_relaxed)) {
                for (std::uint64_t batch = 0; batch < kRateLimitBatch; ++batch) {
                    const int i = static_cast<int>(produced);
                    if ((produced & (kLatencySampleEvery - 1)) == 0) {
                        const auto before = std::chrono::steady_clock::now();
                        ROCKET_LOG_INFO("Cap test {} int={} double={} str={}",
                                        i, i * 42, 3.14159 * i, "test");
                        const auto after = std::chrono::steady_clock::now();
                        latency_samples[t].push_back(
                            static_cast<std::uint64_t>(
                                std::chrono::duration_cast<std::chrono::nanoseconds>(
                                    after - before)
                                    .count()));
                    } else {
                        ROCKET_LOG_INFO("Cap test {} int={} double={} str={}",
                                        i, i * 42, 3.14159 * i, "test");
                    }
                    ++produced;
                }

                if (thread_rate > 0) {
                    const auto due = start_time + std::chrono::nanoseconds(
                        produced * kNanosecondsPerSecond / thread_rate);
                    std::this_thread::sleep_until(due);
                } else if (std::chrono::steady_clock::now() >= end_time) {
                    break;
                }
            }
            counts[t] = produced;
        });
    }

    while (ready.load(std::memory_order_acquire) < n_threads) {
        std::this_thread::yield();
    }
    const auto drops_before = logger.getDroppedCount();
    start_time = std::chrono::steady_clock::now();
    end_time = start_time + std::chrono::seconds(duration_s);
    start.store(true, std::memory_order_release);
    std::this_thread::sleep_until(end_time);
    stop.store(true, std::memory_order_release);
    for (auto& w : workers) w.join();
    auto t1 = std::chrono::steady_clock::now();

    auto wall_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        t1 - start_time).count();
    std::uint64_t total = 0;
    for (auto c : counts) total += c;
    std::uint64_t drops = logger.getDroppedCount() - drops_before;
    std::uint64_t logged = total - drops;

    std::vector<std::uint64_t> merged_samples;
    std::size_t sample_count = 0;
    for (const auto& samples : latency_samples) sample_count += samples.size();
    merged_samples.reserve(sample_count);
    for (auto& samples : latency_samples) {
        merged_samples.insert(merged_samples.end(), samples.begin(), samples.end());
    }
    std::sort(merged_samples.begin(), merged_samples.end());

    double thr = logged * 1000.0 / wall_ms;
    double drop_pct = total > 0 ? drops * 100.0 / total : 0;

    std::cout << "queue=" << queue_kb << "KB slots=" << slots
              << " threads=" << n_threads << " dur=" << duration_s << "s"
              << " target=" << (target_rate == 0 ? "max" : std::to_string(target_rate))
              << "/s\n";
    std::cout << "produced=" << total << " logged=" << logged
              << " dropped=" << drops << " (" << static_cast<int>(drop_pct) << "%)\n";
    std::cout << "sustained=" << static_cast<int>(thr) << " logs/s"
              << "  (per_thread=" << static_cast<int>(thr / n_threads) << ")\n";
    std::cout << "enqueue_latency_ns samples=" << merged_samples.size()
              << " p50=" << percentile(merged_samples, 50)
              << " p90=" << percentile(merged_samples, 90)
              << " p99=" << percentile(merged_samples, 99)
              << " p99.9=" << percentile(merged_samples, 99.9)
              << " max=" << percentile(merged_samples, 100) << "\n";

    logger.stop();
    return 0;
}
// NOLINTEND
