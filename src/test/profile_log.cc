// Standalone profiling target — runs sustained logging for sample/perf attachment.
// No interactive prompts. Runs for a fixed duration then exits.
// NOLINTBEGIN(readability-magic-numbers, cppcoreguidelines-narrowing-conversions)
#include "rocket/common/log.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <thread>
#include <unistd.h>
#include <vector>

int main(int argc, char* argv[]) {
    int duration_sec = 10;
    int threads = 1;
    if (argc > 1) duration_sec = std::atoi(argv[1]);
    if (argc > 2) threads = std::atoi(argv[2]);

    std::filesystem::create_directories("./profile_logs");

    rocket::Logger::Options opts;
    opts.file_path = "./profile_logs/profile.log";
    opts.per_thread_queue_bytes = 16 * 1024 * 1024; // 16MB — zero drops
    opts.flush_interval_ms = 1000;

    auto& logger = rocket::Logger::getInstance();
    logger.start(opts);

    // Warmup
    for (int i = 0; i < 10000; ++i)
        ROCKET_LOG_INFO("Warmup {}", i);
    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    std::cout << "PID=" << getpid() << " DURATION=" << duration_sec << " THREADS=" << threads << "\n";
    std::cout << "PROFILE_READY\n" << std::flush;

    std::atomic<bool> stop{false};
    std::atomic<std::uint64_t> total{0};
    std::vector<std::thread> workers;
    workers.reserve(threads);

    auto t0 = std::chrono::steady_clock::now();
    for (int t = 0; t < threads; ++t) {
        workers.emplace_back([&, t]() {
            int i = 0;
            while (!stop.load(std::memory_order_relaxed)) {
                ROCKET_LOG_INFO("Profile message {} from thread {} int={} double={} str={}",
                    i, t, i * 42, 3.14159 * i, "profile_data");
                ++i;
                total.fetch_add(1, std::memory_order_relaxed);
            }
        });
    }

    std::this_thread::sleep_for(std::chrono::seconds(duration_sec));
    stop.store(true);
    for (auto& w : workers) w.join();
    auto t1 = std::chrono::steady_clock::now();

    auto wall_ms = std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count();
    std::uint64_t n = total.load();
    std::cout << "DONE total=" << n << " wall_ms=" << wall_ms
              << " throughput=" << (n * 1000ULL / static_cast<std::uint64_t>(wall_ms))
              << " dropped=" << logger.getDroppedCount() << "\n";

    logger.stop();
    return 0;
}
// NOLINTEND(readability-magic-numbers, cppcoreguidelines-narrowing-conversions)
