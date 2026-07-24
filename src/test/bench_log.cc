// Benchmark for perf/flame-graph analysis — no rate limiting, pure hot path.
// Build: xmake f -m release && xmake build bench_log
// Profile: sample bench_log <pid> -f perf_data.txt
// NOLINTBEGIN(readability-magic-numbers, cppcoreguidelines-narrowing-conversions)
#include "rocket/common/log.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>
#include <thread>
#include <unistd.h>
#include <vector>

namespace {

constexpr int kLogsPerThread = 500'000;
constexpr int kWarmupLogs = 50'000;

struct LatencySample {
    std::uint64_t ns;
};

// Run a single-threaded benchmark, collecting per-call latency samples.
void BenchSingleThread(int total, std::vector<LatencySample>& samples) {
    samples.clear();
    samples.reserve(total);

    auto& logger = rocket::Logger::getInstance();
    for (int i = 0; i < total; ++i) {
        auto t0 = std::chrono::steady_clock::now();
        ROCKET_LOG_INFO("Bench message {} int={} double={} str={}", i, i * 42, 3.14159 * i, "hello_bench");
        auto t1 = std::chrono::steady_clock::now();
        samples.push_back({static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count())});
    }
}

// Multi-threaded benchmark — each thread records its own latency samples.
void BenchMultiThread(int threads, int per_thread, std::vector<std::vector<LatencySample>>& all_samples) {
    all_samples.clear();
    all_samples.resize(threads);
    for (auto& v : all_samples) {
        v.reserve(per_thread);
    }

    std::atomic<bool> start{false};
    std::atomic<int> ready{0};
    std::vector<std::thread> workers;
    workers.reserve(threads);

    for (int t = 0; t < threads; ++t) {
        workers.emplace_back([&, t]() {
            auto& samples = all_samples[t];
            ready.fetch_add(1);
            while (!start.load()) { /* spin */ }
            for (int i = 0; i < per_thread; ++i) {
                auto t0 = std::chrono::steady_clock::now();
                ROCKET_LOG_INFO("Bench msg {} int={} double={} str={}", i, i * 42, 3.14159 * i, "hello_bench");
                auto t1 = std::chrono::steady_clock::now();
                samples.push_back({static_cast<std::uint64_t>(
                    std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count())});
            }
        });
    }

    while (ready.load() < threads) { std::this_thread::yield(); }
    start.store(true);
    for (auto& w : workers) w.join();
}

void PrintStats(const std::vector<LatencySample>& samples, const char* label) {
    if (samples.empty()) return;

    std::vector<std::uint64_t> ns;
    ns.reserve(samples.size());
    for (auto& s : samples) ns.push_back(s.ns);
    std::sort(ns.begin(), ns.end());

    auto pct = [&](double p) -> std::uint64_t {
        auto idx = static_cast<std::size_t>(ns.size() * p / 100.0);
        if (idx >= ns.size()) idx = ns.size() - 1;
        return ns[idx];
    };

    std::uint64_t sum = 0;
    for (auto v : ns) sum += v;
    double avg = static_cast<double>(sum) / ns.size();

    std::cout << "  " << label << ":\n";
    std::cout << "    count=" << ns.size() << "  avg=" << static_cast<int>(avg) << "ns\n";
    std::cout << "    p50=" << pct(50) << "ns  p75=" << pct(75) << "ns  p90=" << pct(90) << "ns\n";
    std::cout << "    p95=" << pct(95) << "ns  p99=" << pct(99) << "ns  p99.9=" << pct(99.9) << "ns\n";
    std::cout << "    min=" << ns.front() << "ns  max=" << ns.back() << "ns\n";
}

void RunBenchmarks() {
    auto& logger = rocket::Logger::getInstance();

    {
        rocket::Logger::Options opts;
        opts.file_path = "./bench_logs/perf_bench.log";
        opts.per_thread_queue_bytes = 16 * 1024 * 1024; // 16MB — ensure zero drops
        opts.flush_interval_ms = 1000;
        logger.start(opts);
    }

    // Warmup
    std::cout << "Warming up (" << kWarmupLogs << " logs)...\n";
    for (int i = 0; i < kWarmupLogs; ++i) {
        ROCKET_LOG_INFO("Warmup {} int={}", i, i * 2);
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(500));

    // ── Single-thread ──
    std::cout << "\n=== Single-thread benchmark (" << kLogsPerThread << " logs) ===\n";
    {
        std::vector<LatencySample> samples;
        auto t0 = std::chrono::steady_clock::now();
        BenchSingleThread(kLogsPerThread, samples);
        auto t1 = std::chrono::steady_clock::now();
        auto wall_ms = std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count();
        double throughput = static_cast<double>(kLogsPerThread) * 1000.0 / wall_ms;
        std::cout << "  Wall: " << wall_ms << "ms  Throughput: " << static_cast<int>(throughput) << " logs/s\n";
        PrintStats(samples, "1T latency");
    }

    // ── 4-thread ──
    std::cout << "\n=== 4-thread benchmark (" << kLogsPerThread << "/thread) ===\n";
    {
        constexpr int N = 4;
        std::vector<std::vector<LatencySample>> all;
        auto t0 = std::chrono::steady_clock::now();
        BenchMultiThread(N, kLogsPerThread, all);
        auto t1 = std::chrono::steady_clock::now();
        auto wall_ms = std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count();
        double throughput = static_cast<double>(kLogsPerThread * N) * 1000.0 / wall_ms;
        std::cout << "  Wall: " << wall_ms << "ms  Throughput: " << static_cast<int>(throughput) << " logs/s\n";

        std::vector<LatencySample> merged;
        for (auto& v : all) merged.insert(merged.end(), v.begin(), v.end());
        PrintStats(merged, "4T latency (merged)");
    }

    // ── 8-thread ──
    std::cout << "\n=== 8-thread benchmark (" << kLogsPerThread << "/thread) ===\n";
    {
        constexpr int N = 8;
        std::vector<std::vector<LatencySample>> all;
        auto t0 = std::chrono::steady_clock::now();
        BenchMultiThread(N, kLogsPerThread, all);
        auto t1 = std::chrono::steady_clock::now();
        auto wall_ms = std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count();
        double throughput = static_cast<double>(kLogsPerThread * N) * 1000.0 / wall_ms;
        std::cout << "  Wall: " << wall_ms << "ms  Throughput: " << static_cast<int>(throughput) << " logs/s\n";

        std::vector<LatencySample> merged;
        for (auto& v : all) merged.insert(merged.end(), v.begin(), v.end());
        PrintStats(merged, "8T latency (merged)");
    }

    std::cout << "\n  dropped=" << logger.getDroppedCount() << "\n";

    // Signal for profiler attachment
    std::cout << "\n>>> PID=" << getpid() << " — attach profiler now, then press Enter to run profiled round...\n";
    std::cout << "    Run in another terminal: sample " << getpid() << " 10 -f ./bench_logs/perf_sample.txt\n";
    std::cout << "    Or: leaks " << getpid() << "\n";
    std::getchar();

    // Profiled round: sustained logging for 10 seconds
    std::cout << "Running profiled round (10s, 1 thread, max speed)...\n";
    {
        std::atomic<bool> stop{false};
        std::atomic<std::uint64_t> count{0};
        std::thread worker([&]() {
            int i = 0;
            while (!stop.load()) {
                ROCKET_LOG_INFO("Profiled msg {} int={} double={} str={}", i, i * 42, 3.14159 * i, "profile_me");
                ++i;
                count.fetch_add(1);
            }
        });

        std::this_thread::sleep_for(std::chrono::seconds(10));
        stop.store(true);
        worker.join();
        std::cout << "  Logged " << count.load() << " entries in 10s\n";
    }

    std::cout << "\n  final dropped=" << logger.getDroppedCount() << "\n";
    logger.stop();
    std::cout << "Done. Logs in ./bench_logs/\n";
}

} // namespace

int main() {
    std::filesystem::create_directories("./bench_logs");
    std::cout << "=== Logger Perf Benchmark ===\n";
    std::cout << "PID: " << getpid() << "\n";

    try {
        RunBenchmarks();
    } catch (const std::exception& e) {
        std::cerr << "Benchmark failed: " << e.what() << '\n';
        return 1;
    }
    return 0;
}
// NOLINTEND(readability-magic-numbers, cppcoreguidelines-narrowing-conversions)
