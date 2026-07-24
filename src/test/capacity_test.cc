// Quick throughput test: find max rate for given queue size without drops.
// Usage: capacity_test [queue_kb] [threads] [seconds]
// NOLINTBEGIN
#include "rocket/common/log.h"
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <thread>
#include <vector>

int main(int argc, char* argv[]) {
    int queue_kb = 160;
    int n_threads = 1;
    int duration_s = 5;

    if (argc > 1) queue_kb = std::atoi(argv[1]);
    if (argc > 2) n_threads = std::atoi(argv[2]);
    if (argc > 3) duration_s = std::atoi(argv[3]);

    std::size_t qbytes = static_cast<std::size_t>(queue_kb) * 1024;
    int slots = static_cast<int>(qbytes / 120);

    rocket::Logger::Options opts;
    opts.file_path = "/tmp/cap_test.log";
    opts.per_thread_queue_bytes = qbytes;
    opts.flush_interval_ms = 1000;

    auto& logger = rocket::Logger::getInstance();
    logger.start(opts);

    // Warmup
    for (int i = 0; i < 3000; ++i) ROCKET_LOG_INFO("warmup {}", i);
    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    // Run at full speed
    std::atomic<bool> stop{false};
    std::vector<std::thread> workers;
    std::vector<std::uint64_t> counts(n_threads, 0);

    auto t0 = std::chrono::steady_clock::now();
    for (int t = 0; t < n_threads; ++t) {
        workers.emplace_back([&, t]() {
            int i = 0;
            while (!stop.load(std::memory_order_relaxed)) {
                ROCKET_LOG_INFO("Cap test {} int={} double={} str={}", i, i * 42, 3.14159 * i, "test");
                ++i;
            }
            counts[t] = i;
        });
    }

    std::this_thread::sleep_for(std::chrono::seconds(duration_s));
    stop.store(true);
    for (auto& w : workers) w.join();
    auto t1 = std::chrono::steady_clock::now();

    auto wall_ms = std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count();
    std::uint64_t total = 0;
    for (auto c : counts) total += c;
    std::uint64_t drops = logger.getDroppedCount();
    std::uint64_t logged = total - drops;

    double thr = logged * 1000.0 / wall_ms;
    double drop_pct = total > 0 ? drops * 100.0 / total : 0;

    std::cout << "queue=" << queue_kb << "KB slots=" << slots
              << " threads=" << n_threads << " dur=" << duration_s << "s\n";
    std::cout << "produced=" << total << " logged=" << logged
              << " dropped=" << drops << " (" << static_cast<int>(drop_pct) << "%)\n";
    std::cout << "sustained=" << static_cast<int>(thr) << " logs/s"
              << "  (per_thread=" << static_cast<int>(thr / n_threads) << ")\n";

    logger.stop();
    return 0;
}
// NOLINTEND
