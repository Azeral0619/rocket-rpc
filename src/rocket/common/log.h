#pragma once

#include "rocket/common/runtime.h"
#include "rocket/common/singleton.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fmt/base.h>
#include <fstream>
#include <memory>
#include <mutex>
#include <string_view>
#include <sys/types.h>
#include <thread>
#include <vector>

#include <fmt/format.h>
#include <fmt/std.h>

#if defined(__linux__)
#include <sys/syscall.h>
#include <unistd.h>
#elif defined(_WIN32)
#include <windows.h>
#else
#include <functional>
#endif

namespace rocket {

#ifndef ROCKET_MIN_LOG_LEVEL
// NOLINTNEXTLINE(cppcoreguidelines-macro-usage)
#define ROCKET_MIN_LOG_LEVEL 0
#endif

enum class LogLevel : std::uint8_t {
    Debug = 0,
    Info = 1,
    Warn = 2,
    Error = 3,
};

[[nodiscard]] constexpr std::string_view LogLevelToString(LogLevel level) {
    switch (level) {
    case LogLevel::Debug:
        return "DEBUG";
    case LogLevel::Info:
        return "INFO";
    case LogLevel::Warn:
        return "WARN";
    case LogLevel::Error:
        return "ERROR";
    }
    return "UNKNOWN";
}

class Logger final : public Singleton<Logger> {
  public:
    static constexpr std::size_t kDefaultQueueCapacity = 1U << 14;
    static constexpr std::size_t kLogDataSize = 1024;
    static constexpr std::size_t kDefaultFlushIntervalMs = 50;
    static constexpr std::size_t kDefaultMaxFileSize = 1024ULL * 1024ULL * 1024ULL;
    static constexpr std::size_t kCacheLineSize = 64;

    struct LogEntry {
        std::uint64_t thread_id{0};
        std::uint64_t timestamp_ns{0};
        std::uint32_t level{0};
        std::uint32_t size{0};
        bool truncated{false};
        std::array<char, kLogDataSize> data{};
    };

    struct Options {
        std::filesystem::path file_path{"./rocket_rpc.log"};
        std::size_t queue_capacity{kDefaultQueueCapacity};
        std::size_t flush_interval_ms{kDefaultFlushIntervalMs};
        std::size_t max_file_size{kDefaultMaxFileSize};
        LogLevel level{LogLevel::Debug};
    };

    void start();
    void start(const Options& options);
    void stop();
    void reloadFromConfig();
    Logger(const Logger&) = delete;
    Logger& operator=(const Logger&) = delete;
    Logger(Logger&&) = delete;
    Logger& operator=(Logger&&) = delete;

    template <typename... Args>
    void log(LogLevel level, fmt::format_string<Args...> fmt, Args&&... args);

    void flush();

    void setLevel(LogLevel level) noexcept;
    [[nodiscard]] LogLevel level() const noexcept;
    [[nodiscard]] bool isRunning() const noexcept;

  private:
    template <typename T>
    class MpscRingQueue {
      public:
        explicit MpscRingQueue(std::size_t capacity);

        [[nodiscard]] bool enqueue(T&& item);
        [[nodiscard]] bool tryDequeue(T& out);
        bool empty() { return m_enqueue_pos.load(std::memory_order_acquire) == m_dequeue_pos; }

      private:

        struct alignas(kCacheLineSize) Cell {
            std::atomic<std::size_t> sequence;
            T storage;

            Cell() noexcept : sequence(0), storage() {}
            ~Cell() = default;
            Cell(const Cell&) = delete;
            Cell& operator=(const Cell&) = delete;
            Cell(Cell&& other) noexcept
                : sequence(other.sequence.load(std::memory_order_relaxed)), storage(std::move(other.storage)) {}

            Cell& operator=(Cell&& other) noexcept {
                if (this != &other) {
                    sequence.store(other.sequence.load(std::memory_order_relaxed), std::memory_order_relaxed);
                    storage = std::move(other.storage);
                }
                return *this;
            }
        };

        std::size_t m_mask{0};
        std::size_t m_capacity{0};
        std::vector<Cell> m_buffer;
        alignas(kCacheLineSize) std::atomic<std::size_t> m_enqueue_pos{0};
        alignas(kCacheLineSize) std::size_t m_dequeue_pos{0};
    };

    friend class Singleton<Logger>;
    Logger() = default;
    ~Logger();

    void consumerRun();
    void ensureStarted();
    void openLogFile();
    void closeLogFile();
    void rotateIfNeeded();

    static constexpr std::size_t kMaxDequeuePerRound = 256;
    static constexpr std::size_t kWriteBufferReserve = 64ULL * 1024;
    static constexpr int kYearBase = 1900;
    static constexpr std::uint32_t kNotifyEveryNEnqueue = 32;
    static constexpr std::uint32_t kNotifyMask = kNotifyEveryNEnqueue - 1;

    std::unique_ptr<MpscRingQueue<LogEntry>> m_queue;

    std::jthread m_consumer;
    std::atomic<bool> m_running{false};
    std::atomic<LogLevel> m_level{LogLevel::Debug};
    std::atomic<bool> m_need_flush{false};

    std::filesystem::path m_file_path;
    std::size_t m_flush_interval_ms{kDefaultFlushIntervalMs};
    std::size_t m_max_file_size{kDefaultMaxFileSize};

    std::mutex m_lifecycle_mutex;
    // std::mutex m_file_mutex;
    std::condition_variable m_consumer_cv;
    std::mutex m_consumer_mutex;
    // alignas(kCacheLineSize) std::atomic<std::uint32_t> m_consumer_signal{0};
    // std::atomic<bool> m_data_ready{false};

    std::ofstream m_log_file;
    std::size_t m_current_file_size{0};

    std::atomic<std::uint64_t> m_dropped_count{0};
    alignas(kCacheLineSize) std::atomic<std::uint32_t> m_enqueue_notify_counter{0};
};

template <typename... Args>
void Logger::log(LogLevel level, fmt::format_string<Args...> fmt, Args&&... args) {
    if (static_cast<std::uint8_t>(level) < static_cast<std::uint8_t>(m_level.load(std::memory_order_relaxed))) {
        return;
    }

    ensureStarted();

    static thread_local LogEntry entry;
    static thread_local std::uint64_t cached_tid = []() {
#if defined(__linux__)
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg)
        return static_cast<std::uint64_t>(::syscall(SYS_gettid));
#elif defined(_WIN32)
        return static_cast<std::uint64_t>(::GetCurrentThreadId());
#else
        return std::hash<std::thread::id>{}(std::this_thread::get_id());
#endif
    }();
    entry.thread_id = cached_tid;
    entry.timestamp_ns = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::system_clock::now().time_since_epoch())
            .count());
    entry.level = static_cast<std::uint32_t>(level);

    constexpr std::uint64_t kNanosPerSecond = 1'000'000'000ULL;
    constexpr std::uint64_t kNanosPerMilli = 1'000'000ULL;
    constexpr int kYearBase = 1900;

    const auto secs = static_cast<std::time_t>(entry.timestamp_ns / kNanosPerSecond);
    const auto millis = static_cast<int>((entry.timestamp_ns % kNanosPerSecond) / kNanosPerMilli);

    constexpr std::size_t kDateTimeBufferSize = 32;
    static thread_local struct {
        std::time_t last_sec{0};
        std::array<char, kDateTimeBufferSize> datetime_buf{};
        std::size_t len{0};
    } time_cache;

    if (secs != time_cache.last_sec) {
        std::tm tm_buf{};
#if defined(_WIN32)
        localtime_s(&tm_buf, &secs);
#else
        localtime_r(&secs, &tm_buf);
#endif
        auto result = fmt::format_to_n(time_cache.datetime_buf.data(), time_cache.datetime_buf.size() - 1,
                                       "{:04d}-{:02d}-{:02d} {:02d}:{:02d}:{:02d}", tm_buf.tm_year + kYearBase,
                                       tm_buf.tm_mon + 1, tm_buf.tm_mday, tm_buf.tm_hour, tm_buf.tm_min, tm_buf.tm_sec);
        time_cache.len = result.size;
        time_cache.last_sec = secs;
    }

    auto prefix_result = fmt::format_to_n(entry.data.data(), kLogDataSize - 1, "{}.{:03d} [{}] [tid={}]",
                                          std::string_view(time_cache.datetime_buf.data(), time_cache.len), millis,
                                          LogLevelToString(level), entry.thread_id);

    auto& msgId = RunTime::GetRunTime()->m_msgid;
    auto& methodName = RunTime::GetRunTime()->m_method_name;

    auto prefix_len = std::min(prefix_result.size, kLogDataSize - 1);

    if (!msgId.empty()) {
        auto msgid_result =
            fmt::format_to_n(entry.data.data() + prefix_len, kLogDataSize - prefix_len - 1, " [msgid={}]", msgId);
        prefix_len = std::min(prefix_len + msgid_result.size, kLogDataSize - 1);
    }

    if (!methodName.empty()) {
        auto method_result =
            fmt::format_to_n(entry.data.data() + prefix_len, kLogDataSize - prefix_len - 1, " [method={}]", methodName);
        prefix_len = std::min(prefix_len + method_result.size, kLogDataSize - 1);
    }

    if (prefix_len < kLogDataSize - 1) {
        // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-constant-array-index)
        entry.data[prefix_len++] = ' ';
    }

    const auto remaining = kLogDataSize - prefix_len - 5;
    auto msg_result = fmt::format_to_n(entry.data.data() + prefix_len, remaining, fmt, std::forward<Args>(args)...);

    const bool msg_truncated = msg_result.size >= remaining;
    auto total_len = prefix_len + std::min(msg_result.size, remaining);

    if (msg_truncated) {
        entry.data[total_len] = '.';
        entry.data[total_len + 1] = '.';
        entry.data[total_len + 2] = '.';
        total_len += 3;
    }

    entry.data[total_len++] = '\n';

    entry.size = static_cast<std::uint32_t>(total_len);
    entry.truncated = msg_truncated;
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-constant-array-index)
    entry.data[entry.size] = '\0';

    if (!m_queue->enqueue(LogEntry{entry})) {
        m_dropped_count.fetch_add(1, std::memory_order_relaxed);
    } else {
        if ((m_enqueue_notify_counter.fetch_add(1, std::memory_order_relaxed) & kNotifyMask) == kNotifyMask) {
            m_consumer_cv.notify_one();
        }
    }
}

} // namespace rocket

#if ROCKET_MIN_LOG_LEVEL <= 0
#define ROCKET_LOG_DEBUG(fmt, ...) ::rocket::Logger::getInstance().log(::rocket::LogLevel::Debug, fmt, ##__VA_ARGS__)
#else
#define ROCKET_LOG_DEBUG(fmt, ...) (void)0
#endif

#if ROCKET_MIN_LOG_LEVEL <= 1
#define ROCKET_LOG_INFO(fmt, ...) ::rocket::Logger::getInstance().log(::rocket::LogLevel::Info, fmt, ##__VA_ARGS__)
#else
#define ROCKET_LOG_INFO(fmt, ...) (void)0
#endif

#if ROCKET_MIN_LOG_LEVEL <= 2
#define ROCKET_LOG_WARN(fmt, ...) ::rocket::Logger::getInstance().log(::rocket::LogLevel::Warn, fmt, ##__VA_ARGS__)
#else
#define ROCKET_LOG_WARN(fmt, ...) (void)0
#endif

#if ROCKET_MIN_LOG_LEVEL <= 3
#define ROCKET_LOG_ERROR(fmt, ...) ::rocket::Logger::getInstance().log(::rocket::LogLevel::Error, fmt, ##__VA_ARGS__)
#else
#define ROCKET_LOG_ERROR(fmt, ...) (void)0
#endif
