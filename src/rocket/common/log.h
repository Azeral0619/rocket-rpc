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
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <deque>

#include <fmt/format.h>
#include <fmt/std.h>

#if defined(__linux__)
#include <sys/syscall.h>
#include <unistd.h>
#elif defined(_WIN32)
#include <windows.h>
#elif defined(__APPLE__)
#include <pthread.h>
#endif

namespace rocket {

#ifndef ROCKET_MIN_LOG_LEVEL
#ifdef NDEBUG
#define ROCKET_MIN_LOG_LEVEL 1   // release: strip DEBUG
#else
#define ROCKET_MIN_LOG_LEVEL 0   // debug: keep all
#endif
#endif

enum class LogLevel : std::uint8_t { Debug = 0, Info = 1, Warn = 2, Error = 3 };

[[nodiscard]] constexpr std::string_view LogLevelToString(LogLevel level) {
    switch (level) {
    case LogLevel::Debug: return "DEBUG";
    case LogLevel::Info:  return "INFO";
    case LogLevel::Warn:  return "WARN";
    case LogLevel::Error: return "ERROR";
    }
    return "UNKNOWN";
}

class Logger final : public Singleton<Logger> {
  public:
    static constexpr std::size_t kDefaultQueueCapacity = 1U << 14;
    // Queue capacity in *bytes* (not entries), Quill-style.
    // 256 KB ≅ 3500 entries @ 72 B/entry — plenty for burst absorption.
    static constexpr std::size_t kPerThreadQueueBytes = 256 * 1024;
    static constexpr std::size_t kDefaultFlushIntervalMs = 50;
    static constexpr std::size_t kDefaultMaxFileSize = 1024ULL * 1024ULL * 1024ULL;

    struct LogEntry {
        // Per-arg codec: encode arguments with type tag + compact value inline.
        // Consumer reconstructs fmt::format_args via a switch.  No fmt internals.
        static constexpr std::size_t kFmtStorageSize = 80;

        enum class ArgType : std::uint8_t {
            Int = 0, Uint, Int64, Uint64, Double, Bool,
            Cstring, StringView, Ptr
        };

        template <typename... Args>
        void setArgs(fmt::string_view fmt, Args&&... args) {
            constexpr int N = static_cast<int>(sizeof...(Args));
            num_args = static_cast<std::uint8_t>(N);
            fmt_ptr = fmt.data();
            fmt_len = static_cast<std::uint16_t>(fmt.size());

            if constexpr (N == 0) return;

            auto store = fmt::make_format_args(args...);
            auto fargs = fmt::format_args(store);

            // ── Pass 1: measure total size ──────────────────────────
            // Layout: [num_args=1B] [N×tag] [values…] [string data…]
            uint8_t tags[15];
            const char* str_datas[16];
            size_t str_sizes[16];
            int nf = 0;
            size_t total = 1 + static_cast<size_t>(N);          // header + tags
            total = (total + 7) & ~static_cast<size_t>(7);      // align values to 8B

            for (int i = 0; i < N; ++i) {
                ArgType tag = ArgType::Int;
                fargs.get(i).visit([&](auto val) {
                    using T = std::decay_t<decltype(val)>;
                    if constexpr (std::is_same_v<T, int>) {
                        tag = ArgType::Int; total += 4;
                    } else if constexpr (std::is_same_v<T, unsigned int>) {
                        tag = ArgType::Uint; total += 4;
                    } else if constexpr (std::is_same_v<T, long long>) {
                        tag = ArgType::Int64; total += 8;
                    } else if constexpr (std::is_same_v<T, unsigned long long>) {
                        tag = ArgType::Uint64; total += 8;
                    } else if constexpr (std::is_same_v<T, double>) {
                        tag = ArgType::Double; total += 8;
                    } else if constexpr (std::is_same_v<T, bool>) {
                        tag = ArgType::Bool; total += 1;
                    } else if constexpr (std::is_same_v<T, const char*>) {
                        tag = ArgType::Cstring;
                        total += 2;  // offset
                        if (val) {
                            str_datas[nf] = val;
                            str_sizes[nf] = strlen(val) + 1;
                            total += str_sizes[nf];
                            ++nf;
                        }
                    } else if constexpr (std::is_same_v<T, fmt::string_view>) {
                        tag = ArgType::StringView;
                        total += 2 + 8;  // offset + size
                        str_datas[nf] = val.data();
                        str_sizes[nf] = val.size();
                        total += str_sizes[nf];
                        ++nf;
                    } else {
                        tag = ArgType::Ptr;
                        total += sizeof(val);
                    }
                });
                tags[i] = static_cast<uint8_t>(tag);
            }

            // ── Allocate (inline or heap) ──────────────────────────
            char* dst;
            if (total <= kFmtStorageSize) {
                heap_allocated = false;
                dst = fmt_data;
            } else {
                heap_allocated = true;
                dst = new char[total];
                arg_data = dst;
            }

            // ── Pass 2: encode ─────────────────────────────────────
            dst[0] = static_cast<char>(N);
            for (int i = 0; i < N; ++i) dst[1 + i] = static_cast<char>(tags[i]);
            char* p = dst + 1 + N;
            p = alignPtr(p, 8);

            uint16_t* off_buf[16];
            nf = 0;
            for (int i = 0; i < N; ++i) {
                encodeOneArg(p, fargs.get(i), str_datas, str_sizes, off_buf, nf);
            }
            for (int j = 0; j < nf; ++j) {
                *off_buf[j] = static_cast<uint16_t>(p - dst);
                memcpy(p, str_datas[j], str_sizes[j]);
                p += str_sizes[j];
            }
        }

        void runFormat(std::string& out) {
            if (num_args == 0) {
                out.append(fmt_ptr, fmt_len);
                return;
            }
            auto* data = heap_allocated ? arg_data : fmt_data;
            auto* base = data;
            int n = static_cast<int>(*data++);
            const auto* tags = reinterpret_cast<const uint8_t*>(data);
            const char* p = alignPtr(data + n, 8);

            fmt::basic_format_arg<fmt::format_context> decoded[15];
            for (int i = 0; i < n; ++i)
                p = decodeOneArg(base, p, static_cast<ArgType>(tags[i]), decoded[i]);

            fmt::basic_format_args<fmt::format_context> fargs(decoded, n);
            fmt::vformat_to(std::back_inserter(out),
                fmt::string_view(fmt_ptr, fmt_len), fargs);

            if (heap_allocated) delete[] arg_data;
        }

      private:
        static const char* alignPtr(const char* p, int a) {
            auto v = reinterpret_cast<uintptr_t>(p);
            return reinterpret_cast<const char*>((v + a - 1) & ~static_cast<uintptr_t>(a - 1));
        }
        static char* alignPtr(char* p, int a) {
            auto v = reinterpret_cast<uintptr_t>(p);
            return reinterpret_cast<char*>((v + a - 1) & ~static_cast<uintptr_t>(a - 1));
        }

        static ArgType encodeOneArg(char*& p, fmt::basic_format_arg<fmt::format_context> arg,
                                    const char* datas[], size_t sizes[], uint16_t* offs[], int& nf) {
            ArgType tag = ArgType::Int;
            arg.visit([&](auto val) {
                using T = std::decay_t<decltype(val)>;
                if constexpr (std::is_same_v<T, int>) {
                    tag = ArgType::Int; memcpy(p, &val, 4); p += 4;
                } else if constexpr (std::is_same_v<T, unsigned int>) {
                    tag = ArgType::Uint; memcpy(p, &val, 4); p += 4;
                } else if constexpr (std::is_same_v<T, long long>) {
                    tag = ArgType::Int64; memcpy(p, &val, 8); p += 8;
                } else if constexpr (std::is_same_v<T, unsigned long long>) {
                    tag = ArgType::Uint64; memcpy(p, &val, 8); p += 8;
                } else if constexpr (std::is_same_v<T, double>) {
                    tag = ArgType::Double; memcpy(p, &val, 8); p += 8;
                } else if constexpr (std::is_same_v<T, bool>) {
                    tag = ArgType::Bool; *p++ = val ? 1 : 0;
                } else if constexpr (std::is_same_v<T, const char*>) {
                    tag = ArgType::Cstring;
                    offs[nf] = reinterpret_cast<uint16_t*>(p); p += 2;
                    if (val) { datas[nf] = val; sizes[nf] = strlen(val) + 1; ++nf; }
                } else if constexpr (std::is_same_v<T, fmt::string_view>) {
                    tag = ArgType::StringView;
                    offs[nf] = reinterpret_cast<uint16_t*>(p); p += 2;
                    reinterpret_cast<size_t*>(p)[0] = val.size(); p += 8;
                    datas[nf] = val.data(); sizes[nf] = val.size(); ++nf;
                } else if constexpr (std::is_same_v<T, const void*>) {
                    tag = ArgType::Ptr; memcpy(p, &val, 8); p += 8;
                } else {
                    tag = ArgType::Ptr; // fallback
                    memcpy(p, &val, sizeof(val)); p += sizeof(val);
                }
            });
            return tag;
        }

        static const char* decodeOneArg(const char* base, const char* p, ArgType tag,
                                        fmt::basic_format_arg<fmt::format_context>& out) {
            switch (tag) {
            case ArgType::Int:    { int v; memcpy(&v, p, 4); p += 4; out = farg(v); break; }
            case ArgType::Uint:   { unsigned v; memcpy(&v, p, 4); p += 4; out = farg(v); break; }
            case ArgType::Int64:  { long long v; memcpy(&v, p, 8); p += 8; out = farg(v); break; }
            case ArgType::Uint64: { unsigned long long v; memcpy(&v, p, 8); p += 8; out = farg(v); break; }
            case ArgType::Double: { double v; memcpy(&v, p, 8); p += 8; out = farg(v); break; }
            case ArgType::Bool:   { bool v = *p++; out = farg(v); break; }
            case ArgType::Cstring:{
                uint16_t off; memcpy(&off, p, 2); p += 2;
                out = farg(off ? reinterpret_cast<const char*>(base + off) : ""); break;
            }
            case ArgType::StringView: {
                uint16_t off; memcpy(&off, p, 2); p += 2;
                size_t sz; memcpy(&sz, p, 8); p += 8;
                out = farg(fmt::string_view(reinterpret_cast<const char*>(base + off), sz)); break;
            }
            case ArgType::Ptr:    { const void* v; memcpy(&v, p, 8); p += 8; out = farg(v); break; }
            default:              { const void* v; memcpy(&v, p, 8); p += 8; out = farg(v); break; }
            }
            return p;
        }

        template <typename T>
        static fmt::basic_format_arg<fmt::format_context> farg(T&& v) {
            return fmt::basic_format_arg<fmt::format_context>(static_cast<T&&>(v));
        }

      public:
        std::uint64_t thread_id{0};
        std::uint64_t timestamp_ns{0};
        std::uint8_t level{0};
        // Numeric message IDs stay allocation-free in the async log record.
        std::uint64_t msgid{0};
        std::string method_name;
        std::uint8_t num_args{0};
        bool heap_allocated{false};
        const char* fmt_ptr{nullptr};
        std::uint16_t fmt_len{0};
        char* arg_data{fmt_data};
        alignas(std::max_align_t) char fmt_data[kFmtStorageSize];
    };

    struct Options {
        std::filesystem::path file_path{"./rocket_rpc.log"};
        std::size_t per_thread_queue_bytes{kPerThreadQueueBytes};
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

    // Emergency flush: attempt to write all buffered data to disk.
    // Async-signal-safe-ish: avoids heap allocation, uses try_lock.
    // Call from signal handlers (SIGSEGV/SIGABRT) or atexit hooks.
    void flushAll();

    // Install signal handlers that call flushAll() on SIGSEGV/SIGABRT/SIGBUS/SIGFPE.
    // Registers with atexit as well.  Idempotent (install once).
    static void installCrashHandler();

    void setLevel(LogLevel level) noexcept;
    [[nodiscard]] LogLevel level() const noexcept;
    [[nodiscard]] bool isRunning() const noexcept;
    [[nodiscard]] bool shouldLog(LogLevel level);
    [[nodiscard]] std::uint64_t getDroppedCount() const noexcept {
        return m_dropped_count.load(std::memory_order_relaxed);
    }

  private:
    // ── SPSC bounded ring queue (one per thread, single producer, no CAS) ──
    template <typename T>
    class SpscBoundedQueue {
      public:
        static constexpr std::size_t kCacheLine = 64;
        explicit SpscBoundedQueue(std::size_t cap);
        [[nodiscard]] T* tryClaim();
        void publish();
        [[nodiscard]] bool tryDequeue(T& out);
        [[nodiscard]] bool empty() const noexcept;
      private:
        struct alignas(kCacheLine) Cell {
            std::atomic<std::size_t> sequence;
            T storage;
            Cell() noexcept : sequence(0), storage() {}
            Cell(Cell&&) = delete;
        };
        std::size_t m_mask, m_capacity;
        std::unique_ptr<Cell[]> m_buffer;
        std::size_t m_pos{0};              // producer only
        alignas(kCacheLine) std::size_t m_dq{0};  // consumer only, no atomic needed
    };

    // ── Per-thread state ────────────────────────────────────────────────
    struct ThreadSlot {
        std::unique_ptr<SpscBoundedQueue<LogEntry>> queue;
        std::thread::id tid;
        std::uint64_t generation{0};
        bool dead{false}; // guarded by m_slot_mutex
    };

    friend class Singleton<Logger>;
    Logger() = default;
    ~Logger();
    void consumerRun();
    void writeThreadRun();
    void ensureStarted();
    std::shared_ptr<ThreadSlot> registerThisThread();
    void openLogFile();
    void closeLogFile();
    void rotateIfNeeded();

    static constexpr std::size_t kMaxDequeuePerRound = 1024;       // ~150KB per round
    static constexpr std::size_t kWriteThreshold = 256ULL * 1024;  // batch to this size before ::write
    static constexpr std::size_t kWriteBufferReserve = 256ULL * 1024;
    static constexpr std::uint32_t kNotifyEveryNEnqueue = 16;
    static constexpr std::uint32_t kNotifyMask = kNotifyEveryNEnqueue - 1;

    // config
    std::size_t m_per_thread_bytes{kPerThreadQueueBytes};
    std::filesystem::path m_file_path;
    std::size_t m_flush_interval_ms{kDefaultFlushIntervalMs};
    std::size_t m_max_file_size{kDefaultMaxFileSize};
    std::atomic<bool> m_running{false};
    std::atomic<LogLevel> m_level{LogLevel::Debug};
    std::atomic<bool> m_need_flush{false};

    // clock calibration
    std::uint64_t m_epoch_ns{0};
    std::chrono::steady_clock::time_point m_base_steady{};

    // sync (destroyed AFTER thread/queues)
    std::mutex m_lifecycle_mutex;
    std::condition_variable m_consumer_cv;
    std::mutex m_consumer_mutex;

    // per-thread queue registry
    std::atomic<std::uint64_t> m_generation{0};
    std::mutex m_slot_mutex;
    std::deque<std::shared_ptr<ThreadSlot>> m_slots;
    std::size_t m_poll_index{0};

    // consumer (format thread)
    std::jthread m_consumer;

    // write thread (I/O, separate from format thread)
    std::jthread m_writer;
    std::string m_fmt_buf; // format thread writes here
    std::string m_write_buf; // handed to write thread
    std::mutex m_write_mutex;
    std::condition_variable m_write_cv;
    bool m_write_ready{false};

    // file (raw fd — no stdio overhead)
    int m_log_fd{-1};
    std::size_t m_current_file_size{0};

    // counters
    std::atomic<std::uint64_t> m_dropped_count{0};
    alignas(SpscBoundedQueue<LogEntry>::kCacheLine) std::atomic<std::uint32_t> m_enqueue_notify_counter{0};
};

template <typename... Args>
void Logger::log(LogLevel level, fmt::format_string<Args...> fmt, Args&&... args) {
    if (static_cast<std::uint8_t>(level) < static_cast<std::uint8_t>(m_level.load(std::memory_order_relaxed)))
        return;
    if (!m_running.load(std::memory_order_acquire)) [[unlikely]] {
        ensureStarted();
        // start() loads the configured level, which may be stricter than the
        // construction default used by the first shouldLog() check.
        if (static_cast<std::uint8_t>(level) <
            static_cast<std::uint8_t>(m_level.load(std::memory_order_relaxed)))
            return;
    }

    static thread_local std::uint64_t cached_tid = []() {
#if defined(__linux__)
        return static_cast<std::uint64_t>(::syscall(SYS_gettid));
#elif defined(_WIN32)
        return static_cast<std::uint64_t>(::GetCurrentThreadId());
#elif defined(__APPLE__)
        std::uint64_t tid{0}; ::pthread_threadid_np(nullptr, &tid); return tid;
#else
        return std::hash<std::thread::id>{}(std::this_thread::get_id());
#endif
    }();

    // ── steady_clock based timestamp (saves ~30ns vs system_clock) ──────
    const auto now_steady = std::chrono::steady_clock::now();
    const std::uint64_t now_ns = m_epoch_ns +
        static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
            now_steady - m_base_steady).count());

    constexpr std::uint64_t NS_PER_SEC = 1'000'000'000ULL;
    const auto secs = static_cast<std::time_t>(now_ns / NS_PER_SEC);
    const auto millis = static_cast<int>((now_ns % NS_PER_SEC) / 1'000'000ULL);

    // ── lazy-init per-thread SPSC queue ──────────────────────────────────
    static thread_local std::shared_ptr<ThreadSlot> t_slot;
    if (!t_slot || t_slot->generation != m_generation.load(std::memory_order_acquire)) [[unlikely]]
        t_slot = registerThisThread();

    auto slot = t_slot->queue->tryClaim();
    if (!slot) [[unlikely]] {
        m_dropped_count.fetch_add(1, std::memory_order_relaxed);
        return;
    }

    // ── capture metadata ───────────────────────────────────────────────
    auto& entry = *slot;
    entry.thread_id = cached_tid;
    entry.timestamp_ns = now_ns;
    entry.level = static_cast<std::uint8_t>(level);
    auto* rt = RunTime::GetRunTime();
    entry.msgid = rt->m_msgid;
    entry.method_name = rt->m_method_name;

    // ── deferred format: consumer calls this to emit the user message ───
    entry.setArgs(fmt.str, std::forward<Args>(args)...);

    t_slot->queue->publish();

    if ((m_enqueue_notify_counter.fetch_add(1, std::memory_order_relaxed) & kNotifyMask) == kNotifyMask)
        m_consumer_cv.notify_one();
}

} // namespace rocket

#if ROCKET_MIN_LOG_LEVEL <= 0
#define ROCKET_LOG_DEBUG(fmt, ...)                                                            \
    do {                                                                                      \
        if (::rocket::Logger::getInstance().shouldLog(::rocket::LogLevel::Debug))             \
            ::rocket::Logger::getInstance().log(                                              \
                ::rocket::LogLevel::Debug, fmt, ##__VA_ARGS__);                               \
    } while (false)
#else
#define ROCKET_LOG_DEBUG(fmt, ...) (void)0
#endif
#if ROCKET_MIN_LOG_LEVEL <= 1
#define ROCKET_LOG_INFO(fmt, ...)                                                             \
    do {                                                                                      \
        if (::rocket::Logger::getInstance().shouldLog(::rocket::LogLevel::Info))              \
            ::rocket::Logger::getInstance().log(                                              \
                ::rocket::LogLevel::Info, fmt, ##__VA_ARGS__);                                \
    } while (false)
#else
#define ROCKET_LOG_INFO(fmt, ...) (void)0
#endif
#if ROCKET_MIN_LOG_LEVEL <= 2
#define ROCKET_LOG_WARN(fmt, ...)                                                             \
    do {                                                                                      \
        if (::rocket::Logger::getInstance().shouldLog(::rocket::LogLevel::Warn))              \
            ::rocket::Logger::getInstance().log(                                              \
                ::rocket::LogLevel::Warn, fmt, ##__VA_ARGS__);                                \
    } while (false)
#else
#define ROCKET_LOG_WARN(fmt, ...) (void)0
#endif
#if ROCKET_MIN_LOG_LEVEL <= 3
#define ROCKET_LOG_ERROR(fmt, ...)                                                            \
    do {                                                                                      \
        if (::rocket::Logger::getInstance().shouldLog(::rocket::LogLevel::Error))             \
            ::rocket::Logger::getInstance().log(                                              \
                ::rocket::LogLevel::Error, fmt, ##__VA_ARGS__);                               \
    } while (false)
#else
#define ROCKET_LOG_ERROR(fmt, ...) (void)0
#endif
