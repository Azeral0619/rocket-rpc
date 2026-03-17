#include "rocket/common/log.h"
#include "rocket/common/config.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <ctime>
#include <emmintrin.h>
#include <filesystem>
#include <fmt/base.h>
#include <fstream>
#include <ios>
#include <iostream>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>

#include <fmt/format.h>

#if defined(_MSC_VER)
#include <intrin.h>
#endif

#if defined(__x86_64__) || defined(__i386__)
#define cpu_pause() _mm_pause();
#elif defined(__aarch64__)
#define cpu_pause() asm volatile("yield" ::: "memory");
#endif

namespace rocket {
namespace {

constexpr int kShift1 = 1;
constexpr int kShift2 = 2;
constexpr int kShift4 = 4;
constexpr int kShift8 = 8;
constexpr int kShift16 = 16;
constexpr int kShift32 = 32;
constexpr std::uint64_t kNanosPerSecond = 1'000'000'000ULL;
constexpr std::uint64_t kNanosPerMilli = 1'000'000ULL;

LogLevel ParseLogLevel(std::string_view level) {
    if (level == "DEBUG" || level == "debug" || level == "Debug") {
        return LogLevel::Debug;
    }
    if (level == "INFO" || level == "info" || level == "Info") {
        return LogLevel::Info;
    }
    if (level == "WARN" || level == "warn" || level == "Warn") {
        return LogLevel::Warn;
    }
    if (level == "ERROR" || level == "error" || level == "Error") {
        return LogLevel::Error;
    }
    return LogLevel::Debug;
}

Logger::Options BuildOptionsFromConfig() {
    Logger::Options options;
    const auto cfg = Config::getInstance().getConfig();
    if (!cfg) {
        return options;
    }

    if (!cfg->log_file_name.empty()) {
        const std::filesystem::path file_name{cfg->log_file_name};
        if (!cfg->log_file_path.empty() && !file_name.is_absolute()) {
            const std::filesystem::path file_path{cfg->log_file_path};
            options.file_path = file_path / file_name;
        } else {
            options.file_path = file_name;
        }
    }

    options.level = ParseLogLevel(cfg->log_level);

    if (cfg->log_sync_interval > 0) {
        options.flush_interval_ms = static_cast<std::size_t>(cfg->log_sync_interval);
    }

    if (cfg->log_max_file_size > 0) {
        options.max_file_size = static_cast<std::size_t>(cfg->log_max_file_size);
    }

    if (cfg->log_queue_capacity > 0) {
        options.queue_capacity = static_cast<std::size_t>(cfg->log_queue_capacity);
    }

    return options;
}

[[nodiscard]] std::size_t NextPow2(std::size_t value) {
    if (value < 2) {
        return 2;
    }

    value--;
    value |= value >> kShift1;
    value |= value >> kShift2;
    value |= value >> kShift4;
    value |= value >> kShift8;
    value |= value >> kShift16;
    if constexpr (sizeof(std::size_t) >= sizeof(std::uint64_t)) {
        value |= value >> kShift32;
    }
    return value + 1;
}

} // namespace

// ============================================================================
// MPSC Ring Queue 实现
// ============================================================================

template <typename T>
Logger::MpscRingQueue<T>::MpscRingQueue(std::size_t capacity) {
    const std::size_t actual = NextPow2(std::max<std::size_t>(capacity, 2));
    m_capacity = actual;
    m_buffer.resize(actual);
    m_mask = actual - 1;

    for (std::size_t i = 0; i < actual; ++i) {
        m_buffer[i].sequence.store(i, std::memory_order_relaxed);
    }
}

template <typename T>
bool Logger::MpscRingQueue<T>::enqueue(T&& item) {
    std::size_t pos = m_enqueue_pos.load(std::memory_order_relaxed);
    for (;;) {
        Cell& cell = m_buffer[pos & m_mask];
        const std::size_t seq = cell.sequence.load(std::memory_order_acquire);
        const auto diff = static_cast<std::intptr_t>(seq) - static_cast<std::intptr_t>(pos);

        if (diff == 0) [[likely]] {
            if (m_enqueue_pos.compare_exchange_weak(pos, pos + 1, std::memory_order_relaxed)) {
                cell.storage = std::move(item);
                cell.sequence.store(pos + 1, std::memory_order_release);
                return true;
            }
            continue;
        }
        if (diff < 0) [[unlikely]] {
            return false;
        }

        pos = m_enqueue_pos.load(std::memory_order_relaxed);
    }
}

template <typename T>
bool Logger::MpscRingQueue<T>::tryDequeue(T& out) {
    const std::size_t pos = m_dequeue_pos; // 单消费者，无需atomic
    Cell& cell = m_buffer[pos & m_mask];

    const std::size_t seq = cell.sequence.load(std::memory_order_acquire);
    const auto diff = static_cast<std::intptr_t>(seq) - static_cast<std::intptr_t>(pos + 1);

    if (diff < 0) {
        return false;
    }

    out = std::move(cell.storage);
    cell.sequence.store(pos + m_capacity, std::memory_order_release);
    m_dequeue_pos = pos + 1;
    return true;
}

template class Logger::MpscRingQueue<Logger::LogEntry>;

// ============================================================================
// Logger 实现
// ============================================================================

Logger::~Logger() { stop(); }

void Logger::start() { start(BuildOptionsFromConfig()); }

void Logger::start(const Options& options) {
    std::lock_guard<std::mutex> lock(m_lifecycle_mutex);
    if (m_running.load(std::memory_order_acquire)) {
        return;
    }

    m_file_path = options.file_path;
    m_flush_interval_ms = std::max<std::size_t>(1, options.flush_interval_ms);
    m_max_file_size = std::max<std::size_t>(1, options.max_file_size);
    m_level.store(options.level, std::memory_order_release);
    m_dropped_count.store(0, std::memory_order_release);
    m_enqueue_notify_counter.store(0, std::memory_order_release);

    const std::size_t capacity = options.queue_capacity;
    m_queue = std::make_unique<MpscRingQueue<LogEntry>>(capacity);

    // {
    //     std::lock_guard<std::mutex> file_lock(m_file_mutex);
    //     closeLogFile();
    //     openLogFile();
    // }

    m_running.store(true, std::memory_order_release);
    m_consumer = std::jthread([this] { consumerRun(); });
}

void Logger::stop() {
    std::lock_guard<std::mutex> lock(m_lifecycle_mutex);
    if (!m_running.exchange(false, std::memory_order_acq_rel)) {
        return;
    }

    m_consumer_cv.notify_one();
    // m_consumer_signal.fetch_add(1, std::memory_order_release);
    // m_consumer_signal.notify_one();

    if (m_consumer.joinable()) {
        m_consumer.join();
    }

    // {
    //     std::lock_guard<std::mutex> file_lock(m_file_mutex);
    //     closeLogFile();
    // }

    const auto dropped = m_dropped_count.load(std::memory_order_relaxed);
    if (dropped > 0) {
        std::cerr << "[Logger] total dropped logs=" << dropped << '\n';
    }
}

void Logger::reloadFromConfig() {
    const Options options = BuildOptionsFromConfig();

    std::lock_guard<std::mutex> lock(m_lifecycle_mutex);
    m_level.store(options.level, std::memory_order_release);
    m_flush_interval_ms = std::max<std::size_t>(1, options.flush_interval_ms);
    m_max_file_size = std::max<std::size_t>(1, options.max_file_size);

    const bool path_changed = (m_file_path != options.file_path);
    m_file_path = options.file_path;

    // if (m_running.exchange(false, std::memory_order_acq_rel)) {
    //     // m_consumer_signal.fetch_add(1, std::memory_order_release);
    //     // m_consumer_signal.notify_one();
    //     m_consumer_cv.notify_one();
    //     if (m_consumer.joinable()) {
    //         m_consumer.join();
    //     }
    // }

    if (!m_running.load(std::memory_order_acquire)) { // false
        // m_consumer_signal.fetch_add(1, std::memory_order_release);
        // m_consumer_signal.notify_one();
        m_consumer_cv.notify_one();
        if (m_consumer.joinable()) {
            m_consumer.join();
        }
        m_running.store(true, std::memory_order_release);
        m_consumer = std::jthread([this] { consumerRun(); });
    } else { // true
        if (path_changed) {
            m_running.store(false, std::memory_order_release);
            // m_consumer_signal.fetch_add(1, std::memory_order_release);
            // m_consumer_signal.notify_one();
            m_consumer_cv.notify_one();
            if (m_consumer.joinable()) {
                m_consumer.join();
            }
            m_running.store(true, std::memory_order_release);
            m_consumer = std::jthread([this] { consumerRun(); });
        }
    }

    // if (path_changed) {
    //     // std::lock_guard<std::mutex> file_lock(m_file_mutex);
    //     // closeLogFile();
    //     // openLogFile();
    // }
}

void Logger::ensureStarted() {
    if (!m_running.load(std::memory_order_acquire)) {
        start();
    }
}

void Logger::flush() {
    if (!m_running.load(std::memory_order_acquire)) {
        return;
    }
    m_need_flush.store(true, std::memory_order_release);
    // m_consumer_signal.fetch_add(1, std::memory_order_release);
    // m_consumer_signal.notify_one();
    m_consumer_cv.notify_one();
}

void Logger::setLevel(LogLevel level) noexcept { m_level.store(level, std::memory_order_release); }

LogLevel Logger::level() const noexcept { return m_level.load(std::memory_order_acquire); }

bool Logger::isRunning() const noexcept { return m_running.load(std::memory_order_acquire); }

void Logger::consumerRun() {
    closeLogFile();
    openLogFile();

    std::string write_buffer;
    write_buffer.reserve(kWriteBufferReserve);

    LogEntry entry;

    auto last_flush_time = std::chrono::steady_clock::now();

    while (m_running.load(std::memory_order_acquire)) {
        std::size_t dequeued = 0;

        while (m_queue && dequeued < kMaxDequeuePerRound && m_queue->tryDequeue(entry)) {
            ++dequeued;
            write_buffer.append(entry.data.data(), entry.size);
        }

        const bool has_data = !write_buffer.empty();

        if (has_data) {
            if (m_log_file.is_open()) [[likely]] {
                rotateIfNeeded();
                m_log_file.write(write_buffer.data(), static_cast<std::streamsize>(write_buffer.size()));

                m_current_file_size += write_buffer.size();
            } else {
                std::cerr.write(write_buffer.data(), static_cast<std::streamsize>(write_buffer.size()));
            }
            write_buffer.clear();
        }

        const bool flush = m_need_flush.exchange(false, std::memory_order_acq_rel);

        auto now = std::chrono::steady_clock::now();
        const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - last_flush_time);
        const bool timeout_flush = elapsed.count() >= static_cast<long long>(m_flush_interval_ms);

        if (flush || timeout_flush) {
            // std::lock_guard<std::mutex> file_lock(m_file_mutex);
            if (m_log_file.is_open()) [[likely]] {
                m_log_file.flush();
                last_flush_time = now;
            }
        }

        if (dequeued == 0) {
            if (!m_queue->empty() || !m_running.load(std::memory_order_acquire) ||
                m_need_flush.load(std::memory_order_acquire)) {
                continue;
            }
            // constexpr auto kSpinL1 = 32;
            // constexpr auto kSpinL2 = 200;
            // bool continue_flag = false;
            // for (int i = 0; i < kSpinL2; ++i) {
            //     if (!m_queue->empty() || !m_running.load(std::memory_order_acquire) ||
            //         m_need_flush.load(std::memory_order_acquire)) {
            //         continue_flag = true;
            //         break;
            //     }
            //     if (i < kSpinL1) {
            //         cpu_pause();
            //     } else {
            //         std::this_thread::yield();
            //     }
            // }
            // if (continue_flag) {
            //     continue;
            // }

            std::unique_lock<std::mutex> cv_lock(m_consumer_mutex);
            // m_consumer_signal.wait(std::memory_order_acquire);
            now = std::chrono::steady_clock::now();
            auto next_deadline = last_flush_time + std::chrono::milliseconds(m_flush_interval_ms);
            auto remaining = next_deadline - now;
            if (remaining < std::chrono::milliseconds(0)) {
                // remaining = std::chrono::milliseconds(0);
                continue;
            }
            m_consumer_cv.wait_for(cv_lock, remaining, [this] {
                return !m_queue->empty() || !m_running.load(std::memory_order_acquire) ||
                       m_need_flush.load(std::memory_order_acquire);
            });
        }
    }

    while (m_queue && m_queue->tryDequeue(entry)) {
        write_buffer.append(entry.data.data(), entry.size);
    }

    if (!write_buffer.empty()) {
        // std::lock_guard<std::mutex> file_lock(m_file_mutex);
        if (m_log_file.is_open()) {
            m_log_file.write(write_buffer.data(), static_cast<std::streamsize>(write_buffer.size()));
            m_log_file.flush();
        }
    }
    closeLogFile();
}

void Logger::openLogFile() {
    std::error_code ec;
    const auto parent = m_file_path.parent_path();
    if (!parent.empty()) {
        std::filesystem::create_directories(parent, ec);
    }

    m_log_file.open(m_file_path, std::ios::app);
    if (!m_log_file.is_open()) {
        return;
    }

    m_current_file_size = 0;
    if (std::filesystem::exists(m_file_path, ec)) {
        m_current_file_size = std::filesystem::file_size(m_file_path, ec);
    }
}

void Logger::closeLogFile() {
    if (m_log_file.is_open()) {
        m_log_file.flush();
        m_log_file.close();
    }
}

void Logger::rotateIfNeeded() {
    if (m_max_file_size == 0 || m_current_file_size < m_max_file_size) {
        return;
    }

    closeLogFile();

    const auto now = std::chrono::system_clock::now();
    const auto stamp = std::chrono::duration_cast<std::chrono::seconds>(now.time_since_epoch()).count();
    const auto rotated = m_file_path.string() + "." + std::to_string(stamp);

    std::error_code ec;
    std::filesystem::rename(m_file_path, rotated, ec);
    openLogFile();
}

} // namespace rocket
