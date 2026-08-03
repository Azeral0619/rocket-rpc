// Logger hot-path latency benchmark using Quill's benchmark methodology.
//
// - 1 and 4 producer threads
// - 10,000 iterations per thread
// - 20 total messages per iteration, distributed across producers
// - batch latency measured with a calibrated hardware counter and divided by
//   the number of messages in that batch
// - 2.0-2.2 ms busy wait between batches so the backend can catch up
// NOLINTBEGIN(readability-magic-numbers)
#include "rocket/common/log.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <iostream>
#include <random>
#include <thread>
#include <vector>

#if defined(__x86_64__) || defined(__i386__)
#include <x86intrin.h>
#elif defined(__aarch64__)
#include <sys/types.h>
#endif

#if defined(__linux__)
#include <pthread.h>
#include <sched.h>
#endif

namespace {

constexpr std::size_t kMessagesPerIteration = 20;
constexpr std::size_t kIterations = 10'000;
constexpr auto kMinWait = std::chrono::microseconds{2'000};
constexpr auto kMaxWait = std::chrono::microseconds{2'200};

std::uint64_t readTicks() noexcept {
#if defined(__x86_64__) || defined(__i386__)
    return __rdtsc();
#elif defined(__aarch64__)
    std::uint64_t value = 0;
    __asm__ volatile("mrs %0, cntvct_el0" : "=r"(value));
    return value;
#else
    return static_cast<std::uint64_t>(
        std::chrono::steady_clock::now().time_since_epoch().count());
#endif
}

double calibrateNanosecondsPerTick() {
    constexpr auto kSpinDuration = std::chrono::milliseconds{10};
    constexpr std::size_t kTrials = 15;
    std::vector<double> ticks_per_ns;
    ticks_per_ns.reserve(kTrials);

    for (std::size_t trial = 0; trial < kTrials; ++trial) {
        const auto begin_time = std::chrono::steady_clock::now();
        const std::uint64_t begin_ticks = readTicks();
        auto end_time = begin_time;
        std::uint64_t end_ticks = begin_ticks;
        do {
            end_time = std::chrono::steady_clock::now();
            end_ticks = readTicks();
        } while (end_time - begin_time < kSpinDuration);

        const auto elapsed_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                                    end_time - begin_time)
                                    .count();
        ticks_per_ns.push_back(
            static_cast<double>(end_ticks - begin_ticks) /
            static_cast<double>(elapsed_ns));
    }

    auto middle = ticks_per_ns.begin() +
                  static_cast<std::ptrdiff_t>(ticks_per_ns.size() / 2);
    std::nth_element(ticks_per_ns.begin(), middle, ticks_per_ns.end());
    return 1.0 / *middle;
}

void busyWait() {
    thread_local std::mt19937 generator{std::random_device{}()};
    std::uniform_int_distribution<std::int64_t> distribution{
        kMinWait.count(), kMaxWait.count()};
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::microseconds{distribution(generator)};
    while (std::chrono::steady_clock::now() < deadline) {
    }
}

void pinCurrentThread(std::size_t ordinal) noexcept {
#if defined(__linux__)
    cpu_set_t selected;
    CPU_ZERO(&selected);
    const auto cpu_count = std::max(1u, std::thread::hardware_concurrency());
    CPU_SET(static_cast<int>(ordinal % cpu_count), &selected);
    (void)pthread_setaffinity_np(pthread_self(), sizeof(selected), &selected);
#else
    (void)ordinal;
#endif
}

std::size_t messagesForThread(std::size_t total, std::size_t thread_count,
                              std::size_t thread_index) noexcept {
    const std::size_t base = total / thread_count;
    const std::size_t remainder = total % thread_count;
    return base + (thread_index < remainder ? 1 : 0);
}

std::size_t percentileIndex(std::size_t count, double percentile) {
    const auto nearest_rank = static_cast<std::size_t>(
        std::ceil(static_cast<double>(count) * percentile));
    return std::min(count - 1,
                    nearest_rank == 0 ? std::size_t{0} : nearest_rank - 1);
}

void runBenchmark(int thread_count, double nanoseconds_per_tick) {
    auto& logger = rocket::Logger::getInstance();
    const auto dropped_before = logger.getDroppedCount();

    pinCurrentThread(0);

    std::atomic<int> ready{0};
    std::atomic<bool> start{false};
    std::vector<std::vector<std::uint64_t>> all_latencies(
        static_cast<std::size_t>(thread_count));
    std::vector<std::thread> workers;
    workers.reserve(static_cast<std::size_t>(thread_count));

    std::function<void(std::uint64_t, std::uint64_t, double)> log_function =
        [](std::uint64_t i, std::uint64_t j, double value) {
            ROCKET_LOG_INFO("Logging int: {}, int: {}, double: {}", i, j,
                            value);
        };

    for (int thread_index = 0; thread_index < thread_count; ++thread_index) {
        workers.emplace_back([&, thread_index] {
            pinCurrentThread(static_cast<std::size_t>(thread_index + 1));
            auto& latencies = all_latencies[static_cast<std::size_t>(thread_index)];
            latencies.reserve(kIterations);

            // Force thread-local queue registration outside the measured loop.
            ROCKET_LOG_INFO("preallocate");
            ready.fetch_add(1, std::memory_order_acq_rel);
            while (!start.load(std::memory_order_acquire)) {
            }

            const std::size_t messages_per_thread = messagesForThread(
                kMessagesPerIteration, static_cast<std::size_t>(thread_count),
                static_cast<std::size_t>(thread_index));
            for (std::size_t iteration = 0; iteration < kIterations;
                 ++iteration) {
                const double value = static_cast<double>(iteration) +
                                     0.1 * static_cast<double>(iteration);
                const std::uint64_t begin = readTicks();
                for (std::size_t message = 0; message < messages_per_thread;
                     ++message) {
                    log_function(iteration, message, value);
                }
                const std::uint64_t end = readTicks();
                latencies.push_back(static_cast<std::uint64_t>(
                    static_cast<double>(end - begin) /
                    static_cast<double>(messages_per_thread) *
                    nanoseconds_per_tick));
                busyWait();
            }
        });
    }

    while (ready.load(std::memory_order_acquire) != thread_count) {
    }
    logger.flush();
    std::this_thread::sleep_for(std::chrono::milliseconds{100});
    start.store(true, std::memory_order_release);

    for (auto& worker : workers) worker.join();

    std::vector<std::uint64_t> latencies;
    latencies.reserve(kIterations * static_cast<std::size_t>(thread_count));
    for (auto& per_thread : all_latencies) {
        latencies.insert(latencies.end(), per_thread.begin(), per_thread.end());
    }
    std::sort(latencies.begin(), latencies.end());

    const auto dropped = logger.getDroppedCount() - dropped_before;
    std::cout << thread_count << " thread(s), total_messages="
              << kIterations * kMessagesPerIteration << ", dropped=" << dropped
              << "\n"
              << "  p50=" << latencies[percentileIndex(latencies.size(), 0.50)]
              << "ns p75=" << latencies[percentileIndex(latencies.size(), 0.75)]
              << "ns p90=" << latencies[percentileIndex(latencies.size(), 0.90)]
              << "ns p95=" << latencies[percentileIndex(latencies.size(), 0.95)]
              << "ns p99=" << latencies[percentileIndex(latencies.size(), 0.99)]
              << "ns p99.9=" << latencies[percentileIndex(latencies.size(), 0.999)]
              << "ns worst=" << latencies.back() << "ns\n";
}

} // namespace

int main() {
    std::filesystem::remove("/tmp/rocket_log_hot_path.log");

    rocket::Logger::Options options;
    options.file_path = "/tmp/rocket_log_hot_path.log";
    options.per_thread_queue_bytes = 256 * 1024;
    options.flush_interval_ms = 50;
    // Quill's hot_path_latency benchmark sets BackendOptions::sleep_duration
    // to zero so the measured producer path does not include wake-up costs.
    options.backend_sleep_duration = std::chrono::nanoseconds::zero();

    auto& logger = rocket::Logger::getInstance();
    logger.start(options);

    const double nanoseconds_per_tick = calibrateNanosecondsPerTick();
    std::cout << "Quill-compatible hot-path benchmark"
              << " ns_per_tick=" << nanoseconds_per_tick << "\n";
    runBenchmark(1, nanoseconds_per_tick);
    runBenchmark(4, nanoseconds_per_tick);

    logger.stop();
    std::filesystem::remove("/tmp/rocket_log_hot_path.log");
    return 0;
}
// NOLINTEND(readability-magic-numbers)
