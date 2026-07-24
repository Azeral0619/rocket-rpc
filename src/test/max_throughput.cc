#include "rocket/common/log.h"
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <string>
#include <thread>

namespace {

std::pair<double, uint64_t> runAtRate(int target_rate, int duration_ms) {
    auto& logger = rocket::Logger::getInstance();
    auto before = logger.getDroppedCount();
    std::atomic<uint64_t> count{0};
    std::atomic<bool> stop{false};

    std::thread worker([&]() {
        auto interval = std::chrono::nanoseconds(1'000'000'000LL / target_rate);
        auto next = std::chrono::steady_clock::now();
        int i = 0;
        while (!stop.load()) {
            ROCKET_LOG_INFO("Rate test {} int={}", i, i * 42);
            ++i; count.fetch_add(1);
            next += interval;
            auto now = std::chrono::steady_clock::now();
            if (next > now) std::this_thread::sleep_for(next - now);
        }
    });

    std::this_thread::sleep_for(std::chrono::milliseconds(duration_ms));
    stop.store(true);
    worker.join();

    auto after = logger.getDroppedCount();
    uint64_t drops = after - before;
    uint64_t total = count.load();
    double actual_rate = total * 1000.0 / duration_ms;
    return {actual_rate, drops};
}

// Sweep rates to find max no-drop throughput
double findMax(int duration_ms) {
    // Binary search
    int lo = 500000, hi = 10000000;
    double max_clean = lo;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        auto [actual, drops] = runAtRate(mid, duration_ms);
        double loss = drops > 0 ? 100.0 * drops / (actual * duration_ms / 1000.0 + drops) : 0;
        printf("  %8d/s → actual=%.0f  drops=%llu  loss=%.1f%%\n",
               mid, actual, (unsigned long long)drops, loss);
        if (drops == 0) {
            max_clean = actual;
            lo = mid + 1;
        } else {
            hi = mid - 1;
        }
    }
    return max_clean;
}

} // namespace

int main(int argc, char* argv[]) {
    std::filesystem::create_directories("./bench_logs");
    auto& logger = rocket::Logger::getInstance();
    rocket::Logger::Options opts;
    opts.flush_interval_ms = (argc > 1 && std::string(argv[1]) == "--no-disk") ? 999999999 : 1000;
    opts.file_path = (argc > 1 && std::string(argv[1]) == "--no-disk") ? "/dev/null" : "./bench_logs/max.log";
    logger.start(opts);

    for (int i = 0; i < 10000; ++i)
        ROCKET_LOG_INFO("Warmup {}", i);
    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    const bool no_disk = (argc > 1 && std::string(argv[1]) == "--no-disk");
    printf("\n=== Max no-drop throughput %s ===\n", no_disk ? "(NO DISK)" : "(with disk)");
    double max_rate = findMax(2000);
    printf("MAX zero-drop: %.0f logs/s\n\n", max_rate);

    logger.stop();
}
