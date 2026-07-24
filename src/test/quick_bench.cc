#include "rocket/common/log.h"
#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

namespace {

void bench(int threads, int per_thread) {
    std::atomic<bool> start{false};
    std::atomic<int> ready{0};
    std::vector<std::thread> workers;

    for (int t = 0; t < threads; ++t) {
        workers.emplace_back([&]() {
            ready.fetch_add(1);
            while (!start.load()) {}
            for (int i = 0; i < per_thread; ++i)
                ROCKET_LOG_INFO("Bench msg {} int={} double={} str={}", i, i*42, 3.14159*i, "hello");
        });
    }
    while (ready.load() < threads) std::this_thread::yield();
    start.store(true);
    for (auto& w : workers) w.join();
}

void run(int threads, int total) {
    auto& logger = rocket::Logger::getInstance();
    auto before = logger.getDroppedCount();
    auto t0 = std::chrono::steady_clock::now();
    bench(threads, total / threads);
    auto t1 = std::chrono::steady_clock::now();
    auto after = logger.getDroppedCount();
    auto wall = std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count();
    if (wall < 1) wall = 1;
    double tp = total * 1000.0 / wall;
    printf("%dT      | %-7d | %-5lld | %-10d | %llu\n",
           threads, total, (long long)wall, (int)tp, (unsigned long long)(after - before));
}

} // namespace

int main() {
    std::filesystem::create_directories("./bench_logs");
    auto& logger = rocket::Logger::getInstance();
    rocket::Logger::Options opts;
    opts.file_path = "./bench_logs/q.log";
    opts.flush_interval_ms = 1000;
    logger.start(opts);

    printf("Phase  | Logs    | Wall  | Throughput | Drops\n");
    printf("-------+---------+-------+------------+-------\n");

    for (int i = 0; i < 50000; ++i)
        ROCKET_LOG_INFO("Warmup {}", i);
    std::this_thread::sleep_for(std::chrono::milliseconds(300));

    run(1, 500000);
    run(4, 2000000);
    run(8, 4000000);

    logger.stop();
}
