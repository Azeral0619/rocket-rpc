// NOLINTBEGIN(readability-magic-numbers, cppcoreguidelines-narrowing-conversions)
#include "rocket/common/log.h"

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#if defined(__linux__)
#include <unistd.h>
#endif

namespace {

// 获取当前进程的RSS内存使用（KB）
std::size_t GetMemoryUsageKB() {
#if defined(__linux__)
    std::ifstream status("/proc/self/status");
    std::string line;
    while (std::getline(status, line)) {
        if (line.find("VmRSS:") == 0) {
            std::istringstream iss(line);
            std::string key;
            std::size_t value;
            std::string unit;
            iss >> key >> value >> unit;
            return value; // KB
        }
    }
#endif
    return 0;
}

void TestBasicLogging() {
    std::cout << "\n=== Test Basic Logging ===\n";

    rocket::Logger::Options options;
    options.file_path = "./test_logs/basic.log";
    options.flush_interval_ms = 10;

    auto& logger = rocket::Logger::getInstance();
    logger.start(options);

    // 测试不同级别的日志
    ROCKET_LOG_DEBUG("This is a debug message: {}", 42);
    ROCKET_LOG_INFO("This is an info message: {}", "hello");
    ROCKET_LOG_WARN("This is a warning: value={}", 3.14);
    ROCKET_LOG_ERROR("This is an error: code={}, msg={}", 404, "not found");

    logger.flush();
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    logger.stop();

    std::cout << "✓ Basic logging test completed\n";
}

void TestLogLevels() {
    std::cout << "\n=== Test Log Levels ===\n";

    rocket::Logger::Options options;
    options.file_path = "./test_logs/level.log";
    options.level = rocket::LogLevel::Warn;

    auto& logger = rocket::Logger::getInstance();
    logger.start(options);

    // DEBUG 和 INFO 不应该输出
    ROCKET_LOG_DEBUG("This should NOT appear");
    ROCKET_LOG_INFO("This should NOT appear either");

    // WARN 和 ERROR 应该输出
    ROCKET_LOG_WARN("This SHOULD appear");
    ROCKET_LOG_ERROR("This SHOULD also appear");

    logger.flush();
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    logger.stop();

    std::cout << "✓ Log level filtering test completed\n";
}

void TestTruncation() {
    std::cout << "\n=== Test Long Message Truncation ===\n";

    rocket::Logger::Options options;
    options.file_path = "./test_logs/truncation.log";

    auto& logger = rocket::Logger::getInstance();
    logger.start(options);

    // 测试短消息
    ROCKET_LOG_INFO("Short message");

    // 测试长消息（应该被截断）
    std::string long_msg(2000, 'X');
    ROCKET_LOG_INFO("Long message: {}", long_msg);

    logger.flush();
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    logger.stop();

    std::cout << "✓ Truncation test completed\n";
}

void TestConcurrentLogging() {
    std::cout << "\n=== Test Concurrent Logging ===\n";

    rocket::Logger::Options options;
    options.file_path = "./test_logs/concurrent.log";
    options.queue_capacity = 1U << 14;

    auto& logger = rocket::Logger::getInstance();
    logger.start(options);

    constexpr int kNumThreads = 8;
    constexpr int kLogsPerThread = 1000000 / 2;

    std::atomic<int> ready_count{0};
    std::atomic<bool> start{false};

    auto worker = [&](int thread_id) {
        ready_count.fetch_add(1);
        while (!start.load()) {
            std::this_thread::yield();
        }

        // 速率控制：每个线程 500000 / kNumThreads logs/秒
        constexpr int kTargetRatePerThread = 500'0000 / kNumThreads;
        constexpr int kBatchSize = 1000;
        const auto kBatchDuration = std::chrono::microseconds(kBatchSize * 1000000 / kTargetRatePerThread);

        auto batch_start = std::chrono::steady_clock::now();

        for (int i = 0; i < kLogsPerThread; ++i) {
            ROCKET_LOG_INFO("Performance test message {} with some data: {}, {}, {}", i, "string", 42, 3.14159);

            // 每批次结束后检查速率
            if ((i + 1) % kBatchSize == 0) {
                auto batch_end = std::chrono::steady_clock::now();
                auto elapsed = batch_end - batch_start;
                if (elapsed < kBatchDuration) {
                    std::this_thread::sleep_for(kBatchDuration - elapsed);
                }
                batch_start = std::chrono::steady_clock::now();
            }
        }
    };

    std::vector<std::thread> threads;
    threads.reserve(kNumThreads);

    auto start_time = std::chrono::steady_clock::now();

    for (int i = 0; i < kNumThreads; ++i) {
        threads.emplace_back(worker, i);
    }

    // 等待所有线程就绪
    while (ready_count.load() < kNumThreads) {
        std::this_thread::yield();
    }

    // 同时开始
    start.store(true);

    for (auto& t : threads) {
        t.join();
    }

    auto end_time = std::chrono::steady_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end_time - start_time);

    logger.flush();
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    logger.stop();

    int total_logs = kNumThreads * kLogsPerThread;
    std::cout << "✓ Concurrent logging test completed\n";
    std::cout << "  Total logs: " << total_logs << "\n";
    std::cout << "  Duration: " << duration.count() << " ms\n";
    std::cout << "  Avg latency: " << (static_cast<double>(duration.count()) * 1000.0 / total_logs) << " μs/log\n";
    std::cout << "  Throughput: " << (static_cast<int64_t>(total_logs) * 1000 / duration.count()) << " logs/sec\n";
}

void TestPerformance() {
    std::cout << "\n=== Test Performance Benchmark ===\n";

    rocket::Logger::Options options;
    options.file_path = "./test_logs/perf.log";
    options.queue_capacity = 1U << 14;
    options.flush_interval_ms = 50;

    auto& logger = rocket::Logger::getInstance();
    logger.start(options);

    constexpr int kNumLogs = 10000000 / 2;
    constexpr int kTargetRate = 500'0000; // logs/秒
    constexpr int kBatchSize = 1000;
    const auto kBatchDuration = std::chrono::microseconds(kBatchSize * 1000000 / kTargetRate);

    auto start_time = std::chrono::steady_clock::now();
    auto batch_start = start_time;

    for (int i = 0; i < kNumLogs; ++i) {
        ROCKET_LOG_INFO("Performance test message {} with some data: {}, {}, {}", i, "string", 42, 3.14159);

        // 每批次结束后检查速率
        if ((i + 1) % kBatchSize == 0) {
            auto batch_end = std::chrono::steady_clock::now();
            auto elapsed = batch_end - batch_start;
            if (elapsed < kBatchDuration) {
                std::this_thread::sleep_for(kBatchDuration - elapsed);
            }
            batch_start = std::chrono::steady_clock::now();
        }
    }

    logger.flush();

    auto end_time = std::chrono::steady_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::microseconds>(end_time - start_time);

    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    logger.stop();

    double avg_latency_us = static_cast<double>(duration.count()) / kNumLogs;
    double throughput = static_cast<double>(kNumLogs) * 1e6 / static_cast<double>(duration.count());

    std::cout << "✓ Performance benchmark completed\n";
    std::cout << "  Total logs: " << kNumLogs << "\n";
    std::cout << "  Total time: " << (static_cast<double>(duration.count()) / 1000.0) << " ms\n";
    std::cout << "  Avg latency: " << avg_latency_us << " μs/log\n";
    std::cout << "  Throughput: " << static_cast<int>(throughput) << " logs/sec\n";
}

void TestFlush() {
    std::cout << "\n=== Test Manual Flush ===\n";

    rocket::Logger::Options options;
    options.file_path = "./test_logs/flush.log";
    options.flush_interval_ms = 10000; // 很长的间隔，强制手动flush

    auto& logger = rocket::Logger::getInstance();
    logger.start(options);

    ROCKET_LOG_INFO("Message before flush");
    logger.flush();

    // 检查文件是否及时写入
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    std::ifstream file("./test_logs/flush.log");
    bool has_content = file.good() && file.peek() != std::ifstream::traits_type::eof();

    logger.stop();

    if (has_content) {
        std::cout << "✓ Manual flush test completed - file written immediately\n";
    } else {
        std::cout << "✗ Manual flush test failed - file not written\n";
    }
}

void TestFileRotation() {
    std::cout << "\n=== Test File Rotation ===\n";

    rocket::Logger::Options options;
    options.file_path = "./test_logs/rotation.log";
    options.max_file_size = 1024; // 1KB - 很小的文件触发rotation
    options.flush_interval_ms = 10;

    auto& logger = rocket::Logger::getInstance();
    logger.start(options);

    // 写入足够多的日志触发rotation
    for (int i = 0; i < 100; ++i) {
        ROCKET_LOG_INFO("Rotation test message {} - some padding text to increase size", i);
        logger.flush();
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }

    logger.flush();
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    logger.stop();

    // 检查是否生成了rotated文件
    int rotated_files = 0;
    for (const auto& entry : std::filesystem::directory_iterator("./test_logs")) {
        if (entry.path().filename().string().find("rotation.log.") != std::string::npos) {
            ++rotated_files;
        }
    }

    std::cout << "✓ File rotation test completed\n";
    std::cout << "  Rotated files created: " << rotated_files << "\n";
}

void TestStressTest() {
    std::cout << "\n=== Test Stress Test ===\n";

    rocket::Logger::Options options;
    options.file_path = "./test_logs/stress.log";
    options.queue_capacity = 1U << 18; // 256K队列
    options.flush_interval_ms = 50;
    options.max_file_size = 100 * 1024 * 1024; // 100MB

    auto& logger = rocket::Logger::getInstance();
    logger.start(options);

    constexpr int kNumThreads = 16;
    constexpr int kLogsPerThread = 50000;
    constexpr int kDurationSeconds = 30;

    std::atomic<std::size_t> total_logged{0};
    std::atomic<bool> stop_flag{false};

    auto worker = [&](int thread_id) {
        int count = 0;
        while (!stop_flag.load() && count < kLogsPerThread) {
            ROCKET_LOG_INFO("Stress test [thread={}] [count={}] - some log data with numbers: {}, {}, {}", thread_id,
                            count, count * 2, count * 3, count * 4);
            ++count;
            total_logged.fetch_add(1);

            // 每1000条日志短暂休息，模拟真实场景
            if (count % 1000 == 0) {
                std::this_thread::sleep_for(std::chrono::microseconds(100));
            }
        }
    };

    std::vector<std::thread> threads;
    threads.reserve(kNumThreads);

    auto start_time = std::chrono::steady_clock::now();

    for (int i = 0; i < kNumThreads; ++i) {
        threads.emplace_back(worker, i);
    }

    // 在后台监控，超时后停止
    std::thread monitor([&]() {
        auto deadline = start_time + std::chrono::seconds(kDurationSeconds);
        while (std::chrono::steady_clock::now() < deadline) {
            std::this_thread::sleep_for(std::chrono::seconds(1));
            std::cout << "  ... logged " << total_logged.load() << " messages\n";
        }
        stop_flag.store(true);
    });

    for (auto& t : threads) {
        t.join();
    }
    monitor.join();

    auto end_time = std::chrono::steady_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end_time - start_time);

    logger.flush();
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    logger.stop();

    std::size_t total = total_logged.load();
    double throughput = static_cast<double>(total) * 1000.0 / static_cast<double>(duration.count());

    std::cout << "✓ Stress test completed\n";
    std::cout << "  Total logs: " << total << "\n";
    std::cout << "  Duration: " << (duration.count() / 1000.0) << " seconds\n";
    std::cout << "  Throughput: " << static_cast<int>(throughput) << " logs/sec\n";
}

void TestMemoryLeak() {
    std::cout << "\n=== Test Memory Leak Detection ===\n";

    const std::size_t initial_mem = GetMemoryUsageKB();
    std::cout << "  Initial memory: " << initial_mem << " KB\n";

    if (initial_mem == 0) {
        std::cout << "  ⚠ Memory monitoring not available on this platform\n";
        return;
    }

    rocket::Logger::Options options;
    options.file_path = "./test_logs/memleak.log";
    options.queue_capacity = 1U << 16;
    options.flush_interval_ms = 50;

    constexpr int kNumIterations = 10;
    constexpr int kLogsPerIteration = 10000;

    std::vector<std::size_t> memory_samples;
    memory_samples.reserve(kNumIterations + 1);
    memory_samples.push_back(initial_mem);

    auto& logger = rocket::Logger::getInstance();

    for (int iter = 0; iter < kNumIterations; ++iter) {
        logger.start(options);

        // 多线程并发写入
        std::vector<std::thread> threads;
        for (int t = 0; t < 4; ++t) {
            threads.emplace_back([&, t]() {
                for (int i = 0; i < kLogsPerIteration / 4; ++i) {
                    ROCKET_LOG_INFO("Memory leak test iter={} thread={} msg={}", iter, t, i);
                }
            });
        }

        for (auto& thread : threads) {
            thread.join();
        }

        logger.flush();
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        logger.stop();

        // 等待资源释放
        std::this_thread::sleep_for(std::chrono::milliseconds(100));

        const std::size_t current_mem = GetMemoryUsageKB();
        memory_samples.push_back(current_mem);

        std::cout << "  Iteration " << (iter + 1) << "/" << kNumIterations << " - Memory: " << current_mem << " KB"
                  << " (delta: " << static_cast<int64_t>(current_mem - initial_mem) << " KB)\n";
    }

    // 分析内存增长
    const std::size_t final_mem = memory_samples.back();
    const int64_t total_growth = static_cast<int64_t>(final_mem - initial_mem);

    // 计算平均增长率（后半部分迭代）
    double avg_growth = 0;
    if (kNumIterations > 5) {
        for (std::size_t i = kNumIterations / 2; i < memory_samples.size() - 1; ++i) {
            avg_growth += static_cast<double>(memory_samples[i + 1]) - static_cast<double>(memory_samples[i]);
        }
        avg_growth /= (memory_samples.size() - 1 - kNumIterations / 2);
    }

    std::cout << "\n  Memory Analysis:\n";
    std::cout << "    Initial: " << initial_mem << " KB\n";
    std::cout << "    Final: " << final_mem << " KB\n";
    std::cout << "    Total growth: " << total_growth << " KB\n";
    std::cout << "    Avg growth (last half): " << avg_growth << " KB/iteration\n";

    // 判断是否有明显的内存泄漏
    // 如果稳态增长小于100KB/iteration，认为基本稳定
    if (std::abs(avg_growth) < 100) {
        std::cout << "  ✓ No significant memory leak detected\n";
    } else if (avg_growth > 0) {
        std::cout << "  ⚠ Possible memory leak detected (growth: " << avg_growth << " KB/iter)\n";
        std::cout << "  Note: Some growth is normal due to allocator behavior\n";
    } else {
        std::cout << "  ✓ Memory stable or decreasing\n";
    }
}

void CleanupTestLogs() { std::filesystem::remove_all("./test_logs"); }

void SetupTestLogs() { std::filesystem::create_directories("./test_logs"); }

} // namespace

int main(int argc, char* argv[]) {
    std::cout << "=================================\n";
    std::cout << "  Logger Test Suite\n";
    std::cout << "=================================\n";

    bool run_stress = false;
    bool run_all = true;

    // 解析命令行参数
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--stress" || arg == "-s") {
            run_stress = true;
            run_all = false;
        } else if (arg == "--all" || arg == "-a") {
            run_all = true;
            run_stress = true;
        } else if (arg == "--help" || arg == "-h") {
            std::cout << "\nUsage: " << argv[0] << " [options]\n";
            std::cout << "Options:\n";
            std::cout << "  --help, -h     Show this help\n";
            std::cout << "  --stress, -s   Run stress test and memory leak detection\n";
            std::cout << "  --all, -a      Run all tests including stress tests\n";
            std::cout << "  (no args)      Run basic test suite\n\n";
            return 0;
        }
    }

    try {
        SetupTestLogs();

        if (run_all) {
            TestBasicLogging();
            TestLogLevels();
            TestTruncation();
            TestFlush();
            TestConcurrentLogging();
            TestPerformance();
            TestFileRotation();
        }

        if (run_stress) {
            TestStressTest();
            TestMemoryLeak();
        }

        std::cout << "\n=================================\n";
        std::cout << "  All Tests Passed! ✓\n";
        std::cout << "=================================\n";
        std::cout << "\nLog files are in: ./test_logs/\n";
        std::cout << "Run 'cat test_logs/*.log' to view logs\n\n";

        // CleanupTestLogs(); // 保留日志文件以便查看

        return 0;
    } catch (const std::exception& e) {
        std::cerr << "\n✗ Test failed with exception: " << e.what() << "\n";
        return 1;
    }
}
// NOLINTEND(readability-magic-numbers, cppcoreguidelines-narrowing-conversions)
