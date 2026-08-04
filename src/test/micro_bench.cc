// Micro-benchmark to isolate Logger hot-path costs
#include "rocket/common/log.h"
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

using namespace std::chrono;

namespace {

// Measure just the setArgs + runFormat round-trip (no queue, no I/O)
uint64_t benchFormatRoundtrip(int iters) {
    rocket::Logger::LogEntry entry;
    std::array<char, 256> payload{};
    std::string buf;
    auto t0 = steady_clock::now();
    for (int i = 0; i < iters; ++i) {
        buf.clear();
        entry.setArgs(payload.data(), "test {} int={} double={} str={}",
                      i, i*42, 3.14159*i, "hello");
        entry.runFormat(buf, payload.data());
    }
    auto t1 = steady_clock::now();
    return duration_cast<nanoseconds>(t1 - t0).count() / iters;
}

// Profile breakdown by varying number of format args
void profileArgCounts() {
    rocket::Logger::LogEntry entry;
    std::array<char, 256> payload{};
    std::string buf;

    printf("\n=== Format round-trip latency (setArgs + runFormat) ===\n");
    printf("%-20s %8s %8s\n", "format string", "iters", "ns/call");
    printf("%-20s %8s %8s\n", "--------------------", "-------", "-------");

    struct TestCase {
        const char* label;
        const char* fmt;
        int iters;
    };

    // Use different iter counts to keep reasonable runtime
    for (auto& tc : {
        TestCase{"0-arg", "plain message", 2000000},
        TestCase{"1-arg int", "val={}", 2000000},
        TestCase{"2-arg", "{} int={}", 1000000},
        TestCase{"3-arg", "{} int={} dbl={}", 1000000},
        TestCase{"4-arg (benchmark)", "test {} int={} double={} str={}", 1000000},
        TestCase{"8-arg", "{} {} {} {} {} {} {} {}", 500000},
    }) {
        buf.clear();
        auto t0 = steady_clock::now();
        for (int i = 0; i < tc.iters; ++i) {
            buf.clear();
            // We can't dynamically choose args, so we test 4-arg case repeatedly
        }
        auto t1 = steady_clock::now();
    }

    // Manual per-case benchmarks
    // 0-arg
    {
        int n = 2000000;
        buf.clear();
        auto t0 = steady_clock::now();
        for (int i = 0; i < n; ++i) {
            buf.clear();
            entry.setArgs(payload.data(), "plain message");
            entry.runFormat(buf, payload.data());
        }
        auto t1 = steady_clock::now();
        auto ns = duration_cast<nanoseconds>(t1 - t0).count() / n;
        printf("%-20s %8d %8lld\n", "0-arg", n, (long long)ns);
    }

    // 1-arg int
    {
        int n = 2000000;
        buf.clear();
        auto t0 = steady_clock::now();
        for (int i = 0; i < n; ++i) {
            buf.clear();
            entry.setArgs(payload.data(), "val={}", i);
            entry.runFormat(buf, payload.data());
        }
        auto t1 = steady_clock::now();
        auto ns = duration_cast<nanoseconds>(t1 - t0).count() / n;
        printf("%-20s %8d %8lld\n", "1-arg int", n, (long long)ns);
    }

    // 2-arg
    {
        int n = 1000000;
        buf.clear();
        auto t0 = steady_clock::now();
        for (int i = 0; i < n; ++i) {
            buf.clear();
            entry.setArgs(payload.data(), "val={} int={}", i, i*42);
            entry.runFormat(buf, payload.data());
        }
        auto t1 = steady_clock::now();
        auto ns = duration_cast<nanoseconds>(t1 - t0).count() / n;
        printf("%-20s %8d %8lld\n", "2-arg", n, (long long)ns);
    }

    // 3-arg
    {
        int n = 1000000;
        buf.clear();
        auto t0 = steady_clock::now();
        for (int i = 0; i < n; ++i) {
            buf.clear();
            entry.setArgs(payload.data(), "val={} int={} dbl={}",
                          i, i*42, 3.14159*i);
            entry.runFormat(buf, payload.data());
        }
        auto t1 = steady_clock::now();
        auto ns = duration_cast<nanoseconds>(t1 - t0).count() / n;
        printf("%-20s %8d %8lld\n", "3-arg", n, (long long)ns);
    }

    // 4-arg (benchmark case)
    {
        int n = 1000000;
        buf.clear();
        auto t0 = steady_clock::now();
        for (int i = 0; i < n; ++i) {
            buf.clear();
            entry.setArgs(payload.data(),
                          "test {} int={} double={} str={}",
                          i, i*42, 3.14159*i, "hello");
            entry.runFormat(buf, payload.data());
        }
        auto t1 = steady_clock::now();
        auto ns = duration_cast<nanoseconds>(t1 - t0).count() / n;
        printf("%-20s %8d %8lld\n", "4-arg (benchmark)", n, (long long)ns);
    }

    // 8-arg
    {
        int n = 500000;
        buf.clear();
        auto t0 = steady_clock::now();
        for (int i = 0; i < n; ++i) {
            buf.clear();
            entry.setArgs(payload.data(), "{} {} {} {} {} {} {} {}",
                          i, i*1, i*2LL, i*3.0, true, "a", "b",
                          (void*)nullptr);
            entry.runFormat(buf, payload.data());
        }
        auto t1 = steady_clock::now();
        auto ns = duration_cast<nanoseconds>(t1 - t0).count() / n;
        printf("%-20s %8d %8lld\n", "8-arg", n, (long long)ns);
    }

    // Compare: direct fmt::format (no encode/decode)
    {
        int n = 2000000;
        buf.clear();
        auto t0 = steady_clock::now();
        for (int i = 0; i < n; ++i) {
            buf.clear();
            fmt::format_to(std::back_inserter(buf), "test {} int={} double={} str={}", i, i*42, 3.14159*i, "hello");
        }
        auto t1 = steady_clock::now();
        auto ns = duration_cast<nanoseconds>(t1 - t0).count() / n;
        printf("%-20s %8d %8lld\n", "direct fmt::format", n, (long long)ns);
    }
}

} // namespace

int main() {
    profileArgCounts();
}
